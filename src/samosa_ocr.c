/* samosa-ocr — dependency-free OCR sidecar (PP-OCRv6 small pack).
 *
 * R2/R3 of docs/TASKS_READER.md. Turns pixels into positioned text with
 * per-line confidence, reading the Samosa flat pack (tools/ocr_pack.py). It is
 * a faithful C translation of the NumPy reference (tools/ocr_ref.py) that E-R1
 * validated line-for-line against PaddleOCR 3.7.0; the `_selftest` subcommand
 * checks this C forward pass against golden tensors dumped from that reference.
 *
 * Subcommands (SIDECAR_CONTRACT.md family shape):
 *   samosa-ocr read IMAGE [--emit-crops DIR --below CONF]
 *   samosa-ocr detect IMAGE
 *   samosa-ocr recognize IMAGE --box x0,y0,x1,y1
 *   samosa-ocr _selftest GOLDEN_DIR      (dev; validates C vs NumPy golden)
 *   samosa-ocr --version
 *
 * One JSON object on stdout; stable error codes; own CPU/address limits +
 * caller watchdog; lstat/O_NOFOLLOW/fstat file discipline. The neural forward
 * is pack-driven for weights/thresholds/charset but specialised to the small
 * tier's architecture (a medium pack would add the medium arch tables).
 *
 * No paddle/torch/onnx anywhere; only stb_image.h, kernels.h, json.h, libc.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "kernels.h"   /* matmul(y,x,W,S,I,O): y[S,O]=x[S,I]@W^T */
#include "json.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNM
#include "stb_image.h"

#define OCR_VERSION "samosa-ocr 0 (reader-v0)"
#define BN_EPS 1e-5f
#define LN_EPS 1e-6f
#define MAX_LINES 4096

/* ------------------------------ error exit -------------------------------- */
static void emit_err(const char *code) { printf("{\"ok\":false,\"error\":\"%s\"}\n", code); }
static int die(const char *code, int ex) { emit_err(code); exit(ex); }

/* ------------------------------ the pack ---------------------------------- */
typedef struct { char *name; int shape[6]; int ndim; long off; long nbytes; } PackEnt;
typedef struct {
    unsigned char *raw; long raw_len; long data_off;
    PackEnt *ents; int n_ents; jval *meta;
} Pack;

static Pack *pack_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *raw = malloc(n);
    if (!raw || fread(raw, 1, n, f) != (size_t)n) { fclose(f); free(raw); return NULL; }
    fclose(f);
    if (n < 16 || memcmp(raw, "SMSAOCR\0", 8) != 0) { free(raw); return NULL; }
    uint32_t hlen; memcpy(&hlen, raw + 12, 4);
    char *hdr = malloc(hlen + 1); memcpy(hdr, raw + 16, hlen); hdr[hlen] = 0;
    /* the JSON header is followed by NUL padding; trim to the object */
    char *end = strrchr(hdr, '}'); if (end) end[1] = 0;
    char *arena = NULL; jval *root = json_parse(hdr, &arena); free(hdr);
    if (!root) { free(raw); return NULL; }
    Pack *p = calloc(1, sizeof(Pack));
    p->raw = raw; p->raw_len = n; p->data_off = 16 + hlen;
    p->meta = json_get(root, "meta");
    jval *ts = json_get(root, "tensors");
    p->n_ents = ts ? ts->len : 0;
    p->ents = calloc(p->n_ents > 0 ? p->n_ents : 1, sizeof(PackEnt));
    for (int i = 0; i < p->n_ents; i++) {
        PackEnt *e = &p->ents[i];
        e->name = ts->keys[i];
        jval *o = ts->kids[i];
        jval *sh = json_get(o, "shape");
        e->ndim = sh->len;
        long numel = 1;
        for (int d = 0; d < sh->len && d < 6; d++) { e->shape[d] = (int)sh->kids[d]->num; numel *= e->shape[d]; }
        e->off = (long)json_get(o, "offset")->num;
        e->nbytes = (long)json_get(o, "nbytes")->num;
        (void)numel;
    }
    return p;
}

static void pack_free(Pack *p) {
    if (!p) return;
    free(p->raw);
    free(p->ents);
    free(p);
}

static const float *pack_get(Pack *p, const char *name, int *shape, int *ndim) {
    for (int i = 0; i < p->n_ents; i++)
        if (strcmp(p->ents[i].name, name) == 0) {
            if (shape) for (int d = 0; d < p->ents[i].ndim; d++) shape[d] = p->ents[i].shape[d];
            if (ndim) *ndim = p->ents[i].ndim;
            return (const float *)(p->raw + p->data_off + p->ents[i].off);
        }
    return NULL;
}
static int pack_has(Pack *p, const char *name) { return pack_get(p, name, NULL, NULL) != NULL; }

/* ---------------------------- feature maps -------------------------------- */
typedef struct { int C, H, W; float *d; } FM;
static FM fm_new(int C, int H, int W) { FM m = {C, H, W, calloc((size_t)C * H * W, sizeof(float))}; return m; }
static void fm_free(FM *m) { free(m->d); m->d = NULL; }
#define AT(m, c, y, x) ((m).d[((size_t)(c) * (m).H + (y)) * (m).W + (x)])

/* --------------------------- primitive ops -------------------------------- */
static inline float act_relu(float v) { return v > 0 ? v : 0; }
static inline float act_gelu(float v) { return v * 0.5f * (1.0f + erff(v * 0.70710678f)); }
static inline float act_silu(float v) { return v / (1.0f + expf(-v)); }
static inline float act_sigmoid(float v) { return 1.0f / (1.0f + expf(-v)); }
static inline float act_hsig(float v) { float y = v * (1.0f / 6.0f) + 0.5f; return y < 0 ? 0 : (y > 1 ? 1 : y); }

/* grouped conv: in(C,H,W), w[O,Cg,kh,kw], out(O,Ho,Wo). im2col + matmul. */
static FM conv2d(FM in, const float *w, const float *bias, int O,
                 int kh, int kw, int sh, int sw, int ph, int pw, int groups) {
    int Ho = (in.H + 2 * ph - kh) / sh + 1;
    int Wo = (in.W + 2 * pw - kw) / sw + 1;
    FM out = fm_new(O, Ho, Wo);
    int Cg = in.C / groups, Og = O / groups, K = Cg * kh * kw, S = Ho * Wo;
    float *col = malloc((size_t)S * K * sizeof(float));
    float *tmp = malloc((size_t)S * Og * sizeof(float));
    for (int g = 0; g < groups; g++) {
        for (int oh = 0; oh < Ho; oh++) for (int ow = 0; ow < Wo; ow++) {
            int s = oh * Wo + ow;
            float *cp = col + (size_t)s * K;
            for (int cg = 0; cg < Cg; cg++) {
                int c = g * Cg + cg;
                for (int ky = 0; ky < kh; ky++) {
                    int iy = oh * sh - ph + ky;
                    for (int kx = 0; kx < kw; kx++) {
                        int ix = ow * sw - pw + kx;
                        float v = (iy >= 0 && iy < in.H && ix >= 0 && ix < in.W) ? AT(in, c, iy, ix) : 0.0f;
                        cp[(cg * kh + ky) * kw + kx] = v;
                    }
                }
            }
        }
        matmul(tmp, col, w + (size_t)g * Og * K, S, K, Og);
        for (int s = 0; s < S; s++) for (int o = 0; o < Og; o++)
            out.d[(size_t)(g * Og + o) * S + s] = tmp[(size_t)s * Og + o];
    }
    free(col); free(tmp);
    if (bias) for (int o = 0; o < O; o++) { float b = bias[o]; float *op = out.d + (size_t)o * S; for (int s = 0; s < S; s++) op[s] += b; }
    return out;
}

