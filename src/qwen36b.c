/* Motore di inferenza Qwen3.6 Stage-B in C puro.
 * Supporta caricamento del checkpoint quantizzato (resident int4/int8, experts.bin pread 16K, LRU cache)
 * e implementazione di DeltaNet + GQA con kernels.h condivisi.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "st.h"
#include "tok.h"
#include "json.h"
#include "compat.h"
#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#include <notify.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif
#include "kernels.h"
#include "expert_cache.h"
#include "repetition_guard.h"
#include "thinking_budget.h"
#include "samosa_http.h"
#define matmul_qt matmul_qt_impl

static int g_direct = 0;
static int g_idot = 1;
static int g_stateful_idot = 1;
static int g_moe_down_idot = 1;
#if defined(__ARM_NEON) && defined(__ARM_FEATURE_DOTPROD)
static int g_i4s = 1;
#else
static int g_i4s = 2;
#endif

static float *falloc(int64_t n){
    if(n<0 || (uint64_t)n > SIZE_MAX/sizeof(float)){ fprintf(stderr,"falloc: n=%lld fuori range\n",(long long)n); exit(1); }
    float *p=malloc((size_t)n*sizeof(float)); if(!p){fprintf(stderr,"OOM\n");exit(1);} return p; 
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec*1e-9; }
static double peak_rss_gb(void) {
    struct rusage r; getrusage(RUSAGE_SELF,&r);
#ifdef __APPLE__
    return r.ru_maxrss/(1024.0*1024.0*1024.0);
#else
    return r.ru_maxrss/(1024.0*1024.0);
#endif
}
static double rss_gb(void) {
#ifdef __APPLE__
    task_vm_info_data_t info; mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)==KERN_SUCCESS)
        return info.phys_footprint/(1024.0*1024.0*1024.0);
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        long long pages = 0;
        if (fscanf(f, "%*s %lld", &pages) == 1) {
            fclose(f);
            return (double)pages * sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0 * 1024.0);
        }
        fclose(f);
    }
#endif
    return peak_rss_gb();
}

static inline float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }
static inline float silu(float x) { return x * sigmoid(x); }
static inline float siluf(float x) { return x * sigmoid(x); }
static inline float softplus(float x) { if (x > 20.f) return x; return logf(1.f + expf(x)); }

static void softmax_row(float *x, int n) {
    float max = x[0];
    for (int i = 1; i < n; i++) if (x[i] > max) max = x[i];
    double sum = 0;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - max); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= (float)sum;
}

static void rmsnorm_row(float *o, const float *x, const float *w, int n, float eps) {
    double ss = 0;
    for (int i = 0; i < n; i++) ss += (double)x[i] * x[i];
    float scale = 1.0f / sqrtf((float)(ss / n) + eps);
    for (int i = 0; i < n; i++) o[i] = x[i] * scale * (1.0f + w[i]);
}

static void l2norm_head(float *x, int hd, float eps) {
    double ss = 0;
    for (int i = 0; i < hd; i++) ss += (double)x[i] * x[i];
    float r = 1.0f / sqrtf((float)ss + eps);
    for (int i = 0; i < hd; i++) x[i] *= r;
}

/* ---------- config ---------- */
typedef struct {
    int hidden, n_layers, n_heads, n_kv_heads, head_dim;
    int vocab;
    float eps;
    
    // Gated DeltaNet parameters
    int linear_num_value_heads, linear_num_key_heads;
    int linear_value_head_dim, linear_key_head_dim;
    int linear_conv_kernel_dim;
    
    // MoE parameters
    int has_moe;
    int num_experts, num_experts_per_tok;
    int moe_intermediate_size;
    int shared_expert_intermediate_size;
    int mtp_layers;
    
    int *layer_type; // 0 = linear_attention, 1 = full_attention
    float rope_theta;
    float partial_rotary_factor;
} Cfg;

/* ---------- opt-in successively-refinable expert shelves ----------
 *
 * The published v1 store is deliberately an alternate I/O representation,
 * not a new arithmetic format.  Full mode joins high/low two-bit planes into
 * the exact q4 bytes expected by the established kernels.  Base mode replaces
 * each low plane code by c=1, producing q={-7,-3,1,5}.  Mixed mode makes that
 * choice independently for each router-rank/projection pair; cache entries
 * include the resulting three-bit precision mask so outputs cannot depend on
 * cache history.  Approximate modes are experimental and never implicit.
 * Only this compact validated index survives initialization--the 44 MB JSON
 * tree is destroyed immediately afterward.
 */
enum { REFINE_OFF = 0, REFINE_FULL = 1, REFINE_BASE = 2, REFINE_MIXED = 3 };
enum { REFINE_STORAGE_NONE = 0, REFINE_STORAGE_Q4 = 1,
       REFINE_STORAGE_INT8 = 2 };
enum { REFINE_GATE = 0, REFINE_UP = 1, REFINE_DOWN = 2,
       REFINE_PROJECTIONS = 3 };

typedef struct {
    uint64_t rows, cols, q4_row_bytes;
    uint64_t source_q4_offset, source_q4_bytes;
    uint64_t source_scale_offset, source_scale_bytes;
    uint64_t base_offset, base_bytes;
    uint64_t residual_offset, residual_bytes;
    uint64_t scale_offset, scale_bytes;
    uint8_t source_q4_sha256[32];
    uint8_t base_sha256[32];
    uint8_t residual_sha256[32];
    uint8_t scale_sha256[32];
} RefineProjection;

typedef struct {
    uint8_t storage;
    uint64_t source_offset, source_bytes;
    uint8_t source_sha256[32];
    uint64_t int8_offset, int8_bytes;
    uint8_t int8_sha256[32];
    RefineProjection projection[REFINE_PROJECTIONS];
} RefineExpert;

typedef struct {
    int mode, layers, experts, reported, verify_payloads;
    int full_ranks;
    uint8_t base_projection_mask;
    uint8_t *base_layers;
    int base_layer_count;
    int fd_base, fd_residual, fd_scales, fd_int8;
    uint64_t base_size, residual_size, scale_size, int8_size;
    RefineExpert *index;
    _Atomic uint64_t base_read, residual_read, scale_read, int8_read;
    _Atomic uint64_t source_aligned_bytes;
} RefineStore;

/* ---------- layer weights ---------- */
typedef struct {
    int block_type; // 0 = linear_attention, 1 = full_attention
    
    float *in_ln, *post_ln;
    
    // Gated DeltaNet (linear_attention)
    QT in_proj_qkv;
    QT in_proj_z;
    QT in_proj_b;
    QT in_proj_a;
    float *conv1d_w;
    float *A_log;
    float *dt_bias;
    float *norm_w;
    QT out_proj;
    
    // Gated Attention (full_attention)
    QT q_proj;
    QT k_proj;
    QT v_proj;
    QT o_proj;
    float *q_norm;
    float *k_norm;
    
    // FFN (MLP)
    int has_moe;
    int has_shared;
    
    // Dense FFN
    QT gate_proj;
    QT up_proj;
    QT down_proj;
    
    // MoE FFN
    QT router_w;
    
    // Shared Expert
    QT shared_gate;
    QT shared_up;
    QT shared_down;
    float *shared_gate_w;
} Layer;

/* slot di un expert: pesi quantizzati + scale. */
typedef struct {
    int eid; QT g, u, d;
    uint8_t refine_mask;
    uint8_t *slab;
    int64_t slab_cap;
    uint64_t used;
} ESlot;

typedef struct {
    Cfg c;
    shards S;
    QT embed, lm_head;
    float *final_norm;
    Layer *L;
    
    // GQA cache
    float **K, **V;
    int max_t;
    
    // DeltaNet states
    float **conv_state;
    float **recurrent_state;
    
    // Expert caching & streaming
    int fd_exp;
    int fd_exp_direct;
    int64_t *expert_offsets;
    int64_t *expert_sizes;
    uint8_t *expert_sha256;
    /* 0 means the legacy one-scale-per-row q4 container. A positive value
     * selects grouped-q4 gate/up. expert_down_bits distinguishes the all-q4
     * baseline from the mixed row-q8 down-projection candidate. */
    int expert_group_size;
    int expert_down_bits;
    ESlot ws[256]; // scratch loading slots
    /* Byte-budget LRU policy core (expert_cache.h).  Payload handles are heap
     * ESlot*; slabs recycle through eslot_pool so steady-state misses reuse
     * allocations exactly like the old swap scheme did. */
    expert_cache *ec;
    void *ec_workspace;
    uint64_t ec_budget_bytes;
    ESlot **eslot_pool;
    int eslot_pool_n, eslot_pool_cap;
    uint64_t hits, miss;
    double t_edisk, t_emm;
    /* T2.5: buffer transiente per la lettura sequenziale di un intero layer
     * di expert durante il prefill (rilasciato al primo passo di decode). */
    uint8_t *seq_buf;
    int64_t seq_buf_cap;
    uint64_t seq_reads, seq_bytes;
    RefineStore refine;
} Model;

/* T2.5 knobs — MISURATO 2026-07-12, default OFF: anche a cache di pagina
 * completamente fredda i pread sparsi bufferizzati costano 2.6 s su un
 * prefill di 108 s (2,4%) grazie al readahead del kernel, mentre la lettura
 * sequenziale F_NOCACHE paga 5.5 s e rinuncia alla page cache.  Il prefill
 * e' ~97% compute-bound su questa macchina: non esiste crossover in cui il
 * percorso sequenziale vince.  Resta come esperimento (SEQ_PREFILL=1). */
static int g_seq_prefill = 0;
static float g_seq_frac = 0.55f;
static int g_seq_min_s = 64;

static const char *refine_mode_name(int mode) {
    return mode == REFINE_FULL ? "full" : mode == REFINE_BASE ? "base" :
           mode == REFINE_MIXED ? "mixed" : "off";
}

static int refine_projection_mask_parse(const char *text, uint8_t *out) {
    if (!text || !*text || !out) return 0;
    if (!strcmp(text,"all")) { *out=7; return 1; }
    uint8_t mask=0;
    const char *cursor=text;
    while (*cursor) {
        const char *comma=strchr(cursor,',');
        size_t length=comma?(size_t)(comma-cursor):strlen(cursor);
        uint8_t bit=0;
        if (length==4 && !memcmp(cursor,"gate",4)) bit=1u<<REFINE_GATE;
        else if (length==2 && !memcmp(cursor,"up",2)) bit=1u<<REFINE_UP;
        else if (length==4 && !memcmp(cursor,"down",4)) bit=1u<<REFINE_DOWN;
        else return 0;
        if (mask&bit) return 0;
        mask|=bit;
        if (!comma) break;
        cursor=comma+1;
        if (!*cursor) return 0;
    }
    *out=mask;
    return mask!=0;
}

static int refine_decimal_span(const char *text, size_t length, int *out) {
    if (!text || !length || !out) return 0;
    unsigned value=0;
    for (size_t i=0;i<length;++i) {
        if (text[i]<'0' || text[i]>'9') return 0;
        unsigned digit=(unsigned)(text[i]-'0');
        if (value>((unsigned)INT_MAX-digit)/10u) return 0;
        value=value*10u+digit;
    }
    *out=(int)value;
    return 1;
}

static int refine_layer_mask_parse(const char *text, int layers,
                                   uint8_t *mask, int *selected) {
    if (!text || !*text || layers<1 || !mask || !selected) return 0;
    *selected=0;
    if (!strcmp(text,"all")) {
        memset(mask,1,(size_t)layers); *selected=layers; return 1;
    }
    const char *cursor=text;
    while (*cursor) {
        const char *comma=strchr(cursor,',');
        size_t length=comma?(size_t)(comma-cursor):strlen(cursor);
        const char *dash=memchr(cursor,'-',length);
        int first=-1,last=-1;
        if (dash) {
            size_t left=(size_t)(dash-cursor), right=length-left-1u;
            if (!left || !right || memchr(dash+1,'-',right) ||
                !refine_decimal_span(cursor,left,&first) ||
                !refine_decimal_span(dash+1,right,&last)) return 0;
        } else if (!refine_decimal_span(cursor,length,&first)) {
            return 0;
        } else {
            last=first;
        }
        if (first<0 || last<first || last>=layers) return 0;
        for (int layer=first;layer<=last;++layer) {
            if (mask[layer]) return 0;
            mask[layer]=1; ++*selected;
        }
        if (!comma) break;
        cursor=comma+1;
        if (!*cursor) return 0;
    }
    return *selected>0;
}

static void refine_die(const char *format, ...) {
    va_list args;
    fprintf(stderr, "refinable shelves: ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

/* json.h owns every node, object key, and string value independently.  Most
 * old call sites only freed its historical arena pointer (which is NULL now).
 * The refinable manifest is large enough that its actual tree must be released
 * deterministically once the compact index has been extracted. */
static void json_tree_free(jval *value) {
    if (!value) return;
    if (value->t == J_OBJ) {
        for (int i = 0; i < value->len; ++i) {
            free(value->keys[i]);
            json_tree_free(value->kids[i]);
        }
        free(value->keys);
        free(value->kids);
    } else if (value->t == J_ARR) {
        for (int i = 0; i < value->len; ++i) json_tree_free(value->kids[i]);
        free(value->kids);
    } else if (value->t == J_STR) {
        free(value->str);
    }
    free(value);
}

/* Small dependency-free SHA-256 used to authenticate lazily fetched regions.
 * Keeping the verifier here preserves qwen36b's existing single-source build. */
typedef struct {
    uint32_t state[8];
    uint64_t total;
    uint8_t block[64];
    size_t used;
} RefineSha256;

static uint32_t refine_rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t refine_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void refine_sha256_compress(RefineSha256 *ctx, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
        0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
        0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
        0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
        0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
        0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (unsigned i = 0; i < 16; ++i) w[i] = refine_be32(block + 4u * i);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = refine_rotr32(w[i-15],7) ^ refine_rotr32(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = refine_rotr32(w[i-2],17) ^ refine_rotr32(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1=refine_rotr32(e,6)^refine_rotr32(e,11)^refine_rotr32(e,25);
        uint32_t ch=(e&f)^(~e&g), t1=h+s1+ch+k[i]+w[i];
        uint32_t s0=refine_rotr32(a,2)^refine_rotr32(a,13)^refine_rotr32(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c), t2=s0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void refine_sha256_init(RefineSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->total = 0; ctx->used = 0;
}

static void refine_sha256_update(RefineSha256 *ctx, const void *data_, size_t bytes) {
    const uint8_t *data = (const uint8_t *)data_;
    if (UINT64_MAX - ctx->total < bytes) refine_die("SHA-256 input is too large");
    ctx->total += bytes;
    while (bytes) {
        size_t take = 64u - ctx->used;
        if (take > bytes) take = bytes;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take; data += take; bytes -= take;
        if (ctx->used == 64u) { refine_sha256_compress(ctx, ctx->block); ctx->used = 0; }
    }
}

static void refine_sha256_final(RefineSha256 *ctx, uint8_t out[32]) {
    uint64_t bits = ctx->total * 8u;
    ctx->block[ctx->used++] = 0x80u;
    if (ctx->used > 56u) {
        memset(ctx->block + ctx->used, 0, 64u - ctx->used);
        refine_sha256_compress(ctx, ctx->block); ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56u - ctx->used);
    for (unsigned i = 0; i < 8; ++i) ctx->block[63u-i] = (uint8_t)(bits >> (8u*i));
    refine_sha256_compress(ctx, ctx->block);
    for (unsigned i = 0; i < 8; ++i) {
        out[4*i]=(uint8_t)(ctx->state[i]>>24); out[4*i+1]=(uint8_t)(ctx->state[i]>>16);
        out[4*i+2]=(uint8_t)(ctx->state[i]>>8); out[4*i+3]=(uint8_t)ctx->state[i];
    }
}

static void refine_sha256(const void *data, size_t bytes, uint8_t out[32]) {
    RefineSha256 ctx; refine_sha256_init(&ctx); refine_sha256_update(&ctx, data, bytes);
    refine_sha256_final(&ctx, out);
}

static int refine_hex_digest(const char *text, uint8_t out[32]) {
    if (!text || strlen(text) != 64) return 0;
    for (int i = 0; i < 32; ++i) {
        int hi, lo;
        char a=text[2*i], b=text[2*i+1];
        hi=(a>='0'&&a<='9')?a-'0':(a>='a'&&a<='f')?a-'a'+10:-1;
        lo=(b>='0'&&b<='9')?b-'0':(b>='a'&&b<='f')?b-'a'+10:-1;
        if (hi < 0 || lo < 0) return 0;
        out[i]=(uint8_t)((hi<<4)|lo);
    }
    return 1;
}

/* ---------- opt-in adaptive routed-expert count ----------
 *
 * Qwen was trained with num_experts_per_tok experts.  Reducing that count is
 * therefore an approximation and is never enabled implicitly.  Policy
 * configuration is process-global because the command-line program owns one
 * model; keeping it out of Model also lets route metadata be checked before
 * the first forward pass.
 */
enum { MOE_POLICY_OFF = 0, MOE_POLICY_FIXED = 1, MOE_POLICY_MASS = 2 };
typedef struct {
    int mode, fixed_k;
    float mass, max_entropy, min_gap;
    int has_max_entropy, has_min_gap;
    uint64_t decisions, reduced, guarded, omitted;
    uint64_t baseline_bytes, retained_bytes;
    uint64_t histogram[65];
    int reported;
} MoePolicy;
static MoePolicy g_moe_policy;

static const char *moe_policy_name(int mode) {
    return mode == MOE_POLICY_FIXED ? "fixed" : mode == MOE_POLICY_MASS ? "mass" : "off";
}

static int parse_int_strict(const char *text, int *out) {
    if (!text || !*text) return 0;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < INT_MIN || value > INT_MAX) return 0;
    *out = (int)value;
    return 1;
}

static int parse_float_strict(const char *text, float *out) {
    if (!text || !*text) return 0;
    char *end = NULL;
    errno = 0;
    float value = strtof(text, &end);
    if (errno || !end || *end || !isfinite(value)) return 0;
    *out = value;
    return 1;
}

static int moe_policy_configure(const char *fixed, const char *mass,
                                const char *max_entropy, const char *min_gap) {
    MoePolicy p = {0};
    if (fixed && *fixed && mass && *mass) {
        fprintf(stderr, "adaptive MoE: fixed K and mass threshold are mutually exclusive\n");
        return 0;
    }
    if (fixed && *fixed) {
        if (!parse_int_strict(fixed, &p.fixed_k) || p.fixed_k < 1) {
            fprintf(stderr, "adaptive MoE: invalid fixed K '%s' (expected a positive integer)\n", fixed);
            return 0;
        }
        p.mode = MOE_POLICY_FIXED;
    } else if (mass && *mass) {
        if (!parse_float_strict(mass, &p.mass) || p.mass <= 0.f || p.mass > 1.f) {
            fprintf(stderr, "adaptive MoE: invalid mass '%s' (expected 0 < mass <= 1)\n", mass);
            return 0;
        }
        p.mode = MOE_POLICY_MASS;
    }
    if (max_entropy && *max_entropy) {
        if (!parse_float_strict(max_entropy, &p.max_entropy) || p.max_entropy < 0.f) {
            fprintf(stderr, "adaptive MoE: invalid maximum entropy '%s' (expected >= 0)\n", max_entropy);
            return 0;
        }
        p.has_max_entropy = 1;
    }
    if (min_gap && *min_gap) {
        if (!parse_float_strict(min_gap, &p.min_gap) || p.min_gap < 0.f) {
            fprintf(stderr, "adaptive MoE: invalid minimum gap '%s' (expected >= 0)\n", min_gap);
            return 0;
        }
        p.has_min_gap = 1;
    }
    if ((p.has_max_entropy || p.has_min_gap) && p.mode != MOE_POLICY_MASS) {
        fprintf(stderr, "adaptive MoE: entropy/gap guards require a mass policy\n");
        return 0;
    }
    g_moe_policy = p;
    return 1;
}

static void moe_policy_validate_model(const Cfg *c) {
    if (g_moe_policy.mode == MOE_POLICY_OFF) return;
    if (!c->has_moe) {
        fprintf(stderr, "adaptive MoE: policy requires an MoE checkpoint\n");
        exit(2);
    }
    if (c->num_experts_per_tok < 1 || c->num_experts_per_tok > 64) {
        fprintf(stderr, "adaptive MoE: checkpoint selected K=%d is outside supported range 1..64\n",
                c->num_experts_per_tok);
        exit(2);
    }
    if (g_moe_policy.mode == MOE_POLICY_FIXED &&
        g_moe_policy.fixed_k > c->num_experts_per_tok) {
        fprintf(stderr, "adaptive MoE: fixed K=%d exceeds checkpoint trained K=%d\n",
                g_moe_policy.fixed_k, c->num_experts_per_tok);
        exit(2);
    }
}

static void moe_policy_account(const Model *m, int layer, const int *ids,
                               int trained_k, int effective_k, int guarded) {
    MoePolicy *p = &g_moe_policy;
    if (p->mode == MOE_POLICY_OFF) return;
    p->decisions++;
    if (effective_k < trained_k) p->reduced++;
    if (guarded) p->guarded++;
    p->omitted += (uint64_t)(trained_k - effective_k);
    if (effective_k >= 0 && effective_k <= 64) p->histogram[effective_k]++;
    for (int i = 0; i < trained_k; i++) {
        int64_t bytes = m->expert_sizes[(int64_t)layer * m->c.num_experts + ids[i]];
        if (bytes > 0) p->baseline_bytes += (uint64_t)bytes;
        if (i < effective_k && bytes > 0) p->retained_bytes += (uint64_t)bytes;
    }
}

static void moe_policy_report(void) {
    MoePolicy *p = &g_moe_policy;
    if (p->mode == MOE_POLICY_OFF || p->reported) return;
    p->reported = 1;
    double avg = 0.0;
    uint64_t retained_refs = 0;
    for (int k = 1; k <= 64; k++) retained_refs += (uint64_t)k * p->histogram[k];
    if (p->decisions) avg = (double)retained_refs / (double)p->decisions;
    double saved = p->baseline_bytes ?
        100.0 * (1.0 - (double)p->retained_bytes / (double)p->baseline_bytes) : 0.0;
    fprintf(stderr,
        "[moe-policy] mode=%s decisions=%llu reduced=%llu guarded=%llu avg_k=%.3f "
        "omitted=%llu bytes_proxy=%llu/%llu saved=%.2f%%\n",
        moe_policy_name(p->mode), (unsigned long long)p->decisions,
        (unsigned long long)p->reduced, (unsigned long long)p->guarded, avg,
        (unsigned long long)p->omitted, (unsigned long long)p->retained_bytes,
        (unsigned long long)p->baseline_bytes, saved);
    fputs("[moe-policy] effective_k_histogram=", stderr);
    int first = 1;
    for (int k = 1; k <= 64; k++) if (p->histogram[k]) {
        fprintf(stderr, "%s%d:%llu", first ? "" : ",", k,
                (unsigned long long)p->histogram[k]);
        first = 0;
    }
    fputc('\n', stderr);
}

/* ---------- deterministic MoE route trace / replay ----------
 *
 * ROUTE_TRACE=<jsonl> records routing decisions.  ROUTE_REPLAY=<jsonl>
 * validates the trace against the currently computed router output and then
 * feeds the recorded ids/weights to the expert evaluator.  The disabled path
 * is one predictable branch in mlp_moe and performs no allocation or I/O.
 * Prompt text is never written: records contain only input token ids.
 */
enum { ROUTE_OFF = 0, ROUTE_RECORD = 1, ROUTE_REPLAY = 2 };
typedef struct {
    FILE *fp;
    int mode, schema_version, layers, experts, selected_k, rank, hidden;
    uint64_t router_hash, records;
    char *line;
    size_t line_cap;
} RouteTrace;
static RouteTrace g_route;

static void route_die(const char *message) {
    fprintf(stderr, "route trace: %s\n", message);
    exit(2);
}

static uint64_t route_hash_bytes(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}

static uint64_t route_router_hash(const Model *m) {
    uint64_t h = UINT64_C(1469598103934665603);
    const int shape[] = {m->c.n_layers, m->c.hidden, m->c.num_experts,
                         m->c.num_experts_per_tok};
    h = route_hash_bytes(h, shape, sizeof(shape));
    for (int l = 0; l < m->c.n_layers; l++) {
        const QT *t = &m->L[l].router_w;
        h = route_hash_bytes(h, &t->fmt, sizeof(t->fmt));
        if (t->fmt == 0) {
            h = route_hash_bytes(h, t->qf, (size_t)t->O * t->I * sizeof(float));
        } else {
            size_t nb = t->fmt == 1 ? (size_t)t->O * t->I
                                    : (size_t)t->O * ((t->I + 1) / 2);
            h = route_hash_bytes(h, t->fmt == 1 ? (const void *)t->q8
                                                : (const void *)t->q4, nb);
            h = route_hash_bytes(h, t->s, (size_t)t->O * sizeof(float));
        }
    }
    return h;
}

static long route_json_int(const char *line, const char *key, int *ok) {
    char pat[80]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat); char *end = NULL;
    if (!p) { *ok = 0; return 0; }
    long v = strtol(p + strlen(pat), &end, 10);
    if (end == p + strlen(pat)) { *ok = 0; return 0; }
    return v;
}

static int route_json_int_array(const char *line, const char *key, int *out, int n) {
    char pat[80]; snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *p = strstr(line, pat); if (!p) return 0; p += strlen(pat);
    for (int i = 0; i < n; i++) {
        char *end = NULL; long v = strtol(p, &end, 10); if (end == p) return 0;
        out[i] = (int)v; p = end;
        if (i + 1 < n) { if (*p != ',') return 0; p++; }
    }
    return *p == ']';
}

static int route_json_float_array(const char *line, const char *key, float *out, int n) {
    char pat[80]; snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *p = strstr(line, pat); if (!p) return 0; p += strlen(pat);
    for (int i = 0; i < n; i++) {
        char *end = NULL; float v = strtof(p, &end); if (end == p || !isfinite(v)) return 0;
        out[i] = v; p = end;
        if (i + 1 < n) { if (*p != ',') return 0; p++; }
    }
    return *p == ']';
}

static int route_read_nonempty(RouteTrace *rt) {
    for (;;) {
        ssize_t n = getline(&rt->line, &rt->line_cap, rt->fp);
        if (n < 0) return 0;
        const char *p = rt->line; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p) return 1;
    }
}

static float route_json_float(const char *line, const char *key, int *ok);

static void route_trace_init(Model *m) {
    const char *record = getenv("ROUTE_TRACE"), *replay = getenv("ROUTE_REPLAY");
    if ((!record || !*record) && (!replay || !*replay)) return;
    if (record && *record && replay && *replay)
        route_die("ROUTE_TRACE and ROUTE_REPLAY are mutually exclusive");
    if (!m->c.has_moe) route_die("routing capture requires an MoE checkpoint");
    if (m->c.num_experts_per_tok > 64) route_die("selected expert count exceeds supported trace maximum (64)");
    memset(&g_route, 0, sizeof(g_route));
    g_route.layers = m->c.n_layers; g_route.experts = m->c.num_experts;
    g_route.selected_k = m->c.num_experts_per_tok;
    g_route.rank = g_route.selected_k > 8 ? g_route.selected_k : 8;
    if (g_route.rank > g_route.experts) g_route.rank = g_route.experts;
    g_route.hidden = m->c.hidden;
    g_route.router_hash = route_router_hash(m);
    if (record && *record) {
        g_route.mode = ROUTE_RECORD;
        g_route.schema_version = 2;
        g_route.fp = fopen(record, "wb");
        if (!g_route.fp) { perror(record); exit(2); }
        setvbuf(g_route.fp, NULL, _IOFBF, 1 << 20);
        const int policy_k = g_moe_policy.mode == MOE_POLICY_FIXED ? g_moe_policy.fixed_k : 0;
        const float policy_mass = g_moe_policy.mode == MOE_POLICY_MASS ? g_moe_policy.mass : -1.f;
        const float policy_entropy = g_moe_policy.has_max_entropy ? g_moe_policy.max_entropy : -1.f;
        const float policy_gap = g_moe_policy.has_min_gap ? g_moe_policy.min_gap : -1.f;
        fprintf(g_route.fp,
            "{\"type\":\"meta\",\"schema\":\"qwen36-route-v2\",\"layers\":%d,"
            "\"experts\":%d,\"selected_k\":%d,\"rank\":%d,\"hidden\":%d,"
            "\"router_hash\":\"%016llx\",\"policy_mode\":%d,\"policy_fixed_k\":%d,"
            "\"policy_mass\":%.9g,\"policy_max_entropy\":%.9g,\"policy_min_gap\":%.9g}\n",
            g_route.layers, g_route.experts, g_route.selected_k, g_route.rank,
            g_route.hidden, (unsigned long long)g_route.router_hash,
            g_moe_policy.mode, policy_k, policy_mass, policy_entropy, policy_gap);
        fprintf(stderr, "[route] recording v2 JSONL to %s (rank=%d, router=%016llx, policy=%s)\n",
                record, g_route.rank, (unsigned long long)g_route.router_hash,
                moe_policy_name(g_moe_policy.mode));
    } else {
        g_route.mode = ROUTE_REPLAY;
        g_route.fp = fopen(replay, "rb");
        if (!g_route.fp) { perror(replay); exit(2); }
        if (!route_read_nonempty(&g_route)) route_die("replay file has no metadata header");
        int is_v1 = strstr(g_route.line, "\"schema\":\"qwen36-route-v1\"") != NULL;
        int is_v2 = strstr(g_route.line, "\"schema\":\"qwen36-route-v2\"") != NULL;
        int ok = strstr(g_route.line, "\"type\":\"meta\"") != NULL && (is_v1 != is_v2);
        g_route.schema_version = is_v2 ? 2 : 1;
        int layers = (int)route_json_int(g_route.line, "layers", &ok);
        int experts = (int)route_json_int(g_route.line, "experts", &ok);
        int selected_k = (int)route_json_int(g_route.line, "selected_k", &ok);
        int rank = (int)route_json_int(g_route.line, "rank", &ok);
        int hidden = (int)route_json_int(g_route.line, "hidden", &ok);
        const char *hp = strstr(g_route.line, "\"router_hash\":\"");
        char hs[17] = {0};
        if (!hp || strlen(hp += strlen("\"router_hash\":\"")) < 16) ok = 0;
        else memcpy(hs, hp, 16);
        char *hend = NULL; uint64_t hash = strtoull(hs, &hend, 16);
        if (!hend || *hend) ok = 0;
        if (is_v1 && g_moe_policy.mode != MOE_POLICY_OFF) {
            route_die("v1 replay has no adaptive-policy decisions; disable the policy or capture v2");
        }
        if (is_v2) {
            int policy_mode = (int)route_json_int(g_route.line, "policy_mode", &ok);
            int policy_k = (int)route_json_int(g_route.line, "policy_fixed_k", &ok);
            float policy_mass = route_json_float(g_route.line, "policy_mass", &ok);
            float policy_entropy = route_json_float(g_route.line, "policy_max_entropy", &ok);
            float policy_gap = route_json_float(g_route.line, "policy_min_gap", &ok);
            int expected_k = g_moe_policy.mode == MOE_POLICY_FIXED ? g_moe_policy.fixed_k : 0;
            float expected_mass = g_moe_policy.mode == MOE_POLICY_MASS ? g_moe_policy.mass : -1.f;
            float expected_entropy = g_moe_policy.has_max_entropy ? g_moe_policy.max_entropy : -1.f;
            float expected_gap = g_moe_policy.has_min_gap ? g_moe_policy.min_gap : -1.f;
            if (policy_mode != g_moe_policy.mode || policy_k != expected_k ||
                policy_mass != expected_mass || policy_entropy != expected_entropy ||
                policy_gap != expected_gap) {
                fprintf(stderr,
                    "route trace: policy mismatch: trace=(mode=%d k=%d mass=%.9g entropy=%.9g gap=%.9g) "
                    "run=(mode=%d k=%d mass=%.9g entropy=%.9g gap=%.9g)\n",
                    policy_mode, policy_k, policy_mass, policy_entropy, policy_gap,
                    g_moe_policy.mode, expected_k, expected_mass, expected_entropy, expected_gap);
                exit(2);
            }
        }
        if (!ok) route_die("malformed or unsupported replay metadata");
        if (layers != g_route.layers || experts != g_route.experts ||
            selected_k != g_route.selected_k || rank != g_route.rank ||
            hidden != g_route.hidden || hash != g_route.router_hash) {
            fprintf(stderr,
                "route trace: metadata mismatch: trace=(L%d E%d K%d R%d D%d %016llx) "
                "model=(L%d E%d K%d R%d D%d %016llx)\n",
                layers, experts, selected_k, rank, hidden, (unsigned long long)hash,
                g_route.layers, g_route.experts, g_route.selected_k, g_route.rank,
                g_route.hidden, (unsigned long long)g_route.router_hash);
            exit(2);
        }
        fprintf(stderr, "[route] replaying v%d %s (rank=%d, router=%016llx, policy=%s)\n",
                g_route.schema_version, replay, g_route.rank,
                (unsigned long long)g_route.router_hash, moe_policy_name(g_moe_policy.mode));
    }
}

static int route_close(void) {
    moe_policy_report();
    if (g_route.mode == ROUTE_OFF) return 0;
    int rc = 0;
    if (g_route.mode == ROUTE_REPLAY && route_read_nonempty(&g_route)) {
        fprintf(stderr, "route trace: replay has unconsumed records after %llu calls\n",
                (unsigned long long)g_route.records);
        rc = 2;
    }
    if (g_route.mode == ROUTE_RECORD && fflush(g_route.fp) != 0) { perror("route trace flush"); rc = 2; }
    if (fclose(g_route.fp) != 0) { perror("route trace close"); rc = 2; }
    fprintf(stderr, "[route] %s %llu routing records\n",
            rc ? (g_route.mode == ROUTE_RECORD ? "failed after" : "replay rejected after")
               : (g_route.mode == ROUTE_RECORD ? "recorded" : "verified"),
            (unsigned long long)g_route.records);
    free(g_route.line); memset(&g_route, 0, sizeof(g_route));
    return rc;
}

static float route_json_float(const char *line, const char *key, int *ok) {
    char pat[80]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat); char *end = NULL;
    if (!p) { *ok = 0; return 0.f; }
    float v = strtof(p + strlen(pat), &end);
    if (end == p + strlen(pat) || !isfinite(v)) { *ok = 0; return 0.f; }
    return v;
}

static void route_print_ints(FILE *f, const int *v, int n) {
    fputc('[', f); for (int i = 0; i < n; i++) fprintf(f, "%s%d", i ? "," : "", v[i]); fputc(']', f);
}
static void route_print_floats(FILE *f, const float *v, int n) {
    /* Nine significant digits are sufficient to round-trip IEEE float. */
    fputc('[', f); for (int i = 0; i < n; i++) fprintf(f, "%s%.9g", i ? "," : "", v[i]); fputc(']', f);
}

static void route_record_or_replay(int position, int token_id, int layer,
                                   int *ids, const float *scores,
                                   const float *weights, const float *selected_weights,
                                   const float *selected_cumulative, int *effective_k,
                                   float *effective_weights, float entropy, float gap,
                                   const float *cumulative) {
    RouteTrace *rt = &g_route;
    if (rt->mode == ROUTE_OFF) return;
    const int R = rt->rank, K = rt->selected_k;
    if (rt->mode == ROUTE_RECORD) {
        FILE *f = rt->fp;
        fprintf(f, "{\"type\":\"route\",\"position\":%d,\"token_id\":%d,\"layer\":%d,\"ids\":",
                position, token_id, layer);
        route_print_ints(f, ids, R);
        fputs(",\"scores\":", f); route_print_floats(f, scores, R);
        fputs(",\"weights\":", f); route_print_floats(f, weights, R);
        fputs(",\"selected_weights\":", f); route_print_floats(f, selected_weights, K);
        fputs(",\"selected_cumulative_mass\":", f);
        route_print_floats(f, selected_cumulative, K);
        fprintf(f, ",\"effective_k\":%d,\"effective_weights\":", *effective_k);
        route_print_floats(f, effective_weights, K);
        fprintf(f, ",\"entropy\":%.9g,\"top1_top2_gap\":%.9g,\"cumulative_mass\":",
                entropy, gap);
        route_print_floats(f, cumulative, R);
        fputs("}\n", f);
        if (ferror(f)) route_die("write failure");
    } else {
        if (!route_read_nonempty(rt)) route_die("replay ended before inference routing did");
        int ok = strstr(rt->line, "\"type\":\"route\"") != NULL;
        int rpos = (int)route_json_int(rt->line, "position", &ok);
        int rtoken = (int)route_json_int(rt->line, "token_id", &ok);
        int rlayer = (int)route_json_int(rt->line, "layer", &ok);
        int rids[64];
        float rscores[64], rweights[64], rselected[64], rselected_cumulative[64];
        float reffective_weights[64], rcumulative[64];
        if (!route_json_int_array(rt->line, "ids", rids, R) ||
            !route_json_float_array(rt->line, "scores", rscores, R) ||
            !route_json_float_array(rt->line, "weights", rweights, R) ||
            !route_json_float_array(rt->line, "selected_weights", rselected, K) ||
            !route_json_float_array(rt->line, "cumulative_mass", rcumulative, R)) ok = 0;
        int reffective_k = K;
        if (rt->schema_version == 2) {
            reffective_k = (int)route_json_int(rt->line, "effective_k", &ok);
            if (reffective_k < 1 || reffective_k > K ||
                !route_json_float_array(rt->line, "selected_cumulative_mass",
                                        rselected_cumulative, K) ||
                !route_json_float_array(rt->line, "effective_weights",
                                        reffective_weights, K)) ok = 0;
        } else {
            memcpy(rselected_cumulative, selected_cumulative, (size_t)K * sizeof(float));
            memcpy(reffective_weights, rselected, (size_t)K * sizeof(float));
        }
        float rentropy = route_json_float(rt->line, "entropy", &ok);
        float rgap = route_json_float(rt->line, "top1_top2_gap", &ok);
        if (!ok) route_die("malformed replay route record");
        if (rpos != position || rtoken != token_id || rlayer != layer) {
            fprintf(stderr,
                "route trace: sequence mismatch at record %llu: trace=(pos=%d token=%d layer=%d) "
                "run=(pos=%d token=%d layer=%d)\n",
                (unsigned long long)rt->records, rpos, rtoken, rlayer,
                position, token_id, layer);
            exit(2);
        }
        int mismatch = rentropy != entropy || rgap != gap;
        for (int i = 0; i < R; i++)
            if (rids[i] != ids[i] || rscores[i] != scores[i] ||
                rweights[i] != weights[i] || rcumulative[i] != cumulative[i]) mismatch = 1;
        if (reffective_k != *effective_k) mismatch = 1;
        for (int i = 0; i < K; i++) {
            if (rselected[i] != selected_weights[i]) mismatch = 1;
            if (rt->schema_version == 2 &&
                (rselected_cumulative[i] != selected_cumulative[i] ||
                 reffective_weights[i] != effective_weights[i])) mismatch = 1;
        }
        if (mismatch) {
            fprintf(stderr,
                "route trace: router divergence at record %llu (position=%d layer=%d); "
                "recorded routes were not applied\n",
                (unsigned long long)rt->records, position, layer);
            exit(2);
        }
        /* Feed the validated recorded route.  Values are expected to be bit-exact
         * after the %.9g float round-trip, but copying makes replay semantics
         * explicit and keeps future approximate-verification modes possible. */
        memcpy(ids, rids, (size_t)K * sizeof(int));
        memcpy(effective_weights, reffective_weights, (size_t)K * sizeof(float));
        *effective_k = reffective_k;
    }
    rt->records++;
}

/* json_get su una chiave assente in un oggetto obbligatorio: errore esplicito
 * invece di un NULL dereference silenzioso (successe con hidden_size sul
 * checkpoint reale, che annida tutto sotto text_config). */
static jval *json_get_req(jval *o, const char *key, const char *ctx) {
    jval *v = json_get(o, key);
    if (!v) { fprintf(stderr, "config.json: required field missing: '%s' (%s)\n", key, ctx); exit(1); }
    return v;
}

static void load_cfg(Cfg *c, const char *snap) {
    char path[2048]; snprintf(path, sizeof(path), "%s/config.json", snap);
    FILE *f = fopen(path, "rb"); if(!f){perror(path);exit(1);}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *buf = malloc(n+1); if(fread(buf,1,n,f)!=(size_t)n){} buf[n]=0; fclose(f);
    char *arena=NULL; jval *r = json_parse(buf, &arena);

    /* Il checkpoint reale (wrapper multimodale Qwen3_5MoeForConditionalGeneration)
     * annida hidden_size/num_hidden_layers/... sotto "text_config"; le config
     * tiny-oracle li hanno invece a livello radice. Risolvi una sola volta. */
    jval *cfg_root = r;
    jval *tc = json_get(r, "text_config");
    if (tc) cfg_root = tc;

    c->hidden      = (int)json_get_req(cfg_root,"hidden_size","cfg_root")->num;
    c->n_layers    = (int)json_get_req(cfg_root,"num_hidden_layers","cfg_root")->num;
    c->n_heads     = (int)json_get_req(cfg_root,"num_attention_heads","cfg_root")->num;
    c->n_kv_heads  = (int)json_get_req(cfg_root,"num_key_value_heads","cfg_root")->num;
    c->vocab       = (int)json_get_req(cfg_root,"vocab_size","cfg_root")->num;
    c->head_dim    = (int)json_get_req(cfg_root,"head_dim","cfg_root")->num;
    c->eps         = (float)json_get_req(cfg_root,"rms_norm_eps","cfg_root")->num;
    jval *mtp_layers = json_get(cfg_root, "mtp_num_hidden_layers");
    c->mtp_layers = mtp_layers ? (int)mtp_layers->num : 0;
    if (c->mtp_layers < 0 || c->mtp_layers > 16) {
        fprintf(stderr, "config.json: invalid mtp_num_hidden_layers: %d\n", c->mtp_layers);
        exit(1);
    }

    // Gated DeltaNet
    c->linear_num_value_heads = (int)json_get_req(cfg_root,"linear_num_value_heads","cfg_root")->num;
    c->linear_num_key_heads   = (int)json_get_req(cfg_root,"linear_num_key_heads","cfg_root")->num;
    c->linear_value_head_dim  = (int)json_get_req(cfg_root,"linear_value_head_dim","cfg_root")->num;
    c->linear_key_head_dim    = (int)json_get_req(cfg_root,"linear_key_head_dim","cfg_root")->num;
    c->linear_conv_kernel_dim = (int)json_get_req(cfg_root,"linear_conv_kernel_dim","cfg_root")->num;

    // MoE
    jval *me = json_get(cfg_root,"num_experts");
    if (me) {
        c->has_moe = 1;
        c->num_experts = (int)me->num;
        c->num_experts_per_tok = (int)json_get_req(cfg_root,"num_experts_per_tok","cfg_root")->num;
        c->moe_intermediate_size = (int)json_get_req(cfg_root,"moe_intermediate_size","cfg_root")->num;
        c->shared_expert_intermediate_size = (int)json_get_req(cfg_root,"shared_expert_intermediate_size","cfg_root")->num;
    } else {
        c->has_moe = 0;
        c->num_experts = 0;
        c->num_experts_per_tok = 0;
        c->shared_expert_intermediate_size = 0;
        // Dense MLP size
        c->moe_intermediate_size = (int)json_get_req(cfg_root,"intermediate_size","cfg_root")->num;
    }

    // Parse layer types
    jval *lt = json_get_req(cfg_root, "layer_types", "cfg_root");
    c->layer_type = malloc(c->n_layers * sizeof(int));
    for (int i = 0; i < c->n_layers; i++) {
        const char *ty = lt->kids[i]->str;
        if (strcmp(ty, "linear_attention") == 0) {
            c->layer_type[i] = 0;
        } else {
            c->layer_type[i] = 1;
        }
    }

    // Parse RoPE parameters
    c->rope_theta = 10000000.f;
    c->partial_rotary_factor = 0.25f;

    jval *rp = json_get(cfg_root, "rope_parameters");
    if (rp) {
        jval *theta_val = json_get(rp, "rope_theta");
        if (theta_val) c->rope_theta = (float)theta_val->num;
        jval *prf_val = json_get(rp, "partial_rotary_factor");
        if (prf_val) c->partial_rotary_factor = (float)prf_val->num;
    } else {
        jval *theta_val = json_get(cfg_root, "rope_theta");
        if (theta_val) c->rope_theta = (float)theta_val->num;
        jval *prf_val = json_get(cfg_root, "partial_rotary_factor");
        if (prf_val) c->partial_rotary_factor = (float)prf_val->num;
    }

    moe_policy_validate_model(c);
    json_tree_free(r); free(buf); free(arena);
}

static float *load_float_t(Model *m, const char *name) {
    if (st_has(&m->S, name)) {
        float *p = falloc(st_numel(&m->S, name));
        st_read_f32(&m->S, name, p, 0);
        return p;
    }
    char wrapped[512];
    if (strncmp(name, "model.", 6) == 0) {
        snprintf(wrapped, sizeof(wrapped), "model.language_model.%s", name + 6);
    } else {
        snprintf(wrapped, sizeof(wrapped), "model.language_model.%s", name);
    }
    if (st_has(&m->S, wrapped)) {
        float *p = falloc(st_numel(&m->S, wrapped));
        st_read_f32(&m->S, wrapped, p, 0);
        return p;
    }
    fprintf(stderr, "missing float tensor: %s\n", name);
    exit(1);
}

/* Dequantizza UNA riga (un token id) invece dell'intera tabella embed:
 * l'embedding e' 248320xD ma un forward tocca solo S righe per step().
 * Dequantizzare l'intera tabella ad ogni token (com'era prima) costava
 * ~2 GB di alloc+scrittura per token sul checkpoint reale. */
static void embed_gather_row(float *out, const QT *w, int row) {
    int I = w->I;
    if (w->fmt == 0) {
        memcpy(out, w->qf + (int64_t)row * I, I * sizeof(float));
    } else if (w->fmt == 1) {
        float sc = w->s[row];
        const int8_t *r8 = w->q8 + (int64_t)row * I;
        for (int i = 0; i < I; i++) out[i] = (float)r8[i] * sc;
    } else {
        int rb = (I + 1) / 2;
        const uint8_t *r4 = w->q4 + (int64_t)row * rb;
        for (int i = 0; i < I; i++) {
            uint8_t byte = r4[i / 2];
            int val = (i % 2 == 0) ? ((int)(byte & 0x0F) - 8) : ((int)(byte >> 4) - 8);
            float sc = w->fmt == 4
                     ? w->s[(int64_t)row * ((I + w->qgroup - 1) / w->qgroup) + i / w->qgroup]
                     : w->s[row];
            out[i] = (float)val * sc;
        }
    }
}

static QT qt_load(Model *m, const char *name, int O, int I) {
    QT t = {0};
    char nm[512];
    strcpy(nm, name);
    
    st_tensor *tw = st_find(&m->S, name);
    if (!tw) {
        char wrapped[512];
        if (strncmp(name, "model.", 6) == 0) {
            snprintf(wrapped, sizeof(wrapped), "model.language_model.%s", name + 6);
        } else {
            snprintf(wrapped, sizeof(wrapped), "model.language_model.%s", name);
        }
        tw = st_find(&m->S, wrapped);
        if (tw) {
            strcpy(nm, wrapped);
        }
    }
    if (!tw) {
        fprintf(stderr, "missing quantized tensor: %s\n", name);
        exit(1);
    }
    
    t.O = O;
    t.I = I;
    t.qgroup = 0;
    
    char qs_name[512];
    snprintf(qs_name, sizeof(qs_name), "%s.qs", nm);
    st_tensor *ts = st_find(&m->S, qs_name);
    
    if (ts) {
        int64_t nb = tw->nbytes;
        t.fmt = (nb == (int64_t)O * I) ? 1 : 2; // fmt 1 = int8, 2 = int4
        t.qf = NULL;
        t.q8 = malloc(nb);
        st_read_raw(&m->S, nm, t.q8, 0);
        t.q4 = (uint8_t*)t.q8;
        t.s = falloc(O);
        st_read_f32(&m->S, qs_name, t.s, 0);
    } else {
        t.fmt = 0;
        t.qf = falloc((int64_t)O * I);
        st_read_f32(&m->S, nm, t.qf, 0);
        t.q8 = NULL;
        t.q4 = NULL;
        t.s = NULL;
    }
    return t;
}

static void load_manifest(Model *m, const char *snap) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/manifest.json", snap);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: manifest.json missing in %s\n", snap);
        exit(1);
    }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) {}
    buf[n] = 0; fclose(f);
    
    char *arena = NULL;
    jval *root = json_parse(buf, &arena);
    jval *experts = json_get(root, "experts");
    if (!experts) {
        fprintf(stderr, "manifest.json non contiene 'experts'!\n");
        exit(1);
    }

    m->expert_group_size = 0;
    m->expert_down_bits = 4;
    jval *quant = json_get(root, "expert_quantization");
    if (quant) {
        jval *format = json_get(quant, "format");
        jval *group = json_get(quant, "group_size");
        jval *down_bits = json_get(quant, "down_bits");
        int all_q4 = format && format->t == J_STR &&
                     !strcmp(format->str, "groupwise-symmetric-q4-v1");
        int mixed = format && format->t == J_STR &&
                    !strcmp(format->str, "groupwise-q4-gate-up-row-q8-down-v1");
        int expected_down = mixed ? 8 : 4;
        if (quant->t != J_OBJ || (!all_q4 && !mixed) ||
            !group || group->t != J_NUM || !isfinite(group->num) ||
            floor(group->num) != group->num || group->num < 2 ||
            group->num > INT_MAX || ((int)group->num & 1) ||
            m->c.hidden % (int)group->num ||
            m->c.moe_intermediate_size % (int)group->num ||
            (down_bits && (down_bits->t != J_NUM || !isfinite(down_bits->num) ||
                           floor(down_bits->num) != down_bits->num ||
                           (int)down_bits->num != expected_down))) {
            fprintf(stderr, "manifest.json: invalid expert_quantization\n");
            exit(1);
        }
        m->expert_group_size = (int)group->num;
        m->expert_down_bits = expected_down;
    }
    
    int E = m->c.num_experts;
    int NL = m->c.n_layers + m->c.mtp_layers;
    m->expert_offsets = calloc(NL * E, sizeof(int64_t));
    m->expert_sizes = calloc(NL * E, sizeof(int64_t));
    m->expert_sha256 = calloc((size_t)NL * E, 32);
    if (!m->expert_offsets || !m->expert_sizes || !m->expert_sha256) {
        fprintf(stderr, "OOM manifest expert index\n"); exit(1);
    }
    
    for (int l = 0; l < NL; l++) {
        for (int e = 0; e < E; e++) {
            char key[256];
            snprintf(key, sizeof(key), "model.layers.%d.mlp.experts.%d", l, e);
            jval *item = json_get(experts, key);
            if (item) {
                jval *off = json_get(item, "offset");
                jval *sz = json_get(item, "size");
                jval *sha = json_get(item, "sha256");
                if (!off || off->t != J_NUM || !sz || sz->t != J_NUM ||
                    !sha || sha->t != J_STR || off->num < 0 || sz->num <= 0 ||
                    floor(off->num) != off->num || floor(sz->num) != sz->num ||
                    off->num > (double)INT64_MAX || sz->num > (double)INT64_MAX ||
                    !refine_hex_digest(sha->str, m->expert_sha256 + ((size_t)l * E + e) * 32)) {
                    fprintf(stderr, "manifest.json: invalid expert: %s\n", key);
                    exit(1);
                }
                m->expert_offsets[l * E + e] = (int64_t)off->num;
                m->expert_sizes[l * E + e] = (int64_t)sz->num;
            }
        }
    }
    json_tree_free(root); free(arena); free(buf);
}

typedef struct { uint64_t begin, end; } RefineRange;

static jval *refine_req(jval *parent, const char *key, jtype type,
                        const char *context) {
    jval *value = json_get(parent, key);
    if (!value || value->t != type)
        refine_die("manifest field %s.%s is missing or has the wrong type", context, key);
    return value;
}

static uint64_t refine_u64(jval *parent, const char *key, const char *context) {
    jval *value = refine_req(parent, key, J_NUM, context);
    if (!isfinite(value->num) || value->num < 0 || floor(value->num) != value->num ||
        value->num > 9007199254740991.0)
        refine_die("manifest field %s.%s is not an exact non-negative integer", context, key);
    return (uint64_t)value->num;
}

static int refine_int(jval *parent, const char *key, const char *context) {
    uint64_t value = refine_u64(parent, key, context);
    if (value > INT_MAX) refine_die("manifest field %s.%s exceeds INT_MAX", context, key);
    return (int)value;
}

static const char *refine_str(jval *parent, const char *key, const char *context) {
    return refine_req(parent, key, J_STR, context)->str;
}

static void refine_digest_field(jval *parent, const char *key, const char *context,
                                uint8_t digest[32]) {
    const char *text = refine_str(parent, key, context);
    if (!refine_hex_digest(text, digest))
        refine_die("manifest field %s.%s is not a lowercase SHA-256 digest", context, key);
}

static uint64_t refine_add(uint64_t a, uint64_t b, const char *context) {
    if (a > UINT64_MAX - b) refine_die("integer overflow while validating %s", context);
    return a + b;
}

static uint64_t refine_mul(uint64_t a, uint64_t b, const char *context) {
    if (a && b > UINT64_MAX / a) refine_die("integer overflow while validating %s", context);
    return a * b;
}

static uint64_t refine_align(uint64_t value) {
    return refine_add(value, 16383u, "expert alignment") & ~UINT64_C(16383);
}

static void refine_path(char output[2048], const char *dir, const char *file) {
    if (!dir || !*dir || !file || !*file || strchr(file, '/') || !strcmp(file, ".") ||
        !strcmp(file, "..") || snprintf(output, 2048, "%s/%s", dir, file) >= 2048)
        refine_die("unsafe or overlong shelf path");
}

static void refine_hash_file(const char *path, uint64_t expected_size,
                             const uint8_t expected[32], const char *description) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) refine_die("cannot open %s '%s': %s", description, path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != expected_size) {
        close(fd); refine_die("%s size/type does not match the manifest", description);
    }
    uint8_t buffer[65536], actual[32];
    RefineSha256 sha; refine_sha256_init(&sha);
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got > 0) { refine_sha256_update(&sha, buffer, (size_t)got); continue; }
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { int saved=errno; close(fd); refine_die("read %s failed: %s", description, strerror(saved)); }
        break;
    }
    close(fd); refine_sha256_final(&sha, actual);
    if (memcmp(actual, expected, 32)) refine_die("%s SHA-256 does not match the refinable manifest", description);
}

static int refine_open_shelf(const char *dir, jval *shelves, const char *key,
                             const char *expected_file, uint64_t *size_out) {
    jval *record = refine_req(shelves, key, J_OBJ, "shelves");
    const char *file = refine_str(record, "file", key);
    if (strcmp(file, expected_file))
        refine_die("shelf %s names unsupported file '%s'", key, file);
    uint64_t declared = refine_u64(record, "size", key);
    uint8_t ignored_digest[32];
    refine_digest_field(record, "sha256", key, ignored_digest);
    char path[2048]; refine_path(path, dir, file);
    int fd = open(path, O_RDONLY);
    if (fd < 0) refine_die("cannot open shelf '%s': %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != declared) {
        close(fd); refine_die("shelf %s size/type does not match its manifest", key);
    }
    *size_out = declared;
    return fd;
}

static void refine_parse_region(jval *record, const char *name, const char *context,
                                uint64_t *offset, uint64_t *bytes, uint8_t digest[32]) {
    jval *region = refine_req(record, name, J_OBJ, context);
    *offset = refine_u64(region, "offset", name);
    *bytes = refine_u64(region, "size", name);
    refine_digest_field(region, "sha256", name, digest);
}

static int refine_range_compare(const void *left_, const void *right_) {
    const RefineRange *left = left_, *right = right_;
    if (left->begin < right->begin) return -1;
    if (left->begin > right->begin) return 1;
    if (left->end < right->end) return -1;
    return left->end > right->end;
}

static void refine_validate_ranges(RefineRange *ranges, size_t count,
                                   uint64_t shelf_size, const char *name) {
    qsort(ranges, count, sizeof(*ranges), refine_range_compare);
    uint64_t cursor = 0;
    for (size_t i = 0; i < count; ++i) {
        if (ranges[i].begin < cursor || ranges[i].end < ranges[i].begin ||
            ranges[i].end > shelf_size)
            refine_die("%s shelf contains overlapping or out-of-bounds regions", name);
        cursor = ranges[i].end;
    }
    if ((count == 0 && shelf_size != 0) || (count && cursor != shelf_size))
        refine_die("%s shelf has an unreferenced tail", name);
}

static void refine_add_range(RefineRange *ranges, size_t *count,
                             uint64_t offset, uint64_t bytes, uint64_t shelf_size,
                             const char *name) {
    uint64_t end = refine_add(offset, bytes, name);
    if (bytes == 0 || end > shelf_size) refine_die("%s region is empty or out of bounds", name);
    ranges[(*count)++] = (RefineRange){offset, end};
}

static void refine_check_source_identity(jval *root, const char *snap,
                                         Model *m) {
    jval *source = refine_req(root, "source", J_OBJ, "root");
    jval *config = refine_req(source, "config", J_OBJ, "source");
    jval *manifest = refine_req(source, "manifest", J_OBJ, "source");
    jval *experts = refine_req(source, "experts", J_OBJ, "source");
    uint8_t config_sha[32], manifest_sha[32], tree_sha[32];
    if (strcmp(refine_str(config,"file","source.config"), "config.json") ||
        strcmp(refine_str(manifest,"file","source.manifest"), "manifest.json") ||
        strcmp(refine_str(experts,"file","source.experts"), "experts.bin"))
        refine_die("source file identities are unsupported");
    uint64_t config_size=refine_u64(config,"size","source.config");
    uint64_t manifest_size=refine_u64(manifest,"size","source.manifest");
    uint64_t experts_size=refine_u64(experts,"size","source.experts");
    refine_digest_field(config,"sha256","source.config",config_sha);
    refine_digest_field(manifest,"sha256","source.manifest",manifest_sha);
    refine_digest_field(experts,"expert_region_tree_sha256","source.experts",tree_sha);
    char path[2048];
    refine_path(path,snap,"config.json"); refine_hash_file(path,config_size,config_sha,"source config");
    refine_path(path,snap,"manifest.json"); refine_hash_file(path,manifest_size,manifest_sha,"source manifest");
    struct stat st;
    if (fstat(m->fd_exp,&st) || st.st_size < 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != experts_size)
        refine_die("source experts.bin size/type does not match the refinable manifest");
}

static void refine_projection_geometry(const Cfg *c, int projection,
                                       uint64_t *rows, uint64_t *cols) {
    if (projection == REFINE_DOWN) {
        *rows=(uint64_t)c->hidden; *cols=(uint64_t)c->moe_intermediate_size;
    } else {
        *rows=(uint64_t)c->moe_intermediate_size; *cols=(uint64_t)c->hidden;
    }
}

static uint64_t refine_expected_source_bytes(const Cfg *c, int bits) {
    uint64_t raw=0;
    for (int p=0; p<REFINE_PROJECTIONS; ++p) {
        uint64_t rows,cols; refine_projection_geometry(c,p,&rows,&cols);
        uint64_t row_bytes = bits == 4 ? (cols+1u)/2u : cols;
        raw=refine_add(raw,refine_mul(rows,row_bytes,"source projection"),"source expert");
        raw=refine_add(raw,refine_mul(rows,4,"source scales"),"source expert");
    }
    return refine_align(raw);
}

static void refine_parse_q4_expert(Model *m, RefineExpert *entry, jval *record,
                                   int layer, int expert, RefineRange *base_ranges,
                                   size_t *base_count, RefineRange *residual_ranges,
                                   size_t *residual_count, RefineRange *scale_ranges,
                                   size_t *scale_count) {
    jval *projections = refine_req(record,"projections",J_ARR,"expert");
    if (projections->len != REFINE_PROJECTIONS)
        refine_die("layer %d expert %d has %d projections, expected 3",layer,expert,projections->len);
    uint64_t cursor=entry->source_offset;
    static const char *names[3]={"gate","up","down"};
    for (int p=0; p<REFINE_PROJECTIONS; ++p) {
        char context[96]; snprintf(context,sizeof(context),"layer %d expert %d %s",layer,expert,names[p]);
        jval *projection=projections->kids[p];
        if (!projection || projection->t != J_OBJ ||
            strcmp(refine_str(projection,"name",context),names[p]))
            refine_die("%s has an invalid projection name/order",context);
        RefineProjection *dst=&entry->projection[p];
        refine_projection_geometry(&m->c,p,&dst->rows,&dst->cols);
        if (refine_u64(projection,"rows",context)!=dst->rows ||
            refine_u64(projection,"cols",context)!=dst->cols)
            refine_die("%s geometry does not match config.json",context);
        dst->q4_row_bytes=(dst->cols+1u)/2u;
        dst->source_q4_bytes=refine_mul(dst->rows,dst->q4_row_bytes,context);
        dst->source_scale_bytes=refine_mul(dst->rows,4,context);
        if (refine_u64(projection,"q4_row_bytes",context)!=dst->q4_row_bytes)
            refine_die("%s q4 row stride does not match its geometry",context);
        uint64_t ignored_size; uint8_t source_scale_sha[32];
        refine_parse_region(projection,"source_q4",context,&dst->source_q4_offset,
                            &ignored_size,dst->source_q4_sha256);
        if (ignored_size!=dst->source_q4_bytes || dst->source_q4_offset!=cursor)
            refine_die("%s source q4 boundary does not match experts.bin",context);
        cursor=refine_add(cursor,dst->source_q4_bytes,context);
        refine_parse_region(projection,"source_scale",context,&dst->source_scale_offset,
                            &ignored_size,source_scale_sha);
        if (ignored_size!=dst->source_scale_bytes || dst->source_scale_offset!=cursor)
            refine_die("%s source scale boundary does not match experts.bin",context);
        cursor=refine_add(cursor,dst->source_scale_bytes,context);
        refine_parse_region(projection,"base",context,&dst->base_offset,
                            &dst->base_bytes,dst->base_sha256);
        refine_parse_region(projection,"residual",context,&dst->residual_offset,
                            &dst->residual_bytes,dst->residual_sha256);
        refine_parse_region(projection,"scale",context,&dst->scale_offset,
                            &dst->scale_bytes,dst->scale_sha256);
        uint64_t plane=(dst->source_q4_bytes+1u)/2u;
        if (dst->base_bytes!=plane || dst->residual_bytes!=plane ||
            dst->scale_bytes!=dst->source_scale_bytes ||
            (dst->base_offset&16383u) || (dst->residual_offset&16383u) ||
            memcmp(source_scale_sha,dst->scale_sha256,32))
            refine_die("%s plane/scale geometry or binding is invalid",context);
        jval *rule=refine_req(projection,"reconstruction",J_OBJ,context);
        if (strcmp(refine_str(rule,"rule_id","reconstruction"),"calibrated-high2-plus-low2-residual-c1") ||
            refine_int(rule,"c","reconstruction")!=1 ||
            strcmp(refine_str(rule,"base_integer","reconstruction"),"4*high2-7") ||
            strcmp(refine_str(rule,"residual_integer","reconstruction"),"low2-1") ||
            strcmp(refine_str(rule,"full_integer","reconstruction"),"base_integer+residual_integer"))
            refine_die("%s uses an unsupported reconstruction rule",context);
        refine_add_range(base_ranges,base_count,dst->base_offset,dst->base_bytes,m->refine.base_size,"base");
        refine_add_range(residual_ranges,residual_count,dst->residual_offset,dst->residual_bytes,m->refine.residual_size,"residual");
        refine_add_range(scale_ranges,scale_count,dst->scale_offset,dst->scale_bytes,m->refine.scale_size,"scale");
    }
    if (cursor > refine_add(entry->source_offset,entry->source_bytes,"source expert"))
        refine_die("layer %d expert %d projections overrun its source blob",layer,expert);
}