/* Conv2DTranspose (paddle), padding 0. w[Cin,Cout,kh,kw]. */
static FM conv_transpose2d(FM in, const float *w, const float *bias, int Cout,
                           int kh, int kw, int sh, int sw) {
    int Ho = (in.H - 1) * sh + kh, Wo = (in.W - 1) * sw + kw;
    FM out = fm_new(Cout, Ho, Wo);
    for (int ci = 0; ci < in.C; ci++)
        for (int iy = 0; iy < in.H; iy++) for (int ix = 0; ix < in.W; ix++) {
            float v = AT(in, ci, iy, ix);
            if (v == 0.0f) continue;
            for (int co = 0; co < Cout; co++) {
                const float *wp = w + (((size_t)ci * Cout + co) * kh) * kw;
                for (int ky = 0; ky < kh; ky++) for (int kx = 0; kx < kw; kx++)
                    out.d[((size_t)co * Ho + (iy * sh + ky)) * Wo + (ix * sw + kx)] += v * wp[ky * kw + kx];
            }
        }
    if (bias) { int S = Ho * Wo; for (int o = 0; o < Cout; o++) { float b = bias[o]; float *op = out.d + (size_t)o * S; for (int s = 0; s < S; s++) op[s] += b; } }
    return out;
}

static void bn_(FM *m, const float *g, const float *b, const float *mean, const float *var) {
    int S = m->H * m->W;
    for (int c = 0; c < m->C; c++) {
        float a = g[c] / sqrtf(var[c] + BN_EPS), bb = b[c] - mean[c] * a;
        float *p = m->d + (size_t)c * S;
        for (int s = 0; s < S; s++) p[s] = p[s] * a + bb;
    }
}
static void apply_act(FM *m, int kind) { /* 0 none,1 relu,2 gelu,3 silu */
    if (!kind) return;
    size_t n = (size_t)m->C * m->H * m->W;
    for (size_t i = 0; i < n; i++) {
        float v = m->d[i];
        m->d[i] = kind == 1 ? act_relu(v) : kind == 2 ? act_gelu(v) : act_silu(v);
    }
}

/* ------------------------- weight-name helpers ---------------------------- */
static char NB[256];
static const char *N(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(NB, sizeof NB, fmt, ap); va_end(ap); return NB;
}
/* conv+bn block: <pre>.convolution.weight + <pre>.<normkey>.{weight,bias,running_mean,running_var} */
static FM conv_bn(Pack *p, FM in, const char *pre, const char *normkey,
                  int sh, int sw, int groups, int act) {
    int shp[6], nd; char buf[256];
    snprintf(buf, sizeof buf, "%s.convolution.weight", pre);
    const float *w = pack_get(p, buf, shp, &nd);
    int O = shp[0], kh = shp[2], kw = shp[3];
    FM out = conv2d(in, w, NULL, O, kh, kw, sh, sw, (kh - 1) / 2, (kw - 1) / 2, groups);
    char g[256], b[256], mn[256], vr[256];
    snprintf(g, sizeof g, "%s.%s.weight", pre, normkey);
    snprintf(b, sizeof b, "%s.%s.bias", pre, normkey);
    snprintf(mn, sizeof mn, "%s.%s.running_mean", pre, normkey);
    snprintf(vr, sizeof vr, "%s.%s.running_var", pre, normkey);
    bn_(&out, pack_get(p, g, 0, 0), pack_get(p, b, 0, 0), pack_get(p, mn, 0, 0), pack_get(p, vr, 0, 0));
    apply_act(&out, act);
    return out;
}

/* ------------------------- squeeze-excitation ----------------------------- */
static void se_backbone(Pack *p, FM *x, const char *pre_in) {
    /* avg-pool -> conv0(relu) -> conv2(hardsigmoid) -> scale */
    char pre[224]; snprintf(pre, sizeof pre, "%s", pre_in);  /* N() reuses one buffer; copy first */
    int S = x->H * x->W, C = x->C, shp[6], nd;
    float *avg = calloc(C, sizeof(float));
    for (int c = 0; c < C; c++) { float a = 0; float *pp = x->d + (size_t)c * S; for (int s = 0; s < S; s++) a += pp[s]; avg[c] = a / S; }
    const float *w0 = pack_get(p, N("%s.convolutions.0.weight", pre), shp, &nd); int R = shp[0];
    const float *b0 = pack_get(p, N("%s.convolutions.0.bias", pre), 0, 0);
    const float *w2 = pack_get(p, N("%s.convolutions.2.weight", pre), 0, 0);
    const float *b2 = pack_get(p, N("%s.convolutions.2.bias", pre), 0, 0);
    float *red = calloc(R, sizeof(float));
    for (int r = 0; r < R; r++) { float a = b0[r]; for (int c = 0; c < C; c++) a += w0[(size_t)r * C + c] * avg[c]; red[r] = act_relu(a); }
    for (int c = 0; c < C; c++) {
        float a = b2[c]; for (int r = 0; r < R; r++) a += w2[(size_t)c * R + r] * red[r];
        float sc = act_hsig(a); float *pp = x->d + (size_t)c * S; for (int s = 0; s < S; s++) pp[s] *= sc;
    }
    free(avg); free(red);
}
static void se_det(Pack *p, FM *x, const char *pre_in) {
    /* avg-pool -> conv1(relu) -> conv2 -> clip(0.2x+0.5) -> scale */
    char pre[224]; snprintf(pre, sizeof pre, "%s", pre_in);  /* N() reuses one buffer; copy first */
    int S = x->H * x->W, C = x->C, shp[6], nd;
    float *avg = calloc(C, sizeof(float));
    for (int c = 0; c < C; c++) { float a = 0; float *pp = x->d + (size_t)c * S; for (int s = 0; s < S; s++) a += pp[s]; avg[c] = a / S; }
    const float *w1 = pack_get(p, N("%s.conv1.weight", pre), shp, &nd); int R = shp[0];
    const float *b1 = pack_get(p, N("%s.conv1.bias", pre), 0, 0);
    const float *w2 = pack_get(p, N("%s.conv2.weight", pre), 0, 0);
    const float *b2 = pack_get(p, N("%s.conv2.bias", pre), 0, 0);
    float *red = calloc(R, sizeof(float));
    for (int r = 0; r < R; r++) { float a = b1[r]; for (int c = 0; c < C; c++) a += w1[(size_t)r * C + c] * avg[c]; red[r] = act_relu(a); }
    for (int c = 0; c < C; c++) {
        float a = b2[c]; for (int r = 0; r < R; r++) a += w2[(size_t)c * R + r] * red[r];
        float sc = 0.2f * a + 0.5f; sc = sc < 0 ? 0 : (sc > 1 ? 1 : sc);
        float *pp = x->d + (size_t)c * S; for (int s = 0; s < S; s++) pp[s] *= sc;
    }
    free(avg); free(red);
}

/* ---------------------------- LCNetV4 backbone ---------------------------- */
typedef struct { int k, in, out, sh, sw, se; } Blk;
typedef struct { const int (*stem_ch)[3]; int strides[5]; const Blk *blocks; int n_blocks; int hidden_act; } BBArch;