static void refine_init(Model *m, const char *snap, const char *dir, int mode,
                        int verify_payloads, int full_ranks,
                        uint8_t base_projection_mask,
                        const char *base_layers_text) {
    RefineStore *store=&m->refine;
    store->fd_base=store->fd_residual=store->fd_scales=store->fd_int8=-1;
    if (mode==REFINE_OFF) return;
    if (!dir || !*dir) refine_die("mode '%s' requires REFINE_DIR or --refine-dir",refine_mode_name(mode));
    if (!m->c.has_moe) refine_die("refinable shelves require a MoE checkpoint");
    store->mode=mode; store->layers=m->c.n_layers+m->c.mtp_layers;
    store->verify_payloads=verify_payloads;
    store->experts=m->c.num_experts;
    store->full_ranks=full_ranks;
    store->base_projection_mask=base_projection_mask;
    if (mode==REFINE_MIXED &&
        (full_ranks<0 || full_ranks>m->c.num_experts_per_tok ||
         !base_projection_mask || (base_projection_mask&~7u)))
        refine_die("mixed policy is outside the checkpoint's rank/projection geometry");
    if (mode==REFINE_MIXED) {
        store->base_layers=calloc((size_t)m->c.n_layers,1);
        if (!store->base_layers)
            refine_die("OOM allocating mixed layer policy");
        if (!refine_layer_mask_parse(base_layers_text?base_layers_text:"all",
                                     m->c.n_layers,store->base_layers,
                                     &store->base_layer_count))
            refine_die("invalid mixed base-layer list '%s'",
                       base_layers_text?base_layers_text:"all");
    }
    if (store->layers<=0 || store->experts<=0 ||
        (uint64_t)store->layers*(uint64_t)store->experts > SIZE_MAX/sizeof(RefineExpert))
        refine_die("model geometry cannot be indexed safely");

    char manifest_path[2048]; refine_path(manifest_path,dir,"manifest.json");
    FILE *file=fopen(manifest_path,"rb");
    if (!file) refine_die("cannot open manifest '%s': %s",manifest_path,strerror(errno));
    if (fseek(file,0,SEEK_END) || ftell(file)<0) refine_die("cannot size refinable manifest");
    long length=ftell(file);
    if (fseek(file,0,SEEK_SET) || (uint64_t)length>SIZE_MAX-1) refine_die("refinable manifest is too large");
    char *buffer=malloc((size_t)length+1);
    if (!buffer) refine_die("OOM reading refinable manifest");
    if (fread(buffer,1,(size_t)length,file)!=(size_t)length) refine_die("short read of refinable manifest");
    buffer[length]=0; fclose(file);
    char *arena=NULL; jval *root=json_parse(buffer,&arena);
    if (!root || root->t!=J_OBJ) refine_die("manifest root is not an object");
    if (strcmp(refine_str(root,"schema","root"),"colibri.refinable-q4-shelves") ||
        refine_int(root,"version","root")!=1)
        refine_die("unsupported schema or version");
    jval *format=refine_req(root,"format",J_OBJ,"root");
    if (refine_u64(format,"shelf_alignment_bytes","format")!=16384 ||
        refine_int(format,"reconstruction_c","format")!=1 ||
        strcmp(refine_str(format,"base","format"),"high2; integer=4*high2-7") ||
        strcmp(refine_str(format,"residual","format"),"low2; integer=low2-1") ||
        strcmp(refine_str(format,"hash","format"),"SHA-256"))
        refine_die("unsupported refinable q4 format");
    jval *geometry=refine_req(root,"geometry",J_OBJ,"root");
    if (refine_int(geometry,"hidden_size","geometry")!=m->c.hidden ||
        refine_int(geometry,"moe_intermediate_size","geometry")!=m->c.moe_intermediate_size ||
        refine_int(geometry,"num_experts","geometry")!=m->c.num_experts ||
        refine_int(geometry,"num_hidden_layers","geometry")!=m->c.n_layers ||
        refine_int(geometry,"mtp_num_hidden_layers","geometry")!=m->c.mtp_layers)
        refine_die("manifest geometry does not match config.json");
    refine_check_source_identity(root,snap,m);

    jval *shelves=refine_req(root,"shelves",J_OBJ,"root");
    store->fd_base=refine_open_shelf(dir,shelves,"base","experts.base.rq2",&store->base_size);
    store->fd_residual=refine_open_shelf(dir,shelves,"residual","experts.residual.rq2",&store->residual_size);
    store->fd_scales=refine_open_shelf(dir,shelves,"scales","experts.scales.f32",&store->scale_size);
    store->fd_int8=refine_open_shelf(dir,shelves,"passthrough_int8","experts.int8.bin",&store->int8_size);

    size_t entries=(size_t)store->layers*(size_t)store->experts;
    store->index=calloc(entries,sizeof(*store->index));
    size_t projection_capacity=(size_t)m->c.n_layers*(size_t)store->experts*3u;
    RefineRange *base_ranges=malloc(projection_capacity*sizeof(*base_ranges));
    RefineRange *residual_ranges=malloc(projection_capacity*sizeof(*residual_ranges));
    RefineRange *scale_ranges=malloc(projection_capacity*sizeof(*scale_ranges));
    RefineRange *int8_ranges=malloc(entries*sizeof(*int8_ranges));
    if (!store->index || (!base_ranges&&projection_capacity) ||
        (!residual_ranges&&projection_capacity) || (!scale_ranges&&projection_capacity) ||
        (!int8_ranges&&entries)) refine_die("OOM building refinable shelf index");
    size_t nb=0,nr=0,ns=0,ni=0;
    jval *experts=refine_req(root,"experts",J_OBJ,"root");
    if ((size_t)experts->len!=entries)
        refine_die("manifest has %d expert records, expected %zu",experts->len,entries);
    for (int item=0; item<experts->len; ++item) {
        int layer=-1,expert=-1,consumed=0; char canonical[128];
        const char *key=experts->keys[item]; jval *record=experts->kids[item];
        if (!record || record->t!=J_OBJ ||
            sscanf(key,"model.layers.%d.mlp.experts.%d%n",&layer,&expert,&consumed)!=2 ||
            key[consumed] || layer<0 || layer>=store->layers || expert<0 || expert>=store->experts) 
            refine_die("malformed or out-of-range expert key '%s'",key);
        snprintf(canonical,sizeof(canonical),"model.layers.%d.mlp.experts.%d",layer,expert);
        if (strcmp(key,canonical)) refine_die("non-canonical expert key '%s'",key);
        RefineExpert *dst=&store->index[(size_t)layer*store->experts+expert];
        if (dst->storage) refine_die("duplicate expert key '%s'",key);
        if (refine_int(record,"layer",key)!=layer || refine_int(record,"expert",key)!=expert)
            refine_die("record identity does not match key '%s'",key);
        jval *source_blob=refine_req(record,"source_blob",J_OBJ,key);
        refine_parse_region(record,"source_blob",key,&dst->source_offset,
                            &dst->source_bytes,dst->source_sha256);
        (void)source_blob;
        size_t source_index=(size_t)layer*store->experts+expert;
        if (m->expert_sizes[source_index]<=0 || dst->source_offset!=(uint64_t)m->expert_offsets[source_index] ||
            dst->source_bytes!=(uint64_t)m->expert_sizes[source_index] ||
            memcmp(dst->source_sha256,m->expert_sha256+source_index*32,32) ||
            (dst->source_offset&16383u))
            refine_die("source binding mismatch for '%s'",key);
        int bits=layer<m->c.n_layers?4:8;
        if (dst->source_bytes!=refine_expected_source_bytes(&m->c,bits))
            refine_die("source expert geometry mismatch for '%s'",key);
        const char *storage=refine_str(record,"storage",key);
        if (bits==4) {
            if (strcmp(storage,"refinable-q4")) refine_die("q4 expert '%s' is not refinable-q4",key);
            dst->storage=REFINE_STORAGE_Q4;
            refine_parse_q4_expert(m,dst,record,layer,expert,base_ranges,&nb,
                                   residual_ranges,&nr,scale_ranges,&ns);
        } else {
            if (strcmp(storage,"passthrough-int8")) refine_die("MTP expert '%s' is not passthrough-int8",key);
            dst->storage=REFINE_STORAGE_INT8;
            refine_parse_region(record,"output_blob",key,&dst->int8_offset,
                                &dst->int8_bytes,dst->int8_sha256);
            if (dst->int8_bytes!=dst->source_bytes || (dst->int8_offset&16383u) ||
                memcmp(dst->int8_sha256,dst->source_sha256,32))
                refine_die("MTP passthrough binding mismatch for '%s'",key);
            refine_add_range(int8_ranges,&ni,dst->int8_offset,dst->int8_bytes,store->int8_size,"int8");
        }
    }
    for (size_t i=0;i<entries;++i) if (!store->index[i].storage)
        refine_die("refinable manifest is missing an expected expert");
    refine_validate_ranges(base_ranges,nb,store->base_size,"base");
    refine_validate_ranges(residual_ranges,nr,store->residual_size,"residual");
    refine_validate_ranges(scale_ranges,ns,store->scale_size,"scale");
    refine_validate_ranges(int8_ranges,ni,store->int8_size,"int8");
    free(base_ranges); free(residual_ranges); free(scale_ranges); free(int8_ranges);
    json_tree_free(root); free(arena); free(buffer);
    fprintf(stderr,"[refine] EXPERIMENTAL opt-in mode=%s verify=%s dir=%s entries=%zu",
            refine_mode_name(mode),verify_payloads?"per-load":"preverified",dir,entries);
    if (mode==REFINE_MIXED)
        fprintf(stderr," full_ranks=%d base_projection_mask=0x%x base_layers=%d/%d",
                full_ranks,(unsigned)base_projection_mask,
                store->base_layer_count,m->c.n_layers);
    fputc('\n',stderr);
}

static uint8_t refine_mask_for_rank(const Model *m, int layer, int rank) {
    const RefineStore *store=&m->refine;
    if (store->mode==REFINE_BASE) return 0;
    if (store->mode!=REFINE_MIXED || layer>=m->c.n_layers ||
        !store->base_layers[layer] ||
        rank<store->full_ranks) return 7;
    return (uint8_t)(7u&~store->base_projection_mask);
}

static void refine_pread_exact(int fd, void *buffer, uint64_t bytes,
                               uint64_t offset, const char *description) {
    uint8_t *cursor=buffer;
    uint64_t remaining=bytes;
    while (remaining) {
        size_t request=remaining>SSIZE_MAX?(size_t)SSIZE_MAX:(size_t)remaining;
        ssize_t got=pread(fd,cursor,request,(off_t)offset);
        if (got>0) { cursor+=(size_t)got; offset+=(uint64_t)got; remaining-=(uint64_t)got; continue; }
        if (got<0 && errno==EINTR) continue;
        if (got==0) refine_die("unexpected EOF reading %s",description);
        refine_die("pread %s failed: %s",description,strerror(errno));
    }
}

static void refine_verify_payload(const void *payload, size_t bytes,
                                  const uint8_t expected[32], const char *description,
                                  int layer, int expert) {
    uint8_t actual[32]; refine_sha256(payload,bytes,actual);
    if (memcmp(actual,expected,32))
        refine_die("%s SHA-256 mismatch at layer %d expert %d",description,layer,expert);
}

static void refine_join_projection(uint8_t *q4, size_t q4_bytes,
                                   const uint8_t *base, const uint8_t *residual) {
    for (size_t i=0;i<q4_bytes;++i) {
        size_t plane_index=i>>1;
        unsigned shift=(unsigned)(i&1u)*4u;
        uint8_t high=(uint8_t)((base[plane_index]>>shift)&15u);
        /* c=1 base-only means low2=1 for every q4 code: the packed pair is 0b0101. */
        uint8_t low=residual?(uint8_t)((residual[plane_index]>>shift)&15u):5u;
        uint8_t c0=(uint8_t)(((high&3u)<<2)|(low&3u));
        uint8_t c1=(uint8_t)(((high>>2)<<2)|(low>>2));
        q4[i]=(uint8_t)(c0|(c1<<4));
    }
}

static void refine_load_expert(Model *m, int layer, int eid, ESlot *s, int64_t sz,
                               uint8_t refine_mask) {
    RefineStore *store=&m->refine;
    if (layer<0 || layer>=store->layers || eid<0 || eid>=store->experts)
        refine_die("expert identity is outside the refinable index");
    RefineExpert *entry=&store->index[(size_t)layer*store->experts+eid];
    if (entry->source_bytes!=(uint64_t)sz) refine_die("source slot size changed after initialization");
    memset(s->slab,0,(size_t)sz);
    if (entry->storage==REFINE_STORAGE_INT8) {
        refine_pread_exact(store->fd_int8,s->slab,entry->int8_bytes,entry->int8_offset,"int8 passthrough");
        atomic_fetch_add_explicit(&store->int8_read,entry->int8_bytes,memory_order_relaxed);
        if (store->verify_payloads)
            refine_verify_payload(s->slab,(size_t)entry->int8_bytes,entry->int8_sha256,
                                  "int8 passthrough",layer,eid);
    } else if (entry->storage==REFINE_STORAGE_Q4) {
        if (refine_mask&~7u) refine_die("expert precision mask is invalid");
        size_t maximum=0;
        for (int p=0;p<REFINE_PROJECTIONS;++p)
            if (entry->projection[p].base_bytes>maximum)
                maximum=(size_t)entry->projection[p].base_bytes;
        if (maximum && refine_mask && maximum>SIZE_MAX/2)
            refine_die("projection scratch size overflows size_t");
        size_t scratch_bytes=maximum*(refine_mask?2u:1u);
        uint8_t *scratch=malloc(scratch_bytes?scratch_bytes:1);
        if (!scratch) refine_die("OOM allocating bounded projection scratch");
        uint8_t *base=scratch;
        for (int p=0;p<REFINE_PROJECTIONS;++p) {
            RefineProjection *projection=&entry->projection[p];
            uint8_t *residual=(refine_mask&(1u<<p))?scratch+maximum:NULL;
            uint64_t q4_destination=projection->source_q4_offset-entry->source_offset;
            uint64_t scale_destination=projection->source_scale_offset-entry->source_offset;
            refine_pread_exact(store->fd_base,base,projection->base_bytes,
                               projection->base_offset,"base plane");
            atomic_fetch_add_explicit(&store->base_read,projection->base_bytes,memory_order_relaxed);
            if (store->verify_payloads)
                refine_verify_payload(base,(size_t)projection->base_bytes,
                                      projection->base_sha256,"base plane",layer,eid);
            if (residual) {
                refine_pread_exact(store->fd_residual,residual,projection->residual_bytes,
                                   projection->residual_offset,"residual plane");
                atomic_fetch_add_explicit(&store->residual_read,projection->residual_bytes,memory_order_relaxed);
                if (store->verify_payloads)
                    refine_verify_payload(residual,(size_t)projection->residual_bytes,
                                          projection->residual_sha256,"residual plane",layer,eid);
            }
            refine_pread_exact(store->fd_scales,s->slab+scale_destination,
                               projection->scale_bytes,projection->scale_offset,"scale shelf");
            atomic_fetch_add_explicit(&store->scale_read,projection->scale_bytes,memory_order_relaxed);
            if (store->verify_payloads)
                refine_verify_payload(s->slab+scale_destination,(size_t)projection->scale_bytes,
                                      projection->scale_sha256,"scale shelf",layer,eid);
            refine_join_projection(s->slab+q4_destination,(size_t)projection->source_q4_bytes,
                                   base,residual);
            if (store->verify_payloads && residual)
                refine_verify_payload(s->slab+q4_destination,(size_t)projection->source_q4_bytes,
                                      projection->source_q4_sha256,"reconstructed q4",layer,eid);
        }
        free(scratch);
    } else {
        refine_die("expert entry has no supported storage mode");
    }
    atomic_fetch_add_explicit(&store->source_aligned_bytes,(uint64_t)sz,memory_order_relaxed);
    if (store->verify_payloads &&
        (refine_mask==7 || entry->storage==REFINE_STORAGE_INT8))
        refine_verify_payload(s->slab,(size_t)sz,entry->source_sha256,
                              "reconstructed source expert",layer,eid);
}

static void refine_report(Model *m) {
    RefineStore *store=&m->refine;
    if (store->mode==REFINE_OFF || store->reported) return;
    store->reported=1;
    uint64_t base=atomic_load_explicit(&store->base_read,memory_order_relaxed);
    uint64_t residual=atomic_load_explicit(&store->residual_read,memory_order_relaxed);
    uint64_t scales=atomic_load_explicit(&store->scale_read,memory_order_relaxed);
    uint64_t int8=atomic_load_explicit(&store->int8_read,memory_order_relaxed);
    uint64_t source=atomic_load_explicit(&store->source_aligned_bytes,memory_order_relaxed);
    uint64_t actual=base+residual+scales+int8;
    double saved=source ? 100.0*(1.0-(double)actual/(double)source) : 0.0;
    fprintf(stderr,
        "[refine] mode=%s base_read=%llu residual_read=%llu scale_read=%llu "
        "int8_read=%llu actual_read=%llu source_aligned_proxy=%llu saved=%.2f%%\n",
        refine_mode_name(store->mode),(unsigned long long)base,
        (unsigned long long)residual,(unsigned long long)scales,
        (unsigned long long)int8,(unsigned long long)actual,
        (unsigned long long)source,saved);
}

/* Costruisce le viste QT g/u/d dentro s->slab (blob gia' presente).  Estratto
 * da expert_load cosi' il percorso T2.5 (lettura sequenziale dell'intero
 * layer) puo' puntare viste dentro un buffer condiviso senza copiare. */
static void expert_views(Model *m, int layer, ESlot *s) {
    int D = m->c.hidden;
    int I = m->c.moe_intermediate_size;
    /* Il layer MTP (se presente) e' sempre l'indice subito dopo l'ultimo layer
     * reale, non un valore fisso: derivarlo da n_layers invece di hardcodare
     * "40" (vero solo per il checkpoint reale a 40 layer, sbagliato per
     * qualunque altra configurazione, inclusi gli oracoli tiny). */
    int bits = (layer == m->c.n_layers) ? 8 : 4;
    int down_bits = (layer == m->c.n_layers) ? 8 : m->expert_down_bits;
    int group = bits == 4 ? m->expert_group_size : 0;

    int64_t g_w_size = (bits == 8) ? (int64_t)I * D : ((int64_t)I * D + 1) / 2;
    int64_t g_s_size = group ? (int64_t)I * ((D + group - 1) / group) * 4
                             : (int64_t)I * 4;
    int64_t u_w_size = (bits == 8) ? (int64_t)I * D : ((int64_t)I * D + 1) / 2;
    int64_t u_s_size = group ? (int64_t)I * ((D + group - 1) / group) * 4
                             : (int64_t)I * 4;
    int64_t d_w_size = (down_bits == 8) ? (int64_t)D * I : ((int64_t)D * I + 1) / 2;

    s->g.fmt = (bits == 8) ? 1 : group ? 4 : 2;
    s->g.O = I; s->g.I = D; s->g.qgroup = group; s->g.qf = NULL;
    s->g.q8 = (bits == 8) ? (int8_t*)s->slab : NULL;
    s->g.q4 = (bits == 8) ? NULL : s->slab;
    s->g.s  = (float*)(s->slab + g_w_size);

    s->u.fmt = (bits == 8) ? 1 : group ? 4 : 2;
    s->u.O = I; s->u.I = D; s->u.qgroup = group; s->u.qf = NULL;
    s->u.q8 = (bits == 8) ? (int8_t*)(s->slab + g_w_size + g_s_size) : NULL;
    s->u.q4 = (bits == 8) ? NULL : (s->slab + g_w_size + g_s_size);
    s->u.s  = (float*)(s->slab + g_w_size + g_s_size + u_w_size);

    s->d.fmt = (down_bits == 8) ? 1 : group ? 4 : 2;
    s->d.O = D; s->d.I = I; s->d.qgroup = down_bits == 4 ? group : 0; s->d.qf = NULL;
    s->d.q8 = (down_bits == 8) ? (int8_t*)(s->slab + g_w_size + g_s_size + u_w_size + u_s_size) : NULL;
    s->d.q4 = (down_bits == 8) ? NULL : (s->slab + g_w_size + g_s_size + u_w_size + u_s_size);
    s->d.s  = (float*)(s->slab + g_w_size + g_s_size + u_w_size + u_s_size + d_w_size);
}

static void expert_load(Model *m, int layer, int eid, uint8_t refine_mask,
                        ESlot *s) {
    int E = m->c.num_experts;
    int64_t off = m->expert_offsets[layer * E + eid];
    int64_t sz = m->expert_sizes[layer * E + eid];
    
    if (sz == 0) {
        fprintf(stderr, "Error: expert %d at layer %d has an invalid size\n", eid, layer);
        exit(1);
    }
    
    if (!s->slab || sz > s->slab_cap) {
        free(s->slab);
        if (posix_memalign((void**)&s->slab, 4096, sz)) {
            fprintf(stderr, "OOM slot slab\n");
            exit(1);
        }
        s->slab_cap = sz;
    }
    
    if (m->refine.mode!=REFINE_OFF) {
        refine_load_expert(m,layer,eid,s,sz,refine_mask);
    } else {
        int fd = (g_direct && m->fd_exp_direct >= 0) ? m->fd_exp_direct : m->fd_exp;
        refine_pread_exact(fd,s->slab,(uint64_t)sz,(uint64_t)off,"source expert blob");
    }
    
    expert_views(m, layer, s);
    s->eid = eid;
    s->refine_mask=refine_mask;
}

static void expert_prefetch(Model *m, int layer, int eid, uint8_t refine_mask) {
    int E = m->c.num_experts;
    int64_t off = m->expert_offsets[layer * E + eid];
    int64_t sz = m->expert_sizes[layer * E + eid];
    if (sz > 0 && m->refine.mode!=REFINE_OFF) {
        RefineExpert *entry=&m->refine.index[(size_t)layer*E+eid];
        if (entry->storage==REFINE_STORAGE_INT8) {
            posix_fadvise(m->refine.fd_int8,(off_t)entry->int8_offset,
                          (off_t)entry->int8_bytes,POSIX_FADV_WILLNEED);
        } else {
            for (int p=0;p<REFINE_PROJECTIONS;++p) {
                RefineProjection *projection=&entry->projection[p];
                posix_fadvise(m->refine.fd_base,(off_t)projection->base_offset,
                              (off_t)projection->base_bytes,POSIX_FADV_WILLNEED);
                if (refine_mask&(1u<<p))
                    posix_fadvise(m->refine.fd_residual,(off_t)projection->residual_offset,
                                  (off_t)projection->residual_bytes,POSIX_FADV_WILLNEED);
                posix_fadvise(m->refine.fd_scales,(off_t)projection->scale_offset,
                              (off_t)projection->scale_bytes,POSIX_FADV_WILLNEED);
            }
        }
    } else if (sz > 0) {
        posix_fadvise(m->fd_exp, off, sz, POSIX_FADV_WILLNEED);
    }
}

/* ---------- byte-budget expert cache (T5.5) ---------- */

/* RAM disponibile ADESSO (GB), stessa semantica di glm.c: pagine recuperabili
 * senza swap.  Misurata DOPO il caricamento dei tensori residenti, quindi il
 * residente e' gia' escluso dal disponibile. */
#ifndef __APPLE__
static long long read_cgroup_stat(const char *key) {
    FILE *f = fopen("/sys/fs/cgroup/memory.stat", "r");
    if (!f) return -1;
    char line[256];
    long long val = -1;
    while (fgets(line, sizeof(line), f)) {
        char k[64];
        long long v;
        if (sscanf(line, "%63s %lld", k, &v) == 2) {
            if (strcmp(k, key) == 0) {
                val = v;
                break;
            }
        }
    }
    fclose(f);
    return val;
}

static double cgroup_mem_available_gb(void) {
    long long limit = -1;
    long long current = -1;
    FILE *f_curr = fopen("/sys/fs/cgroup/memory.current", "r");
    if (f_curr) {
        if (fscanf(f_curr, "%lld", &current) != 1) current = -1;
        fclose(f_curr);
    }
    if (current < 0) return -1.0;
    
    // Subtract page cache (file) as it is reclaimable
    long long file_size = read_cgroup_stat("file");
    if (file_size > 0 && current > file_size) {
        current -= file_size;
    }
    
    FILE *f_max = fopen("/sys/fs/cgroup/memory.max", "r");
    if (f_max) {
        char buf[64];
        if (fgets(buf, sizeof(buf), f_max)) {
            if (strncmp(buf, "max", 3) != 0) limit = atoll(buf);
        }
        fclose(f_max);
    }
    FILE *f_high = fopen("/sys/fs/cgroup/memory.high", "r");
    if (f_high) {
        char buf[64];
        if (fgets(buf, sizeof(buf), f_high)) {
            if (strncmp(buf, "max", 3) != 0) {
                long long high = atoll(buf);
                if (limit < 0 || high < limit) limit = high;
            }
        }
        fclose(f_high);
    }
    if (limit > 0 && limit > current) {
        return (double)(limit - current) / (1024.0 * 1024.0 * 1024.0);
    }
    return -1.0;
}

static int linux_memory_pressure_level(void) {
    long long limit = -1;
    long long current = -1;
    FILE *f_curr = fopen("/sys/fs/cgroup/memory.current", "r");
    if (f_curr) {
        if (fscanf(f_curr, "%lld", &current) != 1) current = -1;
        fclose(f_curr);
    }
    if (current > 0) {
        // Subtract page cache (file) as it is reclaimable
        long long file_size = read_cgroup_stat("file");
        if (file_size > 0 && current > file_size) {
            current -= file_size;
        }
        
        FILE *f_max = fopen("/sys/fs/cgroup/memory.max", "r");
        if (f_max) {
            char buf[64];
            if (fgets(buf, sizeof(buf), f_max)) {
                if (strncmp(buf, "max", 3) != 0) limit = atoll(buf);
            }
            fclose(f_max);
        }
        FILE *f_high = fopen("/sys/fs/cgroup/memory.high", "r");
        if (f_high) {
            char buf[64];
            if (fgets(buf, sizeof(buf), f_high)) {
                if (strncmp(buf, "max", 3) != 0) {
                    long long high = atoll(buf);
                    if (limit < 0 || high < limit) limit = high;
                }
            }
            fclose(f_high);
        }
        if (limit > 0) {
            double ratio = (double)current / (double)limit;
            if (ratio > 0.90) return 4;
            if (ratio > 0.80) return 2;
            return 1;
        }
    }
    FILE *f_mem = fopen("/proc/meminfo", "r");
    if (f_mem) {
        char ln[256];
        double total = 0, avail = 0;
        int found = 0;
        while (fgets(ln, sizeof(ln), f_mem)) {
            if (sscanf(ln, "MemTotal: %lf", &total) == 1) found++;
            if (sscanf(ln, "MemAvailable: %lf", &avail) == 1) found++;
            if (found == 2) break;
        }
        fclose(f_mem);
        if (total > 0 && avail > 0) {
            double ratio = avail / total;
            if (ratio < 0.08 || avail < 1048576.0) return 4;
            if (ratio < 0.15 || avail < 2097152.0) return 2;
        }
    }
    return 1;
}
#endif

static double mem_available_gb(void) {
#ifdef __APPLE__
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vm;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &cnt) != KERN_SUCCESS) return 0;
    return ((double)vm.free_count + (double)vm.inactive_count +
            (double)vm.purgeable_count) * (double)sysconf(_SC_PAGESIZE) / 1e9;
#else
    double cg_avail = cgroup_mem_available_gb();
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return cg_avail > 0 ? cg_avail : 0;
    char ln[256]; double kb = 0;
    while (fgets(ln, sizeof(ln), f)) if (sscanf(ln, "MemAvailable: %lf", &kb) == 1) break;
    fclose(f);
    double host_avail = kb / 1e6;
    if (cg_avail > 0 && cg_avail < host_avail) {
        return cg_avail;
    }
    return host_avail;
#endif
}

/* Il core restituisce la proprieta' del payload qui su ogni eviction /
 * pressure / destroy: lo slab resta vivo nel pool cosi' i prossimi miss lo
 * riusano senza malloc, come faceva il vecchio swap ws<->cache. */
static void eslot_release(void *context, const ecache_release_event *event) {
    Model *m = (Model*)context;
    ESlot *slot = (ESlot*)event->payload;
    if (m->eslot_pool_n < m->eslot_pool_cap) {
        m->eslot_pool[m->eslot_pool_n++] = slot;
    } else {
        free(slot->slab);
        free(slot);
    }
}

static uint64_t eslot_pool_trim(Model *m,int keep){
    if(keep<0)keep=0;
    uint64_t released=0;
    while(m->eslot_pool_n>keep){
        ESlot *slot=m->eslot_pool[--m->eslot_pool_n];
        if(slot->slab_cap>0)released+=(uint64_t)slot->slab_cap;
        free(slot->slab);free(slot);
    }
    return released;
}

static ecache_key eslot_key(int layer, int eid, uint8_t refine_mask) {
    /* La maschera di precisione entra nella chiave (3 bit sopra l'expert id)
     * per conservare il determinismo mask-aware di T5.3 negli esperimenti
     * espliciti; nel percorso di produzione e' costante. */
    ecache_key key = { (uint32_t)layer,
                       (uint32_t)eid | ((uint32_t)refine_mask << 16) };
    return key;
}

/* Pressione memoria (T3.3 minimale).  La sorgente dispatch
 * DISPATCH_SOURCE_TYPE_MEMORYPRESSURE NON consegna eventi a processi non
 * privilegiati su questo sistema (verificato 2026-07-12 con una probe
 * standalone e una transizione pulita 1->2 del kernel), quindi il thread di
 * calcolo legge direttamente kern.memorystatus_vm_pressure_level (1=normal,
 * 2=warn, 4=critical — lettura non privilegiata, ~1 us) con un contatore di
 * decimazione e un cooldown anti-thrashing tra un reclaim e l'altro. */
static void ecache_service_pressure(Model *m) {
    static int poll_decimator = 0;
    static double last_reclaim_ts = 0;
    if (++poll_decimator < 16) return;
    poll_decimator = 0;
    int level = 0;
#ifdef __APPLE__
    int apple_level = 0; size_t len = sizeof(apple_level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &apple_level, &len, NULL, 0) == 0) {
        level = apple_level;
    }
#else
    level = linux_memory_pressure_level();
#endif
    if (level < 2) return;
    double now = now_s();
    if (now - last_reclaim_ts < 10.0) return;
    last_reclaim_ts = now;
    ecache_stats stats;
    ecache_get_stats(m->ec, &stats);
    int critical = level >= 4;
    uint64_t target = critical ? 0 : (stats.payload_bytes / 4) * 3;
    uint64_t reclaimed = 0;
    ecache_apply_pressure(m->ec, ECACHE_PRESSURE_CRITICAL, target, &reclaimed);
    eslot_pool_trim(m,0);
    fprintf(stderr, "[ecache] memory pressure %s: released %.1f MB\n",
            critical ? "CRITICAL" : "WARN", (double)reclaimed / 1e6);
}