/* small det backbone */
static const Blk DET_BLK[] = {
    {3,48,48,1,1,1},{3,48,48,1,1,0},
    {3,48,96,2,2,0},{3,96,96,1,1,1},{3,96,96,1,1,0},
    {3,96,192,2,2,0},{3,192,192,1,1,1},{3,192,192,1,1,0},{3,192,192,1,1,1},{3,192,192,1,1,0},
    {3,192,384,2,2,0},{3,384,384,1,1,1},{3,384,384,1,1,0}
};
static const int DET_STAGE_END[4] = {2,5,10,13};
static const int DET_STEM[3] = {3,24,48};
/* small rec backbone */
static const Blk REC_BLK[] = {
    {3,96,96,1,1,1},
    {3,96,96,1,1,0},{3,96,96,1,1,0},
    {3,96,192,2,1,0},{3,192,192,1,1,1},{3,192,192,1,1,0},{3,192,192,1,1,1},{3,192,192,1,1,0},{3,192,192,1,1,1},{3,192,192,1,1,0},
    {3,192,384,2,1,0},{3,384,384,1,1,1},{3,384,384,1,1,0}
};
static const int REC_STAGE_END[4] = {1,3,10,13};
static const int REC_STEM[3] = {3,48,96};

/* stem (large): returns first feature; caller passes stem channel table + strides */
static FM stem_large(Pack *p, FM in, const int stem[3], const int st[5], int act) {
    /* names: model.backbone.encoder.convolution.stem{1,2a,2b,3,4} */
    const char *B = "model.backbone.encoder.convolution";
    FM x1 = conv_bn(p, in, N("%s.stem1", B), "normalization", st[0], st[0], 1, act);
    /* F.pad [0,1,0,1] (right,bottom) */
    FM x1p = fm_new(x1.C, x1.H + 1, x1.W + 1);
    for (int c = 0; c < x1.C; c++) for (int y = 0; y < x1.H; y++) for (int xx = 0; xx < x1.W; xx++) AT(x1p, c, y, xx) = AT(x1, c, y, xx);
    fm_free(&x1);
    FM x2 = conv_bn(p, x1p, N("%s.stem2a", B), "normalization", st[1], st[1], 1, act);
    FM x2p = fm_new(x2.C, x2.H + 1, x2.W + 1);
    for (int c = 0; c < x2.C; c++) for (int y = 0; y < x2.H; y++) for (int xx = 0; xx < x2.W; xx++) AT(x2p, c, y, xx) = AT(x2, c, y, xx);
    fm_free(&x2);
    FM x2b = conv_bn(p, x2p, N("%s.stem2b", B), "normalization", st[2], st[2], 1, act);
    fm_free(&x2p);
    /* maxpool k2 s1 ceil on x1p -> H-1,W-1 */
    FM pooled = fm_new(x1p.C, x1p.H - 1, x1p.W - 1);
    for (int c = 0; c < x1p.C; c++) for (int y = 0; y < pooled.H; y++) for (int xx = 0; xx < pooled.W; xx++) {
        float m = AT(x1p, c, y, xx);
        float a = AT(x1p, c, y, xx + 1); if (a > m) m = a;
        a = AT(x1p, c, y + 1, xx); if (a > m) m = a;
        a = AT(x1p, c, y + 1, xx + 1); if (a > m) m = a;
        AT(pooled, c, y, xx) = m;
    }
    fm_free(&x1p);
    /* concat [pooled, x2b] on channel */
    FM cat = fm_new(pooled.C + x2b.C, pooled.H, pooled.W);
    memcpy(cat.d, pooled.d, (size_t)pooled.C * pooled.H * pooled.W * sizeof(float));
    memcpy(cat.d + (size_t)pooled.C * pooled.H * pooled.W, x2b.d, (size_t)x2b.C * x2b.H * x2b.W * sizeof(float));
    fm_free(&pooled); fm_free(&x2b);
    FM x3 = conv_bn(p, cat, N("%s.stem3", B), "normalization", st[3], st[3], 1, act); fm_free(&cat);
    FM x4 = conv_bn(p, x3, N("%s.stem4", B), "normalization", st[4], st[4], 1, act); fm_free(&x3);
    (void)stem;
    return x4;
}

/* run backbone; write the 4 stage outputs into outs[] (caller frees) */
static void backbone(Pack *p, FM in, const Blk *blk, const int stage_end[4],
                     const int stem[3], int act, FM outs[4]) {
    int st[5] = {2,1,1,2,1};
    FM x = stem_large(p, in, stem, st, act);
    int nb = stage_end[3];
    int stage = 0;
    for (int i = 0; i < nb; i++) {
        const Blk *b = &blk[i];
        char pre[128]; snprintf(pre, sizeof pre, "model.backbone.encoder.blocks.%d.blocks.%d", stage, i - (stage ? stage_end[stage - 1] : 0));
        int rep_dw = (b->sh == 1 && b->sw == 1 && b->in == b->out);
        FM y;
        if (rep_dw) {
            int shp[6], nd; const float *w = pack_get(p, N("%s.token_conv.weight", pre), shp, &nd);
            const float *bi = pack_get(p, N("%s.token_conv.bias", pre), 0, 0);
            y = conv2d(x, w, bi, shp[0], shp[2], shp[3], 1, 1, shp[2] / 2, shp[3] / 2, b->in);
        } else {
            char tc[160]; snprintf(tc, sizeof tc, "%s.token_conv", pre);
            y = conv_bn(p, x, tc, "normalization", b->sh, b->sw, b->in, 0);
        }
        fm_free(&x);
        if (b->se) se_backbone(p, &y, N("%s.token_squeeze_excitation", pre));
        /* residual = y */
        FM res = fm_new(y.C, y.H, y.W); memcpy(res.d, y.d, (size_t)y.C * y.H * y.W * sizeof(float));
        char cc1[160]; snprintf(cc1, sizeof cc1, "%s.channel_conv1", pre);
        FM h = conv_bn(p, y, cc1, "normalization", 1, 1, 1, 2 /*gelu*/); fm_free(&y);
        char cc2[160]; snprintf(cc2, sizeof cc2, "%s.channel_conv2", pre);
        FM h2 = conv_bn(p, h, cc2, "normalization", 1, 1, 1, 0); fm_free(&h);
        if (b->in == b->out && b->sh == 1 && b->sw == 1) {
            size_t n = (size_t)h2.C * h2.H * h2.W; for (size_t k = 0; k < n; k++) h2.d[k] += res.d[k];
        }
        fm_free(&res);
        x = h2;
        if (i + 1 == stage_end[stage]) {
            outs[stage] = fm_new(x.C, x.H, x.W);
            memcpy(outs[stage].d, x.d, (size_t)x.C * x.H * x.W * sizeof(float));
            stage++;
        }
    }
    fm_free(&x);
}

/* ------------------------------ detector ---------------------------------- */
static FM interp2x(FM in) {
    FM o = fm_new(in.C, in.H * 2, in.W * 2);
    for (int c = 0; c < in.C; c++) for (int y = 0; y < o.H; y++) for (int x = 0; x < o.W; x++)
        AT(o, c, y, x) = AT(in, c, y / 2, x / 2);
    return o;
}