static void model_init(Model *m, const char *snap, const char *refine_dir,
                       int refine_mode, int refine_verify,
                       int refine_full_ranks, uint8_t refine_base_projections,
                       const char *refine_base_layers) {
    g_direct = getenv("DIRECT") ? atoi(getenv("DIRECT")) : 0;
    /* IDOT=0 -> kernel f32 esatti (dequant-on-use senza quantizzare le
     * attivazioni): stesso interruttore A/B di glm.c, serve per la
     * validazione quantization-aware contro l'oracolo torch. */
    g_idot = getenv("IDOT") ? atoi(getenv("IDOT")) : 1;
    /* Optional narrower validation switch: keep the large MoE matmuls on
     * the accelerated path while preserving float activations in DeltaNet,
     * whose recurrent state carries error across every later token. */
    g_stateful_idot = getenv("IDOT_STATEFUL")
                       ? atoi(getenv("IDOT_STATEFUL")) : g_idot;
    g_moe_down_idot = getenv("IDOT_MOE_DOWN")
                    ? atoi(getenv("IDOT_MOE_DOWN")) : g_idot;
    if (getenv("SEQ_PREFILL")) g_seq_prefill = atoi(getenv("SEQ_PREFILL"));
    if (getenv("SEQ_PREFILL_FRAC")) g_seq_frac = (float)atof(getenv("SEQ_PREFILL_FRAC"));
    if (getenv("SEQ_PREFILL_MIN_S")) g_seq_min_s = atoi(getenv("SEQ_PREFILL_MIN_S"));
    memset(m, 0, sizeof(*m));
    load_cfg(&m->c, snap);
    
    st_init(&m->S, snap);
    load_manifest(m, snap);
    if (m->expert_group_size && refine_mode != REFINE_OFF) {
        fprintf(stderr,
                "refinable shelves: legacy row-q4 shelves are incompatible with groupwise experts\n");
        exit(1);
    }
    fprintf(stderr, "[weights] expert_quant=%s%s%d down_bits=%d\n",
            m->expert_group_size ?
                (m->expert_down_bits == 8 ? "groupwise-q4-gate-up/q8-down group="
                                          : "groupwise-q4 group=")
                : "legacy-row-q4",
            m->expert_group_size ? "" : " group=",
            m->expert_group_size, m->expert_down_bits);
    
    // Open experts file
    char exp_path[1024];
    snprintf(exp_path, sizeof(exp_path), "%s/experts.bin", snap);
    m->fd_exp = open(exp_path, O_RDONLY);
    if (m->fd_exp < 0) { perror(exp_path); exit(1); }
#ifdef __APPLE__
    m->fd_exp_direct = compat_open_direct(exp_path);
#elif defined(O_DIRECT)
    m->fd_exp_direct = open(exp_path, O_RDONLY | O_DIRECT);
#else
    m->fd_exp_direct = -1;
#endif

    /* Parse and discard the large JSON manifest before resident tensors are
     * materialized, keeping initialization peak memory bounded. */
    refine_init(m,snap,refine_dir,refine_mode,refine_verify,
                refine_full_ranks,refine_base_projections,refine_base_layers);

    Cfg *c = &m->c;
    int D = c->hidden;
    
    m->embed = qt_load(m, "model.embed_tokens.weight", c->vocab, D);
    m->lm_head = qt_load(m, "lm_head.weight", c->vocab, D);
    m->final_norm = load_float_t(m, "model.norm.weight");
    
    m->L = malloc(c->n_layers * sizeof(Layer));
    for (int i = 0; i < c->n_layers; i++) {
        Layer *l = &m->L[i];
        l->block_type = c->layer_type[i];
        
        char nm[512];
        #define PM(s) (snprintf(nm, sizeof(nm), "model.layers.%d." s, i), nm)
        
        l->in_ln = load_float_t(m, PM("input_layernorm.weight"));
        l->post_ln = load_float_t(m, PM("post_attention_layernorm.weight"));
        
        if (l->block_type == 0) {
            // DeltaNet linear attention
            int conv_dim = c->linear_key_head_dim * c->linear_num_key_heads * 2 + c->linear_value_head_dim * c->linear_num_value_heads;
            int value_dim = c->linear_value_head_dim * c->linear_num_value_heads;
            
            l->in_proj_qkv = qt_load(m, PM("linear_attn.in_proj_qkv.weight"), conv_dim, D);
            l->in_proj_z = qt_load(m, PM("linear_attn.in_proj_z.weight"), value_dim, D);
            l->in_proj_b = qt_load(m, PM("linear_attn.in_proj_b.weight"), c->linear_num_value_heads, D);
            l->in_proj_a = qt_load(m, PM("linear_attn.in_proj_a.weight"), c->linear_num_value_heads, D);
            
            l->conv1d_w = load_float_t(m, PM("linear_attn.conv1d.weight"));
            l->A_log = load_float_t(m, PM("linear_attn.A_log"));
            l->dt_bias = load_float_t(m, PM("linear_attn.dt_bias"));
            l->norm_w = load_float_t(m, PM("linear_attn.norm.weight"));
            l->out_proj = qt_load(m, PM("linear_attn.out_proj.weight"), D, value_dim);
        } else {
            // GQA attention
            int q_dim = c->n_heads * c->head_dim;
            int kv_dim = c->n_kv_heads * c->head_dim;
            
            l->q_proj = qt_load(m, PM("self_attn.q_proj.weight"), 2 * q_dim, D);
            l->k_proj = qt_load(m, PM("self_attn.k_proj.weight"), kv_dim, D);
            l->v_proj = qt_load(m, PM("self_attn.v_proj.weight"), kv_dim, D);
            l->o_proj = qt_load(m, PM("self_attn.o_proj.weight"), D, q_dim);
            
            l->q_norm = load_float_t(m, PM("self_attn.q_norm.weight"));
            l->k_norm = load_float_t(m, PM("self_attn.k_norm.weight"));
        }
        
        // FFN
        l->has_moe = c->has_moe;
        if (l->has_moe) {
            l->router_w = qt_load(m, PM("mlp.gate.weight"), c->num_experts, D);
            
            l->has_shared = 1;
            int sI = c->shared_expert_intermediate_size;
            l->shared_gate = qt_load(m, PM("mlp.shared_expert.gate_proj.weight"), sI, D);
            l->shared_up   = qt_load(m, PM("mlp.shared_expert.up_proj.weight"), sI, D);
            l->shared_down = qt_load(m, PM("mlp.shared_expert.down_proj.weight"), D, sI);
            l->shared_gate_w = load_float_t(m, PM("mlp.shared_expert_gate.weight"));
        } else {
            l->gate_proj = qt_load(m, PM("mlp.gate_proj.weight"), c->moe_intermediate_size, D);
            l->up_proj   = qt_load(m, PM("mlp.up_proj.weight"), c->moe_intermediate_size, D);
            l->down_proj = qt_load(m, PM("mlp.down_proj.weight"), D, c->moe_intermediate_size);
        }
    }
    
    // Byte-budget expert cache (T5.5): global budget + per-layer floors + LRU.
    int NL = c->n_layers + c->mtp_layers;
    int E = c->num_experts;
    uint64_t max_slab = 0, total_expert_bytes = 0;
    for (size_t i = 0; i < (size_t)NL * (size_t)E; i++) {
        int64_t sz = m->expert_sizes[i];
        if (sz > 0) {
            if ((uint64_t)sz > max_slab) max_slab = (uint64_t)sz;
            total_expert_bytes += (uint64_t)sz + sizeof(ESlot);
        }
    }
    if (max_slab == 0) max_slab = 1 << 20;
    uint64_t budget;
    const char *budget_env = getenv("EBUDGET_GB");
    const char *ecap_env = getenv("ECACHE");
    const char *budget_mode;
    uint32_t max_entries;
    if (budget_env) {
        double gb = atof(budget_env);
        if (gb <= 0) { fprintf(stderr, "EBUDGET_GB must be positive\n"); exit(1); }
        budget = (uint64_t)(gb * 1e9);
        max_entries = (uint32_t)((size_t)NL * (size_t)E);
        budget_mode = "EBUDGET_GB";
    } else if (ecap_env) {
        /* Compatibilita' script/baseline esistenti: ECACHE=slot-per-layer
         * diventa un budget equivalente in byte con lo stesso tetto di slot. */
        int slots = atoi(ecap_env);
        if (slots < 1) slots = 1;
        budget = (uint64_t)slots * (uint64_t)NL * (max_slab + sizeof(ESlot));
        max_entries = (uint32_t)((uint64_t)slots * (uint64_t)NL);
        budget_mode = "ECACHE-compat";
    } else if (getenv("EBUDGET_AUTO")) {
        /* AUTO: 88% del disponibile adesso (il residente e' gia' caricato,
         * quindi gia' escluso), meno slack per OS + KV + scratch ws.
         * NON e' il default: l'A/B di produzione (2026-07-12) ha misurato che
         * su M3 single-thread una cache grande RALLENTA (matmul +41% da slab
         * freddi sparsi su 6 GB; il prefetch copre gia' i miss dallo
         * streaming SSD).  Vedi docs/bench_log.md. */
        double slack = 1.5e9 + 64.0 * (double)max_slab;
        double avail = mem_available_gb() * 1e9;
        double bytes = avail * 0.88 - slack;
        if (bytes < 512e6) bytes = 512e6;
        budget = (uint64_t)bytes;
        max_entries = (uint32_t)((size_t)NL * (size_t)E);
        budget_mode = "AUTO";
    } else {
        /* Default: budget equivalente ai 16 slot/layer storici — misurato
         * alla pari o piu' veloce di ogni budget maggiore provato. */
        budget = (uint64_t)16 * (uint64_t)NL * (max_slab + sizeof(ESlot));
        max_entries = (uint32_t)(16u * (uint64_t)NL);
        budget_mode = "default-16-slot";
    }
    if (budget > total_expert_bytes) budget = total_expert_bytes;
    m->ec_budget_bytes = budget;
    ecache_config ecfg = {
        .budget_bytes = budget,
        .payload_alignment = 4096,
        .max_entries = max_entries ? max_entries : 1,
        .layer_count = (uint32_t)NL,
        .policy = ECACHE_POLICY_LRU,
    };
    ecache_layer_floor *floors = calloc((size_t)NL, sizeof(*floors));
    if (!floors) { fprintf(stderr, "OOM ecache floors\n"); exit(1); }
    for (int i = 0; i < NL; i++) floors[i].min_base_entries = 2;
    size_t workspace_size = 0;
    ecache_status st = ecache_workspace_size(&ecfg, &workspace_size);
    if (st != ECACHE_OK) {
        fprintf(stderr, "ecache workspace: %s\n", ecache_status_string(st));
        exit(1);
    }
    m->ec_workspace = malloc(workspace_size);
    if (!m->ec_workspace) { fprintf(stderr, "OOM ecache workspace\n"); exit(1); }
    ecache_callbacks callbacks = { eslot_release, m };
    st = ecache_init(m->ec_workspace, workspace_size, &ecfg, floors, &callbacks, &m->ec);
    free(floors);
    if (st != ECACHE_OK) {
        fprintf(stderr, "ecache init: %s\n", ecache_status_string(st));
        exit(1);
    }
    /* Scratch already retains up to 64 reusable miss slabs. This auxiliary
     * pool only absorbs short-lived size mismatches/multiple evictions; a
     * large persistent pool looked like a per-turn RAM leak in the app. */
    m->eslot_pool_cap = 32;
    m->eslot_pool = calloc((size_t)m->eslot_pool_cap, sizeof(ESlot*));
    if (!m->eslot_pool) { fprintf(stderr, "OOM eslot pool\n"); exit(1); }
    fprintf(stderr,
            "[ecache] budget=%.2f GB (%s) max_entries=%u layers=%d policy=LRU "
            "(expert max %.2f MB, tutti gli expert %.2f GB)\n",
            (double)budget / 1e9, budget_mode, ecfg.max_entries, NL,
            (double)max_slab / 1e6, (double)total_expert_bytes / 1e9);
    route_trace_init(m);
}

static void rope_head(float *x, int pos, float theta, int head_dim, int rotary_dim) {
    int h = rotary_dim / 2;
    for (int j = 0; j < h; j++) {
        float inv = powf(theta, -2.0f * j / rotary_dim);
        float ang = pos * inv, cs = cosf(ang), sn = sinf(ang);
        float a = x[j], b = x[j+h];
        x[j]   = a*cs - b*sn;
        x[j+h] = b*cs + a*sn;
    }
}

static void attention_gqa(Model *m, Layer *l, int layer, float *x, int S, int pos_base, float *out) {
    Cfg *c = &m->c;
    int H = c->n_heads, hd = c->head_dim;
    int G = c->n_kv_heads;
    int grp = H / G;
    
    if (pos_base + S > m->max_t) {
        fprintf(stderr, "Error: cache bounds check failed: pos_base=%d, S=%d, max_t=%d\n", pos_base, S, m->max_t);
        exit(1);
    }
    
    float *q_and_gate = falloc((int64_t)S * 2 * H * hd);
    matmul_qt(q_and_gate, x, &l->q_proj, S, g_idot, g_i4s);
    
    float *q = falloc((int64_t)S * H * hd);
    float *gate = falloc((int64_t)S * H * hd);
    
    for (int s = 0; s < S; s++) {
        for (int h = 0; h < H; h++) {
            float *src = q_and_gate + (int64_t)s * 2 * H * hd + h * hd * 2;
            float *dst_q = q + (int64_t)s * H * hd + h * hd;
            float *dst_g = gate + (int64_t)s * H * hd + h * hd;
            memcpy(dst_q, src, hd * sizeof(float));
            memcpy(dst_g, src + hd, hd * sizeof(float));
        }
    }
    free(q_and_gate);
    
    float *k = falloc((int64_t)S * G * hd);
    float *v = falloc((int64_t)S * G * hd);
    matmul_qt(k, x, &l->k_proj, S, g_idot, g_i4s);
    matmul_qt(v, x, &l->v_proj, S, g_idot, g_i4s);
    
    for (int s = 0; s < S; s++) {
        for (int h = 0; h < H; h++) {
            rmsnorm_row(q + (int64_t)s * H * hd + h * hd, q + (int64_t)s * H * hd + h * hd, l->q_norm, hd, c->eps);
        }
        for (int g = 0; g < G; g++) {
            rmsnorm_row(k + (int64_t)s * G * hd + g * hd, k + (int64_t)s * G * hd + g * hd, l->k_norm, hd, c->eps);
        }
    }
    
    int rotary_dim = (int)(hd * c->partial_rotary_factor);
    for (int s = 0; s < S; s++) {
        int pos = pos_base + s;
        for (int h = 0; h < H; h++) {
            rope_head(q + (int64_t)s * H * hd + h * hd, pos, c->rope_theta, hd, rotary_dim);
        }
        for (int g = 0; g < G; g++) {
            rope_head(k + (int64_t)s * G * hd + g * hd, pos, c->rope_theta, hd, rotary_dim);
        }
    }
    
    for (int s = 0; s < S; s++) {
        int t = pos_base + s;
        for (int g = 0; g < G; g++) {
            memcpy(m->K[layer] + ((int64_t)g * m->max_t + t) * hd, k + (int64_t)s * G * hd + g * hd, hd * sizeof(float));
            memcpy(m->V[layer] + ((int64_t)g * m->max_t + t) * hd, v + (int64_t)s * G * hd + g * hd, hd * sizeof(float));
        }
    }
    free(k); free(v);
    
    float *ctx = falloc((int64_t)S * H * hd);
    float scale = 1.f / sqrtf((float)hd);

    /* Punteggi di attenzione su HEAP, dimensionati al contesto reale: il
     * vecchio buffer fisso sc[4096] sfondava lo stack (SIGABRT via
     * __stack_chk_fail) alla prima generazione con contesto > 4096 token —
     * mai colpito finche' i tetti di generazione erano piccoli.  Una fetta
     * per thread; collapse(2)+static garantisce che ogni iterazione usi solo
     * la fetta del proprio thread. */
#ifdef _OPENMP
    int sc_threads = omp_get_max_threads();
#else
    int sc_threads = 1;
#endif
    float *sc_pool = falloc((int64_t)sc_threads * m->max_t);

    #pragma omp parallel for collapse(2) schedule(static)
    for (int g = 0; g < G; g++) {
        for (int h_in_g = 0; h_in_g < grp; h_in_g++) {
            int h = g * grp + h_in_g;
            for (int s = 0; s < S; s++) {
                int qpos = pos_base + s;
                const float *qv = q + (int64_t)s * H * hd + h * hd;

#ifdef _OPENMP
                float *sc = sc_pool + (int64_t)omp_get_thread_num() * m->max_t;
#else
                float *sc = sc_pool;
#endif
                for (int t = 0; t <= qpos; t++) {
                    const float *kv = m->K[layer] + ((int64_t)g * m->max_t + t) * hd;
                    float acc = 0;
                    for (int dd = 0; dd < hd; dd++) acc += qv[dd] * kv[dd];
                    sc[t] = acc * scale;
                }
                
                softmax_row(sc, qpos + 1);
                
                float *cx = ctx + (int64_t)s * H * hd + h * hd;
                for (int dd = 0; dd < hd; dd++) cx[dd] = 0;
                for (int t = 0; t <= qpos; t++) {
                    const float *vrow = m->V[layer] + ((int64_t)g * m->max_t + t) * hd;
                    float a = sc[t];
                    for (int dd = 0; dd < hd; dd++) cx[dd] += a * vrow[dd];
                }
            }
        }
    }
    free(sc_pool);
    free(q);
    
    for (int64_t j = 0; j < (int64_t)S * H * hd; j++) {
        ctx[j] = ctx[j] * sigmoid(gate[j]);
    }
    free(gate);
    
    matmul_qt(out, ctx, &l->o_proj, S, g_idot, g_i4s);
    free(ctx);
}

static void attention_deltanet(Model *m, Layer *l, int layer, float *x, int S, int pos_base, float *out) {
    Cfg *c = &m->c;
    int conv_dim = c->linear_key_head_dim * c->linear_num_key_heads * 2 + c->linear_value_head_dim * c->linear_num_value_heads;
    
    int key_dim   = c->linear_key_head_dim * c->linear_num_key_heads;
    int value_dim = c->linear_value_head_dim * c->linear_num_value_heads;
    int num_v_heads = c->linear_num_value_heads;
    int num_k_heads = c->linear_num_key_heads;
    int head_k_dim = c->linear_key_head_dim;
    int head_v_dim = c->linear_value_head_dim;
    int grp = num_v_heads / num_k_heads;
    
    float *mixed_qkv = falloc((int64_t)S * conv_dim);
    matmul_qt(mixed_qkv, x, &l->in_proj_qkv, S, g_stateful_idot, g_i4s);
    
    float *z = falloc((int64_t)S * value_dim);
    matmul_qt(z, x, &l->in_proj_z, S, g_stateful_idot, g_i4s);
    
    float *b_proj = falloc((int64_t)S * num_v_heads);
    float *a_proj = falloc((int64_t)S * num_v_heads);
    matmul_qt(b_proj, x, &l->in_proj_b, S, g_stateful_idot, g_i4s);
    matmul_qt(a_proj, x, &l->in_proj_a, S, g_stateful_idot, g_i4s);
    
    // Gated DeltaNet Convolution state update and causal conv.
    // T2.6 fase 1: ogni canale porta il proprio stato (v0,v1,v2) e la sua
    // catena nei token e' indipendente dagli altri canali — il loop esterno
    // sui canali si parallelizza mantenendo l'aritmetica di ciascun canale
    // nell'ordine identico (bit-exact rispetto alla versione sequenziale).
    float *conv_out = falloc((int64_t)S * conv_dim);

    #pragma omp parallel for schedule(static)
    for (int ch = 0; ch < conv_dim; ch++) {
        float v0 = m->conv_state[layer][ch * 3 + 0];
        float v1 = m->conv_state[layer][ch * 3 + 1];
        float v2 = m->conv_state[layer][ch * 3 + 2];
        const float w0 = l->conv1d_w[ch * 4 + 0], w1 = l->conv1d_w[ch * 4 + 1];
        const float w2 = l->conv1d_w[ch * 4 + 2], w3 = l->conv1d_w[ch * 4 + 3];
        for (int s = 0; s < S; s++) {
            float val = mixed_qkv[(int64_t)s * conv_dim + ch];
            float sum = v0 * w0 + v1 * w1 + v2 * w2 + val * w3;
            conv_out[(int64_t)s * conv_dim + ch] = silu(sum);
            v0 = v1; v1 = v2; v2 = val;
        }
        m->conv_state[layer][ch * 3 + 0] = v0;
        m->conv_state[layer][ch * 3 + 1] = v1;
        m->conv_state[layer][ch * 3 + 2] = v2;
    }
    free(mixed_qkv);
    
    // Split conv_out into query, key, value
    float *q = falloc((int64_t)S * key_dim);
    float *k = falloc((int64_t)S * key_dim);
    float *v = falloc((int64_t)S * value_dim);
    
    for (int s = 0; s < S; s++) {
        memcpy(q + (int64_t)s * key_dim,   conv_out + (int64_t)s * conv_dim, key_dim * sizeof(float));
        memcpy(k + (int64_t)s * key_dim,   conv_out + (int64_t)s * conv_dim + key_dim, key_dim * sizeof(float));
        memcpy(v + (int64_t)s * value_dim, conv_out + (int64_t)s * conv_dim + 2 * key_dim, value_dim * sizeof(float));
    }
    free(conv_out);
    
    // L2-normalization for query & key along head_dim
    for (int s = 0; s < S; s++) {
        for (int h = 0; h < num_k_heads; h++) {
            l2norm_head(q + (int64_t)s * key_dim + h * head_k_dim, head_k_dim, 1e-6f);
            l2norm_head(k + (int64_t)s * key_dim + h * head_k_dim, head_k_dim, 1e-6f);
        }
    }
    
    // Query scaling
    float q_scale = 1.0f / sqrtf((float)head_k_dim);
    for (int64_t j = 0; j < (int64_t)S * key_dim; j++) q[j] *= q_scale;
    
    // DeltaNet Recurrent Update.
    // T2.6 fase 1: ogni V-head possiede la sua matrice di stato e la sua
    // catena token-sequenziale non tocca gli altri head — parallelizzare
    // sugli head (32 su questo modello) lascia l'ordine aritmetico di ogni
    // head IDENTICO alla versione sequenziale (continuazione bit-exact).
    float *attn_out = falloc((int64_t)S * value_dim);

    #pragma omp parallel for schedule(dynamic, 1)
    for (int v_h = 0; v_h < num_v_heads; v_h++) {
        int k_h = v_h / grp;
        // Recurrent state matrix S [head_k_dim, head_v_dim]
        float *state_mat = m->recurrent_state[layer] + ((int64_t)v_h * head_k_dim * head_v_dim);
        for (int s = 0; s < S; s++) {
            float *q_t = q + (int64_t)s * key_dim + k_h * head_k_dim;
            float *k_t = k + (int64_t)s * key_dim + k_h * head_k_dim;
            float *v_t = v + (int64_t)s * value_dim + v_h * head_v_dim;
            float *z_t = z + (int64_t)s * value_dim + v_h * head_v_dim;

            float beta_t = sigmoid(b_proj[s * num_v_heads + v_h]);
            float dt_t = softplus(a_proj[s * num_v_heads + v_h] + l->dt_bias[v_h]);
            float decay_t = expf(-expf(l->A_log[v_h]) * dt_t);
            
            // 1. Decay state matrix: S = S * decay_t
            for (int i = 0; i < head_k_dim * head_v_dim; i++) {
                state_mat[i] *= decay_t;
            }
            
            // 2. Compute kv_mem_j = sum_i k_i * S_ij
            float kv_mem[256]; // head_v_dim max 256
            for (int j = 0; j < head_v_dim; j++) {
                float acc = 0.0f;
                for (int i = 0; i < head_k_dim; i++) {
                    acc += k_t[i] * state_mat[i * head_v_dim + j];
                }
                kv_mem[j] = acc;
            }
            
            // 3. Compute delta_j = (v_j - kv_mem_j) * beta_t
            float delta[256];
            for (int j = 0; j < head_v_dim; j++) {
                delta[j] = (v_t[j] - kv_mem[j]) * beta_t;
            }
            
            // 4. Update state matrix S_ij = S_ij + k_i * delta_j
            for (int i = 0; i < head_k_dim; i++) {
                for (int j = 0; j < head_v_dim; j++) {
                    state_mat[i * head_v_dim + j] += k_t[i] * delta[j];
                }
            }
            
            // 5. Compute attention output y_j = sum_i q_i * S_ij
            float y[256];
            for (int j = 0; j < head_v_dim; j++) {
                float acc = 0.0f;
                for (int i = 0; i < head_k_dim; i++) {
                    acc += q_t[i] * state_mat[i * head_v_dim + j];
                }
                y[j] = acc;
            }
            
            // 6. Gated RMSNorm: y = rmsnorm(y) * norm_w * silu(z_t)
            double ss = 0;
            for (int j = 0; j < head_v_dim; j++) ss += (double)y[j] * y[j];
            float r = 1.0f / sqrtf((float)(ss / head_v_dim) + c->eps);
            
            float *out_head = attn_out + (int64_t)s * value_dim + v_h * head_v_dim;
            for (int j = 0; j < head_v_dim; j++) {
                out_head[j] = (y[j] * r) * l->norm_w[j] * silu(z_t[j]);
            }
        }
    }
    
    free(q); free(k); free(v); free(z); free(b_proj); free(a_proj);
    
    // Output projection
    matmul_qt(out, attn_out, &l->out_proj, S, g_stateful_idot, g_i4s);
    free(attn_out);
}