static FM det_forward(Pack *p, FM in) {
    FM outs[4]; backbone(p, in, DET_BLK, DET_STAGE_END, DET_STEM, 1 /*relu*/, outs);
    /* neck */
    FM fused[4];
    for (int i = 0; i < 4; i++) {
        int shp[6], nd; const float *w = pack_get(p, N("model.neck.insert_conv.%d.in_conv.weight", i), shp, &nd);
        FM h = conv2d(outs[i], w, NULL, shp[0], 1, 1, 1, 1, 0, 0, 1);
        fm_free(&outs[i]);
        FM se = fm_new(h.C, h.H, h.W); memcpy(se.d, h.d, (size_t)h.C * h.H * h.W * sizeof(float));
        se_det(p, &se, N("model.neck.insert_conv.%d.squeeze_excitation_block", i));
        size_t n = (size_t)h.C * h.H * h.W; for (size_t k = 0; k < n; k++) h.d[k] += se.d[k];
        fm_free(&se); fused[i] = h;
    }
    for (int i = 2; i >= 0; i--) {
        FM up = interp2x(fused[i + 1]);
        size_t n = (size_t)fused[i].C * fused[i].H * fused[i].W;
        for (size_t k = 0; k < n; k++) fused[i].d[k] += up.d[k];
        fm_free(&up);
    }
    FM proc[4];
    for (int i = 0; i < 4; i++) {
        int shp[6], nd;
        const float *dw = pack_get(p, N("model.neck.input_conv.%d.depthwise_convolution.weight", i), shp, &nd);
        const float *db = pack_get(p, N("model.neck.input_conv.%d.depthwise_convolution.bias", i), 0, 0);
        int k = shp[2];
        FM d = conv2d(fused[i], dw, db, shp[0], k, k, 1, 1, k / 2, k / 2, fused[i].C);
        fm_free(&fused[i]);
        const float *pw = pack_get(p, N("model.neck.input_conv.%d.pointwise_convolution.weight", i), shp, &nd);
        FM pwm = conv2d(d, pw, NULL, shp[0], 1, 1, 1, 1, 0, 0, 1); fm_free(&d);
        FM se = fm_new(pwm.C, pwm.H, pwm.W); memcpy(se.d, pwm.d, (size_t)pwm.C * pwm.H * pwm.W * sizeof(float));
        se_det(p, &se, N("model.neck.input_conv.%d.squeeze_excitation_module", i));
        size_t n = (size_t)pwm.C * pwm.H * pwm.W; for (size_t j = 0; j < n; j++) pwm.d[j] += se.d[j];
        fm_free(&se); proc[i] = pwm;
    }
    /* upsample scales [1,2,4,8], concat reversed */
    int scales[4] = {1,2,4,8};
    FM up[4];
    for (int i = 0; i < 4; i++) {
        FM cur = proc[i];
        for (int s = scales[i]; s > 1; s /= 2) { FM t = interp2x(cur); if (cur.d != proc[i].d) fm_free(&cur); cur = t; }
        up[i] = cur;
    }
    int Cc = up[0].C * 4, H = up[0].H, W = up[0].W;
    FM neck = fm_new(Cc, H, W);
    int off = 0;
    for (int i = 3; i >= 0; i--) { memcpy(neck.d + (size_t)off * H * W, up[i].d, (size_t)up[i].C * H * W * sizeof(float)); off += up[i].C; }
    for (int i = 0; i < 4; i++) { if (up[i].d != proc[i].d) fm_free(&up[i]); fm_free(&proc[i]); }
    /* head: conv_down(relu) -> conv_up^T(relu) -> conv_final^T -> sigmoid */
    int shp[6], nd; const float *cdw = pack_get(p, "head.conv_down.convolution.weight", shp, &nd);
    FM hd = conv2d(neck, cdw, NULL, shp[0], shp[2], shp[3], 1, 1, shp[2] / 2, shp[3] / 2, 1); fm_free(&neck);
    bn_(&hd, pack_get(p, "head.conv_down.norm.weight", 0, 0), pack_get(p, "head.conv_down.norm.bias", 0, 0),
        pack_get(p, "head.conv_down.norm.running_mean", 0, 0), pack_get(p, "head.conv_down.norm.running_var", 0, 0));
    apply_act(&hd, 1);
    const float *cuw = pack_get(p, "head.conv_up.convolution.weight", shp, &nd);
    const float *cub = pack_get(p, "head.conv_up.convolution.bias", 0, 0);
    FM hu = conv_transpose2d(hd, cuw, cub, shp[1], shp[2], shp[3], 2, 2); fm_free(&hd);
    bn_(&hu, pack_get(p, "head.conv_up.norm.weight", 0, 0), pack_get(p, "head.conv_up.norm.bias", 0, 0),
        pack_get(p, "head.conv_up.norm.running_mean", 0, 0), pack_get(p, "head.conv_up.norm.running_var", 0, 0));
    apply_act(&hu, 1);
    const float *cfw = pack_get(p, "head.conv_final.weight", shp, &nd);
    const float *cfb = pack_get(p, "head.conv_final.bias", 0, 0);
    FM hf = conv_transpose2d(hu, cfw, cfb, shp[1], shp[2], shp[3], 2, 2); fm_free(&hu);
    size_t n = (size_t)hf.C * hf.H * hf.W; for (size_t i = 0; i < n; i++) hf.d[i] = act_sigmoid(hf.d[i]);
    return hf; /* (1,H,W) prob map */
}

/* ------------------------------ recognizer -------------------------------- */
static void layernorm_row(float *v, int D, const float *g, const float *b) {
    float m = 0; for (int i = 0; i < D; i++) m += v[i]; m /= D;
    float var = 0; for (int i = 0; i < D; i++) { float d = v[i] - m; var += d * d; } var /= D;
    float inv = 1.0f / sqrtf(var + LN_EPS);
    for (int i = 0; i < D; i++) v[i] = (v[i] - m) * inv * g[i] + b[i];
}
/* out[T,O] = in[T,I] @ W[O,I]^T + bias */
static void linear(float *out, const float *in, const float *w, const float *bias, int T, int I, int O) {
    matmul(out, in, w, T, I, O);
    if (bias) for (int t = 0; t < T; t++) for (int o = 0; o < O; o++) out[(size_t)t * O + o] += bias[o];
}