static void mlp_moe(Model *m, Layer *l, int layer, float *x, int S, float *out,
                    int pos_base, const int *token_ids) {
    Cfg *c = &m->c;
    int D = c->hidden, E = c->num_experts, K = c->num_experts_per_tok, I = c->moe_intermediate_size;
    int sI = c->shared_expert_intermediate_size;
    /* Reclaim di pressione solo qui, prima di ogni lookup: mai tra un
     * ecache_get e il compute che usa il puntatore restituito. */
    ecache_service_pressure(m);
    /* T2.5: il buffer sequenziale serve solo al prefill; al primo passo di
     * decode (S==1) viene restituito al sistema. */
    if (S == 1 && m->seq_buf) {
        free(m->seq_buf); m->seq_buf = NULL; m->seq_buf_cap = 0;
    }
    
    float *logits = falloc((int64_t)S * E);
    matmul_qt(logits, x, &l->router_w, S, g_idot, g_i4s);
    
    // Clean output
    memset(out, 0, (int64_t)S * D * sizeof(float));
    
    // Parse routing experts for all S tokens
    int *idxs = malloc((int64_t)S * K * sizeof(int));
    float *ws = malloc((int64_t)S * K * sizeof(float));
    uint8_t *refine_masks=malloc((size_t)S*(size_t)K);
    int *keff = malloc(S * sizeof(int));
    if (!idxs || !ws || !refine_masks || !keff) {
        fprintf(stderr,"OOM allocating routed-expert policy state\n"); exit(1);
    }
    
    for (int s = 0; s < S; s++) {
        float *pr = logits + (int64_t)s * E;
        const int R = g_route.mode == ROUTE_OFF ? K : g_route.rank;
        int idx[64];
        float raw[64], prob[64], selected[64], selected_cumulative[64];
        float effective_weights[64] = {0}, cumulative[64];

        /* Ranking raw logits or their softmax gives the same stable ordering.
         * Rank once, before softmax destroys the pre-normalization scores. */
        for (int kk = 0; kk < R; kk++) {
            int best = -1; float bv = -1e30f;
            for (int e = 0; e < E; e++) {
                int taken = 0;
                for (int j = 0; j < kk; j++) if (idx[j] == e) { taken = 1; break; }
                if (!taken && pr[e] > bv) { bv = pr[e]; best = e; }
            }
            idx[kk] = best; raw[kk] = bv;
        }

        softmax_row(pr, E);
        float entropy = 0.f;
        const int need_metrics = g_route.mode != ROUTE_OFF ||
                                 g_moe_policy.mode == MOE_POLICY_MASS;
        if (need_metrics) {
            for (int e = 0; e < E; e++) if (pr[e] > 0.f) entropy -= pr[e] * logf(pr[e]);
        }
        if (g_route.mode != ROUTE_OFF) {
            float mass = 0.f;
            for (int kk = 0; kk < R; kk++) {
                prob[kk] = pr[idx[kk]];
                mass += prob[kk]; cumulative[kk] = mass;
            }
        } else {
            /* Preserve the original top-K hot path: no entropy/log calls and
             * no work for ranks that the evaluator will not consume. */
            for (int kk = 0; kk < K; kk++) prob[kk] = pr[idx[kk]];
        }

        float sm = 0.0f;
        for (int kk = 0; kk < K; kk++) sm += prob[kk];
        float selected_mass = 0.f;
        for (int kk = 0; kk < K; kk++) {
            selected[kk] = prob[kk] / sm;
            selected_mass += selected[kk];
            selected_cumulative[kk] = selected_mass;
        }
        /* The final selected-normalized mass is mathematically one.  Pinning
         * it avoids a threshold of exactly 1.0 depending on summation drift;
         * it does not alter the checkpoint's baseline expert weights. */
        selected_cumulative[K - 1] = 1.f;

        int effective_k = K, guarded = 0;
        float gap = R > 1 ? raw[0] - raw[1] : 0.f;
        if (g_moe_policy.mode == MOE_POLICY_FIXED) {
            effective_k = g_moe_policy.fixed_k;
        } else if (g_moe_policy.mode == MOE_POLICY_MASS) {
            for (effective_k = 1; effective_k < K; effective_k++)
                if (selected_cumulative[effective_k - 1] >= g_moe_policy.mass) break;
            if (effective_k < K &&
                ((g_moe_policy.has_max_entropy && entropy > g_moe_policy.max_entropy) ||
                 (g_moe_policy.has_min_gap && gap < g_moe_policy.min_gap))) {
                effective_k = K;
                guarded = 1;
            }
        }
        if (effective_k == K) {
            memcpy(effective_weights, selected, (size_t)K * sizeof(float));
        } else {
            float retained_mass = 0.f;
            for (int kk = 0; kk < effective_k; kk++) retained_mass += prob[kk];
            for (int kk = 0; kk < effective_k; kk++)
                effective_weights[kk] = prob[kk] / retained_mass;
        }

        if (g_route.mode != ROUTE_OFF) {
            route_record_or_replay(pos_base + s, token_ids[s], layer, idx, raw,
                                   prob, selected, selected_cumulative, &effective_k,
                                   effective_weights, entropy, gap, cumulative);
        }
        moe_policy_account(m, layer, idx, K, effective_k, guarded);
        
        for (int kk = 0; kk < K; kk++) {
            idxs[(int64_t)s * K + kk] = idx[kk];
            ws[(int64_t)s * K + kk] = effective_weights[kk];
            refine_masks[(size_t)s*(size_t)K+(size_t)kk]=
                refine_mask_for_rank(m,layer,kk);
        }
        keff[s] = effective_k;
    }
    free(logits);
    
    // Prefill batch-union: unique experts list
    int *uniq = malloc((size_t)E*8u*sizeof(int));
    uint8_t *uniq_masks=malloc((size_t)E*8u);
    int nu = 0;
    {
        uint8_t *seen = calloc((size_t)E*8u,1);
        if (!uniq || !uniq_masks || !seen) {
            fprintf(stderr,"OOM allocating routed-expert union\n"); exit(1);
        }
        for (int s = 0; s < S; s++) {
            for (int kk = 0; kk < keff[s]; kk++) {
                int e = idxs[(int64_t)s * K + kk];
                uint8_t mask=refine_masks[(size_t)s*(size_t)K+(size_t)kk];
                size_t key=(size_t)mask*(size_t)E+(size_t)e;
                if (!seen[key]) {
                    seen[key]=1; uniq[nu]=e; uniq_masks[nu]=mask; ++nu;
                }
            }
        }
        free(seen);
    }
    
    /* T2.5: quando l'unione di layer copre la maggior parte degli expert
     * (tipico dei prefill lunghi), UNA lettura sequenziale dell'intera
     * regione del layer sostituisce centinaia di pread sparsi.  Il buffer e'
     * transiente; le viste QT puntano direttamente dentro di esso (zero
     * copie, stessi byte → output bit-identico).  La cache non viene ne'
     * consultata ne' popolata su questo percorso: sono gli stessi blob del
     * contenitore, e ammettere ~256 expert farebbe solo churn di eviction. */
    ESlot *seq_slots = NULL;
    if (g_seq_prefill && m->refine.mode == REFINE_OFF && layer < c->n_layers &&
        S >= g_seq_min_s && nu >= (int)(g_seq_frac * (float)E)) {
        int64_t lo = INT64_MAX, hi = 0; int ok = 1;
        for (int e = 0; e < E && ok; e++) {
            int64_t off = m->expert_offsets[(size_t)layer * E + e];
            int64_t sz  = m->expert_sizes[(size_t)layer * E + e];
            if (sz <= 0) { ok = 0; break; }
            if (off < lo) lo = off;
            if (off + sz > hi) hi = off + sz;
        }
        if (ok) {
            int64_t span = hi - lo;
            if (span > m->seq_buf_cap) {
                free(m->seq_buf);
                if (posix_memalign((void**)&m->seq_buf, 16384, (size_t)span)) {
                    fprintf(stderr, "OOM seq prefill buffer\n"); exit(1);
                }
                m->seq_buf_cap = span;
            }
            double t0 = now_s();
            int fd = (m->fd_exp_direct >= 0) ? m->fd_exp_direct : m->fd_exp;
            refine_pread_exact(fd, m->seq_buf, (uint64_t)span, (uint64_t)lo,
                               "sequential layer expert region");
            m->t_edisk += now_s() - t0;
            m->seq_reads++; m->seq_bytes += (uint64_t)span;
            seq_slots = calloc((size_t)nu, sizeof(ESlot));
            if (!seq_slots) { fprintf(stderr, "OOM seq slots\n"); exit(1); }
            for (int u2 = 0; u2 < nu; u2++) {
                int eid = uniq[u2];
                seq_slots[u2].slab = m->seq_buf +
                    (m->expert_offsets[(size_t)layer * E + eid] - lo);
                seq_slots[u2].eid = eid;
                seq_slots[u2].refine_mask = uniq_masks[u2];
                expert_views(m, layer, &seq_slots[u2]);
            }
        }
    }

    // Evaluate experts in batches of 64
    float *xg = falloc((int64_t)S * D);
    float *gg = falloc((int64_t)S * I);
    float *uu = falloc((int64_t)S * I);
    float *hh = falloc((int64_t)S * D);
    int *rows = malloc(S * sizeof(int));
    float *rw = malloc(S * sizeof(float));

    for (int base = 0; base < nu; base += 64) {
        int nb = (nu - base < 64) ? (nu - base) : 64;
        ESlot *use[64]; int missk[64]; int nmiss = 0;

        if (seq_slots) {
            for (int j = 0; j < nb; j++) use[j] = &seq_slots[base + j];
        } else
        for (int j = 0; j < nb; j++) {
            int eid = uniq[base + j];
            uint8_t mask=uniq_masks[base+j]; use[j] = NULL;
            ecache_lookup_result found; ecache_view view;
            if (ecache_get(m->ec, eslot_key(layer,eid,mask),
                           ECACHE_REQUIRE_BASE, &found, &view) == ECACHE_OK &&
                found == ECACHE_LOOKUP_HIT) {
                m->hits++; use[j] = (ESlot*)view.base_payload;
            } else {
                use[j] = &m->ws[nmiss];
                missk[nmiss++] = j;
                m->miss++;
            }
        }
        
        // Load expert misses from experts.bin in parallel
        if (nmiss) {
            double t0 = now_s();
            #pragma omp parallel for schedule(dynamic, 1)
            for (int q = 0; q < nmiss; q++) {
                int item=base+missk[q];
                expert_load(m,layer,uniq[item],uniq_masks[item],&m->ws[q]);
            }
            m->t_edisk += now_s() - t0;
        }
        
        // Prefetch next batch asynchronoulsy
        if (!seq_slots && base + 64 < nu) {
            int nb2 = (nu - (base + 64) < 64) ? (nu - (base + 64)) : 64;
            for (int j = 0; j < nb2; j++) {
                int eid = uniq[base + 64 + j];
                uint8_t mask=uniq_masks[base+64+j];
                ecache_view view; /* peek: niente recency, niente contatori */
                if (ecache_peek(m->ec, eslot_key(layer,eid,mask), &view) != ECACHE_OK)
                    expert_prefetch(m,layer,eid,mask);
            }
        }
        
        // Execute math
        for (int j = 0; j < nb; j++) {
            int eid = uniq[base + j];
            uint8_t mask=uniq_masks[base+j]; ESlot *e = use[j];
            int nr = 0;
            for (int s = 0; s < S; s++) {
                for (int kk = 0; kk < keff[s]; kk++) {
                    if (idxs[(int64_t)s * K + kk] == eid &&
                        refine_masks[(size_t)s*(size_t)K+(size_t)kk]==mask) {
                        rows[nr] = s; rw[nr] = ws[(int64_t)s * K + kk]; nr++; break;
                    }
                }
            }
            if (!nr) continue;
            
            for (int r = 0; r < nr; r++) memcpy(xg + (int64_t)r * D, x + (int64_t)rows[r] * D, D * sizeof(float));
            
            double t0 = now_s();
            matmul_qt(gg, xg, &e->g, nr, g_idot, g_i4s);
            matmul_qt(uu, xg, &e->u, nr, g_idot, g_i4s);
            for (int64_t z = 0; z < (int64_t)nr * I; z++) gg[z] = siluf(gg[z]) * uu[z];
            matmul_qt(hh, gg, &e->d, nr, g_moe_down_idot, g_i4s);
            
            for (int r = 0; r < nr; r++) {
                float *os = out + (int64_t)rows[r] * D;
                float wgt = rw[r];
                const float *hr = hh + (int64_t)r * D;
                for (int d = 0; d < D; d++) os[d] += wgt * hr[d];
            }
            m->t_emm += now_s() - t0;
        }
        
        // Admission: ogni miss calcolato viene offerto al budget globale.  Il
        // core decide le eviction; NO_SPACE degrada a compute-and-discard.
        for (int q = 0; q < nmiss; q++) {
            ESlot *slot;
            if (m->eslot_pool_n > 0) slot = m->eslot_pool[--m->eslot_pool_n];
            else {
                slot = calloc(1, sizeof(ESlot));
                if (!slot) { fprintf(stderr, "OOM eslot\n"); exit(1); }
            }
            /* Trasferimento: lo slab caricato (con le viste QT che ci puntano
             * dentro) passa all'handle; lo scratch eredita il vecchio slab
             * dell'handle per il riuso al prossimo miss. */
            uint8_t *spare_slab = slot->slab; int64_t spare_cap = slot->slab_cap;
            *slot = m->ws[q];
            m->ws[q].slab = spare_slab; m->ws[q].slab_cap = spare_cap;
            m->ws[q].eid = -1;
            uint64_t charged = (uint64_t)slot->slab_cap + sizeof(ESlot);
            int64_t source_size = m->expert_sizes[(size_t)layer * m->c.num_experts + slot->eid];
            if (ecache_insert_base(m->ec, eslot_key(layer,slot->eid,slot->refine_mask),
                                   slot, charged,
                                   source_size > 0 ? (uint64_t)source_size : charged,
                                   ECACHE_ADMIT_DEMAND, NULL) != ECACHE_OK) {
                /* Restituisci lo slab caricato allo scratch e l'handle al pool. */
                uint8_t *loaded_slab = slot->slab; int64_t loaded_cap = slot->slab_cap;
                slot->slab = m->ws[q].slab; slot->slab_cap = m->ws[q].slab_cap;
                m->ws[q].slab = loaded_slab; m->ws[q].slab_cap = loaded_cap;
                if (m->eslot_pool_n < m->eslot_pool_cap) m->eslot_pool[m->eslot_pool_n++] = slot;
                else { free(slot->slab); free(slot); }
            }
        }
    }
    
    free(seq_slots);
    free(uniq); free(uniq_masks); free(idxs); free(ws); free(refine_masks); free(keff);
    free(xg); free(gg); free(uu); free(hh); free(rows); free(rw);
    
    // Shared expert evaluation
    float *sg = falloc((int64_t)S * sI);
    float *su = falloc((int64_t)S * sI);
    float *shh = falloc((int64_t)S * D);
    matmul_qt(sg, x, &l->shared_gate, S, g_idot, g_i4s);
    matmul_qt(su, x, &l->shared_up, S, g_idot, g_i4s);
    for (int64_t z = 0; z < (int64_t)S * sI; z++) sg[z] = siluf(sg[z]) * su[z];
    matmul_qt(shh, sg, &l->shared_down, S, g_moe_down_idot, g_i4s);
    
    // Sigmoid Gating for Shared Expert
    for (int s = 0; s < S; s++) {
        const float *xs = x + (int64_t)s * D;
        double gate_logit = 0;
        for (int d = 0; d < D; d++) gate_logit += (double)xs[d] * l->shared_gate_w[d];
        float gate_prob = sigmoid((float)gate_logit);
        
        float *os = out + (int64_t)s * D;
        const float *sh_s = shh + (int64_t)s * D;
        for (int d = 0; d < D; d++) os[d] += gate_prob * sh_s[d];
    }
    
    free(sg); free(su); free(shh);
}

static float *step(Model *m, const int *ids, int S, int pos_base) {
    Cfg *c = &m->c;
    int D = c->hidden;
    
    float *x = falloc((int64_t)S * D);
    for (int s = 0; s < S; s++) {
        embed_gather_row(x + (int64_t)s * D, &m->embed, ids[s]);
    }
    
    float *nrm = falloc((int64_t)S * D);
    float *tmp = falloc((int64_t)S * D);
    
    for (int i = 0; i < c->n_layers; i++) {
        Layer *l = &m->L[i];
        
        // Input layernorm
        for (int s = 0; s < S; s++) {
            rmsnorm_row(nrm + (int64_t)s * D, x + (int64_t)s * D, l->in_ln, D, c->eps);
        }
        
        // Attention mixer
        if (l->block_type == 0) {
            attention_deltanet(m, l, i, nrm, S, pos_base, tmp);
        } else {
            attention_gqa(m, l, i, nrm, S, pos_base, tmp);
        }
        for (int64_t j = 0; j < (int64_t)S * D; j++) x[j] += tmp[j];
        
        // Post layernorm
        for (int s = 0; s < S; s++) {
            rmsnorm_row(nrm + (int64_t)s * D, x + (int64_t)s * D, l->post_ln, D, c->eps);
        }
        
        // MLP (Dense or MoE)
        if (l->has_moe) {
            mlp_moe(m, l, i, nrm, S, tmp, pos_base, ids);
        } else {
            float *g = falloc((int64_t)S * c->moe_intermediate_size);
            float *u = falloc((int64_t)S * c->moe_intermediate_size);
            float *h = falloc((int64_t)S * D);
            
            matmul_qt(g, nrm, &l->gate_proj, S, g_idot, g_i4s);
            matmul_qt(u, nrm, &l->up_proj, S, g_idot, g_i4s);
            for (int64_t j = 0; j < (int64_t)S * c->moe_intermediate_size; j++) g[j] = siluf(g[j]) * u[j];
            matmul_qt(h, g, &l->down_proj, S, g_idot, g_i4s);
            
            memcpy(tmp, h, (int64_t)S * D * sizeof(float));
            free(g); free(u); free(h);
        }
        for (int64_t j = 0; j < (int64_t)S * D; j++) x[j] += tmp[j];
    }
    
    float *last = falloc(D);
    rmsnorm_row(last, x + (int64_t)(S - 1) * D, m->final_norm, D, c->eps);
    
    float *logit = falloc(c->vocab);
    /* matmul_qt dequantizza on-the-fly riga per riga (kernel int8/int4 dot);
     * niente bisogno di materializzare l'intera tabella lm_head ad ogni token. */
    matmul_qt(logit, last, &m->lm_head, 1, g_idot, g_i4s);
    
    free(x); free(nrm); free(tmp); free(last);
    return logit;
}

static void forward_all(Model *m, const int *full, int nfull, int *pred) {
    Cfg *c = &m->c;
    m->max_t = nfull;
    
    // Allocate caches
    m->K = calloc(c->n_layers, sizeof(float*));
    m->V = calloc(c->n_layers, sizeof(float*));
    for (int i = 0; i < c->n_layers; i++) {
        if (c->layer_type[i] == 1) {
            m->K[i] = falloc((int64_t)c->n_kv_heads * m->max_t * c->head_dim);
            m->V[i] = falloc((int64_t)c->n_kv_heads * m->max_t * c->head_dim);
        }
    }
    m->conv_state = calloc(c->n_layers, sizeof(float*));
    m->recurrent_state = calloc(c->n_layers, sizeof(float*));
    for (int i = 0; i < c->n_layers; i++) {
        if (c->layer_type[i] == 0) {
            int conv_dim = c->linear_key_head_dim * c->linear_num_key_heads * 2 + c->linear_value_head_dim * c->linear_num_value_heads;
            m->conv_state[i] = calloc(conv_dim * 3, sizeof(float));
            m->recurrent_state[i] = calloc(c->linear_num_value_heads * c->linear_key_head_dim * c->linear_value_head_dim, sizeof(float));
        }
    }
    
    for (int s = 0; s < nfull; s++) {
        int token = full[s];
        float *logit = step(m, &token, 1, s);
        int best = 0; float bv = logit[0];
        for (int i = 1; i < c->vocab; i++) {
            if (logit[i] > bv) { bv = logit[i]; best = i; }
        }
        pred[s] = best;
        free(logit);
    }
}

typedef int (*TokenSink)(int token, void *ctx); /* return nonzero to stop */
typedef struct {
    int sample, top_k;
    float temperature, top_p;
    /* Penalita' di presenza in stile OpenAI, sottratta dal logit di ogni
     * token GIA' GENERATO in questa risposta.  Default 0 (sampling ufficiale
     * intatto).  La famiglia Qwen la raccomanda per i deployment quantizzati
     * contro le ripetizioni; qui mitiga il raddoppio sporadico delle parole
     * funzione dell'int4 (vedi docs/bench_log.md 2026-07-12). */
    float presence_penalty;
    /* Penalita' solo sull'ULTIMO token emesso: colpisce esattamente il
     * raddoppio immediato ("of of") senza punire il riuso legittimo dei
     * token nel resto del documento (essenziale per il codice). */
    float last_token_penalty;
    int last_token;
    uint8_t *seen;          /* bitmap vocab, allocata da generate quando serve */
    uint64_t rng;
    int thinking_budget;    /* reasoning tokens before forced close; 0 disables */
    int think_close_token;
    int thinking_open;
    int thinking_forced;
    ThinkingBudgetTransition thinking_transition;
    atomic_int *cancel_flag; /* cooperative server cancellation; NULL in CLI */
} GenOptions;
typedef struct {
    int prompt;
    int generated;
    int model_stopped;
    int repetition_stopped;
    int cancelled;
    int thinking_forced;
    int session_save_requested;
    int session_save_failed;
    int session_save_skipped;
    double prefill_s, decode_s, total_s;
} GenStats;

static uint64_t rng_next(uint64_t *s) {
    uint64_t x = *s ? *s : 0x9e3779b97f4a7c15ULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
    return x * 2685821657736338717ULL;
}

static float penalized(const GenOptions *o, int id, float v) {
    if (!o) return v;
    if (o->presence_penalty > 0.f && o->seen && (o->seen[id >> 3] & (1u << (id & 7))))
        v -= o->presence_penalty;
    if (o->last_token_penalty > 0.f && id == o->last_token)
        v -= o->last_token_penalty;
    return v;
}

static void mark_seen(GenOptions *o, int id) {
    if (!o) return;
    if (o->seen) o->seen[id >> 3] |= (uint8_t)(1u << (id & 7));
    o->last_token = id;
}

static int choose_token(const float *logit, int vocab, GenOptions *o) {
    if (!o || !o->sample || o->temperature <= 0.f) {
        int best = 0; float bv = penalized(o, 0, logit[0]);
        for (int i = 1; i < vocab; i++) {
            float v = penalized(o, i, logit[i]);
            if (v > bv) { bv = v; best = i; }
        }
        mark_seen(o, best);
        return best;
    }
    int k=o->top_k; if(k<1) k=vocab; if(k>vocab) k=vocab; if(k>256) k=256;
    int ids[256], n=0; float vals[256];
    for(int id=0;id<vocab;id++) {
        float v=penalized(o,id,logit[id]); if(!isfinite(v)) continue;
        if(n==k && v<=vals[n-1]) continue;
        int p=n<k?n:n-1; if(n<k)n++;
        while(p>0 && v>vals[p-1]) { if(p<k){vals[p]=vals[p-1];ids[p]=ids[p-1];} p--; }
        vals[p]=v; ids[p]=id;
    }
    if(!n) return 0;
    float weights[256], total=0.f, inv=1.f/o->temperature;
    for(int i=0;i<n;i++){ weights[i]=expf((vals[i]-vals[0])*inv); total+=weights[i]; }
    float target=(o->top_p>0.f&&o->top_p<1.f)?o->top_p*total:total, cum=0.f;
    int keep=0; do { cum+=weights[keep++]; } while(keep<n && cum<target);
    double u=(double)(rng_next(&o->rng)>>11)*(1.0/9007199254740992.0)*cum, acc=0;
    int chosen=ids[keep-1];
    for(int i=0;i<keep;i++){ acc+=weights[i]; if(u<acc){ chosen=ids[i]; break; } }
    mark_seen(o, chosen);
    return chosen;
}

static int choose_controlled_token(const float *logit, int vocab, GenOptions *o,
                                   int generated) {
    int token;
    if (o && thinking_budget_next(&o->thinking_transition, o->thinking_open,
                                  o->thinking_budget, generated, &token)) {
        mark_seen(o, token);
        if (!o->thinking_forced)
            fprintf(stderr, "[decode] thinking budget %d reached; appending "
                    "Qwen early-stop transition\n", o->thinking_budget);
        o->thinking_forced = 1;
        if (token == o->think_close_token) o->thinking_open = 0;
        return token;
    }
    token = choose_token(logit, vocab, o);
    if (o && o->thinking_open && token == o->think_close_token)
        o->thinking_open = 0;
    return token;
}

static void adjust_threads(Model *m, int step_index) {
#if defined(_OPENMP)
    // 1. If OMP_NUM_THREADS is explicitly set, never override it.
    if (getenv("OMP_NUM_THREADS")) {
        return;
    }
    // 2. Only adapt if SAMOSA_FAST=1 is explicitly requested.
    const char *fast_env = getenv("SAMOSA_FAST");
    if (!fast_env || strcmp(fast_env, "1") != 0) {
        return;
    }

    // Static variables to track the dynamic state
    static int initialized = 0;
    static int current_threads = 0;
    static int cool_default = 0;
    static int max_threads = 0;
    static int consecutive_green = 0;
    static int consecutive_critical = 0;
    static int in_container = -1;

    if (!initialized) {
        // Detect container status
        in_container = 0;
        if (access("/.dockerenv", F_OK) == 0) {
            in_container = 1;
        } else {
            FILE *f_cg = fopen("/proc/self/cgroup", "r");
            if (f_cg) {
                char line[256];
                while (fgets(line, sizeof(line), f_cg)) {
                    if (strstr(line, "docker") || strstr(line, "containerd")) {
                        in_container = 1;
                        break;
                    }
                }
                fclose(f_cg);
            }
        }

        // Determine thread boundaries
        int physical_cores = 1;
#ifdef __APPLE__
        int pcores = 0; size_t pl = sizeof(pcores);
        if (!sysctlbyname("hw.perflevel0.physicalcpu", &pcores, &pl, NULL, 0) && pcores > 0) {
            physical_cores = pcores;
        } else {
            physical_cores = 4; // conservative default P-cores for Apple Silicon
        }
#else
        int logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
        int smt = 0;
        FILE *f_smt = fopen("/sys/devices/system/cpu/smt/active", "r");
        if (f_smt) {
            char status[16] = {0};
            if (fscanf(f_smt, "%15s", status) == 1) {
                if (strcmp(status, "1") == 0 || strcmp(status, "active") == 0) {
                    smt = 1;
                }
            }
            fclose(f_smt);
        } else {
            FILE *f_sib = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "r");
            if (f_sib) {
                char sib[64] = {0};
                if (fgets(sib, sizeof(sib), f_sib)) {
                    if (strchr(sib, ',') || strchr(sib, '-')) smt = 1;
                }
                fclose(f_sib);
            } else {
#if defined(__x86_64__)
                smt = 1;
#else
                smt = 0;
#endif
            }
        }
        physical_cores = smt ? (logical / 2) : logical;
#endif
        if (physical_cores < 1) physical_cores = 1;

        // Check cgroup cpu limit to avoid exceeding quota
        int cgroup_threads = 0;
        FILE *f_cpu = fopen("/sys/fs/cgroup/cpu.max", "r");
        if (f_cpu) {
            char quota_str[64];
            long long quota = -1, period = -1;
            if (fscanf(f_cpu, "%59s %lld", quota_str, &period) == 2) {
                if (strcmp(quota_str, "max") != 0) {
                    quota = atoll(quota_str);
                    if (quota > 0 && period > 0) {
                        cgroup_threads = (int)((quota + period - 1) / period);
                    }
                }
            }
            fclose(f_cpu);
        }

        cool_default = physical_cores / 2;
        if (cool_default < 1) cool_default = 1;
        max_threads = physical_cores;
        if (cgroup_threads > 0 && cgroup_threads < max_threads) {
            max_threads = cgroup_threads;
            if (cool_default > max_threads) cool_default = max_threads;
        }

        if (in_container) {
            /* Container: the guest cannot see the host's thermal sensors, so the
             * adaptive loop has no signal to close on.  Stay at the cool default
             * rather than pinning every core: Windows/Linux delivery is Docker
             * (see docs/TASKS_WINDOWS.md), so this path routinely runs on thin
             * laptops where "all cores, no feedback" is exactly the case --fast
             * is supposed to protect against.  OMP_NUM_THREADS remains the
             * explicit override for anyone who wants max on a cooled machine. */
            current_threads = cool_default;
            omp_set_num_threads(current_threads);
            fprintf(stderr, "[threads] --fast: no thermal visibility in a container; "
                            "holding the cool default (threads=%d, max=%d). "
                            "Set OMP_NUM_THREADS=%d to override on a well-cooled machine.\n",
                    current_threads, max_threads, max_threads);
            fflush(stderr);
        } else {
            // Start at the cool default
            current_threads = cool_default;
            omp_set_num_threads(current_threads);
            fprintf(stderr, "[threads] adaptive thermal control initialized: cool=%d max=%d\n", cool_default, max_threads);
            fflush(stderr);
        }
        initialized = 1;
    }

    // In container, we do not adapt
    if (in_container) {
        return;
    }

    // Only sample/adapt every 4 tokens
    if (step_index <= 0 || step_index % 4 != 0) {
        return;
    }

    // Obtain thermal status
    // 0 = normal (green), 1 = warning/fair, >= 2 = serious/critical
    int thermal_level = 0; 
#ifdef __APPLE__
    int token;
    uint64_t notify_state = 0;
    if (notify_register_check("com.apple.system.thermalpressurelevel", &token) == NOTIFY_STATUS_OK) {
        notify_get_state(token, &notify_state);
        thermal_level = (int)notify_state;
    }
#else
    // Linux thermal zone temp in millidegrees C
    int max_temp = 0;
    for (int zone = 0; zone < 10; zone++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
        FILE *f_t = fopen(path, "r");
        if (f_t) {
            int temp = 0;
            if (fscanf(f_t, "%d", &temp) == 1) {
                if (temp > max_temp) max_temp = temp;
            }
            fclose(f_t);
        }
    }
    /* UNVALIDATED THRESHOLDS.  85 C / 75 C and the 4-sample / 16-token cadence
     * below are reasoned defaults, not measured ones: E-H3 (the threads ->
     * sustained tok/s -> thermal-pressure curve, docs/TASKS_HARDWARE.md) has not
     * been run, so nothing here is tuned against real hardware.  They are
     * deliberately conservative — the controller can only move between
     * cool_default and max_threads, so a wrong threshold costs throughput, never
     * safety.  Do not present this loop as tuned until E-H3 exists, and re-run it
     * after H2 (SIMD dispatch): 7.6x less CPU time per token will move the curve. */
    if (max_temp > 0) {
        if (max_temp >= 85000) thermal_level = 2;       /* serious */
        else if (max_temp >= 75000) thermal_level = 1;  /* fair */
        else thermal_level = 0;                         /* normal */
    }
#endif

    // Adaptation logic
    if (thermal_level >= 2) {
        consecutive_green = 0;
        consecutive_critical++;
        if (consecutive_critical >= 1) { // Immediate back-off
            if (current_threads > cool_default) {
                current_threads--;
                omp_set_num_threads(current_threads);
                fprintf(stderr, "[threads] thermal pressure detected (%d); backing off to %d threads\n", thermal_level, current_threads);
                fflush(stderr);
            }
            consecutive_critical = 0;
        }
    } else if (thermal_level == 0) {
        consecutive_critical = 0;
        consecutive_green++;
        if (consecutive_green >= 4) { // Requires 16 tokens of normal temperature to scale up
            if (current_threads < max_threads) {
                current_threads++;
                omp_set_num_threads(current_threads);
                fprintf(stderr, "[threads] thermal normal; ramping up to %d threads\n", current_threads);
                fflush(stderr);
            }
            consecutive_green = 0;
        }
    } else {
        // Warning/fair level: keep current thread count stable
        consecutive_green = 0;
        consecutive_critical = 0;
    }
#endif
}

static void generate(Model *m, const int *prompt, int np, int n_new, int *out,
                     TokenSink sink, void *sink_ctx, GenOptions *options,
                     GenStats *stats) {
    Cfg *c = &m->c;
    m->max_t = np + n_new;
    
    m->K = calloc(c->n_layers, sizeof(float*));
    m->V = calloc(c->n_layers, sizeof(float*));
    for (int i = 0; i < c->n_layers; i++) {
        if (c->layer_type[i] == 1) {
            m->K[i] = falloc((int64_t)c->n_kv_heads * m->max_t * c->head_dim);
            m->V[i] = falloc((int64_t)c->n_kv_heads * m->max_t * c->head_dim);
        }
    }
    m->conv_state = calloc(c->n_layers, sizeof(float*));
    m->recurrent_state = calloc(c->n_layers, sizeof(float*));
    for (int i = 0; i < c->n_layers; i++) {
        if (c->layer_type[i] == 0) {
            int conv_dim = c->linear_key_head_dim * c->linear_num_key_heads * 2 + c->linear_value_head_dim * c->linear_num_value_heads;
            m->conv_state[i] = calloc(conv_dim * 3, sizeof(float));
            m->recurrent_state[i] = calloc(c->linear_num_value_heads * c->linear_key_head_dim * c->linear_value_head_dim, sizeof(float));
        }
    }
    
    for (int i = 0; i < np; i++) out[i] = prompt[i];
    if (options && options->presence_penalty > 0.f && !options->seen)
        options->seen = calloc(((size_t)c->vocab + 7) / 8, 1);
    double gen_t0=now_s();
    float *logit = step(m, prompt, np, 0);
    double prefill_done=now_s();
    int len = np;
    int generated=0;
    int model_stopped=0;
    int repetition_stopped=0;
    int cancelled=0;
    
    for (int s = 0; s < n_new; s++) {
        adjust_threads(m, s);
        if (options && options->cancel_flag &&
            atomic_load_explicit(options->cancel_flag, memory_order_relaxed)) {
            cancelled=1;
            break;
        }
        int best = choose_controlled_token(logit, c->vocab, options, generated);
        free(logit);
        out[len++] = best;
        generated++;
        int sink_result = sink ? sink(best, sink_ctx) : 0;
        if (sink_result) {
            if (sink_result == 1) model_stopped=1;
            else cancelled=1;
            break;
        }
        int repeated_period = sink ? repeated_tail_period(out + np, generated) : 0;
        if (repeated_period) {
            fprintf(stderr,"[decode] stopped repeated token cycle period=%d repeats=16\n",
                    repeated_period);
            repetition_stopped=1;
            break;
        }
        if (s == n_new - 1) break;
        int one = best;
        logit = step(m, &one, 1, len - 1);
    }
    if(stats) {
        double done=now_s();
        stats->generated=generated;
        stats->model_stopped=model_stopped;
        stats->repetition_stopped=repetition_stopped;
        stats->cancelled=cancelled;
        stats->thinking_forced=options ? options->thinking_forced : 0;
        stats->prefill_s=prefill_done-gen_t0;
        stats->decode_s=done-prefill_done;
        stats->total_s=done-gen_t0;
    }
}

/* ---------- T4.4 sessioni warm-resume (QWSESS01) ----------
 *
 * Lo stato DeltaNet e' FISSO (~63 MB) e il KV copre solo i 10 layer GQA
 * (~40 KB/token), quindi un'intera conversazione riprende da uno snapshot di
 * ~70–100 MB indipendentemente dalla lunghezza.  Semantica dei token: dopo
 * generate() l'ULTIMO token scelto non e' ancora passato per step(), quindi
 * lo snapshot registra len token ma len-1 righe KV; la ripresa esegue step
 * sull'ultimo token come primo elemento del chunk di continuazione, il che
 * rende la continuazione bit-identica a una sessione mai interrotta.
 *
 * Formato (little-endian, chiuso da SHA-256 dell'intero prefisso):
 *   magic "QWSESS01" | u32 geometria[9] | u32 len | u32 kv_rows |
 *   u8 layer_type[n_layers] | i32 tokens[len] |
 *   per layer GQA:      f32 K[G][kv_rows][hd], f32 V[G][kv_rows][hd]
 *   per layer DeltaNet: f32 conv[conv_dim*3], f32 rec[vh*kh*vhd]
 *   | u8 sha256[32]
 */
#define SESSION_MAGIC "QWSESS01"
#define SAMOSA_MAX_CONTEXT_TOKENS 24576

static void session_write(FILE *f, RefineSha256 *sha, const void *data, size_t bytes) {
    refine_sha256_update(sha, data, bytes);
    if (fwrite(data, 1, bytes, f) != bytes) {
        fprintf(stderr, "session: write failed (%s)\n", strerror(errno));
        exit(1);
    }
}

static void session_geometry(const Cfg *c, uint32_t g[9]) {
    g[0]=(uint32_t)c->hidden; g[1]=(uint32_t)c->n_layers;
    g[2]=(uint32_t)c->n_kv_heads; g[3]=(uint32_t)c->head_dim;
    g[4]=(uint32_t)c->vocab; g[5]=(uint32_t)c->linear_key_head_dim;
    g[6]=(uint32_t)c->linear_num_key_heads;
    g[7]=(uint32_t)c->linear_num_value_heads;
    g[8]=(uint32_t)c->linear_value_head_dim;
}