/* returns argmax idx[T] and maxprob[T]; T set via *Tout. logits softmaxed. */
static void rec_forward(Pack *p, FM in, int *idx_out, float *prob_out, int *Tout) {
    FM outs[4]; backbone(p, in, REC_BLK, REC_STAGE_END, REC_STEM, 1, outs);
    for (int i = 0; i < 3; i++) fm_free(&outs[i]);
    FM feat = outs[3];
    /* avg_pool2d kernel (3,2) stride (3,2) */
    int Ho = feat.H / 3, Wo = feat.W / 2;
    FM ap = fm_new(feat.C, Ho, Wo);
    for (int c = 0; c < feat.C; c++) for (int y = 0; y < Ho; y++) for (int x = 0; x < Wo; x++) {
        float a = 0; for (int dy = 0; dy < 3; dy++) for (int dx = 0; dx < 2; dx++) a += AT(feat, c, y * 3 + dy, x * 2 + dx);
        AT(ap, c, y, x) = a / 6.0f;
    }
    fm_free(&feat);
    /* encoder conv blocks */
    int shp[6], nd;
    const float *w0 = pack_get(p, "head.encoder.conv_block.0.convolution.weight", shp, &nd); int hid = shp[0];
    FM residual = conv_bn(p, ap, "head.encoder.conv_block.0", "normalization", 1, 1, 1, 3 /*silu*/);
    FM h1 = conv_bn(p, ap, "head.encoder.conv_block.1", "normalization", 1, 1, 1, 3); fm_free(&ap);
    const float *w2 = pack_get(p, "head.encoder.conv_block.2.convolution.weight", shp, &nd);
    int k2h = shp[2], k2w = shp[3];
    FM h2 = conv_bn(p, h1, "head.encoder.conv_block.2", "normalization", 1, 1, hid, 3);
    { size_t n = (size_t)h1.C * h1.H * h1.W; for (size_t i = 0; i < n; i++) h1.d[i] += h2.d[i]; }
    fm_free(&h2);
    (void)w0; (void)w2; (void)k2h; (void)k2w;
    /* to sequence: (C,H,W) flatten(H*W) -> tokens [T=H*W, C] */
    int Hh = h1.H, Ww = h1.W, T = Hh * Ww, D = hid;
    float *seq = malloc((size_t)T * D * sizeof(float));
    for (int c = 0; c < D; c++) for (int t = 0; t < T; t++) seq[(size_t)t * D + c] = h1.d[(size_t)c * T + t];
    /* svtr blocks */
    int depth = 2, nh = 8, hd = D / nh;
    float scale = 1.0f / sqrtf((float)hd);
    float *ln = malloc((size_t)T * D * sizeof(float));
    float *qkv = malloc((size_t)T * 3 * D * sizeof(float));
    float *attn = malloc((size_t)T * D * sizeof(float));
    float *scores = malloc((size_t)T * sizeof(float));
    float *mlp = malloc((size_t)T * (2 * D) * sizeof(float));
    for (int blk = 0; blk < depth; blk++) {
        char pre[96]; snprintf(pre, sizeof pre, "head.encoder.svtr_block.%d", blk);
        memcpy(ln, seq, (size_t)T * D * sizeof(float));
        const float *g1 = pack_get(p, N("%s.layer_norm1.weight", pre), 0, 0), *b1 = pack_get(p, N("%s.layer_norm1.bias", pre), 0, 0);
        for (int t = 0; t < T; t++) layernorm_row(ln + (size_t)t * D, D, g1, b1);
        const float *wq = pack_get(p, N("%s.self_attn.qkv.weight", pre), 0, 0), *bq = pack_get(p, N("%s.self_attn.qkv.bias", pre), 0, 0);
        linear(qkv, ln, wq, bq, T, D, 3 * D);
        /* per head attention */
        for (int h = 0; h < nh; h++) {
            for (int ti = 0; ti < T; ti++) {
                const float *q = qkv + (size_t)ti * 3 * D + h * hd;
                float mx = -1e30f;
                for (int tj = 0; tj < T; tj++) {
                    const float *kk = qkv + (size_t)tj * 3 * D + D + h * hd;
                    float dp = 0; for (int e = 0; e < hd; e++) dp += q[e] * kk[e];
                    dp *= scale; scores[tj] = dp; if (dp > mx) mx = dp;
                }
                float sm = 0; for (int tj = 0; tj < T; tj++) { scores[tj] = expf(scores[tj] - mx); sm += scores[tj]; }
                float inv = 1.0f / sm;
                float *ao = attn + (size_t)ti * D + h * hd;
                for (int e = 0; e < hd; e++) ao[e] = 0;
                for (int tj = 0; tj < T; tj++) {
                    const float *vv = qkv + (size_t)tj * 3 * D + 2 * D + h * hd; float a = scores[tj] * inv;
                    for (int e = 0; e < hd; e++) ao[e] += a * vv[e];
                }
            }
        }
        const float *wp = pack_get(p, N("%s.self_attn.projection.weight", pre), 0, 0), *bp = pack_get(p, N("%s.self_attn.projection.bias", pre), 0, 0);
        linear(ln, attn, wp, bp, T, D, D);
        for (size_t i = 0; i < (size_t)T * D; i++) seq[i] += ln[i];
        /* mlp */
        memcpy(ln, seq, (size_t)T * D * sizeof(float));
        const float *g2 = pack_get(p, N("%s.layer_norm2.weight", pre), 0, 0), *b2 = pack_get(p, N("%s.layer_norm2.bias", pre), 0, 0);
        for (int t = 0; t < T; t++) layernorm_row(ln + (size_t)t * D, D, g2, b2);
        const float *f1w = pack_get(p, N("%s.mlp.fc1.weight", pre), shp, &nd); int F = shp[0];
        const float *f1b = pack_get(p, N("%s.mlp.fc1.bias", pre), 0, 0);
        linear(mlp, ln, f1w, f1b, T, D, F);
        for (size_t i = 0; i < (size_t)T * F; i++) mlp[i] = act_silu(mlp[i]);
        const float *f2w = pack_get(p, N("%s.mlp.fc2.weight", pre), 0, 0), *f2b = pack_get(p, N("%s.mlp.fc2.bias", pre), 0, 0);
        linear(ln, mlp, f2w, f2b, T, F, D);
        for (size_t i = 0; i < (size_t)T * D; i++) seq[i] += ln[i];
    }
    /* final norm */
    const float *gn = pack_get(p, "head.encoder.norm.weight", 0, 0), *bn = pack_get(p, "head.encoder.norm.bias", 0, 0);
    for (int t = 0; t < T; t++) layernorm_row(seq + (size_t)t * D, D, gn, bn);
    /* reshape (T=Hh*Ww,D)->(D,Hh,Ww), + residual, squeeze Hh(==1) -> (Ww,D) */
    /* residual is (D,Hh,Ww) channel-major; seq token t=(y*Ww+x). add. Then final seq per width. */
    float *seqW = malloc((size_t)Ww * D * sizeof(float));
    for (int x = 0; x < Ww; x++) for (int c = 0; c < D; c++) {
        /* Hh==1 => token index = x */
        float val = seq[(size_t)x * D + c] + residual.d[(size_t)c * (Hh * Ww) + x];
        seqW[(size_t)x * D + c] = val;
    }
    fm_free(&residual); fm_free(&h1);
    /* head linear D->classes, softmax, argmax/maxprob per column */
    const float *hw = pack_get(p, "head.head.weight", shp, &nd); int Cl = shp[0];
    const float *hb = pack_get(p, "head.head.bias", 0, 0);
    float *logits = malloc((size_t)Cl * sizeof(float));
    for (int x = 0; x < Ww; x++) {
        const float *sx = seqW + (size_t)x * D;
        float mx = -1e30f; int am = 0;
        for (int o = 0; o < Cl; o++) { float a = hb[o]; const float *wo = hw + (size_t)o * D; for (int e = 0; e < D; e++) a += sx[e] * wo[e]; logits[o] = a; if (a > mx) { mx = a; am = o; } }
        float sm = 0; for (int o = 0; o < Cl; o++) sm += expf(logits[o] - mx);
        idx_out[x] = am; prob_out[x] = 1.0f / sm; /* softmax max = exp(0)/sum */
    }
    *Tout = Ww;
    free(logits); free(seqW); free(seq); free(ln); free(qkv); free(attn); free(scores); free(mlp);
}

/* -------------------------------- charset --------------------------------- */
typedef struct { char **s; int n; } Charset;
static Charset charset_load(const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/charset.txt", dir);
    FILE *f = fopen(path, "rb"); Charset c = {0, 0};
    if (!f) return c;
    char line[64]; int cap = 32; c.s = malloc(cap * sizeof(char *));
    while (fgets(line, sizeof line, f)) {
        int L = strlen(line); while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (c.n >= cap) { cap *= 2; c.s = realloc(c.s, cap * sizeof(char *)); }
        c.s[c.n++] = strdup(line);
    }
    fclose(f); return c;
}
/* CTC greedy decode: dedup + drop blank(0); conf = mean maxprob over kept. */
static float ctc_decode(const int *idx, const float *prob, int T, Charset *cs, char *out, int outsz) {
    int prev = -1; float sum = 0; int cnt = 0, pos = 0;
    for (int t = 0; t < T; t++) {
        int c = idx[t];
        if (c != prev && c != 0) {
            const char *ch = (c < cs->n) ? cs->s[c] : "?";
            if (strcmp(ch, "blank") == 0) { prev = c; continue; }
            int l = strlen(ch);
            if (pos + l < outsz) { memcpy(out + pos, ch, l); pos += l; }
            sum += prob[t]; cnt++;
        }
        prev = c;
    }
    out[pos] = 0;
    return cnt ? sum / cnt : 0.0f;
}

/* ------------------------- image + preprocessing -------------------------- */
typedef struct { int w, h; unsigned char *bgr; } Img; /* interleaved BGR u8 */

static Img load_image_checked(const char *path) {
    /* file discipline: lstat, reject symlink, O_NOFOLLOW, fstat regular */
    struct stat lst; Img im = {0, 0, NULL};
    if (lstat(path, &lst) != 0) return im;
    if (S_ISLNK(lst.st_mode)) return im;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return im;
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) { close(fd); return im; }
    int w, h, ch; unsigned char *rgb = stbi_load_from_file(fdopen(fd, "rb"), &w, &h, &ch, 3);
    if (!rgb) { return im; }
    unsigned char *bgr = malloc((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) { bgr[i * 3] = rgb[i * 3 + 2]; bgr[i * 3 + 1] = rgb[i * 3 + 1]; bgr[i * 3 + 2] = rgb[i * 3]; }
    stbi_image_free(rgb);
    im.w = w; im.h = h; im.bgr = bgr;
    return im;
}

/* bilinear resize BGR u8 -> BGR u8 (cv2 INTER_LINEAR convention) */
static unsigned char *resize_bilinear(const unsigned char *src, int sw, int sh, int dw, int dh) {
    unsigned char *dst = malloc((size_t)dw * dh * 3);
    double fx = (double)sw / dw, fy = (double)sh / dh;
    for (int y = 0; y < dh; y++) {
        double sy = (y + 0.5) * fy - 0.5; int y0 = (int)floor(sy); double wy = sy - y0;
        int y1 = y0 + 1; if (y0 < 0) { y0 = 0; } if (y1 < 0) y1 = 0; if (y0 > sh - 1) y0 = sh - 1; if (y1 > sh - 1) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            double sx = (x + 0.5) * fx - 0.5; int x0 = (int)floor(sx); double wx = sx - x0;
            int x1 = x0 + 1; if (x0 < 0) x0 = 0; if (x1 < 0) x1 = 0; if (x0 > sw - 1) x0 = sw - 1; if (x1 > sw - 1) x1 = sw - 1;
            for (int c = 0; c < 3; c++) {
                double v = (1 - wx) * (1 - wy) * src[(y0 * sw + x0) * 3 + c] + wx * (1 - wy) * src[(y0 * sw + x1) * 3 + c]
                         + (1 - wx) * wy * src[(y1 * sw + x0) * 3 + c] + wx * wy * src[(y1 * sw + x1) * 3 + c];
                int iv = (int)lround(v); dst[(y * dw + x) * 3 + c] = iv < 0 ? 0 : (iv > 255 ? 255 : iv);
            }
        }
    }
    return dst;
}

/* det preprocess: resize (max-limit + /32), normalize -> FM(3,H,W). */
static FM preprocess_det(Img im, Pack *p, int *src_h, int *src_w, float *ratio_h, float *ratio_w) {
    (void)p;
    int L = 960, m = 32;
    int h = im.h, w = im.w;
    double ratio = (h > w ? h : w) > L ? (double)L / (h > w ? h : w) : 1.0;
    int rh = (int)(h * ratio), rw = (int)(w * ratio);
    rh = (int)(lround(rh / (double)m) * m); if (rh < m) rh = m;
    rw = (int)(lround(rw / (double)m) * m); if (rw < m) rw = m;
    unsigned char *rs = (rh == h && rw == w) ? NULL : resize_bilinear(im.bgr, w, h, rw, rh);
    const unsigned char *px = rs ? rs : im.bgr;
    FM x = fm_new(3, rh, rw);
    float mean[3] = {0.485f, 0.456f, 0.406f}, std[3] = {0.229f, 0.224f, 0.225f};
    for (int c = 0; c < 3; c++) for (int y = 0; y < rh; y++) for (int xx = 0; xx < rw; xx++)
        AT(x, c, y, xx) = ((px[(y * rw + xx) * 3 + c] / 255.0f) - mean[c]) / std[c];
    if (rs) free(rs);
    *src_h = h; *src_w = w; *ratio_h = (float)rh / h; *ratio_w = (float)rw / w;
    return x;
}

/* rec preprocess: crop BGR -> FM(3,48,W). max_wh_ratio shared by caller. */
static FM preprocess_rec(const unsigned char *crop, int cw, int ch, double max_wh_ratio) {
    int imgH = 48, imgW = (int)(imgH * max_wh_ratio);
    if (imgW > 3200) imgW = 3200;
    int resized_w;
    double r = cw / (double)ch;
    if (ceil(imgH * r) > imgW) resized_w = imgW; else resized_w = (int)ceil(imgH * r);
    if (resized_w < 1) resized_w = 1;
    unsigned char *rs = resize_bilinear(crop, cw, ch, resized_w, imgH);
    FM x = fm_new(3, imgH, imgW);
    for (int c = 0; c < 3; c++) for (int y = 0; y < imgH; y++) for (int xx = 0; xx < resized_w; xx++)
        AT(x, c, y, xx) = ((rs[(y * resized_w + xx) * 3 + c] / 255.0f) - 0.5f) / 0.5f;
    free(rs);
    return x;
}

/* --------------------------- DB box extraction ---------------------------- */
/* Connected-components on (prob>thresh), bbox + unclip. Documented simplification
 * of cv2 minAreaRect + pyclipper: exact for axis-aligned printed lines, which is
 * the v1 scope; rotated-text parity is a refinement (see report). */
typedef struct { int x0, y0, x1, y1; float score; } Box;
static int db_boxes(FM prob, int src_h, int src_w, float thresh, float box_thresh,
                    float unclip_ratio, Box *boxes, int maxb) {
    int H = prob.H, W = prob.W;
    unsigned char *bm = calloc((size_t)H * W, 1);
    for (int i = 0; i < H * W; i++) bm[i] = prob.d[i] > thresh ? 1 : 0;
    int *lab = calloc((size_t)H * W, sizeof(int));
    int *stack = malloc((size_t)H * W * sizeof(int));
    int nb = 0;
    double ws = (double)src_w / W, hs = (double)src_h / H;
    for (int i = 0; i < H * W && nb < maxb; i++) {
        if (!bm[i] || lab[i]) continue;
        int sp = 0; stack[sp++] = i; lab[i] = nb + 1;
        int x0 = W, y0 = H, x1 = 0, y1 = 0, area = 0; double ssum = 0;
        while (sp) {
            int q = stack[--sp]; int qy = q / W, qx = q % W;
            if (qx < x0) x0 = qx; if (qx > x1) x1 = qx; if (qy < y0) y0 = qy; if (qy > y1) y1 = qy;
            area++; ssum += prob.d[q];
            int nbr[4] = {q - 1, q + 1, q - W, q + W};
            int okx[4] = {qx > 0, qx < W - 1, qy > 0, qy < H - 1};
            for (int k = 0; k < 4; k++) if (okx[k] && bm[nbr[k]] && !lab[nbr[k]]) { lab[nbr[k]] = nb + 1; stack[sp++] = nbr[k]; }
        }
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        if (bw < 3 && bh < 3) continue;
        float score = (float)(ssum / area);
        if (score < box_thresh) continue;
        /* unclip: distance = area*ratio/perimeter, expand bbox */
        double perim = 2.0 * (bw + bh);
        double dist = area * unclip_ratio / perim;
        double ex0 = (x0 - dist) * ws, ey0 = (y0 - dist) * hs, ex1 = (x1 + 1 + dist) * ws, ey1 = (y1 + 1 + dist) * hs;
        Box *b = &boxes[nb];
        b->x0 = (int)lround(ex0 < 0 ? 0 : ex0); b->y0 = (int)lround(ey0 < 0 ? 0 : ey0);
        b->x1 = (int)lround(ex1 > src_w ? src_w : ex1); b->y1 = (int)lround(ey1 > src_h ? src_h : ey1);
        b->score = score; nb++;
    }
    free(bm); free(lab); free(stack);
    return nb;
}
static int box_cmp(const void *a, const void *b) {
    const Box *x = a, *y = b;
    if (abs(x->y0 - y->y0) < 10) return x->x0 - y->x0;
    return x->y0 - y->y0;
}