static int context_fits(int existing,int extra,int generated){
    if(existing<0||extra<0||generated<0)return 0;
    if(existing>SAMOSA_MAX_CONTEXT_TOKENS||
       extra>SAMOSA_MAX_CONTEXT_TOKENS-existing)return 0;
    return generated<=SAMOSA_MAX_CONTEXT_TOKENS-existing-extra;
}

/* Cheap preflight before KV allocation. session_resume still performs the
 * complete SHA-256 verification before trusting the saved state. */
static int session_peek(Model *m,const char *path,int *len_out,int *last_out){
    FILE *file=fopen(path,"rb");if(!file)return 0;
    char magic[8];uint32_t geometry[9],expected[9],len=0,kv_rows=0;int ok=1;
    session_geometry(&m->c,expected);
    ok=fread(magic,1,8,file)==8&&!memcmp(magic,SESSION_MAGIC,8);
    ok=ok&&fread(geometry,1,sizeof(geometry),file)==sizeof(geometry)&&
       !memcmp(geometry,expected,sizeof(geometry));
    ok=ok&&fread(&len,1,4,file)==4&&fread(&kv_rows,1,4,file)==4&&
       len>=2&&len<=SAMOSA_MAX_CONTEXT_TOKENS&&kv_rows==len-1;
    for(int i=0;ok&&i<m->c.n_layers;i++){
        uint8_t type=0;ok=fread(&type,1,1,file)==1&&type==(uint8_t)m->c.layer_type[i];
    }
    long token_start=ok?ftell(file):-1;int last=-1;
    if(token_start<0||fseek(file,token_start+(long)(len-1)*4,SEEK_SET)||
       fread(&last,1,4,file)!=4)ok=0;
    fclose(file);if(!ok)return 0;
    *len_out=(int)len;*last_out=last;return 1;
}

static int session_save(Model *m, const int *tokens, int len, const char *path) {
    Cfg *c = &m->c;
    if (len < 2) { fprintf(stderr, "session: nothing to save (len=%d)\n", len); return 1; }
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "session: cannot create %s (%s)\n", tmp, strerror(errno)); return 1; }
    RefineSha256 sha; refine_sha256_init(&sha);
    session_write(f, &sha, SESSION_MAGIC, 8);
    uint32_t geo[9]; session_geometry(c, geo);
    session_write(f, &sha, geo, sizeof(geo));
    uint32_t len_u = (uint32_t)len, kv_rows = (uint32_t)(len - 1);
    session_write(f, &sha, &len_u, 4);
    session_write(f, &sha, &kv_rows, 4);
    for (int i = 0; i < c->n_layers; i++) {
        uint8_t t = (uint8_t)c->layer_type[i];
        session_write(f, &sha, &t, 1);
    }
    session_write(f, &sha, tokens, (size_t)len * sizeof(int));
    int G = c->n_kv_heads, hd = c->head_dim;
    for (int i = 0; i < c->n_layers; i++) {
        if (c->layer_type[i] == 1) {
            for (int g = 0; g < G; g++) {
                session_write(f, &sha, m->K[i] + ((int64_t)g * m->max_t) * hd,
                              (size_t)kv_rows * hd * sizeof(float));
            }
            for (int g = 0; g < G; g++) {
                session_write(f, &sha, m->V[i] + ((int64_t)g * m->max_t) * hd,
                              (size_t)kv_rows * hd * sizeof(float));
            }
        } else {
            int conv_dim = c->linear_key_head_dim * c->linear_num_key_heads * 2 +
                           c->linear_value_head_dim * c->linear_num_value_heads;
            session_write(f, &sha, m->conv_state[i], (size_t)conv_dim * 3 * sizeof(float));
            session_write(f, &sha, m->recurrent_state[i],
                          (size_t)c->linear_num_value_heads * c->linear_key_head_dim *
                          c->linear_value_head_dim * sizeof(float));
        }
    }
    uint8_t digest[32]; refine_sha256_final(&sha, digest);
    if (fwrite(digest, 1, 32, f) != 32 || fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) {
        fprintf(stderr, "session: flush/fsync failed (%s)\n", strerror(errno));
        remove(tmp); return 1;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "session: rename failed (%s)\n", strerror(errno));
        remove(tmp); return 1;
    }
    struct stat st;
    double mb = (stat(path, &st) == 0) ? st.st_size / 1e6 : 0;
    fprintf(stderr, "[session] saved %s: %d tokens, %.1f MB\n", path, len, mb);
    return 0;
}

static void session_die(const char *path, const char *why) {
    fprintf(stderr, "session: %s is invalid: %s\n", path, why);
    exit(1);
}

/* Vero se il pezzo decodificato termina una frase: .!? dopo aver ignorato
 * spazi, a-capo e chiusure tipo )]"'*` in coda. */
static int piece_ends_sentence(const char *piece, int n) {
    while (n > 0) {
        char c = piece[n-1];
        if (c==' '||c=='\t'||c=='\n'||c=='\r'||c==')'||c==']'||c=='"'||c=='\''||c=='*'||c=='`') n--;
        else break;
    }
    return n > 0 && (piece[n-1]=='.' || piece[n-1]=='!' || piece[n-1]=='?');
}

/* Un turno annullato non deve persistere un troncamento a metà frase nella
 * storia durevole: il modello imita le risposte tronche e i turni successivi
 * degenerano fino a fermarsi dopo pochi token. Teniamo il prefisso fino
 * all'ultima frase completa fuori da un blocco <think> aperto; 0 = nessuna
 * frase completa, non sovrascrivere lo snapshot precedente. Lo stato
 * ricorrente DeltaNet resta quello corrente: residuo semantico innocuo dei
 * token scartati, mentre token e righe KV sono un prefisso esatto. */
static int cancelled_turn_trim(Tok *tok, const int *transcript, int answer_start,
                               int final_len, int think_open, int think_close) {
    int last_open = -1, last_close = -1;
    for (int i = answer_start; i < final_len; i++) {
        if (transcript[i] == think_open) last_open = i;
        else if (transcript[i] == think_close) last_close = i;
    }
    if (last_open > last_close) final_len = last_open;
    int from = last_close >= answer_start ? last_close + 1 : answer_start;
    int boundary = -1;
    for (int i = from; i < final_len; i++) {
        int id = transcript[i]; char piece[512];
        int n = tok_decode(tok, &id, 1, piece, (int)sizeof(piece) - 1);
        if (n > 0 && piece_ends_sentence(piece, n)) boundary = i;
    }
    return boundary >= 0 ? boundary + 1 : 0;
}

static void session_read(FILE *f, RefineSha256 *sha, void *out, size_t bytes,
                         const char *path) {
    if (fread(out, 1, bytes, f) != bytes) session_die(path, "file troncato");
    refine_sha256_update(sha, out, bytes);
}

/* Carica lo snapshot e alloca gli stati per altre `reserve` posizioni oltre
 * quelle salvate.  Restituisce il transcript salvato in un buffer con
 * capacita' len+reserve (malloc, caller-owned), pronto per l'append. */
static int *session_resume(Model *m, const char *path, int reserve, int *len_out) {
    Cfg *c = &m->c;
    if (reserve < 1) { fprintf(stderr, "session: reserve must be positive\n"); exit(1); }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "session: cannot open %s (%s)\n", path, strerror(errno)); exit(1); }
    RefineSha256 sha; refine_sha256_init(&sha);
    char magic[8];
    session_read(f, &sha, magic, 8, path);
    if (memcmp(magic, SESSION_MAGIC, 8) != 0) session_die(path, "magic errato");
    uint32_t geo[9], expect[9]; session_geometry(c, expect);
    session_read(f, &sha, geo, sizeof(geo), path);
    if (memcmp(geo, expect, sizeof(geo)) != 0)
        session_die(path, "geometria diversa dal modello caricato");
    uint32_t len_u = 0, kv_rows = 0;
    session_read(f, &sha, &len_u, 4, path);
    session_read(f, &sha, &kv_rows, 4, path);
    if (len_u < 2 || kv_rows != len_u - 1) session_die(path, "contatori incoerenti");
    if(!context_fits((int)len_u,0,reserve))
        session_die(path,"limite contesto 24576 token superato");
    for (int i = 0; i < c->n_layers; i++) {
        uint8_t t = 0; session_read(f, &sha, &t, 1, path);
        if (t != (uint8_t)c->layer_type[i]) session_die(path, "layer_type incoerente");
    }
    int *tokens = malloc(((size_t)len_u + (size_t)reserve) * sizeof(int));
    if (!tokens) { fprintf(stderr, "OOM session tokens\n"); exit(1); }
    session_read(f, &sha, tokens, (size_t)len_u * sizeof(int), path);

    m->max_t = (int)len_u + reserve;
    m->K = calloc(c->n_layers, sizeof(float*));
    m->V = calloc(c->n_layers, sizeof(float*));
    m->conv_state = calloc(c->n_layers, sizeof(float*));
    m->recurrent_state = calloc(c->n_layers, sizeof(float*));
    int G = c->n_kv_heads, hd = c->head_dim;
    for (int i = 0; i < c->n_layers; i++) {
        if (c->layer_type[i] == 1) {
            m->K[i] = falloc((int64_t)G * m->max_t * hd);
            m->V[i] = falloc((int64_t)G * m->max_t * hd);
            memset(m->K[i], 0, (size_t)G * m->max_t * hd * sizeof(float));
            memset(m->V[i], 0, (size_t)G * m->max_t * hd * sizeof(float));
            for (int g = 0; g < G; g++)
                session_read(f, &sha, m->K[i] + ((int64_t)g * m->max_t) * hd,
                             (size_t)kv_rows * hd * sizeof(float), path);
            for (int g = 0; g < G; g++)
                session_read(f, &sha, m->V[i] + ((int64_t)g * m->max_t) * hd,
                             (size_t)kv_rows * hd * sizeof(float), path);
        } else {
            int conv_dim = c->linear_key_head_dim * c->linear_num_key_heads * 2 +
                           c->linear_value_head_dim * c->linear_num_value_heads;
            m->conv_state[i] = calloc((size_t)conv_dim * 3, sizeof(float));
            m->recurrent_state[i] = calloc((size_t)c->linear_num_value_heads *
                c->linear_key_head_dim * c->linear_value_head_dim, sizeof(float));
            session_read(f, &sha, m->conv_state[i], (size_t)conv_dim * 3 * sizeof(float), path);
            session_read(f, &sha, m->recurrent_state[i],
                         (size_t)c->linear_num_value_heads * c->linear_key_head_dim *
                         c->linear_value_head_dim * sizeof(float), path);
        }
    }
    uint8_t expect_digest[32], digest[32];
    refine_sha256_final(&sha, expect_digest);
    if (fread(digest, 1, 32, f) != 32) session_die(path, "trailer mancante");
    if (memcmp(digest, expect_digest, 32) != 0) session_die(path, "SHA-256 non corrisponde");
    fclose(f);
    fprintf(stderr, "[session] resumed %s: %d tokens, %u KV rows\n", path, (int)len_u, kv_rows);
    *len_out = (int)len_u;
    return tokens;
}

/* Continua una sessione ripresa: il chunk di continuazione inizia con
 * l'ultimo token salvato (non ancora "steppato") seguito dagli eventuali
 * token del nuovo turno; pos_base = len-1.  Equivale bit-a-bit alla stessa
 * sequenza in una sessione mai interrotta. */
static void generate_continue(Model *m, int *hist, int hist_len,
                              const int *extra, int n_extra, int n_new, int *out,
                              TokenSink sink, void *sink_ctx, GenOptions *options,
                              GenStats *stats) {
    Cfg *c = &m->c;
    if (options && options->presence_penalty > 0.f && !options->seen)
        options->seen = calloc(((size_t)c->vocab + 7) / 8, 1);
    int n_cont = 1 + n_extra;
    int *cont = malloc((size_t)n_cont * sizeof(int));
    cont[0] = hist[hist_len - 1];
    for (int i = 0; i < n_extra; i++) {
        cont[1 + i] = extra[i];
        hist[hist_len + i] = extra[i]; /* il transcript completo serve al re-save */
    }
    double t0 = now_s();
    float *logit = step(m, cont, n_cont, hist_len - 1);
    double prefill_done = now_s();
    free(cont);
    int len = hist_len + n_extra;
    int generated = 0;
    int model_stopped = 0;
    int repetition_stopped = 0;
    int cancelled = 0;
    for (int s = 0; s < n_new; s++) {
        adjust_threads(m, s);
        if (options && options->cancel_flag &&
            atomic_load_explicit(options->cancel_flag, memory_order_relaxed)) {
            cancelled=1;
            break;
        }
        int best = choose_controlled_token(logit, c->vocab, options, generated);
        free(logit);
        out[generated++] = best;
        hist[len++] = best; /* il chiamante garantisce la capacita' */
        int sink_result = sink ? sink(best, sink_ctx) : 0;
        if (sink_result) {
            if (sink_result == 1) model_stopped=1;
            else cancelled=1;
            break;
        }
        int repeated_period = sink ? repeated_tail_period(out, generated) : 0;
        if (repeated_period) {
            fprintf(stderr,"[decode] stopped repeated token cycle period=%d repeats=16\n",
                    repeated_period);
            repetition_stopped=1;
            break;
        }
        if (s == n_new - 1) break;
        int one = best;
        logit = step(m, &one, 1, len - 1);
    }
    if (stats) {
        double done = now_s();
        stats->generated = generated;
        stats->model_stopped = model_stopped;
        stats->repetition_stopped = repetition_stopped;
        stats->cancelled = cancelled;
        stats->thinking_forced = options ? options->thinking_forced : 0;
        stats->prefill_s = prefill_done - t0;
        stats->decode_s = done - prefill_done;
        stats->total_s = done - t0;
    }
}

static void teacher_die(const char *format, ...) {
    va_list args;
    fprintf(stderr,"teacher metrics: ");
    va_start(args,format); vfprintf(stderr,format,args); va_end(args);
    fputc('\n',stderr); exit(2);
}

/* Independent sequence state for corpus teacher-forcing.  Expert cache
 * entries intentionally survive between sequences; attention and DeltaNet
 * state must not. */
static void teacher_state_begin(Model *m, int max_t) {
    Cfg *c=&m->c;
    if (max_t<1) teacher_die("sequence has no usable positions");
    m->max_t=max_t;
    m->K=calloc((size_t)c->n_layers,sizeof(float*));
    m->V=calloc((size_t)c->n_layers,sizeof(float*));
    m->conv_state=calloc((size_t)c->n_layers,sizeof(float*));
    m->recurrent_state=calloc((size_t)c->n_layers,sizeof(float*));
    if (!m->K || !m->V || !m->conv_state || !m->recurrent_state)
        teacher_die("OOM allocating sequence state");
    for (int i=0;i<c->n_layers;++i) {
        if (c->layer_type[i]==1) {
            m->K[i]=calloc((size_t)c->n_kv_heads*(size_t)max_t*(size_t)c->head_dim,
                           sizeof(float));
            m->V[i]=calloc((size_t)c->n_kv_heads*(size_t)max_t*(size_t)c->head_dim,
                           sizeof(float));
            if (!m->K[i] || !m->V[i]) teacher_die("OOM allocating GQA state");
        } else {
            size_t conv_dim=(size_t)c->linear_key_head_dim*(size_t)c->linear_num_key_heads*2u+
                            (size_t)c->linear_value_head_dim*(size_t)c->linear_num_value_heads;
            size_t recurrent=(size_t)c->linear_num_value_heads*
                             (size_t)c->linear_key_head_dim*
                             (size_t)c->linear_value_head_dim;
            m->conv_state[i]=calloc(conv_dim*3u,sizeof(float));
            m->recurrent_state[i]=calloc(recurrent,sizeof(float));
            if (!m->conv_state[i] || !m->recurrent_state[i])
                teacher_die("OOM allocating DeltaNet state");
        }
    }
}

static void teacher_state_end(Model *m) {
    for (int i=0;i<m->c.n_layers;++i) {
        free(m->K?m->K[i]:NULL); free(m->V?m->V[i]:NULL);
        free(m->conv_state?m->conv_state[i]:NULL);
        free(m->recurrent_state?m->recurrent_state[i]:NULL);
    }
    free(m->K); free(m->V); free(m->conv_state); free(m->recurrent_state);
    m->K=m->V=m->conv_state=m->recurrent_state=NULL;
    m->max_t=0;
}

static int *read_int_array(jval *o, const char *key, int *n_out) {
    jval *a = json_get(o, key);
    int *r = malloc(a->len * sizeof(int));
    for (int i = 0; i < a->len; i++) r[i] = (int)a->kids[i]->num;
    *n_out = a->len; return r;
}

/* Text-only subset of Qwen3.6's published chat template.  Vision, tools and
 * prior assistant/tool turns belong in the server layer; keeping this small
 * entry point exact for the common one-turn prompt makes the Stage-B binary
 * usable without a Python tokenizer. */
static char *qwen_chat_prompt(const char *user, const char *system, int no_thinking) {
    const char *prefix = "<|im_start|>user\n";
    const char *middle_think = "<|im_end|>\n<|im_start|>assistant\n<think>\n";
    /* Official tokenizer chat_template for enable_thinking=false. */
    const char *middle_direct = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    const char *middle = no_thinking ? middle_direct : middle_think;
    const char *sprefix = "<|im_start|>system\n";
    const char *send = "<|im_end|>\n";
    size_t n = strlen(prefix) + strlen(user) + strlen(middle) + 1;
    if (system && *system) n += strlen(sprefix) + strlen(system) + strlen(send);
    char *out = malloc(n);
    if (system && *system)
        snprintf(out, n, "%s%s%s%s%s%s%s", sprefix, system, send, prefix, user, middle, "");
    else
        snprintf(out, n, "%s%s%s", prefix, user, middle);
    return out;
}

/* ---------- corpus-bound teacher metric stream (QWTFM001) ---------- */
typedef struct { int *ids, count; } TeacherSequence;

static void teacher_put_u16(uint8_t *p, uint16_t value) {
    p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8);
}
static void teacher_put_u32(uint8_t *p, uint32_t value) {
    p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8);
    p[2]=(uint8_t)(value>>16); p[3]=(uint8_t)(value>>24);
}
static void teacher_put_u64(uint8_t *p, uint64_t value) {
    for (unsigned i=0;i<8;++i) p[i]=(uint8_t)(value>>(8u*i));
}
static void teacher_put_f64(uint8_t *p, double value) {
    uint64_t bits; memcpy(&bits,&value,sizeof(bits)); teacher_put_u64(p,bits);
}
static void teacher_write(FILE *file, RefineSha256 *digest,
                          const void *payload, size_t bytes) {
    if (bytes && fwrite(payload,1,bytes,file)!=bytes) teacher_die("output write failed");
    refine_sha256_update(digest,payload,bytes);
}

static void teacher_lse_top5(const float *logits, int vocab, double *lse,
                             uint32_t top5[5]) {
    float maximum=-INFINITY, values[5]={-INFINITY,-INFINITY,-INFINITY,-INFINITY,-INFINITY};
    int ids[5]={-1,-1,-1,-1,-1};
    for (int token=0;token<vocab;++token) {
        float value=logits[token];
        if (!isfinite(value)) teacher_die("model emitted a non-finite logit");
        if (value>maximum) maximum=value;
        int position=5;
        for (int rank=0;rank<5;++rank) {
            if (value>values[rank] ||
                (value==values[rank] && (ids[rank]<0 || token<ids[rank]))) {
                position=rank; break;
            }
        }
        if (position<5) {
            for (int rank=4;rank>position;--rank) {
                values[rank]=values[rank-1]; ids[rank]=ids[rank-1];
            }
            values[position]=value; ids[position]=token;
        }
    }
    double total=0.0;
    for (int token=0;token<vocab;++token) total+=exp((double)logits[token]-maximum);
    *lse=(double)maximum+log(total);
    if (!isfinite(*lse)) teacher_die("logsumexp is non-finite");
    for (int rank=0;rank<5;++rank) top5[rank]=(uint32_t)ids[rank];
}

static int teacher_is_calibration(uint64_t ordinal, uint64_t positions,
                                  uint32_t calibration) {
    for (uint32_t index=0;index<calibration;++index) {
        uint64_t factor=(uint64_t)index*2u+1u;
        if (positions>UINT64_MAX/factor)
            teacher_die("calibration ordinal calculation overflow");
        uint64_t selected=(factor*positions)/(2u*(uint64_t)calibration);
        if (selected==ordinal) return 1;
        if (selected>ordinal) return 0;
    }
    return 0;
}

static void teacher_fsync_parent(const char *path) {
    char parent[2048];
    if (strlen(path)>=sizeof(parent)) teacher_die("output path is too long");
    strcpy(parent,path); char *slash=strrchr(parent,'/');
    if (!slash) strcpy(parent,".");
    else if (slash==parent) slash[1]=0;
    else *slash=0;
    int fd=open(parent,O_RDONLY);
    if (fd<0 || fsync(fd)) teacher_die("cannot fsync output directory: %s",strerror(errno));
    close(fd);
}

static int run_teacher_capture(Model *m, const char *tokenizer_path,
                               const char *corpus_path, const char *output_path,
                               int requested_calibration) {
    if (!tokenizer_path || !corpus_path || !output_path)
        teacher_die("tokenizer, corpus, and output paths are required");
    FILE *corpus=fopen(corpus_path,"rb");
    if (!corpus) teacher_die("cannot open corpus '%s': %s",corpus_path,strerror(errno));
    if (fseek(corpus,0,SEEK_END)) teacher_die("cannot seek corpus");
    long corpus_length=ftell(corpus);
    if (corpus_length<0 || fseek(corpus,0,SEEK_SET)) teacher_die("cannot size corpus");
    char *corpus_bytes=malloc((size_t)corpus_length+1u);
    if (!corpus_bytes) teacher_die("OOM reading corpus");
    if (fread(corpus_bytes,1,(size_t)corpus_length,corpus)!=(size_t)corpus_length)
        teacher_die("short read of corpus");
    fclose(corpus); corpus_bytes[corpus_length]=0;
    uint8_t corpus_sha[32]; refine_sha256(corpus_bytes,(size_t)corpus_length,corpus_sha);
    char *arena=NULL; jval *root=json_parse(corpus_bytes,&arena);
    if (!root || root->t!=J_OBJ) teacher_die("corpus root must be an object");
    jval *schema=json_get(root,"schema_version"), *prompts=json_get(root,"prompts");
    if (!schema || schema->t!=J_NUM || schema->num!=1 ||
        !prompts || prompts->t!=J_ARR || prompts->len<1)
        teacher_die("corpus must contain schema_version=1 and a non-empty prompts array");
    if ((uint64_t)prompts->len>UINT32_MAX) teacher_die("too many corpus sequences");
    int sequence_count=prompts->len;

    Tok tokenizer; tok_load(&tokenizer,tokenizer_path);
    TeacherSequence *sequences=calloc((size_t)sequence_count,sizeof(*sequences));
    if (!sequences) teacher_die("OOM allocating corpus sequences");
    uint64_t positions=0;
    for (int sequence=0;sequence<sequence_count;++sequence) {
        jval *item=prompts->kids[sequence];
        if (!item || item->t!=J_OBJ) teacher_die("corpus prompt %d is not an object",sequence);
        jval *prompt=json_get(item,"prompt"), *direct=json_get(item,"no_thinking");
        if (!prompt || prompt->t!=J_STR || (direct && direct->t!=J_BOOL))
            teacher_die("corpus prompt %d has invalid prompt/no_thinking fields",sequence);
        char *templated=qwen_chat_prompt(prompt->str,NULL,direct?direct->boolean:0);
        size_t text_bytes=strlen(templated);
        if (text_bytes>(size_t)INT_MAX-32u) teacher_die("corpus prompt %d is too large",sequence);
        int capacity=(int)text_bytes+32;
        sequences[sequence].ids=malloc((size_t)capacity*sizeof(int));
        if (!sequences[sequence].ids) teacher_die("OOM tokenizing corpus");
        sequences[sequence].count=tok_encode(&tokenizer,templated,(int)text_bytes,
                                              sequences[sequence].ids,capacity);
        free(templated);
        if (sequences[sequence].count<2)
            teacher_die("corpus prompt %d tokenized to fewer than two tokens",sequence);
        uint64_t add=(uint64_t)sequences[sequence].count-1u;
        if (positions>UINT64_MAX-add) teacher_die("teacher position count overflow");
        positions+=add;
    }
    json_tree_free(root); free(arena); free(corpus_bytes);
    if (requested_calibration<1 || requested_calibration>128)
        teacher_die("calibration count must be in 1..128");
    uint32_t calibration=(uint32_t)requested_calibration;
    if ((uint64_t)calibration>positions) calibration=(uint32_t)positions;

    if (!strcmp(output_path,"-")) teacher_die("binary teacher output requires a file path");
    if (!access(output_path,F_OK)) teacher_die("output already exists (refusing to overwrite)");
    char temporary[2304];
    if (snprintf(temporary,sizeof(temporary),"%s.tmp.%ld",output_path,(long)getpid())
        >=(int)sizeof(temporary)) teacher_die("output path is too long");
    int output_fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (output_fd<0) teacher_die("cannot create output temporary: %s",strerror(errno));
    FILE *output=fdopen(output_fd,"wb");
    if (!output) teacher_die("cannot open output stream: %s",strerror(errno));
    RefineSha256 stream_sha; refine_sha256_init(&stream_sha);
    uint8_t header[80]={0}; memcpy(header,"QWTFM001",8);
    teacher_put_u16(header+8,1); teacher_put_u16(header+10,0);
    teacher_put_u32(header+12,80); memcpy(header+16,corpus_sha,32);
    teacher_put_u32(header+48,(uint32_t)m->c.vocab);
    teacher_put_u32(header+52,(uint32_t)sequence_count);
    teacher_put_u64(header+56,positions); teacher_put_u32(header+64,calibration);
    teacher_put_u32(header+68,1); teacher_put_u32(header+72,56);
    teacher_write(output,&stream_sha,header,sizeof(header));

    size_t logits_bytes=(size_t)m->c.vocab*4u;
    uint8_t *encoded_logits=malloc(logits_bytes);
    if (!encoded_logits) teacher_die("OOM allocating bounded logit encoder");
    uint64_t ordinal=0; double started=now_s();
    for (int sequence=0;sequence<sequence_count;++sequence) {
        TeacherSequence *current=&sequences[sequence];
        teacher_state_begin(m,current->count);
        for (int position=0;position<current->count-1;++position,++ordinal) {
            float *logits=step(m,&current->ids[position],1,position);
            int target=current->ids[position+1]; double lse; uint32_t top5[5];
            teacher_lse_top5(logits,m->c.vocab,&lse,top5);
            int full=teacher_is_calibration(ordinal,positions,calibration);
            uint8_t record[56]={0};
            teacher_put_u32(record,(uint32_t)sequence);
            teacher_put_u32(record+4,(uint32_t)position);
            teacher_put_u32(record+8,(uint32_t)target);
            teacher_put_u32(record+12,full?1u:0u);
            teacher_put_u32(record+16,full?(uint32_t)m->c.vocab:0u);
            teacher_put_f64(record+20,(double)logits[target]);
            teacher_put_f64(record+28,lse);
            for (int rank=0;rank<5;++rank) teacher_put_u32(record+36+4*rank,top5[rank]);
            teacher_write(output,&stream_sha,record,sizeof(record));
            if (full) {
                for (int token=0;token<m->c.vocab;++token) {
                    uint32_t bits; memcpy(&bits,&logits[token],sizeof(bits));
                    teacher_put_u32(encoded_logits+4*(size_t)token,bits);
                }
                teacher_write(output,&stream_sha,encoded_logits,logits_bytes);
            }
            free(logits);
        }
        teacher_state_end(m);
        fprintf(stderr,"[teacher] sequence=%d/%d positions=%llu/%llu\n",
                sequence+1,sequence_count,(unsigned long long)ordinal,
                (unsigned long long)positions);
    }
    free(encoded_logits);
    for (int sequence=0;sequence<sequence_count;++sequence) free(sequences[sequence].ids);
    free(sequences);
    if (ordinal!=positions) teacher_die("internal teacher position mismatch");
    uint8_t digest[32]; refine_sha256_final(&stream_sha,digest);
    uint8_t trailer[48]={0}; memcpy(trailer,"QWTFEND1",8);
    teacher_put_u64(trailer+8,positions); memcpy(trailer+16,digest,32);
    if (fwrite(trailer,1,sizeof(trailer),output)!=sizeof(trailer) || fflush(output) || fsync(output_fd))
        teacher_die("cannot durably finish teacher stream");
    if (fclose(output)) teacher_die("cannot close teacher stream");
    if (link(temporary,output_path)) teacher_die("cannot publish output: %s",strerror(errno));
    if (unlink(temporary)) teacher_die("cannot remove output temporary: %s",strerror(errno));
    teacher_fsync_parent(output_path);
    fprintf(stderr,"[teacher] wrote=%s sequences=%d positions=%llu calibration=%u "
                   "elapsed=%.3fs\n",output_path,sequence_count,
            (unsigned long long)positions,calibration,now_s()-started);
    return 0;
}