/* --------------------------------- JSON ----------------------------------- */
static void json_escape(const char *s, char *out, int outsz) {
    int o = 0;
    for (; *s && o < outsz - 8; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20) { o += snprintf(out + o, outsz - o, "\\u%04x", c); }
        else out[o++] = c;
    }
    out[o] = 0;
}

/* ------------------------------ subcommands ------------------------------- */
static const char *pack_dir(void) {
    const char *e = getenv("SAMOSA_OCR_PACK");
    if (e) return e;
    static char def[1024]; const char *home = getenv("HOME");
    snprintf(def, sizeof def, "%s/.samosa/models/ocr-pack-v1", home ? home : ".");
    return def;
}
static Pack *open_pack_role(const char *dir, const char *role) {
    char path[1100]; snprintf(path, sizeof path, "%s/%s.bin", dir, role);
    return pack_open(path);
}

static void set_limits(void) {
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = 120; setrlimit(RLIMIT_CPU, &rl);
#ifdef RLIMIT_AS
    rl.rlim_cur = rl.rlim_max = (rlim_t)4096 * 1024 * 1024; setrlimit(RLIMIT_AS, &rl); /* best-effort */
#endif
}

static int cmd_detect(const char *dir, const char *image) {
    Pack *dp = open_pack_role(dir, "det");
    if (!dp) return die("ocr_unavailable", 65), 65;
    Img im = load_image_checked(image);
    if (!im.bgr) return die("image_invalid", 65), 65;
    int sh, sw; float rh, rw;
    FM x = preprocess_det(im, dp, &sh, &sw, &rh, &rw);
    FM prob = det_forward(dp, x); fm_free(&x);
    Box boxes[MAX_LINES];
    int nb = db_boxes(prob, sh, sw, 0.2f, 0.45f, 1.4f, boxes, MAX_LINES);
    qsort(boxes, nb, sizeof(Box), box_cmp);
    printf("{\"ok\":true,\"page_width\":%d,\"page_height\":%d,\"lines\":[", sw, sh);
    for (int i = 0; i < nb; i++)
        printf("%s{\"bbox\":[%d,%d,%d,%d],\"det_score\":%.4f}", i ? "," : "",
               boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1, boxes[i].score);
    printf("]}\n");
    fm_free(&prob); free(im.bgr);
    return 0;
}

/* axis-aligned crop of BGR image (clamped) */
static unsigned char *crop_bgr(Img im, int x0, int y0, int x1, int y1, int *cw, int *ch) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > im.w) x1 = im.w; if (y1 > im.h) y1 = im.h;
    int w = x1 - x0, h = y1 - y0; if (w < 1 || h < 1) return NULL;
    unsigned char *c = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) memcpy(c + (size_t)y * w * 3, im.bgr + (size_t)((y0 + y) * im.w + x0) * 3, (size_t)w * 3);
    *cw = w; *ch = h; return c;
}

static int recognize_crop(Pack *rp, Charset *cs, const unsigned char *crop, int cw, int ch,
                          double mwr, char *text, int tsz, float *conf) {
    FM x = preprocess_rec(crop, cw, ch, mwr);
    int *idx = malloc(x.W * sizeof(int)); float *pr = malloc(x.W * sizeof(float)); int T = 0;
    rec_forward(rp, x, idx, pr, &T);
    *conf = ctc_decode(idx, pr, T, cs, text, tsz);
    fm_free(&x); free(idx); free(pr);
    return 0;
}

static const char *classify_crop_script(const unsigned char *bgr, int w, int h, float conf) {
    if (conf >= 0.84f) return "printed";
    if (!bgr || w < 4 || h < 4) return "uncertain";
    
    double mean = 0.0, var = 0.0, diag_trans = 0.0;
    int total = w * h;
    for (int i = 0; i < total; i++) {
        double gray = (bgr[i*3] + bgr[i*3+1] + bgr[i*3+2]) / 3.0;
        mean += gray;
    }
    mean /= total;
    for (int i = 0; i < total; i++) {
        double gray = (bgr[i*3] + bgr[i*3+1] + bgr[i*3+2]) / 3.0;
        var += (gray - mean) * (gray - mean);
    }
    var /= total;

    for (int y = 0; y < h - 1; y++) {
        for (int x = 0; x < w - 1; x++) {
            double g1 = (bgr[(y * w + x) * 3] + bgr[(y * w + x) * 3 + 1] + bgr[(y * w + x) * 3 + 2]) / 3.0;
            double g2 = (bgr[((y + 1) * w + (x + 1)) * 3] + bgr[((y + 1) * w + (x + 1)) * 3 + 1] + bgr[((y + 1) * w + (x + 1)) * 3 + 2]) / 3.0;
            if (fabs(g1 - g2) > 40.0) diag_trans += 1.0;
        }
    }
    double trans_density = diag_trans / (w * h);
    if (var > 1200.0 && trans_density > 0.08) return "handwritten";
    return "uncertain";
}

static int cmd_read(const char *dir, const char *image, const char *emit_dir, float below) {
    Pack *dp = open_pack_role(dir, "det"), *rp = open_pack_role(dir, "rec");
    Pack *hp = open_pack_role(dir, "rec_hand");
    if (!dp || !rp) return die("ocr_unavailable", 65), 65;
    Charset cs = charset_load(dir);
    if (!cs.n) return die("ocr_unavailable", 65), 65;
    Img im = load_image_checked(image);
    if (!im.bgr) return die("image_invalid", 65), 65;
    int sh, sw; float rh, rw;
    FM x = preprocess_det(im, dp, &sh, &sw, &rh, &rw);
    FM prob = det_forward(dp, x); fm_free(&x);
    Box boxes[MAX_LINES];
    int nb = db_boxes(prob, sh, sw, 0.2f, 0.45f, 1.4f, boxes, MAX_LINES);
    qsort(boxes, nb, sizeof(Box), box_cmp);
    fm_free(&prob);
    /* shared max_wh_ratio across the page (matches paddle batching) */
    double mwr = 320.0 / 48.0;
    for (int i = 0; i < nb; i++) { double r = (boxes[i].x1 - boxes[i].x0) / (double)(boxes[i].y1 - boxes[i].y0 + 1); if (r > mwr) mwr = r; }
    printf("{\"ok\":true,\"page_width\":%d,\"page_height\":%d,\"lines\":[", sw, sh);
    int emitted = 0;
    for (int i = 0; i < nb; i++) {
        int cw, chh; unsigned char *c = crop_bgr(im, boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1, &cw, &chh);
        if (!c) continue;
        char text[4096]; float conf; recognize_crop(rp, &cs, c, cw, chh, mwr, text, sizeof text, &conf);
        char esc[8192]; json_escape(text, esc, sizeof esc);
        const char *script = classify_crop_script(c, cw, chh, conf);
        const char *reader = "vlm_crop";
        if (conf >= 0.84f) {
            reader = "rec_print";
        } else if (!strcmp(script, "handwritten")) {
            if (hp) {
                float hconf = 0.0f;
                char htext[4096];
                recognize_crop(hp, &cs, c, cw, chh, mwr, htext, sizeof htext, &hconf);
                if (hconf > conf) {
                    conf = hconf;
                    json_escape(htext, esc, sizeof esc);
                }
            }
            reader = "rec_hand";
        }
        printf("%s{\"bbox\":[%d,%d,%d,%d],\"text\":\"%s\",\"conf\":%.4f,\"script\":\"%s\",\"reader\":\"%s\"}",
               i ? "," : "", boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1, esc, conf, script, reader);
        if (emit_dir && conf < below) {
            char cp[1200]; snprintf(cp, sizeof cp, "%s/crop_%03d.ppm", emit_dir, i);
            FILE *cf = fopen(cp, "wb");
            if (cf) { fprintf(cf, "P6\n%d %d\n255\n", cw, chh);
                for (int q = 0; q < cw * chh; q++) { unsigned char rgb[3] = {c[q*3+2], c[q*3+1], c[q*3]} ; fwrite(rgb, 1, 3, cf); }
                fclose(cf); chmod(cp, 0600); emitted++; }
        }
        free(c);
    }
    if (hp) pack_free(hp);
    pack_free(dp);
    pack_free(rp);
    printf("],\"emitted_crops\":%d}\n", emitted);
    free(im.bgr);
    return 0;
}

/* --------------------------------- selftest ------------------------------- */
static float *read_gold(const char *path, char names[][32], int shapes[][6], int nds[], int *ntensors, char *text, int tsz) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint32_t n; if (fread(&n, 4, 1, f) != 1) { fclose(f); return NULL; }
    *ntensors = n;
    static float *bufs[8]; /* returns first tensor ptr; stores all via out params */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t nl; fread(&nl, 4, 1, f); fread(names[i], 1, nl, f); names[i][nl] = 0;
        uint32_t nd; fread(&nd, 4, 1, f); nds[i] = nd; long numel = 1;
        for (uint32_t d = 0; d < nd; d++) { uint32_t dim; fread(&dim, 4, 1, f); shapes[i][d] = dim; numel *= dim; }
        bufs[i] = malloc(numel * sizeof(float)); fread(bufs[i], sizeof(float), numel, f);
    }
    uint32_t tl; fread(&tl, 4, 1, f); if (tl >= (uint32_t)tsz) tl = tsz - 1;
    if (text) { fread(text, 1, tl, f); text[tl] = 0; }
    fclose(f);
    /* pack pointers into a static structure via globals */
    extern float *g_gold[8]; for (uint32_t i = 0; i < n; i++) g_gold[i] = bufs[i];
    return bufs[0];
}
float *g_gold[8];

static int cmd_selftest(const char *dir, const char *golden) {
    Pack *dp = open_pack_role(dir, "det"), *rp = open_pack_role(dir, "rec");
    Charset cs = charset_load(dir);
    if (!dp || !rp || !cs.n) { fprintf(stderr, "selftest: pack missing\n"); return 1; }
    int fails = 0;
    /* det */
    char gp[1200]; snprintf(gp, sizeof gp, "%s/det.gold", golden);
    char names[8][32]; int shapes[8][6], nds[8], nt; char txt[64];
    read_gold(gp, names, shapes, nds, &nt, txt, sizeof txt);
    int di = strcmp(names[0], "det_in") == 0 ? 0 : 1, dp_i = 1 - di;
    FM x = fm_new(shapes[di][1], shapes[di][2], shapes[di][3]);
    memcpy(x.d, g_gold[di], (size_t)x.C * x.H * x.W * sizeof(float));
    FM prob = det_forward(dp, x); fm_free(&x);
    float *gprob = g_gold[dp_i]; float dmax = 0;
    size_t np = (size_t)prob.C * prob.H * prob.W;
    for (size_t i = 0; i < np; i++) { float d = fabsf(prob.d[i] - gprob[i]); if (d > dmax) dmax = d; }
    printf("selftest det: shape %dx%dx%d  max_abs_diff=%.3e  %s\n", prob.C, prob.H, prob.W, dmax, dmax < 1e-3 ? "OK" : "FAIL");
    if (dmax >= 1e-3) fails++;
    fm_free(&prob);
    /* rec */
    snprintf(gp, sizeof gp, "%s/rec.gold", golden);
    char rnames[8][32]; int rshapes[8][6], rnds[8], rnt; char rtxt[256];
    read_gold(gp, rnames, rshapes, rnds, &rnt, rtxt, sizeof rtxt);
    int ri = -1, ra = -1, rm = -1;
    for (int i = 0; i < rnt; i++) { if (!strcmp(rnames[i], "rec_in")) ri = i; else if (!strcmp(rnames[i], "rec_argmax")) ra = i; else if (!strcmp(rnames[i], "rec_maxprob")) rm = i; }
    FM rx = fm_new(rshapes[ri][1], rshapes[ri][2], rshapes[ri][3]);
    memcpy(rx.d, g_gold[ri], (size_t)rx.C * rx.H * rx.W * sizeof(float));
    int *idx = malloc(rx.W * sizeof(int)); float *pr = malloc(rx.W * sizeof(float)); int T = 0;
    rec_forward(rp, rx, idx, pr, &T);
    int Tg = rshapes[ra][1]; int argmax_ok = (T == Tg); float pmax = 0;
    for (int t = 0; t < T && t < Tg; t++) { if (idx[t] != (int)g_gold[ra][t]) argmax_ok = 0; float d = fabsf(pr[t] - g_gold[rm][t]); if (d > pmax) pmax = d; }
    char text[4096]; float conf = ctc_decode(idx, pr, T, &cs, text, sizeof text);
    printf("selftest rec: T=%d(g=%d) argmax=%s maxprob_diff=%.3e text=%s\n", T, Tg, argmax_ok ? "OK" : "FAIL", pmax, text);
    printf("selftest rec: expected=%s  %s\n", rtxt, strcmp(text, rtxt) == 0 ? "TEXT-OK" : "TEXT-DIFF");
    if (!argmax_ok || strcmp(text, rtxt) != 0) fails++;
    fm_free(&rx); free(idx); free(pr);
    printf("selftest: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

/* ---------------------------------- main ---------------------------------- */
int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) { printf("%s\n", OCR_VERSION); return 0; }
    if (argc < 3) { fprintf(stderr, "usage: samosa-ocr {read|detect|recognize|_selftest} ...\n"); return 64; }
    const char *cmd = argv[1];
    const char *dir = pack_dir();
    if (strcmp(cmd, "_selftest") == 0) return cmd_selftest(dir, argv[2]);
    set_limits();
    if (strcmp(cmd, "detect") == 0) return cmd_detect(dir, argv[2]);
    if (strcmp(cmd, "read") == 0) {
        const char *emit = NULL; float below = 0.84f;
        for (int i = 3; i < argc - 1; i++) { if (!strcmp(argv[i], "--emit-crops")) emit = argv[i + 1]; if (!strcmp(argv[i], "--below")) below = atof(argv[i + 1]); }
        return cmd_read(dir, argv[2], emit, below);
    }
    if (strcmp(cmd, "recognize") == 0) {
        Pack *rp = open_pack_role(dir, "rec"); Charset cs = charset_load(dir);
        if (!rp || !cs.n) return die("ocr_unavailable", 65), 65;
        Img im = load_image_checked(argv[2]); if (!im.bgr) return die("image_invalid", 65), 65;
        int x0 = 0, y0 = 0, x1 = im.w, y1 = im.h;
        for (int i = 3; i < argc - 1; i++) if (!strcmp(argv[i], "--box")) sscanf(argv[i + 1], "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
        int cw, ch; unsigned char *c = crop_bgr(im, x0, y0, x1, y1, &cw, &ch);
        if (!c) return die("image_invalid", 65), 65;
        double mwr = 320.0 / 48.0; double r = cw / (double)ch; if (r > mwr) mwr = r;
        char text[4096]; float conf; recognize_crop(rp, &cs, c, cw, ch, mwr, text, sizeof text, &conf);
        char esc[8192]; json_escape(text, esc, sizeof esc);
        printf("{\"ok\":true,\"text\":\"%s\",\"conf\":%.4f}\n", esc, conf);
        free(c); free(im.bgr); return 0;
    }
    fprintf(stderr, "unknown subcommand\n"); return 64;
}