typedef struct { Tok *tok; int eos, eot, stream; } ChatSink;
static int chat_token_sink(int id, void *opaque) {
    ChatSink *s = opaque;
    if (s->stream && id != s->eos && id != s->eot) {
        char piece[4096];
        int n = tok_decode(s->tok, &id, 1, piece, (int)sizeof(piece) - 1);
        if (n) fwrite(piece, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    return id == s->eos || id == s->eot;
}

static void load_generation_options(const char *snap, GenOptions *o) {
    *o=(GenOptions){.sample=1,.top_k=20,.temperature=1.f,.top_p=.95f,.last_token=-1,
                    .rng=(uint64_t)time(NULL)^(uint64_t)getpid()};
    char path[2048]; snprintf(path,sizeof(path),"%s/generation_config.json",snap);
    FILE *f=fopen(path,"rb"); if(!f)return;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=malloc(n+1); if(fread(buf,1,n,f)!=(size_t)n){} buf[n]=0; fclose(f);
    char *arena=NULL; jval *r=json_parse(buf,&arena), *v;
    if((v=json_get(r,"do_sample")) && v->t==J_BOOL)o->sample=v->boolean;
    if((v=json_get(r,"top_k")))o->top_k=(int)v->num;
    if((v=json_get(r,"top_p")))o->top_p=(float)v->num;
    if((v=json_get(r,"temperature")))o->temperature=(float)v->num;
    free(buf); free(arena);
}

/* Testo di continuazione per un nuovo turno utente su una sessione ripresa.
 * Se il transcript salvato non termina con <|im_end|> (interruzione per
 * budget token), il turno assistant precedente va chiuso nel testo. */
static char *qwen_chat_continuation(const char *user, int no_thinking,
                                    int ended_with_im_end) {
    const char *close_turn = ended_with_im_end ? "\n" : "<|im_end|>\n";
    const char *prefix = "<|im_start|>user\n";
    const char *middle_think = "<|im_end|>\n<|im_start|>assistant\n<think>\n";
    const char *middle_direct = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    const char *middle = no_thinking ? middle_direct : middle_think;
    size_t n = strlen(close_turn) + strlen(prefix) + strlen(user) + strlen(middle) + 1;
    char *out = malloc(n);
    snprintf(out, n, "%s%s%s%s", close_turn, prefix, user, middle);
    return out;
}

static int run_chat(Model *m, const char *tokenizer_path, const char *user,
                    const char *system, int n_new, int no_thinking, int stream,
                    GenOptions *options, const char *save_session,
                    const char *resume_session, int resume_decode,
                    Tok *shared_tokenizer,
                    TokenSink output_sink, void *output_ctx,
                    GenStats *stats_out) {
    Tok local_tokenizer;
    Tok *tok=shared_tokenizer;
    if(!tok){ tok=&local_tokenizer; tok_load(tok,tokenizer_path); }
    ChatSink cli_sink = { .tok=tok, .eos=tok_id_of(tok, "<|im_end|>"),
                          .eot=tok_id_of(tok, "<|endoftext|>"), .stream=stream };
    TokenSink active_sink = output_sink ? output_sink : chat_token_sink;
    void *active_ctx = output_sink ? output_ctx : &cli_sink;
    if (!no_thinking) {
        options->think_close_token = tok_id_of(tok, "</think>");
        if (options->think_close_token < 0) {
            fprintf(stderr, "tokenizer: </think> token missing\n");
            return 1;
        }
        options->thinking_open = 1;
        options->thinking_forced = 0;
        options->thinking_transition.position = 0;
        options->thinking_transition.count = tok_encode(
            tok, QWEN_THINKING_EARLY_STOP_TEXT,
            (int)strlen(QWEN_THINKING_EARLY_STOP_TEXT),
            options->thinking_transition.tokens,
            THINKING_EARLY_STOP_MAX_TOKENS);
        if (options->thinking_transition.count <= 0 ||
            options->thinking_transition.count >= THINKING_EARLY_STOP_MAX_TOKENS) {
            fprintf(stderr, "tokenizer: invalid Qwen thinking early-stop transition\n");
            return 1;
        }
        int transition_has_close = 0;
        for (int i = 0; i < options->thinking_transition.count; ++i)
            if (options->thinking_transition.tokens[i] == options->think_close_token)
                transition_has_close = 1;
        if (!transition_has_close) {
            fprintf(stderr, "tokenizer: Qwen early-stop transition lacks </think>\n");
            return 1;
        }
        /* Exact saved-state continuation must not inject a token absent from
         * the original run. A new user turn still receives the normal cap. */
        if (resume_decode > 0) options->thinking_budget = 0;
    }
    GenStats stats={0};
    int *transcript = NULL; int final_len = 0; int np = 0;

    if (resume_session) {
        if (resume_decode > 0) n_new = resume_decode;
        int *extra = NULL; int n_extra = 0;
        char *cont_text = NULL;
        int hlen_hint=0,last_hint=-1;
        if(!session_peek(m,resume_session,&hlen_hint,&last_hint)){
            fprintf(stderr,"session: invalid header: %s\n",resume_session);
            return 1;
        }
        if (resume_decode > 0) {
            /* modalita' di validazione: continua la decodifica esatta */
        } else {
            if (!user) { fprintf(stderr, "--resume-session requires --chat or --resume-decode\n"); return 1; }
            int ended = (last_hint == cli_sink.eos);
            cont_text = qwen_chat_continuation(user, no_thinking, ended);
            int cap = (int)strlen(cont_text) + 32;
            extra = malloc((size_t)cap * sizeof(int));
            n_extra = tok_encode(tok, cont_text, (int)strlen(cont_text), extra, cap);
            if(n_extra<=0){fprintf(stderr,"session: continuation produced no tokens\n");
                free(extra);free(cont_text);return 1;}
        }
        if(!context_fits(hlen_hint,n_extra,n_new)){
            fprintf(stderr,"session: this turn would exceed the %d-token total context limit\n",
                    SAMOSA_MAX_CONTEXT_TOKENS);
            free(extra);free(cont_text);return 1;
        }
        int hlen=0;
        int *hist=session_resume(m,resume_session,n_extra+n_new,&hlen);
        if (!output_sink) {
            printf("%s", "\n");
            fflush(stdout);
        }
        int *gen_out = malloc((size_t)n_new * sizeof(int));
        generate_continue(m, hist, hlen, extra, n_extra, n_new, gen_out,
                          active_sink, active_ctx, options, &stats);
        if (!stream && !output_sink) {
            for (int i = 0; i < stats.generated; i++) {
                int id = gen_out[i];
                if (id == cli_sink.eos || id == cli_sink.eot) break;
                char piece[4096];
                int n = tok_decode(tok, &id, 1, piece, (int)sizeof(piece) - 1);
                if (n) fwrite(piece, 1, (size_t)n, stdout);
            }
        }
        transcript = hist;
        np = hlen + n_extra;
        final_len = hlen + n_extra + stats.generated;
        free(gen_out); free(extra); free(cont_text);
    } else {
        char *text = qwen_chat_prompt(user, system, no_thinking);
        int cap = (int)strlen(text) + 32;
        int *prompt = malloc((size_t)cap * sizeof(int));
        np = tok_encode(tok, text, (int)strlen(text), prompt, cap);
        if (!np) { fprintf(stderr, "The prompt produced no tokens\n"); free(text); free(prompt); return 1; }
        if(!context_fits(0,np,n_new)){
            fprintf(stderr,"This turn would exceed the %d-token total context limit\n",
                    SAMOSA_MAX_CONTEXT_TOKENS);
            free(text);free(prompt);return 1;
        }
        int *out = malloc((size_t)(np + n_new) * sizeof(int));
        if (!output_sink) {
            printf("%s", "\n");
            fflush(stdout);
        }
        generate(m, prompt, np, n_new, out, active_sink, active_ctx, options, &stats);
        if (!stream && !output_sink) {
            for (int i = np; i < np + n_new; i++) {
                int id = out[i];
                if (id == cli_sink.eos || id == cli_sink.eot) break;
                char piece[4096];
                int n = tok_decode(tok, &id, 1, piece, (int)sizeof(piece) - 1);
                if (n) fwrite(piece, 1, (size_t)n, stdout);
            }
        }
        transcript = out;
        final_len = np + stats.generated;
        free(text); free(prompt);
    }
    if (!output_sink) {
        putchar('\n');
        if (stats.cancelled) {
            puts("[Samosa stopped this generation on request.]");
        } else if (stats.repetition_stopped) {
            puts("[Samosa stopped generation after detecting a repeated token cycle.]");
        } else if (!stats.model_stopped) {
            printf("[Samosa reached the %d-token limit; this answer may be incomplete. "
                   "Use --max-tokens to raise the ceiling.]\n", n_new);
        }
    }
    stats.session_save_requested = save_session != NULL;
    int save_len = final_len;
    if (save_session && stats.cancelled) {
        save_len = cancelled_turn_trim(tok, transcript, np, final_len,
                                       tok_id_of(tok, "<think>"),
                                       tok_id_of(tok, "</think>"));
        if (!save_len) {
            stats.session_save_skipped = 1;
            fprintf(stderr, "[session] cancelled turn had no complete sentence: "
                            "kept the previous snapshot\n");
        } else if (save_len < final_len) {
            fprintf(stderr, "[session] cancelled turn: saved %d/%d tokens "
                            "(up to the last complete sentence)\n",
                    save_len - np, final_len - np);
        }
    }
    if (save_session && !stats.session_save_skipped)
        stats.session_save_failed = session_save(m, transcript, save_len, save_session) != 0;
    stats.prompt=np;
    double hit_rate=(m->hits+m->miss)?(double)m->hits/(double)(m->hits+m->miss):0.0;
    double decode_tps=(stats.generated>1 && stats.decode_s>0)
        ? (double)(stats.generated-1)/stats.decode_s : 0.0;
    fprintf(stderr,"[stats] prompt=%d generated=%d stop=%s thinking=%s prefill=%.3fs (%.2f tok/s) "
        "decode=%.3fs (%.2f tok/s) total=%.3fs expert_hit=%llu/%llu (%.1f%%) "
        "expert_disk=%.3fs expert_mm=%.3fs peak_rss=%.2f GB\n",
        np,stats.generated,stats.cancelled?"cancelled":
                           stats.repetition_stopped?"repetition-guard":
                           stats.model_stopped?"model":"limit",
        stats.thinking_forced?"forced-close":"model-controlled",
        stats.prefill_s,stats.prefill_s>0?np/stats.prefill_s:0.0,
        stats.decode_s,decode_tps,stats.total_s,
        (unsigned long long)m->hits,(unsigned long long)(m->hits+m->miss),100.0*hit_rate,
        m->t_edisk,m->t_emm,peak_rss_gb());
    {
        ecache_stats est; ecache_get_stats(m->ec,&est);
        fprintf(stderr,"[ecache] budget=%.2f GB payload=%.2f GB peak=%.2f GB "
            "entries=%u evictions=%llu bytes_read=%.2f GB bytes_avoided=%.2f GB "
            "failed_admissions=%llu pressure_warn=%llu pressure_critical=%llu\n",
            (double)est.budget_bytes/1e9,(double)est.payload_bytes/1e9,
            (double)est.peak_payload_bytes/1e9,est.entries,
            (unsigned long long)est.evictions,
            (double)est.base_bytes_read/1e9,(double)est.base_bytes_avoided/1e9,
            (unsigned long long)est.failed_admissions,
            (unsigned long long)est.pressure_warn_events,
            (unsigned long long)est.pressure_critical_events);
    }
    if (m->seq_reads)
        fprintf(stderr,"[seqio] layer_reads=%llu bytes=%.2f GB\n",
                (unsigned long long)m->seq_reads,(double)m->seq_bytes/1e9);
    free(transcript);
    if (stats_out) *stats_out=stats;
    if (options) {
        free(options->seen);
        options->seen=NULL;
    }
    teacher_state_end(m);
#ifdef __APPLE__
    /* Multi-eviction admissions can leave surplus reusable expert slabs even
     * though the live cache payload is flat. Scratch already owns up to 64
     * reusable miss slabs, so discard the auxiliary surplus between turns,
     * then ask Darwin to return those and other free KV/scratch pages. */
    uint64_t pool_released=eslot_pool_trim(m,0);
    size_t relieved=malloc_zone_pressure_relief(NULL,0);
    if(pool_released>=(1u<<20)||relieved>=(1u<<20))
        fprintf(stderr,"[memory] freed_pool=%.1f MB allocator_relief=%.1f MB\n",
                (double)pool_released/(1024.0*1024.0),
                (double)relieved/(1024.0*1024.0));
#else
    uint64_t pool_released=eslot_pool_trim(m,0);
    int relieved = 0;
#ifdef __GLIBC__
    relieved = malloc_trim(0);
#endif
    if(pool_released>=(1u<<20)||relieved)
        fprintf(stderr,"[memory] freed_pool=%.1f MB allocator_relief=%s\n",
                (double)pool_released/(1024.0*1024.0),
                relieved ? "trimmed" : "no-op");
#endif
    if(!shared_tokenizer)tok_free(tok);
    refine_report(m);
    return route_close();
}

/* ---------- A0: resident localhost app server ---------- */
typedef struct {
    char *data;
    size_t length, capacity;
} ServeBuffer;

static int serve_buffer_append(ServeBuffer *buffer, const char *data, size_t length) {
    if(buffer->length+length+1<buffer->length)return 0;
    size_t need=buffer->length+length+1;
    if(need>buffer->capacity){
        size_t capacity=buffer->capacity?buffer->capacity:1024;
        while(capacity<need){ if(capacity>SIZE_MAX/2)return 0; capacity*=2; }
        char *next=realloc(buffer->data,capacity); if(!next)return 0;
        buffer->data=next; buffer->capacity=capacity;
    }
    memcpy(buffer->data+buffer->length,data,length); buffer->length+=length;
    buffer->data[buffer->length]=0; return 1;
}

static int serve_json_escape(ServeBuffer *out, const char *text, size_t length) {
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<length;i++){
        unsigned char c=(unsigned char)text[i];
        if(c=='"'||c=='\\'){ char pair[2]={'\\',(char)c}; if(!serve_buffer_append(out,pair,2))return 0; }
        else if(c=='\n'){ if(!serve_buffer_append(out,"\\n",2))return 0; }
        else if(c=='\r'){ if(!serve_buffer_append(out,"\\r",2))return 0; }
        else if(c=='\t'){ if(!serve_buffer_append(out,"\\t",2))return 0; }
        else if(c<0x20){ char encoded[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};
            if(!serve_buffer_append(out,encoded,6))return 0; }
        else if(!serve_buffer_append(out,(const char *)&text[i],1))return 0;
    }
    return 1;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t next_ticket, serving_ticket;
    int active, waiting, max_waiting, stopping;
} ServeScheduler;

static void serve_scheduler_init(ServeScheduler *scheduler,int max_waiting){
    memset(scheduler,0,sizeof(*scheduler)); scheduler->max_waiting=max_waiting;
    pthread_mutex_init(&scheduler->mu,NULL); pthread_cond_init(&scheduler->cv,NULL);
}

/* 1 admitted, 0 queue full, -1 shutting down. */
static int serve_scheduler_acquire(ServeScheduler *scheduler,double *wait_s){
    double started=now_s(); pthread_mutex_lock(&scheduler->mu);
    if(scheduler->stopping){pthread_mutex_unlock(&scheduler->mu);return -1;}
    if((scheduler->active||scheduler->waiting) && scheduler->waiting>=scheduler->max_waiting){
        pthread_mutex_unlock(&scheduler->mu); return 0;
    }
    uint64_t ticket=scheduler->next_ticket++; scheduler->waiting++;
    while(!scheduler->stopping &&
          (scheduler->active || ticket!=scheduler->serving_ticket))
        pthread_cond_wait(&scheduler->cv,&scheduler->mu);
    scheduler->waiting--;
    if(scheduler->stopping){pthread_mutex_unlock(&scheduler->mu);return -1;}
    scheduler->active=1; scheduler->serving_ticket++;
    pthread_mutex_unlock(&scheduler->mu); if(wait_s)*wait_s=now_s()-started; return 1;
}

static void serve_scheduler_release(ServeScheduler *scheduler){
    pthread_mutex_lock(&scheduler->mu); scheduler->active=0;
    pthread_cond_broadcast(&scheduler->cv); pthread_mutex_unlock(&scheduler->mu);
}

typedef struct {
    Model *model;
    Tok tokenizer;
    const char *tokenizer_path;
    const char *snapshot;
    char chats_dir[PATH_MAX];
    char app_html_path[PATH_MAX];
    char app_logo_path[PATH_MAX];
    ServeScheduler scheduler;
    atomic_int cancel;
    pthread_mutex_t stats_mu;
    GenStats last_stats;
    int has_last_stats;
    double started;
    int port;
} SamosaServeContext;

static int serve_static_file(int fd,const char *path,const char *content_type,
                             const char *extra){
    if(!path||!*path)return 0;
    FILE *file=fopen(path,"rb");if(!file)return 0;
    if(fseek(file,0,SEEK_END)){fclose(file);return 0;}
    long end=ftell(file);if(end<0||end>(1<<20)){fclose(file);return 0;}
    rewind(file);size_t length=(size_t)end;unsigned char *data=malloc(length?length:1);
    if(!data){fclose(file);return 0;}
    int ok=fread(data,1,length,file)==length&&!ferror(file);fclose(file);
    if(ok)ok=samosa_http_headers(fd,200,content_type,length,extra)&&
             (!length||samosa_send_all(fd,data,length));
    free(data);return ok;
}

typedef struct {
    int fd, stream, thinking_open, close_token, eos_token, eot_token;
    Tok *tokenizer;
    atomic_int *cancel;
    ServeBuffer reasoning, content;
} ServeTokenSink;

static int serve_sse_piece(ServeTokenSink *sink,const char *field,
                           const char *piece,size_t length){
    ServeBuffer event={0};
    const char *prefix="data: {\"object\":\"chat.completion.chunk\","
        "\"choices\":[{\"index\":0,\"delta\":{\"";
    const char *suffix="\"},\"finish_reason\":null}]}\n\n";
    int ok=serve_buffer_append(&event,prefix,strlen(prefix)) &&
        serve_buffer_append(&event,field,strlen(field)) &&
        serve_buffer_append(&event,"\":\"",3) &&
        serve_json_escape(&event,piece,length) &&
        serve_buffer_append(&event,suffix,strlen(suffix));
    if(ok)ok=samosa_send_all(sink->fd,event.data,event.length);
    free(event.data); return ok;
}

static int serve_token_sink(int token,void *opaque){
    ServeTokenSink *sink=(ServeTokenSink *)opaque;
    if(atomic_load_explicit(sink->cancel,memory_order_relaxed))return 2;
    if(token==sink->eos_token||token==sink->eot_token)return 1;
    if(token==sink->close_token){sink->thinking_open=0;return 0;}
    char piece[4096]; int n=tok_decode(sink->tokenizer,&token,1,piece,sizeof(piece)-1);
    if(n<=0)return 1; /* end-of-turn/end-of-text special token */
    ServeBuffer *target=sink->thinking_open?&sink->reasoning:&sink->content;
    if(!serve_buffer_append(target,piece,(size_t)n)){atomic_store(sink->cancel,1);return 2;}
    if(sink->stream && !serve_sse_piece(sink,sink->thinking_open?"reasoning":"content",
                                        piece,(size_t)n)){
        atomic_store(sink->cancel,1);return 2;
    }
    return 0;
}

static int serve_valid_id(const char *id){
    if(!id||!*id||strlen(id)>64)return 0;
    for(const unsigned char *p=(const unsigned char *)id;*p;p++)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||
             (*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return 0;
    return 1;
}

static int serve_mkdir(const char *path){
    char copy[PATH_MAX]; if(strlen(path)>=sizeof(copy))return 0;
    strcpy(copy,path);
    for(char *p=copy+1;*p;p++)if(*p=='/'){*p=0;if(mkdir(copy,0700)&&errno!=EEXIST)return 0;*p='/';}
    return !mkdir(copy,0700)||errno==EEXIST;
}

static jval *serve_json_field(jval *object,const char *key,jtype type){
    jval *value=json_get(object,key); return value&&value->t==type?value:NULL;
}

static const char *serve_last_user(jval *root,const char **system){
    jval *messages=serve_json_field(root,"messages",J_ARR); const char *user=NULL; *system=NULL;
    if(!messages)return NULL;
    for(int i=0;i<messages->len;i++){
        jval *message=messages->kids[i];
        jval *role=serve_json_field(message,"role",J_STR);
        jval *content=serve_json_field(message,"content",J_STR);
        if(!role||!content)continue;
        if(!strcmp(role->str,"system") && !*system)*system=content->str;
        else if(!strcmp(role->str,"user"))user=content->str;
    }
    return user;
}

/* Validate the exact tokenized turn before queueing, allocating KV, or
 * sending stream headers.  Returns 1 when it fits, 0 when it exceeds the
 * product cap, and -1 for an unreadable/incompatible saved session. */
static int serve_context_preflight(SamosaServeContext *ctx,const char *user,
                                   const char *system,int no_thinking,
                                   const char *resume_session,int generated){
    int existing=0,last=-1;
    char *text=NULL;
    if(resume_session){
        if(!session_peek(ctx->model,resume_session,&existing,&last))return -1;
        int im_end=tok_id_of(&ctx->tokenizer,"<|im_end|>");
        text=qwen_chat_continuation(user,no_thinking,last==im_end);
    }else text=qwen_chat_prompt(user,system,no_thinking);
    if(!text)return -1;
    size_t bytes=strlen(text);
    if(bytes>(size_t)INT_MAX-32u){free(text);return 0;}
    int capacity=(int)bytes+32;
    int *tokens=malloc((size_t)capacity*sizeof(int));
    if(!tokens){free(text);return -1;}
    int extra=tok_encode(&ctx->tokenizer,text,(int)bytes,tokens,capacity);
    free(tokens);free(text);
    if(extra<=0)return -1;
    return context_fits(existing,extra,generated)?1:0;
}

static int serve_finish_reason(const GenStats *stats,const char **closure){
    if(stats->cancelled){*closure="cancelled";return 4;}
    if(stats->repetition_stopped){*closure="repetition";return 3;}
    if(stats->thinking_forced)*closure="budget_transition";else *closure="natural";
    return stats->model_stopped?1:2; /* stop / length */
}

static int serve_send_nonstream(int fd,ServeTokenSink *sink,const GenStats *stats){
    const char *closure; int finish=serve_finish_reason(stats,&closure);
    const char *reason=finish==1?"stop":finish==2?"length":finish==3?"repetition":"cancelled";
    ServeBuffer body={0}; char prefix[512],suffix[896];
    int n=snprintf(prefix,sizeof(prefix),
        "{\"object\":\"chat.completion\",\"model\":\"qwen3.6-35b-a3b\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"reasoning\":\"");
    int ok=n>0&&serve_buffer_append(&body,prefix,(size_t)n)&&
        serve_json_escape(&body,sink->reasoning.data?sink->reasoning.data:"",sink->reasoning.length)&&
        serve_buffer_append(&body,"\",\"content\":\"",strlen("\",\"content\":\""))&&
        serve_json_escape(&body,sink->content.data?sink->content.data:"",sink->content.length);
    n=snprintf(suffix,sizeof(suffix),
        "\"},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,"
        "\"completion_tokens\":%d,\"total_tokens\":%d},\"samosa\":{"
        "\"thinking_closure\":\"%s\",\"tokens_per_second\":%.2f,"
        "\"rss_gb\":%.2f,\"session_saved\":%s}}",reason,stats->prompt,stats->generated,
        stats->prompt+stats->generated,closure,
        stats->generated>1&&stats->decode_s>0?(stats->generated-1)/stats->decode_s:0,
        rss_gb(),stats->session_save_requested
            ? ((stats->session_save_failed||stats->session_save_skipped)?"false":"true") : "null");
    ok=ok&&n>0&&serve_buffer_append(&body,suffix,(size_t)n)&&
       samosa_http_headers(fd,200,"application/json",body.length,NULL)&&
       samosa_send_all(fd,body.data,body.length);
    free(body.data);return ok;
}

static int serve_send_stream_end(int fd,const GenStats *stats){
    const char *closure; int finish=serve_finish_reason(stats,&closure);
    const char *reason=finish==1?"stop":finish==2?"length":finish==3?"repetition":"cancelled";
    char event[1152]; int n=snprintf(event,sizeof(event),
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,"
        "\"delta\":{},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,"
        "\"completion_tokens\":%d,\"total_tokens\":%d},\"samosa\":{"
        "\"thinking_closure\":\"%s\",\"tokens_per_second\":%.2f,"
        "\"rss_gb\":%.2f,\"session_saved\":%s}}\n\ndata: [DONE]\n\n",reason,stats->prompt,
        stats->generated,stats->prompt+stats->generated,closure,
        stats->generated>1&&stats->decode_s>0?(stats->generated-1)/stats->decode_s:0,
        rss_gb(),stats->session_save_requested
            ? ((stats->session_save_failed||stats->session_save_skipped)?"false":"true") : "null");
    return n>0&&(size_t)n<sizeof(event)&&samosa_send_all(fd,event,(size_t)n);
}

static int samosa_serve_chat(SamosaServeContext *ctx,int fd,jval *root){
    const char *system=NULL,*user=serve_last_user(root,&system);
    if(!user)return samosa_http_json_error(fd,400,"invalid_messages",
        "A text user message is required.");
    jval *stream_value=json_get(root,"stream"); int stream=stream_value&&stream_value->t==J_BOOL&&stream_value->boolean;
    int max_tokens=8192; jval *value=json_get(root,"max_tokens");
    if(!value)value=json_get(root,"max_completion_tokens");
    if(value){if(value->t!=J_NUM||value->num<1||value->num>8192||floor(value->num)!=value->num)
        return samosa_http_json_error(fd,400,"invalid_max_tokens","max_tokens must be an integer in 1..8192.");
        max_tokens=(int)value->num;}
    int no_thinking=0,thinking_code=0;
    value=json_get(root,"thinking");
    if(value&&value->t==J_BOOL)no_thinking=!value->boolean;
    else if(value&&value->t==J_STR){
        if(!strcmp(value->str,"off"))no_thinking=1;
        else if(!strcmp(value->str,"code")||!strcmp(value->str,"precise-code"))thinking_code=1;
        else if(strcmp(value->str,"general"))return samosa_http_json_error(fd,400,
            "invalid_thinking","thinking must be off, general, or code.");
    }
    GenOptions options; load_generation_options(ctx->snapshot,&options);
    if(no_thinking){options.temperature=.7f;options.top_p=.8f;options.presence_penalty=1.5f;}
    else if(thinking_code){options.temperature=.6f;options.top_p=.95f;options.presence_penalty=0;}
    else {options.temperature=1.f;options.top_p=.95f;options.presence_penalty=1.5f;}
    options.top_k=20; options.thinking_budget=no_thinking?0:(thinking_code?2048:1024);
    if((value=json_get(root,"thinking_budget"))){
        if(value->t!=J_NUM||value->num<0||value->num>8192||floor(value->num)!=value->num)
            return samosa_http_json_error(fd,400,"invalid_thinking_budget",
                "thinking_budget must be an integer in 0..8192.");
        options.thinking_budget=(int)value->num;
    }
    if((value=json_get(root,"temperature"))){if(value->t!=J_NUM||value->num<0||value->num>2)
        return samosa_http_json_error(fd,400,"invalid_temperature","temperature must be in 0..2.");
        options.temperature=(float)value->num;options.sample=value->num>0;}
    if((value=json_get(root,"top_p"))){if(value->t!=J_NUM||value->num<=0||value->num>1)
        return samosa_http_json_error(fd,400,"invalid_top_p","top_p must be in (0,1].");options.top_p=(float)value->num;}
    if((value=json_get(root,"top_k"))){if(value->t!=J_NUM||value->num<1||value->num>256||floor(value->num)!=value->num)
        return samosa_http_json_error(fd,400,"invalid_top_k","top_k must be an integer in 1..256.");options.top_k=(int)value->num;}
    if((value=json_get(root,"seed"))){if(value->t!=J_NUM||value->num<0||floor(value->num)!=value->num)
        return samosa_http_json_error(fd,400,"invalid_seed","seed must be a non-negative integer.");options.rng=(uint64_t)value->num;}
    options.cancel_flag=&ctx->cancel;

    char session_path[PATH_MAX]={0}; const char *save_session=NULL,*resume_session=NULL;
    value=serve_json_field(root,"conversation_id",J_STR);
    if(value){
        if(!serve_valid_id(value->str))return samosa_http_json_error(fd,400,
            "invalid_conversation_id","conversation_id must use letters, numbers, dash, or underscore.");
        char directory[PATH_MAX];
        if(snprintf(directory,sizeof(directory),"%s/%s",ctx->chats_dir,value->str)>=(int)sizeof(directory)||
           !serve_mkdir(directory)||
           snprintf(session_path,sizeof(session_path),"%s/session.qws",directory)>=(int)sizeof(session_path))
            return samosa_http_json_error(fd,500,"session_path_failed","Unable to create conversation storage.");
        save_session=session_path; if(!access(session_path,R_OK))resume_session=session_path;
    }

    int context_ok=serve_context_preflight(ctx,user,system,no_thinking,
                                            resume_session,max_tokens);
    if(context_ok<0)return samosa_http_json_error(fd,409,"invalid_session",
        "The saved conversation is invalid or incompatible with this model.");
    if(!context_ok)return samosa_http_json_error(fd,400,"context_limit",
        "This turn could exceed Samosa Chat's 24576-token total context limit. "
        "Start a new chat, shorten the message, or request fewer output tokens.");

    double wait_s=0; int admitted=serve_scheduler_acquire(&ctx->scheduler,&wait_s);
    if(admitted==0)return samosa_http_json_error(fd,429,"queue_full","The inference queue is full.");
    if(admitted<0)return samosa_http_json_error(fd,503,"shutting_down","Samosa is shutting down.");
    /* A queued request for the same conversation may have created or replaced
     * the snapshot while this request waited. Refresh and revalidate while we
     * hold exclusive model admission, before allocation or response headers. */
    if(save_session)resume_session=!access(session_path,R_OK)?session_path:NULL;
    context_ok=serve_context_preflight(ctx,user,system,no_thinking,
                                       resume_session,max_tokens);
    if(context_ok<0){serve_scheduler_release(&ctx->scheduler);
        return samosa_http_json_error(fd,409,"invalid_session",
            "The saved conversation header is invalid or incompatible with this model.");}
    if(!context_ok){serve_scheduler_release(&ctx->scheduler);
        return samosa_http_json_error(fd,400,"context_limit",
            "This turn could exceed Samosa Chat's 24576-token total context limit. "
            "Start a new chat, shorten the message, or request fewer output tokens.");}
    atomic_store(&ctx->cancel,0); g_moe_down_idot=thinking_code?0:g_idot;
    ServeTokenSink sink={.fd=fd,.stream=stream,.thinking_open=!no_thinking,
        .close_token=tok_id_of(&ctx->tokenizer,"</think>"),.tokenizer=&ctx->tokenizer,
        .eos_token=tok_id_of(&ctx->tokenizer,"<|im_end|>"),
        .eot_token=tok_id_of(&ctx->tokenizer,"<|endoftext|>"),.cancel=&ctx->cancel};
    int sent=stream?samosa_http_stream_headers(fd):1;
    GenStats stats={0}; int result=1;
    if(sent)result=run_chat(ctx->model,ctx->tokenizer_path,user,system,max_tokens,
        no_thinking,1,&options,save_session,resume_session,0,&ctx->tokenizer,
        serve_token_sink,&sink,&stats);
    if(sent && !result){ if(stream)serve_send_stream_end(fd,&stats); else serve_send_nonstream(fd,&sink,&stats); }
    pthread_mutex_lock(&ctx->stats_mu);ctx->last_stats=stats;ctx->has_last_stats=1;pthread_mutex_unlock(&ctx->stats_mu);
    free(sink.reasoning.data);free(sink.content.data);serve_scheduler_release(&ctx->scheduler);
    (void)wait_s; return result?0:1;
}

static int samosa_serve_handler(SamosaHttpServer *server,int fd,
                                const SamosaHttpRequest *request,void *opaque){
    SamosaServeContext *ctx=(SamosaServeContext *)opaque;
    if(!strcmp(request->method,"GET")&&!strcmp(request->path,"/")){
        const char *policy="Content-Security-Policy: default-src 'self'; img-src 'self'; "
            "style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; "
            "object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n";
        if(serve_static_file(fd,ctx->app_html_path,"text/html; charset=utf-8",policy))return 1;
        return samosa_http_json_error(fd,500,"app_missing","The local app asset is missing. Reinstall Samosa Chat.");
    }
    if(!strcmp(request->method,"GET")&&!strcmp(request->path,"/assets/samosa-chat.png")){
        if(serve_static_file(fd,ctx->app_logo_path,"image/png",NULL))return 1;
        return samosa_http_json_error(fd,404,"logo_missing","The local app logo is missing.");
    }
    if(!strcmp(request->method,"GET")&&!strcmp(request->path,"/healthz")){
        GenStats stats={0};int has;
        pthread_mutex_lock(&ctx->stats_mu);stats=ctx->last_stats;has=ctx->has_last_stats;pthread_mutex_unlock(&ctx->stats_mu);
        pthread_mutex_lock(&ctx->scheduler.mu);int active=ctx->scheduler.active,queued=ctx->scheduler.waiting;pthread_mutex_unlock(&ctx->scheduler.mu);
        char body[1024];snprintf(body,sizeof(body),
            "{\"status\":\"ok\",\"model\":\"qwen3.6-35b-a3b\",\"rss_gb\":%.2f,"
            "\"context_limit_tokens\":%d,\"uptime_seconds\":%.0f,"
            "\"scheduler\":{\"active\":%s,\"queued\":%d,"
            "\"max_queue\":%d},\"last_generation\":{\"available\":%s,"
            "\"tokens\":%d,\"tokens_per_second\":%.2f}}",rss_gb(),
            SAMOSA_MAX_CONTEXT_TOKENS,now_s()-ctx->started,
            active?"true":"false",queued,ctx->scheduler.max_waiting,has?"true":"false",
            stats.generated,stats.generated>1&&stats.decode_s>0?(stats.generated-1)/stats.decode_s:0);
        return samosa_http_response(fd,200,"application/json",body,NULL);
    }
    if(!strcmp(request->method,"GET")&&!strcmp(request->path,"/v1/models"))
        return samosa_http_response(fd,200,"application/json",
            "{\"object\":\"list\",\"data\":[{\"id\":\"qwen3.6-35b-a3b\","
            "\"object\":\"model\",\"owned_by\":\"samosa\"}]}",NULL);
    if(!strcmp(request->method,"POST")&&!strcmp(request->path,"/v1/cancel")){
        atomic_store(&ctx->cancel,1);
        return samosa_http_response(fd,200,"application/json","{\"cancelled\":true}",NULL);
    }
    if(!strcmp(request->method,"POST")&&!strcmp(request->path,"/v1/shutdown")){
        atomic_store(&ctx->cancel,1);pthread_mutex_lock(&ctx->scheduler.mu);
        ctx->scheduler.stopping=1;pthread_cond_broadcast(&ctx->scheduler.cv);pthread_mutex_unlock(&ctx->scheduler.mu);
        samosa_http_response(fd,200,"application/json","{\"shutting_down\":true}",NULL);
        samosa_http_server_stop(server);return 1;
    }
    if(!strcmp(request->method,"POST")&&!strcmp(request->path,"/v1/chat/completions")){
        char *arena=NULL;jval *root=json_parse(request->body,&arena);
        if(!root||root->t!=J_OBJ){json_free(root);return samosa_http_json_error(fd,400,"invalid_json","A JSON object is required.");}
        int result=samosa_serve_chat(ctx,fd,root);json_free(root);free(arena);return result;
    }
    return samosa_http_json_error(fd,404,"not_found","Endpoint not found.");
}

static SamosaHttpServer *g_signal_server=NULL;
static SamosaServeContext *g_signal_context=NULL;
static void samosa_serve_signal(int signal_number){
    (void)signal_number;if(g_signal_context)atomic_store(&g_signal_context->cancel,1);
    if(g_signal_server)samosa_http_server_stop(g_signal_server);
}

static int run_samosa_serve(Model *model,const char *snapshot,
                            const char *tokenizer_path,int port,int max_queue){
    SamosaServeContext context={.model=model,.tokenizer_path=tokenizer_path,
        .snapshot=snapshot,.started=now_s(),.port=port};
    atomic_init(&context.cancel,0);pthread_mutex_init(&context.stats_mu,NULL);
    serve_scheduler_init(&context.scheduler,max_queue);tok_load(&context.tokenizer,tokenizer_path);
    const char *configured=getenv("SAMOSA_CHATS_DIR");
    if(configured)snprintf(context.chats_dir,sizeof(context.chats_dir),"%s",configured);
    else {const char *home=getenv("HOME");snprintf(context.chats_dir,sizeof(context.chats_dir),
        "%s/.samosa/chats",home?home:".");}
    configured=getenv("SAMOSA_APP_HTML");
    snprintf(context.app_html_path,sizeof(context.app_html_path),"%s",configured?configured:"app.html");
    configured=getenv("SAMOSA_APP_LOGO");
    snprintf(context.app_logo_path,sizeof(context.app_logo_path),"%s",configured?configured:"samosa-chat.png");
    if(!serve_mkdir(context.chats_dir)){fprintf(stderr,"serve: cannot create %s\n",context.chats_dir);return 2;}
    SamosaHttpServer server;
    if(!samosa_http_server_init(&server,port,samosa_serve_handler,&context)){
        fprintf(stderr,"serve: cannot bind 127.0.0.1:%d: %s\n",port,strerror(errno));return 2;}
    g_signal_server=&server;g_signal_context=&context;signal(SIGINT,samosa_serve_signal);signal(SIGTERM,samosa_serve_signal);
    fprintf(stderr,"[serve] ready http://127.0.0.1:%d queue=%d chats=%s\n",
        server.port,max_queue,context.chats_dir);fflush(stderr);
    int ok=samosa_http_server_run(&server);
    samosa_http_server_destroy(&server);tok_free(&context.tokenizer);
    pthread_mutex_destroy(&context.stats_mu);pthread_cond_destroy(&context.scheduler.cv);
    pthread_mutex_destroy(&context.scheduler.mu);g_signal_server=NULL;g_signal_context=NULL;
    return ok?0:2;
}

int main(int argc, char **argv) {
#if defined(_OPENMP)
    if (!getenv("OMP_NUM_THREADS")) {
#ifdef __APPLE__
        /* Default: META' dei P-core (2 su questo M3) — scelta esplicita
         * dell'utente (2026-07-12) per un telaio fanless piu' fresco al tatto:
         * ~7.3 tok/s invece dei ~9.5 a 4 thread.  La pressione termica OS resta
         * a zero in entrambi i casi; e' una preferenza di comfort, non un limite
         * del silicio.  OMP_NUM_THREADS resta l'override esplicito (es. =4 per
         * la velocita' piena). */
        int pcores = 0; size_t pl = sizeof(pcores);
        if (!sysctlbyname("hw.perflevel0.physicalcpu", &pcores, &pl, NULL, 0) && pcores > 0) {
            int cool = pcores / 2; if (cool < 1) cool = 1;
            omp_set_num_threads(cool);
        }
#else
        int threads = 0;
        // 1. Check cgroup CPU quota
        FILE *f_cpu = fopen("/sys/fs/cgroup/cpu.max", "r");
        if (f_cpu) {
            char quota_str[64];
            long long quota = -1, period = -1;
            if (fscanf(f_cpu, "%59s %lld", quota_str, &period) == 2) {
                if (strcmp(quota_str, "max") != 0) {
                    quota = atoll(quota_str);
                    if (quota > 0 && period > 0) {
                        threads = (int)((quota + period - 1) / period);
                    }
                }
            }
            fclose(f_cpu);
        }
        
        if (threads <= 0) {
            // 2. Check P-cores on Intel hybrid Linux
            FILE *f_pcore = fopen("/sys/devices/cpu_core/cpus", "r");
            int pcores = 0;
            if (f_pcore) {
                char range[128];
                if (fgets(range, sizeof(range), f_pcore)) {
                    int count = 0;
                    char *range_copy = strdup(range);
                    if (range_copy) {
                        char *tok = strtok(range_copy, ",");
                        while (tok) {
                            int start, end;
                            if (sscanf(tok, "%d-%d", &start, &end) == 2) {
                                count += (end - start + 1);
                            } else if (sscanf(tok, "%d", &start) == 1) {
                                count += 1;
                            }
                            tok = strtok(NULL, ",");
                        }
                        free(range_copy);
                        if (count > 0) pcores = count / 2;
                    }
                }
                fclose(f_pcore);
            }
            
            if (pcores > 0) {
                threads = pcores / 2;
            } else {
                int logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
                int smt = 0;
                FILE *f_smt = fopen("/sys/devices/system/cpu/smt/active", "r");
                if (f_smt) {
                    char status[16] = {0};
                    if (fscanf(f_smt, "%15s", status) == 1) {
                        if (strcmp(status, "1") == 0 || strcmp(status, "active") == 0) {
                            smt = 1;
                        }
                    }
                    fclose(f_smt);
                } else {
                    FILE *f_sib = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "r");
                    if (f_sib) {
                        char sib[64] = {0};
                        if (fgets(sib, sizeof(sib), f_sib)) {
                            if (strchr(sib, ',') || strchr(sib, '-')) smt = 1;
                        }
                        fclose(f_sib);
                    } else {
#if defined(__x86_64__)
                        smt = 1;
#else
                        smt = 0;
#endif
                    }
                }
                int physical = smt ? (logical / 2) : logical;
                if (physical < 1) physical = 1;
                threads = physical / 2;
            }
        }
        if (threads < 1) threads = 1;
        omp_set_num_threads(threads);
#endif
    }
#endif
    const char *snap = getenv("SNAP");
    const char *refpath = getenv("REF");
    const char *chat = NULL, *system = NULL;
    const char *tokenizer_path = getenv("TOKENIZER");
    const char *teacher_corpus = NULL, *teacher_output = NULL;
    int teacher_calibration = 128;
    int n_chat = 128, no_thinking = 0, thinking_code = 0;
    int serve_mode=0, serve_port=8642, serve_queue=4;
    int stream = 0, bad_option = 0, greedy=0;
    int cli_top_k=-1, cli_thinking_budget=-1;
    float cli_top_p=-1.f, cli_temp=-1.f, cli_presence=-1.f, cli_no_doubling=-1.f;
    uint64_t cli_seed=0;
    const char *cli_save_session = NULL, *cli_resume_session = NULL;
    int cli_resume_decode = 0;
    const char *moe_fixed = getenv("MOE_K"), *moe_mass = getenv("MOE_MASS");
    const char *moe_max_entropy = getenv("MOE_MAX_ENTROPY");
    const char *moe_min_gap = getenv("MOE_MIN_GAP");
    const char *cli_moe_fixed = NULL, *cli_moe_mass = NULL;
    const char *cli_moe_max_entropy = NULL, *cli_moe_min_gap = NULL;
    const char *refine_dir = getenv("REFINE_DIR");
    const char *refine_mode_text = getenv("REFINE_MODE");
    const char *cli_refine_dir = NULL, *cli_refine_mode = NULL;
    const char *refine_full_ranks_text=getenv("REFINE_FULL_RANKS");
    const char *refine_base_projections_text=getenv("REFINE_BASE_PROJECTIONS");
    const char *refine_base_layers_text=getenv("REFINE_BASE_LAYERS");
    const char *cli_refine_full_ranks=NULL, *cli_refine_base_projections=NULL;
    const char *cli_refine_base_layers=NULL;
    int refine_verify=0;
    const char *refine_verify_text=getenv("REFINE_VERIFY");
    if (refine_verify_text &&
        (!parse_int_strict(refine_verify_text,&refine_verify) ||
         (refine_verify!=0 && refine_verify!=1))) {
        fprintf(stderr,"refinable shelves: REFINE_VERIFY must be 0 or 1\n");
        return 2;
    }

    /* Real-model interactive path.  Keep the legacy oracle invocation below
     * unchanged so tiny-fixture regressions stay script-compatible. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chat") && i + 1 < argc) chat = argv[++i];
        else if (!strcmp(argv[i], "--serve")) serve_mode=1;
        else if (!strcmp(argv[i], "--port") && i+1<argc) {
            if(!parse_int_strict(argv[++i],&serve_port)||serve_port<1||serve_port>65535){
                fprintf(stderr,"--port requires an integer in 1..65535\n");bad_option=1;}
        }
        else if (!strcmp(argv[i], "--queue") && i+1<argc) {
            if(!parse_int_strict(argv[++i],&serve_queue)||serve_queue<0||serve_queue>64){
                fprintf(stderr,"--queue requires an integer in 0..64\n");bad_option=1;}
        }
        else if (!strcmp(argv[i], "--system") && i + 1 < argc) system = argv[++i];
        else if ((!strcmp(argv[i], "--tokens") || !strcmp(argv[i], "--max-tokens")) && i + 1 < argc) n_chat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tokenizer") && i + 1 < argc) tokenizer_path = argv[++i];
        else if (!strcmp(argv[i], "--no-thinking")) {
            if (thinking_code) {
                fprintf(stderr,"--no-thinking and --thinking-code are mutually exclusive\n");
                bad_option = 1;
            }
            no_thinking = 1;
        }
        else if (!strcmp(argv[i], "--thinking-code")) {
            if (no_thinking) {
                fprintf(stderr,"--thinking-code and --no-thinking are mutually exclusive\n");
                bad_option = 1;
            }
            thinking_code = 1;
        }
        else if (!strcmp(argv[i], "--stream")) stream = 1;
        else if (!strcmp(argv[i], "--save-session")) {
            if (i + 1 >= argc) { fprintf(stderr, "--save-session requires a path\n"); bad_option = 1; }
            else cli_save_session = argv[++i];
        }
        else if (!strcmp(argv[i], "--resume-session")) {
            if (i + 1 >= argc) { fprintf(stderr, "--resume-session requires a path\n"); bad_option = 1; }
            else cli_resume_session = argv[++i];
        }
        else if (!strcmp(argv[i], "--resume-decode")) {
            if (i + 1 >= argc) { fprintf(stderr, "--resume-decode requires a token count\n"); bad_option = 1; }
            else cli_resume_decode = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--greedy")) greedy = 1;
        else if (!strcmp(argv[i], "--temperature") && i+1<argc) cli_temp=(float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--top-k") && i+1<argc) cli_top_k=atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top-p") && i+1<argc) cli_top_p=(float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i+1<argc) cli_seed=(uint64_t)strtoull(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--presence-penalty") && i+1<argc) cli_presence=(float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-doubling") && i+1<argc) cli_no_doubling=(float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--thinking-budget") && i+1<argc) {
            if (!parse_int_strict(argv[++i], &cli_thinking_budget) || cli_thinking_budget < 0) {
                fprintf(stderr, "--thinking-budget requires a non-negative integer\n");
                bad_option = 1;
            }
        }
        else if (!strcmp(argv[i], "--moe-k")) {
            if (i + 1 >= argc) { fprintf(stderr, "--moe-k requires a value\n"); bad_option = 1; }
            else cli_moe_fixed = argv[++i];
        }
        else if (!strcmp(argv[i], "--moe-mass")) {
            if (i + 1 >= argc) { fprintf(stderr, "--moe-mass requires a value\n"); bad_option = 1; }
            else cli_moe_mass = argv[++i];
        }
        else if (!strcmp(argv[i], "--moe-max-entropy")) {
            if (i + 1 >= argc) { fprintf(stderr, "--moe-max-entropy requires a value\n"); bad_option = 1; }
            else cli_moe_max_entropy = argv[++i];
        }
        else if (!strcmp(argv[i], "--moe-min-gap")) {
            if (i + 1 >= argc) { fprintf(stderr, "--moe-min-gap requires a value\n"); bad_option = 1; }
            else cli_moe_min_gap = argv[++i];
        }
        else if (!strcmp(argv[i], "--refine-dir")) {
            if (i + 1 >= argc) { fprintf(stderr, "--refine-dir requires a value\n"); bad_option = 1; }
            else cli_refine_dir = argv[++i];
        }
        else if (!strcmp(argv[i], "--refine-mode")) {
            if (i + 1 >= argc) { fprintf(stderr, "--refine-mode requires full, base, or mixed\n"); bad_option = 1; }
            else cli_refine_mode = argv[++i];
        }
        else if (!strcmp(argv[i], "--refine-full-ranks")) {
            if (i + 1 >= argc) { fprintf(stderr, "--refine-full-ranks requires a value\n"); bad_option = 1; }
            else cli_refine_full_ranks=argv[++i];
        }
        else if (!strcmp(argv[i], "--refine-base-projections")) {
            if (i + 1 >= argc) { fprintf(stderr, "--refine-base-projections requires a value\n"); bad_option = 1; }
            else cli_refine_base_projections=argv[++i];
        }
        else if (!strcmp(argv[i], "--refine-base-layers")) {
            if (i + 1 >= argc) { fprintf(stderr, "--refine-base-layers requires a value\n"); bad_option = 1; }
            else cli_refine_base_layers=argv[++i];
        }
        else if (!strcmp(argv[i], "--refine-verify")) refine_verify=1;
        else if (!strcmp(argv[i], "--teacher-corpus")) {
            if (i + 1 >= argc) { fprintf(stderr, "--teacher-corpus requires a path\n"); bad_option = 1; }
            else teacher_corpus = argv[++i];
        }
        else if (!strcmp(argv[i], "--teacher-output")) {
            if (i + 1 >= argc) { fprintf(stderr, "--teacher-output requires a path\n"); bad_option = 1; }
            else teacher_output = argv[++i];
        }
        else if (!strcmp(argv[i], "--teacher-calibration")) {
            if (i + 1 >= argc ||
                !parse_int_strict(argv[i + 1], &teacher_calibration)) {
                fprintf(stderr, "--teacher-calibration requires an integer in 1..128\n");
                bad_option = 1;
            } else {
                ++i;
            }
        }
        else if (!strncmp(argv[i], "--", 2)) { fprintf(stderr, "Unknown option: %s\n", argv[i]); bad_option = 1; }
    }
    if (bad_option) return 1;
    if(serve_mode && (chat||cli_resume_session||teacher_corpus)){
        fprintf(stderr,"--serve cannot be combined with chat, resume, or teacher modes\n");return 2;}
    if ((teacher_corpus != NULL) != (teacher_output != NULL)) {
        fprintf(stderr,
            "teacher metrics: --teacher-corpus and --teacher-output must be supplied together\n");
        return 2;
    }
    int teacher_mode = teacher_corpus != NULL;
    if (teacher_mode && chat) {
        fprintf(stderr,"teacher metrics: --chat cannot be combined with teacher capture\n");
        return 2;
    }
    if (teacher_calibration < 1 || teacher_calibration > 128) {
        fprintf(stderr,"teacher metrics: --teacher-calibration must be in 1..128\n");
        return 2;
    }
    /* A command-line policy mode takes precedence over a stale environment
     * mode.  Guards independently use CLI-over-environment precedence. */
    if (cli_moe_fixed || cli_moe_mass) {
        moe_fixed = cli_moe_fixed;
        moe_mass = cli_moe_mass;
    }
    if (cli_moe_max_entropy) moe_max_entropy = cli_moe_max_entropy;
    if (cli_moe_min_gap) moe_min_gap = cli_moe_min_gap;
    if (!moe_policy_configure(moe_fixed, moe_mass, moe_max_entropy, moe_min_gap)) return 2;
    if (cli_refine_dir) refine_dir=cli_refine_dir;
    if (cli_refine_mode) refine_mode_text=cli_refine_mode;
    if (cli_refine_full_ranks) refine_full_ranks_text=cli_refine_full_ranks;
    if (cli_refine_base_projections)
        refine_base_projections_text=cli_refine_base_projections;
    if (cli_refine_base_layers) refine_base_layers_text=cli_refine_base_layers;
    int refine_mode=REFINE_OFF;
    int refine_full_ranks=-1;
    uint8_t refine_base_projections=7;
    if (refine_mode_text && *refine_mode_text) {
        if (!strcmp(refine_mode_text,"full")) refine_mode=REFINE_FULL;
        else if (!strcmp(refine_mode_text,"base")) refine_mode=REFINE_BASE;
        else if (!strcmp(refine_mode_text,"mixed")) refine_mode=REFINE_MIXED;
        else {
            fprintf(stderr,"refinable shelves: invalid mode '%s' (expected full, base, or mixed)\n",
                    refine_mode_text);
            return 2;
        }
    }
    if (refine_mode==REFINE_MIXED) {
        if (!parse_int_strict(refine_full_ranks_text,&refine_full_ranks) ||
            refine_full_ranks<0) {
            fprintf(stderr,
                "refinable shelves: mixed mode requires REFINE_FULL_RANKS or --refine-full-ranks >= 0\n");
            return 2;
        }
        if (refine_base_projections_text &&
            !refine_projection_mask_parse(refine_base_projections_text,
                                          &refine_base_projections)) {
            fprintf(stderr,
                "refinable shelves: base projections must be all or a unique comma list of gate,up,down\n");
            return 2;
        }
    } else if ((refine_full_ranks_text && *refine_full_ranks_text) ||
               (refine_base_projections_text && *refine_base_projections_text) ||
               (refine_base_layers_text && *refine_base_layers_text)) {
        fprintf(stderr,
            "refinable shelves: rank/projection policy options require --refine-mode mixed\n");
        return 2;
    }
    if ((refine_dir && *refine_dir) != (refine_mode!=REFINE_OFF)) {
        fprintf(stderr,
            "refinable shelves: --refine-dir/REFINE_DIR and --refine-mode/REFINE_MODE must be supplied together\n");
        return 2;
    }
    if (!chat && !cli_resume_session && !serve_mode && !teacher_mode && g_moe_policy.mode != MOE_POLICY_OFF) {
        fprintf(stderr,
            "adaptive MoE: policies are forbidden in oracle/validation mode; use --chat for explicit experiments\n");
        return 2;
    }
    if (!chat && !cli_resume_session && !serve_mode && !teacher_mode && refine_mode==REFINE_BASE) {
        fprintf(stderr,
            "refinable shelves: base mode is forbidden in oracle/validation mode; use full mode for exact validation\n");
        return 2;
    }
    if (!chat && !cli_resume_session && !serve_mode && !teacher_mode && refine_mode==REFINE_MIXED) {
        fprintf(stderr,
            "refinable shelves: mixed mode is forbidden in oracle/validation mode; use full mode for exact validation\n");
        return 2;
    }
    if (teacher_mode) {
        if (!snap && argc > 1 && argv[1][0] != '-') snap = argv[1];
        if (!snap) {
            fprintf(stderr,
                "Usage: SNAP=<dir> ./qwen36b --teacher-corpus CORPUS.json "
                "--teacher-output CAPTURE.qtf [--teacher-calibration N] "
                "[--tokenizer file]\n");
            return 1;
        }
        if (!tokenizer_path) tokenizer_path = "tokenizer_qwen36.json";
        fprintf(stderr,
            "[teacher] corpus=%s output=%s tokenizer=%s calibration=%d\n",
            teacher_corpus,teacher_output,tokenizer_path,teacher_calibration);
        if (g_moe_policy.mode != MOE_POLICY_OFF) {
            fprintf(stderr,
                "[moe-policy] EXPERIMENTAL teacher candidate mode=%s k=%d mass=%.9g "
                "max_entropy=%.9g min_gap=%.9g\n",
                moe_policy_name(g_moe_policy.mode),g_moe_policy.fixed_k,
                g_moe_policy.mode==MOE_POLICY_MASS?g_moe_policy.mass:-1.f,
                g_moe_policy.has_max_entropy?g_moe_policy.max_entropy:-1.f,
                g_moe_policy.has_min_gap?g_moe_policy.min_gap:-1.f);
        }
        Model m;
        model_init(&m,snap,refine_dir,refine_mode,refine_verify,
                   refine_full_ranks,refine_base_projections,
                   refine_base_layers_text);
        int result=run_teacher_capture(&m,tokenizer_path,teacher_corpus,
                                       teacher_output,teacher_calibration);
        refine_report(&m);
        int route_result=route_close();
        return result ? result : route_result;
    }
    if(serve_mode){
        if(!snap&&argc>1&&argv[1][0]!='-')snap=argv[1];
        if(!snap){fprintf(stderr,"Usage: SNAP=<dir> ./qwen36b --serve [--port 8642] [--queue 4] [--tokenizer file]\n");return 1;}
        if(!tokenizer_path)tokenizer_path="tokenizer_qwen36.json";
        Model m;model_init(&m,snap,refine_dir,refine_mode,refine_verify,
                           refine_full_ranks,refine_base_projections,
                           refine_base_layers_text);
        return run_samosa_serve(&m,snap,tokenizer_path,serve_port,serve_queue);
    }
    if (chat || cli_resume_session) {
        if (!snap && argc > 1 && argv[1][0] != '-') snap = argv[1];
        if (!snap) { fprintf(stderr, "Usage: SNAP=<dir> ./qwen36b --chat <prompt> [--stream] [--no-thinking] [--system <prompt>] [--tokens N] [--tokenizer file] [--moe-k N | --moe-mass P]\n"); return 1; }
        if (!tokenizer_path) tokenizer_path = "tokenizer_qwen36.json";
        if (n_chat <= 0) { fprintf(stderr, "--tokens must be positive\n"); return 1; }
        GenOptions options; load_generation_options(snap,&options);
        /* Long thinking runs are a precision-sensitive path. The accelerated
         * kernel quantizes every activation row to int8 on top of the stored
         * int4 weights. Same-prompt/seed controls isolated the unstable choke
         * point to routed/shared expert down projections: keeping only those
         * inputs in float crossed the fast run's repetition point, completed
         * the requested HTML, and retained useful speed. IDOT=0 remains the
         * full-float validation override; an explicit IDOT keeps precedence. */
        if (thinking_code && !getenv("IDOT") && !getenv("IDOT_MOE_DOWN"))
            setenv("IDOT_MOE_DOWN", "0", 0);
        /* Qwen3.6 publishes different sampling profiles for direct, general
         * thinking, and precise coding/WebDev.  The old runner loaded only
         * generation_config.json, which omits presence_penalty and therefore
         * ran every mode as temp=1/top_p=.95/presence=0.  Controlled same-seed
         * A/B runs showed that this made general thinking enter repetition
         * attractors and made WebDev reasoning run away.  Apply the official
         * mode profile unless the user explicitly overrides a field. */
        const char *profile = no_thinking ? "direct" :
                              thinking_code ? "think-code" : "think-general";
        if (no_thinking) {
            if (cli_temp < 0) options.temperature = .7f;
            if (cli_top_p < 0) options.top_p = .8f;
            if (cli_presence < 0) options.presence_penalty = 1.5f;
        } else if (thinking_code) {
            if (cli_temp < 0) options.temperature = .6f;
            if (cli_top_p < 0) options.top_p = .95f;
            if (cli_presence < 0) options.presence_penalty = 0.f;
        } else {
            if (cli_temp < 0) options.temperature = 1.f;
            if (cli_top_p < 0) options.top_p = .95f;
            if (cli_presence < 0) options.presence_penalty = 1.5f;
        }
        if (cli_top_k < 0) options.top_k = 20;
        if(greedy)options.sample=0; if(cli_temp>=0)options.temperature=cli_temp;
        if(cli_presence>=0)options.presence_penalty=cli_presence;
        if(cli_no_doubling>=0)options.last_token_penalty=cli_no_doubling;
        if(cli_top_k>=0)options.top_k=cli_top_k; if(cli_top_p>=0)options.top_p=cli_top_p;
        if(cli_seed)options.rng=cli_seed;
        options.thinking_budget = no_thinking ? 0 :
            cli_thinking_budget >= 0 ? cli_thinking_budget :
            thinking_code ? 2048 : 1024;
        if(options.temperature<=0 || options.top_k<1 || options.top_p<=0 || options.top_p>1){
            fprintf(stderr,"Invalid sampling parameters\n"); return 1;
        }
        int idot_enabled = getenv("IDOT") ? atoi(getenv("IDOT")) : 1;
        int stateful_idot_enabled = getenv("IDOT_STATEFUL")
                                  ? atoi(getenv("IDOT_STATEFUL")) : idot_enabled;
        int moe_down_idot_enabled = getenv("IDOT_MOE_DOWN")
                                  ? atoi(getenv("IDOT_MOE_DOWN")) : idot_enabled;
        const char *activation_mode = !idot_enabled ? "f32-exact"
                                    : !stateful_idot_enabled ? "mixed-stateful-f32"
                                    : !moe_down_idot_enabled ? "mixed-moe-down-f32"
                                    : "int8-fast";
        fprintf(stderr,"[decode] %s profile=%s activations=%s temp=%.3g top_k=%d top_p=%.3g "
                       "presence=%.3g max_tokens=%d thinking_budget=%d\n",
                options.sample?"sampling":"greedy",profile,activation_mode,options.temperature,
                options.top_k,options.top_p,options.presence_penalty,n_chat,
                options.thinking_budget);
        if (g_moe_policy.mode != MOE_POLICY_OFF) {
            fprintf(stderr,
                "[moe-policy] EXPERIMENTAL opt-in mode=%s k=%d mass=%.9g max_entropy=%.9g min_gap=%.9g\n",
                moe_policy_name(g_moe_policy.mode), g_moe_policy.fixed_k,
                g_moe_policy.mode == MOE_POLICY_MASS ? g_moe_policy.mass : -1.f,
                g_moe_policy.has_max_entropy ? g_moe_policy.max_entropy : -1.f,
                g_moe_policy.has_min_gap ? g_moe_policy.min_gap : -1.f);
        }
        Model m; model_init(&m,snap,refine_dir,refine_mode,refine_verify,
                            refine_full_ranks,refine_base_projections,
                            refine_base_layers_text);
        return run_chat(&m, tokenizer_path, chat, system, n_chat, no_thinking, stream,&options,
                        cli_save_session, cli_resume_session, cli_resume_decode,
                        NULL, NULL, NULL, NULL);
    }
    
    if (snap) {
        if (argc > 1) refpath = argv[1];
    } else {
        if (argc > 1) snap = argv[1];
        if (argc > 2) refpath = argv[2];
    }
    
    if (!snap) {
        fprintf(stderr, "Usage: SNAP=<dir> [REF=<json>] ./qwen36b | SNAP=<dir> ./qwen36b --chat <prompt> [--stream] [--no-thinking] [--system <prompt>] [--tokens N]\n");
        return 1;
    }
    if (!refpath) {
        refpath = "c/ref_qwen36.json";
    }
    
    FILE *f = fopen(refpath, "rb");
    if (!f) { perror(refpath); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1); if (fread(buf, 1, n, f) != (size_t)n) {} buf[n] = 0; fclose(f);
    char *arena = NULL; jval *ref = json_parse(buf, &arena);
    int np, nfull;
    int *prompt = read_int_array(ref, "prompt_ids", &np);
    int *full = read_int_array(ref, "full_ids", &nfull);
    int n_new = nfull - np;
    
    printf("== Samosa C engine (Qwen3.6, quantized) ==\n");
    Model m; model_init(&m,snap,refine_dir,refine_mode,refine_verify,
                        refine_full_ranks,refine_base_projections,
                        refine_base_layers_text);
    
    int *out = malloc((np + n_new) * sizeof(int));
    
    if (getenv("TF")) {
        int nfull_tf = 0;
        int *tf = read_int_array(ref, "tf_pred", &nfull_tf);
        int *pred = malloc(nfull * sizeof(int));
        double t0 = now_s();
        forward_all(&m, full, nfull, pred);
        double dt = now_s() - t0;
        int ok = 0;
        for (int i = 0; i < nfull; i++) {
            if (pred[i] == tf[i]) {
                ok++;
            } else {
                printf("Mismatch greedy TF a pos %d: C pred = %d, Oracle tf = %d\n", i, pred[i], tf[i]);
            }
        }
        printf("PREFILL (greedy teacher-forcing) C vs oracolo: %d/%d posizioni | %.1f pos/s\n", ok, nfull, nfull / dt);
        free(pred); free(tf);
        
        jval *rand_ids_val = json_get(ref, "tf_rand_ids");
        if (rand_ids_val) {
            int n_rand = 0;
            int *rand_ids = read_int_array(ref, "tf_rand_ids", &n_rand);
            int n_rand_pred = 0;
            int *tf_rand_pred = read_int_array(ref, "tf_rand_pred", &n_rand_pred);
            int *pred_rand = malloc(n_rand * sizeof(int));
            
            t0 = now_s();
            forward_all(&m, rand_ids, n_rand, pred_rand);
            dt = now_s() - t0;
            
            int ok_rand = 0;
            for (int i = 0; i < n_rand; i++) {
                if (pred_rand[i] == tf_rand_pred[i]) {
                    ok_rand++;
                } else {
                    printf("Mismatch random TF a pos %d: C pred = %d, Oracle tf = %d\n", i, pred_rand[i], tf_rand_pred[i]);
                }
            }
            printf("PREFILL (random teacher-forcing) C vs oracolo: %d/%d posizioni | %.1f pos/s\n", ok_rand, n_rand, n_rand / dt);
            free(pred_rand); free(rand_ids); free(tf_rand_pred);
        }
        
        free(buf); free(arena);
        refine_report(&m);
        return route_close();
    }
    
    double t = now_s();
    generate(&m, prompt, np, n_new, out, NULL, NULL, NULL, NULL);
    double dt = now_s() - t;
    
    int match = 0;
    printf("\nReference  : "); for (int i = np; i < nfull; i++) printf("%d ", full[i]);
    printf("\nC engine   : "); for (int i = np; i < nfull; i++) { printf("%d ", out[i]); if (out[i] == full[i]) match++; }
    printf("\nMatching tokens: %d/%d\n", match, n_new);
    printf("Speed: %.2f tok/s (%.2fs for %d tokens)\n", n_new / dt, dt, n_new);
    
    free(buf); free(arena);
    refine_report(&m);
    return route_close();
}
