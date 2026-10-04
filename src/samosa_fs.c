/*
 * samosa-fs -- short-lived filesystem metadata sidecar for Jobs.
 *
 * v1 started read-only: survey, list, metadata. It ports the discovery
 * semantics from tools/jobs_fs.py into a bounded process:
 * symlinks rejected, regular files only, magic-byte typing, UTF-8 fallback,
 * metadata-only capped reads, and SHA-256 dedup over full bytes or
 * prefix+"\0truncated\0"+size when the scan cap is hit.
 *
 * v2 adds the mutation core: move and undo. The gateway still owns the
 * approval boundary; this sidecar performs one constrained filesystem verb.
 */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_MAX_FILE_BYTES (25UL * 1024UL * 1024UL)
#define MAX_SCAN_OUTPUT_BYTES (16UL * 1024UL * 1024UL)
#define CPU_SECONDS 10

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} Sha256;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    unsigned char *data;
    size_t len;
    int truncated;
} ReadBuf;

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} PathList;

typedef struct {
    char **hashes;
    size_t len;
    size_t cap;
} HashSet;

typedef struct {
    char *path;
    char *name;
    char *media_type;
    char hash[65];
    off_t size;
    double mtime;
} FileItem;

typedef struct {
    FileItem *items;
    size_t len;
    size_t cap;
} ItemList;

typedef struct {
    char *path;
    char *reason;
} SkipItem;

typedef struct {
    SkipItem *items;
    size_t len;
    size_t cap;
} SkipList;

typedef struct {
    const char *media_type;
    size_t count;
    unsigned long long bytes;
} TypeCount;

typedef struct {
    TypeCount *items;
    size_t len;
    size_t cap;
} TypeCounts;

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(Sha256 *ctx, const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; ++i) {
        const unsigned char *p = block + i * 4;
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(Sha256 *ctx) {
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(ctx->h, init, sizeof(init));
    ctx->bits = 0;
    ctx->used = 0;
}

static void sha256_update(Sha256 *ctx, const void *data_, size_t len) {
    const unsigned char *data = (const unsigned char *)data_;
    ctx->bits += (uint64_t)len * 8u;
    while (len) {
        size_t n = 64u - ctx->used;
        if (n > len) n = len;
        memcpy(ctx->block + ctx->used, data, n);
        ctx->used += n;
        data += n;
        len -= n;
        if (ctx->used == 64u) {
            sha256_compress(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha256_final(Sha256 *ctx, unsigned char out[32]) {
    uint64_t bits = ctx->bits;
    int i;
    ctx->block[ctx->used++] = 0x80u;
    if (ctx->used > 56u) {
        while (ctx->used < 64u) ctx->block[ctx->used++] = 0;
        sha256_compress(ctx, ctx->block);
        ctx->used = 0;
    }
    while (ctx->used < 56u) ctx->block[ctx->used++] = 0;
    for (i = 7; i >= 0; --i)
        ctx->block[ctx->used++] = (unsigned char)(bits >> (i * 8));
    sha256_compress(ctx, ctx->block);
    for (i = 0; i < 8; ++i) {
        out[i * 4] = (unsigned char)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)ctx->h[i];
    }
}

static void sha256_hex(const unsigned char digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

static int set_limits(void) {
    struct rlimit limit;
    limit.rlim_cur = limit.rlim_max = 384UL * 1024UL * 1024UL;
    (void)setrlimit(RLIMIT_AS, &limit);
    (void)setrlimit(RLIMIT_DATA, &limit);
    limit.rlim_cur = limit.rlim_max = CPU_SECONDS;
    return setrlimit(RLIMIT_CPU, &limit) == 0;
}

static void put_error(const char *code) {
    printf("{\"ok\":false,\"error\":\"%s\"}\n", code);
}

static int buf_reserve(Buffer *buf, size_t extra) {
    size_t need;
    char *next;
    if (extra > MAX_SCAN_OUTPUT_BYTES || buf->len > MAX_SCAN_OUTPUT_BYTES - extra)
        return 0;
    need = buf->len + extra + 1;
    if (need <= buf->cap)
        return 1;
    if (!buf->cap) buf->cap = 4096;
    while (buf->cap < need) {
        if (buf->cap > MAX_SCAN_OUTPUT_BYTES / 2) buf->cap = MAX_SCAN_OUTPUT_BYTES + 1;
        else buf->cap *= 2;
    }
    if (buf->cap > MAX_SCAN_OUTPUT_BYTES + 1)
        return 0;
    next = realloc(buf->data, buf->cap);
    if (!next)
        return 0;
    buf->data = next;
    return 1;
}

static int buf_putn(Buffer *buf, const char *text, size_t n) {
    if (!buf_reserve(buf, n))
        return 0;
    memcpy(buf->data + buf->len, text, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 1;
}

static int buf_put(Buffer *buf, const char *text) {
    return buf_putn(buf, text, strlen(text));
}

static int buf_printf(Buffer *buf, const char *format, ...) {
    va_list args;
    va_list copy;
    int written;
    va_start(args, format);
    va_copy(copy, args);
    written = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (written < 0 || !buf_reserve(buf, (size_t)written)) {
        va_end(args);
        return 0;
    }
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, format, args);
    va_end(args);
    buf->len += (size_t)written;
    return 1;
}

static int buf_json_string(Buffer *buf, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (!buf_put(buf, "\"")) return 0;
    for (; *p; ++p) {
        char escaped[7];
        switch (*p) {
        case '\\': if (!buf_put(buf, "\\\\")) return 0; break;
        case '"': if (!buf_put(buf, "\\\"")) return 0; break;
        case '\b': if (!buf_put(buf, "\\b")) return 0; break;
        case '\f': if (!buf_put(buf, "\\f")) return 0; break;
        case '\n': if (!buf_put(buf, "\\n")) return 0; break;
        case '\r': if (!buf_put(buf, "\\r")) return 0; break;
        case '\t': if (!buf_put(buf, "\\t")) return 0; break;
        default:
            if (*p < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                if (!buf_put(buf, escaped)) return 0;
            } else if (!buf_putn(buf, (const char *)p, 1)) {
                return 0;
            }
        }
    }
    return buf_put(buf, "\"");
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int append_path(PathList *list, const char *path) {
    char **next;
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 32;
        next = realloc(list->items, cap * sizeof(*next));
        if (!next) return 0;
        list->items = next;
        list->cap = cap;
    }
    list->items[list->len] = xstrdup(path);
    if (!list->items[list->len]) return 0;
    list->len++;
    return 1;
}

static int path_cmp(const void *a, const void *b) {
    const char *pa = *(const char * const *)a;
    const char *pb = *(const char * const *)b;
    return strcmp(pa, pb);
}

static int join_path(char *out, size_t cap, const char *dir, const char *name) {
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n >= 0 && (size_t)n < cap;
}

static int has_dotdot_component(const char *path) {
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0'))
            return 1;
        while (*p && *p != '/') p++;
    }
    return 0;
}

static int inside_root_abs(const char *root_abs, const char *path) {
    size_t n = strlen(root_abs);
    if (strcmp(root_abs, "/") == 0)
        return path[0] == '/';
    return strncmp(root_abs, path, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static int make_absolute_path(char *out, size_t cap, const char *path) {
    char cwd[PATH_MAX];
    int n;
    if (!path || !*path) return 0;
    if (path[0] == '/') {
        n = snprintf(out, cap, "%s", path);
    } else {
        if (!getcwd(cwd, sizeof(cwd))) return 0;
        n = snprintf(out, cap, "%s/%s", cwd, path);
    }
    return n >= 0 && (size_t)n < cap;
}

static int canonicalize_parent_path(char *out, size_t cap, const char *path) {
    char abs_path[PATH_MAX];
    char probe[PATH_MAX];
    char suffix[PATH_MAX] = "";
    char *slash;
    char real[PATH_MAX];
    int n;
    if (!make_absolute_path(abs_path, sizeof(abs_path), path))
        return 0;
    snprintf(probe, sizeof(probe), "%s", abs_path);
    for (;;) {
        if (realpath(probe, real)) {
            if (suffix[0])
                n = snprintf(out, cap, "%s/%s", real, suffix);
            else
                n = snprintf(out, cap, "%s", real);
            return n >= 0 && (size_t)n < cap;
        }
        slash = strrchr(probe, '/');
        if (!slash)
            return 0;
        {
            char next_suffix[PATH_MAX];
            const char *name = slash + 1;
            if (suffix[0]) n = snprintf(next_suffix, sizeof(next_suffix), "%s/%s", name, suffix);
            else n = snprintf(next_suffix, sizeof(next_suffix), "%s", name);
            if (n < 0 || (size_t)n >= sizeof(next_suffix))
                return 0;
            snprintf(suffix, sizeof(suffix), "%s", next_suffix);
        }
        if (slash == probe) {
            probe[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

static int ensure_parent_dirs(const char *path, const char *root_abs) {
    char tmp[PATH_MAX];
    char *slash;
    char *p;
    if (!make_absolute_path(tmp, sizeof(tmp), path)) return 0;
    slash = strrchr(tmp, '/');
    if (!slash) return 0;
    if (slash == tmp) return 1;
    *slash = '\0';
    if (has_dotdot_component(tmp) || !inside_root_abs(root_abs, tmp))
        return 0;
    for (p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (strcmp(tmp, root_abs) != 0 && inside_root_abs(root_abs, tmp)) {
            struct stat st;
            if (lstat(tmp, &st) != 0) {
                if (errno != ENOENT || mkdir(tmp, 0777) != 0)
                    return 0;
            } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                return 0;
            }
        }
        *p = '/';
    }
    {
        struct stat st;
        if (lstat(tmp, &st) != 0) {
            if (errno != ENOENT || mkdir(tmp, 0777) != 0)
                return 0;
        } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            return 0;
        }
    }
    return 1;
}

static int collect_paths(const char *root, int recursive, PathList *paths) {
    DIR *dir = opendir(root);
    struct dirent *de;
    if (!dir) return 0;
    while ((de = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!join_path(path, sizeof(path), root, de->d_name)) {
            closedir(dir);
            return 0;
        }
        if (!append_path(paths, path)) {
            closedir(dir);
            return 0;
        }
        if (recursive && lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            if (!collect_paths(path, recursive, paths)) {
                closedir(dir);
                return 0;
            }
        }
    }
    closedir(dir);
    return 1;
}

static const char *detect_media_type(const unsigned char *data, size_t len) {
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return "image/jpeg";
    if (len >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return "image/png";
    if (len >= 4 && data[0] == '%' && data[1] == 'P' && data[2] == 'D' && data[3] == 'F')
        return "application/pdf";
    return NULL;
}

static int is_valid_utf8_text(const unsigned char *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = data[i++];
        int cont = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            if (cp < 32 && cp != 9 && cp != 10 && cp != 13)
                return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { cont = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { cont = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { cont = 3; cp = c & 0x07; }
        else return 0;
        if ((size_t)cont > len - i)
            return 0;
        if (c == 0xe0 && data[i] < 0xa0) return 0;
        if (c == 0xed && data[i] >= 0xa0) return 0;
        if (c == 0xf0 && data[i] < 0x90) return 0;
        if (c == 0xf4 && data[i] >= 0x90) return 0;
        while (cont--) {
            if ((data[i] & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (data[i++] & 0x3f);
        }
        if (cp < 32 && cp != 9 && cp != 10 && cp != 13)
            return 0;
    }
    return 1;
}

static double stat_mtime(const struct stat *st) {
#if defined(__APPLE__) && defined(__MACH__)
    return (double)st->st_mtimespec.tv_sec + (double)st->st_mtimespec.tv_nsec / 1000000000.0;
#elif defined(_BSD_SOURCE) || defined(_SVID_SOURCE) || defined(_DEFAULT_SOURCE) || defined(_POSIX_C_SOURCE)
    return (double)st->st_mtim.tv_sec + (double)st->st_mtim.tv_nsec / 1000000000.0;
#else
    return (double)st->st_mtime;
#endif
}

static int read_up_to(int fd, size_t limit, ReadBuf *out) {
    unsigned char *data = NULL;
    size_t total = 0;
    if (limit) {
        data = malloc(limit);
        if (!data) return 0;
    }
    while (total < limit) {
        ssize_t n = read(fd, data + total, limit - total);
        if (n < 0) {
            free(data);
            return 0;
        }
        if (n == 0) {
            out->data = data;
            out->len = total;
            out->truncated = 0;
            return 1;
        }
        total += (size_t)n;
    }
    {
        unsigned char one;
        ssize_t n = read(fd, &one, 1);
        if (n < 0) {
            free(data);
            return 0;
        }
        out->data = data;
        out->len = total;
        out->truncated = n > 0;
        return 1;
    }
}

static void hash_for_scan(const unsigned char *data, size_t len, int truncated, off_t size, char out[65]) {
    Sha256 sha;
    unsigned char digest[32];
    char size_text[64];
    sha256_init(&sha);
    sha256_update(&sha, data, len);
    if (truncated) {
        static const unsigned char marker[] = "\0truncated\0";
        snprintf(size_text, sizeof(size_text), "%lld", (long long)size);
        sha256_update(&sha, marker, sizeof(marker) - 1);
        sha256_update(&sha, size_text, strlen(size_text));
    }
    sha256_final(&sha, digest);
    sha256_hex(digest, out);
}

static int add_hash(HashSet *set, const char *hash) {
    char **next;
    size_t i;
    for (i = 0; i < set->len; ++i)
        if (strcmp(set->hashes[i], hash) == 0)
            return 0;
    if (set->len == set->cap) {
        size_t cap = set->cap ? set->cap * 2 : 32;
        next = realloc(set->hashes, cap * sizeof(*next));
        if (!next) return -1;
        set->hashes = next;
        set->cap = cap;
    }
    set->hashes[set->len] = xstrdup(hash);
    if (!set->hashes[set->len]) return -1;
    set->len++;
    return 1;
}

static int append_skip(SkipList *list, const char *path, const char *reason) {
    SkipItem *next;
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 32;
        next = realloc(list->items, cap * sizeof(*next));
        if (!next) return 0;
        list->items = next;
        list->cap = cap;
    }
    list->items[list->len].path = xstrdup(path);
    list->items[list->len].reason = xstrdup(reason);
    if (!list->items[list->len].path || !list->items[list->len].reason)
        return 0;
    list->len++;
    return 1;
}

static int append_item(ItemList *list, const char *path, const char *media_type,
                       const char hash[65], off_t size, double mtime) {
    FileItem *next;
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 32;
        next = realloc(list->items, cap * sizeof(*next));
        if (!next) return 0;
        list->items = next;
        list->cap = cap;
    }
    list->items[list->len].path = xstrdup(path);
    list->items[list->len].name = xstrdup(base_name(path));
    list->items[list->len].media_type = xstrdup(media_type);
    if (!list->items[list->len].path || !list->items[list->len].name ||
        !list->items[list->len].media_type)
        return 0;
    memcpy(list->items[list->len].hash, hash, 65);
    list->items[list->len].size = size;
    list->items[list->len].mtime = mtime;
    list->len++;
    return 1;
}

static int process_file(const char *path, size_t max_bytes, int metadata_only,
                        ItemList *items, SkipList *skips, HashSet *seen,
                        FileItem *single) {
    struct stat path_st, st, st2;
    int flags = O_RDONLY;
    int fd, added;
    ReadBuf data = {0};
    const char *media_type;
    char hash[65];
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (lstat(path, &path_st) != 0) {
        return append_skip(skips, path, "cannot stat");
    }
    if (S_ISLNK(path_st.st_mode)) {
        return append_skip(skips, path, "cannot open (O_NOFOLLOW): symlink");
    }
    fd = open(path, flags);
    if (fd < 0) {
        return append_skip(skips, path, errno == ELOOP ? "cannot open (O_NOFOLLOW): symlink" : "cannot open");
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_dev != path_st.st_dev || st.st_ino != path_st.st_ino) {
        close(fd);
        return append_skip(skips, path, "not a regular file");
    }
    if (!metadata_only && (uintmax_t)st.st_size > max_bytes) {
        close(fd);
        return append_skip(skips, path, "exceeds max_file_bytes");
    }
    if (st.st_size == 0) {
        close(fd);
        return append_skip(skips, path, "empty file");
    }
    if (!read_up_to(fd, max_bytes, &data)) {
        close(fd);
        return append_skip(skips, path, "cannot read");
    }
    if (fstat(fd, &st2) != 0 || st2.st_size != st.st_size || stat_mtime(&st2) != stat_mtime(&st)) {
        free(data.data);
        close(fd);
        return append_skip(skips, path, "file changed during read");
    }
    close(fd);

    hash_for_scan(data.data, data.len, data.truncated, st.st_size, hash);
    media_type = detect_media_type(data.data, data.len < 8 ? data.len : 8);
    if (!media_type) {
        if (!data.truncated && is_valid_utf8_text(data.data, data.len))
            media_type = "text/plain";
        else if (metadata_only)
            media_type = "application/octet-stream";
        else {
            free(data.data);
            return append_skip(skips, path, "unsupported: not a recognized image/PDF and not valid UTF-8 text");
        }
    }
    free(data.data);

    if (seen) {
        added = add_hash(seen, hash);
        if (added < 0) return 0;
        if (added == 0)
            return append_skip(skips, path, "duplicate content (same SHA-256 as earlier file)");
    }
    if (single) {
        memset(single, 0, sizeof(*single));
        single->path = xstrdup(path);
        single->name = xstrdup(base_name(path));
        single->media_type = xstrdup(media_type);
        if (!single->path || !single->name || !single->media_type) return 0;
        memcpy(single->hash, hash, 65);
        single->size = st.st_size;
        single->mtime = stat_mtime(&st);
        return 1;
    }
    return append_item(items, path, media_type, hash, st.st_size, stat_mtime(&st));
}

static int scan_root(const char *root, int recursive, size_t max_bytes,
                     ItemList *items, SkipList *skips) {
    PathList paths = {0};
    HashSet seen = {0};
    size_t i;
    if (!collect_paths(root, recursive, &paths))
        return 0;
    qsort(paths.items, paths.len, sizeof(paths.items[0]), path_cmp);
    for (i = 0; i < paths.len; ++i) {
        struct stat st;
        if (lstat(paths.items[i], &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
            continue;
        if (!process_file(paths.items[i], max_bytes, 1, items, skips, &seen, NULL))
            return 0;
    }
    return 1;
}

/* ---- Chutni inventory: streaming, safe, metadata-only scan (T0.2) ----
 *
 * Unlike scan_root() above (which materializes the whole tree into a
 * PathList and sorts it -- unbounded memory on a large folder/drive, and one
 * bad opendir() aborts the entire scan), this walks depth-first and emits one
 * NDJSON record per entry directly to stdout as it is discovered. Memory is
 * bounded by directory depth, not total file count. An unreadable descendant
 * is recorded with a stable skip reason and the scan continues past it.
 * Content is never read here -- no hashing, no magic-byte typing. A hash
 * request is a separate, explicit, full-file SHA-256 (chutni-hash below);
 * this scan never returns a misleading truncated/prefix hash. Symlinks are
 * never followed (skipped outright), so they cannot be used to escape the
 * canonical root. */

typedef struct {
    int include_hidden;
    int cross_filesystems;
    char **exclude_names;
    size_t exclude_count;
    unsigned int max_depth;
    unsigned long long max_files;
    unsigned long long max_directories;
    unsigned long long max_seconds;
    unsigned long long max_file_bytes;
    unsigned long long max_eligible_bytes;
    unsigned long long deadline_ms;
} ChutniPolicy;

typedef struct {
    unsigned long long files;
    unsigned long long eligible_bytes;
    unsigned long long skipped;
    unsigned long long directories_seen;
    unsigned long long directories_entered;
    const char *limiting_reason;
    int stop;
} ChutniCounters;

static volatile sig_atomic_t g_chutni_canceled = 0;

static void chutni_on_signal(int signum) {
    (void)signum;
    g_chutni_canceled = 1;
}

static unsigned long long chutni_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
}

/* Test-only determinism seam: SAMOSA_CHUTNI_TEST_DELAY_US inserts a per-file
 * delay so cancellation tests can exercise the two-second stop bound against
 * a small fixture instead of needing hundreds of thousands of real files.
 * Looked up once; zero cost in production where the variable is unset. */
static long chutni_test_delay_us(void) {
    static long delay = -1;
    if (delay < 0) {
        const char *v = getenv("SAMOSA_CHUTNI_TEST_DELAY_US");
        delay = v ? atol(v) : 0;
        if (delay < 0) delay = 0;
    }
    return delay;
}

static int chutni_generated_name_excluded(const char *name) {
    static const char *generated[] = {
        ".git", ".svn", ".hg", "node_modules", ".cache", "__pycache__",
        ".venv", "venv", "env", "target", "build", "dist", "DerivedData",
        ".Trash", ".mypy_cache", ".pytest_cache", ".ruff_cache",
        "site-packages", ".next", ".nuxt", ".yarn", ".pnpm-store",
        ".gradle", "coverage", ".idea", NULL
    };
    static const char *packages[] = {
        ".app", ".bundle", ".framework", ".plugin", ".xcodeproj",
        ".xcworkspace", ".photoslibrary", NULL
    };
    for (const char **p = generated; *p; p++)
        if (!strcasecmp(*p, name)) return 1;
    size_t length = strlen(name);
    for (const char **p = packages; *p; p++) {
        size_t suffix_length = strlen(*p);
        if (length > suffix_length && !strcasecmp(name + length - suffix_length, *p))
            return 1;
    }
    return 0;
}

static int chutni_user_name_excluded(const ChutniPolicy *policy, const char *name) {
    for (size_t i = 0; i < policy->exclude_count; ++i)
        if (strcasecmp(policy->exclude_names[i], name) == 0)
            return 1;
    return 0;
}

static int chutni_python_environment(const char *parent, const char *name) {
    char marker[PATH_MAX];
    struct stat st;
    int n = snprintf(marker, sizeof marker, "%s/%s/pyvenv.cfg", parent, name);
    return n >= 0 && (size_t)n < sizeof marker &&
           lstat(marker, &st) == 0 && S_ISREG(st.st_mode);
}

static int chutni_emit_file(const char *rel_path, const struct stat *st) {
    Buffer out = {0};
    int ok = buf_put(&out, "{\"type\":\"file\",\"rel_path\":") &&
             buf_json_string(&out, rel_path) &&
             buf_printf(&out, ",\"size\":%lld,\"mtime\":%.9f,\"dev\":%lld,\"ino\":%llu}\n",
                        (long long)st->st_size, stat_mtime(st),
                        (long long)st->st_dev, (unsigned long long)st->st_ino);
    if (!ok) { free(out.data); return 0; }
    fputs(out.data, stdout);
    free(out.data);
    return 1;
}

static int chutni_emit_skip(const char *rel_path, const char *reason) {
    Buffer out = {0};
    int ok = buf_put(&out, "{\"type\":\"skip\",\"rel_path\":") &&
             buf_json_string(&out, rel_path) &&
             buf_put(&out, ",\"reason\":") &&
             buf_json_string(&out, reason) &&
             buf_put(&out, "}\n");
    if (!ok) { free(out.data); return 0; }
    fputs(out.data, stdout);
    free(out.data);
    return 1;
}

static void chutni_inventory_policy_fingerprint(const ChutniPolicy *policy,
                                                const struct stat *root_st,
                                                char out[65]) {
    Sha256 sha;
    unsigned char digest[32];
    char identity[256];
    int n = snprintf(identity, sizeof identity,
        "samosa-folder-policy-v3\n%llu:%llu\n%d:%d:%u:%llu:%llu:%llu:%llu:%llu\n",
        (unsigned long long)root_st->st_dev, (unsigned long long)root_st->st_ino,
        policy->include_hidden, policy->cross_filesystems, policy->max_depth,
        policy->max_files, policy->max_directories, policy->max_seconds,
        policy->max_file_bytes, policy->max_eligible_bytes);
    sha256_init(&sha);
    if (n > 0) sha256_update(&sha, identity, (size_t)n);
    for (size_t i = 0; i < policy->exclude_count; i++) {
        sha256_update(&sha, policy->exclude_names[i], strlen(policy->exclude_names[i]));
        sha256_update(&sha, "\n", 1);
    }
    sha256_final(&sha, digest);
    sha256_hex(digest, out);
}

/* Returns 0 only on an unrecoverable output failure (caller reports
 * output_too_large); an unreadable directory is a recorded skip, not a
 * failure, so the scan can continue past it. */
static int chutni_walk(dev_t root_dev, const char *dir_abs, const char *rel_prefix,
                       unsigned int depth,
                       const ChutniPolicy *policy, ChutniCounters *counters) {
    DIR *dir;
    struct dirent *de;
    if (g_chutni_canceled) return 1;
    if (policy->deadline_ms && chutni_monotonic_ms() >= policy->deadline_ms) {
        counters->limiting_reason = "deadline";
        counters->stop = 1;
        return 1;
    }
    dir = opendir(dir_abs);
    if (!dir) {
        if (!chutni_emit_skip(rel_prefix[0] ? rel_prefix : ".",
                              errno == EACCES ? "permission_denied" : "unreadable"))
            return 0;
        counters->skipped++;
        return 1;
    }
    counters->directories_entered++;
    while (!g_chutni_canceled && !counters->stop && (de = readdir(dir)) != NULL) {
        char child_abs[PATH_MAX];
        char child_rel[PATH_MAX];
        struct stat st;
        int n;
        if (policy->deadline_ms && chutni_monotonic_ms() >= policy->deadline_ms) {
            counters->limiting_reason = "deadline";
            counters->stop = 1;
            break;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        {
            long delay_us = chutni_test_delay_us();
            if (delay_us > 0) usleep((useconds_t)delay_us);
        }
        if (policy->deadline_ms && chutni_monotonic_ms() >= policy->deadline_ms) {
            counters->limiting_reason = "deadline";
            counters->stop = 1;
            break;
        }
        if (!join_path(child_abs, sizeof(child_abs), dir_abs, de->d_name)) {
            if (!chutni_emit_skip(rel_prefix[0] ? rel_prefix : ".", "unreadable")) {
                closedir(dir); return 0;
            }
            counters->skipped++;
            continue;
        }
        n = rel_prefix[0]
            ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, de->d_name)
            : snprintf(child_rel, sizeof(child_rel), "%s", de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child_rel)) {
            if (!chutni_emit_skip(rel_prefix[0] ? rel_prefix : ".", "unreadable")) {
                closedir(dir); return 0;
            }
            counters->skipped++;
            continue;
        }
        if (lstat(child_abs, &st) != 0) {
            if (!chutni_emit_skip(child_rel, "unreadable")) { closedir(dir); return 0; }
            counters->skipped++;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            if (!chutni_emit_skip(child_rel, "symlink")) { closedir(dir); return 0; }
            counters->skipped++;
            continue;
        }
        if (de->d_name[0] == '.' && !policy->include_hidden) {
            if (!chutni_emit_skip(child_rel, "hidden_excluded")) { closedir(dir); return 0; }
            counters->skipped++;
            continue;
        }
        if (chutni_generated_name_excluded(de->d_name)) {
            if (!chutni_emit_skip(child_rel, "generated_tree")) { closedir(dir); return 0; }
            counters->skipped++;
            continue;
        }
        if (chutni_user_name_excluded(policy, de->d_name)) {
            if (!chutni_emit_skip(child_rel, "user_exclusion")) { closedir(dir); return 0; }
            counters->skipped++;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            /* Adjacent portable stores contain generated databases and
             * artifacts, including transient SQLite files. Never inventory
             * those as source documents when scanning a parent folder. */
            size_t name_length = strlen(de->d_name);
            if (name_length > 7 && !strcasecmp(de->d_name + name_length - 7, ".chutni")) {
                if (!chutni_emit_skip(child_rel, "generated_tree")) { closedir(dir); return 0; }
                counters->skipped++;
                continue;
            }
            if (chutni_python_environment(dir_abs, de->d_name)) {
                if (!chutni_emit_skip(child_rel, "python_environment_marker")) { closedir(dir); return 0; }
                counters->skipped++;
                continue;
            }
            if (counters->directories_seen >= policy->max_directories) {
                if (!chutni_emit_skip(child_rel, "directory_limit")) { closedir(dir); return 0; }
                counters->skipped++;
                counters->limiting_reason = "maximum_directories";
                counters->stop = 1;
                continue;
            }
            counters->directories_seen++;
            if (depth >= policy->max_depth) {
                if (!chutni_emit_skip(child_rel, "depth_limit")) { closedir(dir); return 0; }
                counters->skipped++;
                counters->limiting_reason = "maximum_depth";
                continue;
            }
            if (!policy->cross_filesystems && st.st_dev != root_dev) {
                if (!chutni_emit_skip(child_rel, "cross_filesystem")) { closedir(dir); return 0; }
                counters->skipped++;
                continue;
            }
            if (!chutni_walk(root_dev, child_abs, child_rel, depth + 1, policy, counters)) {
                closedir(dir);
                return 0;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            if (!chutni_emit_skip(child_rel, "not_regular_file")) { closedir(dir); return 0; }
            counters->skipped++;
            continue;
        }
        if (counters->files >= policy->max_files) {
            if (!chutni_emit_skip(child_rel, "file_limit")) { closedir(dir); return 0; }
            counters->skipped++;
            counters->limiting_reason = "maximum_files";
            counters->stop = 1;
            break;
        }
        unsigned long long file_bytes = st.st_size > 0 ? (unsigned long long)st.st_size : 0;
        if (file_bytes > policy->max_file_bytes) {
            if (!chutni_emit_skip(child_rel, "file_size_limit")) { closedir(dir); return 0; }
            counters->skipped++;
            counters->limiting_reason = "maximum_file_bytes";
            continue;
        }
        if (file_bytes > policy->max_eligible_bytes - counters->eligible_bytes) {
            if (!chutni_emit_skip(child_rel, "eligible_bytes_limit")) { closedir(dir); return 0; }
            counters->skipped++;
            counters->limiting_reason = "maximum_eligible_bytes";
            counters->stop = 1;
            break;
        }
        if (!chutni_emit_file(child_rel, &st)) { closedir(dir); return 0; }
        counters->files++;
        counters->eligible_bytes += file_bytes;
    }
    closedir(dir);
    return 1;
}

static int command_chutni_inventory(const char *root, const ChutniPolicy *policy) {
    char root_abs[PATH_MAX];
    struct stat root_st;
    ChutniCounters counters = {0};
    if (!root || !realpath(root, root_abs)) {
        put_error("folder_unavailable");
        return 65;
    }
    if (!policy->max_directories || !policy->max_files) {
        put_error("invalid_budget");
        return 64;
    }
    if (policy->deadline_ms == 0) {
        /* A zero deadline is reserved for an expired monotonic clock; the CLI
         * always supplies a positive duration. */
        put_error("invalid_budget");
        return 64;
    }
    if (lstat(root_abs, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        put_error("folder_unavailable");
        return 65;
    }
    counters.directories_seen = 1;
    char policy_fingerprint[65];
    chutni_inventory_policy_fingerprint(policy, &root_st, policy_fingerprint);
    if (!chutni_walk(root_st.st_dev, root_abs, "", 0, policy, &counters)) {
        put_error("output_too_large");
        return 65;
    }
    {
        Buffer out = {0};
        int ok = buf_printf(&out,
            "{\"type\":\"done\",\"canceled\":%s,\"partial\":%s,\"files\":%llu,\"eligible_bytes\":%llu,\"skipped\":%llu,"
            "\"directories_seen\":%llu,\"directories_entered\":%llu,\"limiting_reason\":\"%s\","
            "\"policy_fingerprint\":\"%s\"}\n",
            g_chutni_canceled ? "true" : "false",
            (g_chutni_canceled || counters.limiting_reason) ? "true" : "false",
            counters.files, counters.eligible_bytes, counters.skipped, counters.directories_seen,
            counters.directories_entered,
            g_chutni_canceled ? "canceled" :
                (counters.limiting_reason ? counters.limiting_reason : "none"),
            policy_fingerprint);
        if (!ok) { free(out.data); put_error("output_too_large"); return 65; }
        fputs(out.data, stdout);
        free(out.data);
    }
    return g_chutni_canceled ? 2 : 0;
}

/* chutni-hash: the only place a Chutni scan reads content, and only on
 * explicit request for one already-inventoried path. Always a complete
 * SHA-256 over the whole file -- never a capped/prefix hash -- with the same
 * no-follow-open + fstat-identity-before-and-after discipline as
 * validate_move_source() above, so a file rewritten mid-hash is caught
 * rather than silently hashed under a stale identity. */
static int command_chutni_hash(const char *root, const char *target) {
    char root_abs[PATH_MAX], target_abs[PATH_MAX], resolved[PATH_MAX];
    struct stat path_st, st, st2;
    int fd, flags = O_RDONLY;
    Sha256 sha;
    unsigned char digest[32];
    char hex[65];
    unsigned char block[1 << 16];
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (!root || !target || has_dotdot_component(target)) {
        put_error("bad_args");
        return 64;
    }
    if (!realpath(root, root_abs)) {
        put_error("folder_unavailable");
        return 65;
    }
    if (target[0] == '/') {
        if (snprintf(target_abs, sizeof(target_abs), "%s", target) >= (int)sizeof(target_abs)) {
            put_error("bad_args");
            return 64;
        }
    } else if (!join_path(target_abs, sizeof(target_abs), root_abs, target)) {
        put_error("bad_args");
        return 64;
    }
    if (!realpath(target_abs, resolved)) {
        put_error("cannot_open");
        return 65;
    }
    if (!inside_root_abs(root_abs, resolved)) {
        put_error("path_outside_scope");
        return 65;
    }
    snprintf(target_abs, sizeof(target_abs), "%s", resolved);
    if (lstat(target_abs, &path_st) != 0) {
        put_error("cannot_open");
        return 65;
    }
    if (S_ISLNK(path_st.st_mode)) {
        put_error("symlink");
        return 65;
    }
    fd = open(target_abs, flags);
    if (fd < 0) {
        put_error(errno == EACCES ? "permission_denied" : "cannot_open");
        return 65;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_dev != path_st.st_dev || st.st_ino != path_st.st_ino) {
        close(fd);
        put_error("not_regular_file");
        return 65;
    }
    sha256_init(&sha);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0) { close(fd); put_error("cannot_read"); return 65; }
        if (n == 0) break;
        sha256_update(&sha, block, (size_t)n);
        if (g_chutni_canceled) { close(fd); put_error("canceled"); return 2; }
    }
    if (fstat(fd, &st2) != 0 || st2.st_size != st.st_size || stat_mtime(&st2) != stat_mtime(&st)) {
        close(fd);
        put_error("changed_during_read");
        return 65;
    }
    close(fd);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    {
        Buffer out = {0};
        int ok = buf_put(&out, "{\"ok\":true,\"sha256\":") && buf_json_string(&out, hex) &&
                 buf_printf(&out, ",\"size\":%lld}\n", (long long)st.st_size);
        if (!ok) { free(out.data); put_error("output_too_large"); return 65; }
        fputs(out.data, stdout);
        free(out.data);
    }
    return 0;
}

static int type_count_add(TypeCounts *counts, const char *media_type, off_t size) {
    TypeCount *next;
    size_t i;
    for (i = 0; i < counts->len; ++i) {
        if (strcmp(counts->items[i].media_type, media_type) == 0) {
            counts->items[i].count++;
            counts->items[i].bytes += (unsigned long long)size;
            return 1;
        }
    }
    if (counts->len == counts->cap) {
        size_t cap = counts->cap ? counts->cap * 2 : 8;
        next = realloc(counts->items, cap * sizeof(*next));
        if (!next) return 0;
        counts->items = next;
        counts->cap = cap;
    }
    counts->items[counts->len].media_type = media_type;
    counts->items[counts->len].count = 1;
    counts->items[counts->len].bytes = (unsigned long long)size;
    counts->len++;
    return 1;
}

static int type_count_cmp(const void *a, const void *b) {
    const TypeCount *ta = (const TypeCount *)a;
    const TypeCount *tb = (const TypeCount *)b;
    return strcmp(ta->media_type, tb->media_type);
}

/* The list interface is consumed by document jobs.  Keep the absolute path for
 * the existing sidecar callers, but also expose the jailed, root-relative path
 * and the magic-byte type under their explicit contract names. */
static const char *relative_to_root(const char *root, const char *path) {
    size_t n = strlen(root);
    if (!strncmp(root, path, n)) {
        if (root[n - 1] == '/') return path + n;
        if (path[n] == '/') return path + n + 1;
    }
    return path;
}

static int emit_items(Buffer *out, const ItemList *items, const char *root) {
    size_t i;
    if (!buf_put(out, "\"items\":[")) return 0;
    for (i = 0; i < items->len; ++i) {
        const FileItem *it = &items->items[i];
        const char *rel = relative_to_root(root, it->path);
        if (i && !buf_put(out, ",")) return 0;
        if (!buf_put(out, "{\"path\":") || !buf_json_string(out, it->path) ||
            !buf_put(out, ",\"name\":") || !buf_json_string(out, it->name) ||
            !buf_put(out, ",\"rel_path\":") || !buf_json_string(out, rel) ||
            !buf_put(out, ",\"media_type\":") || !buf_json_string(out, it->media_type) ||
            !buf_put(out, ",\"magic_type\":") || !buf_json_string(out, it->media_type) ||
            !buf_put(out, ",\"input_sha256\":") || !buf_json_string(out, it->hash) ||
            !buf_printf(out, ",\"size\":%lld,\"mtime\":%.9f}", (long long)it->size, it->mtime))
            return 0;
    }
    return buf_put(out, "]");
}

static int emit_skips(Buffer *out, const SkipList *skips) {
    size_t i;
    if (!buf_put(out, "\"skipped\":[")) return 0;
    for (i = 0; i < skips->len; ++i) {
        if (i && !buf_put(out, ",")) return 0;
        if (!buf_put(out, "{\"path\":") || !buf_json_string(out, skips->items[i].path) ||
            !buf_put(out, ",\"reason\":") || !buf_json_string(out, skips->items[i].reason) ||
            !buf_put(out, "}"))
            return 0;
    }
    return buf_put(out, "]");
}

static int emit_list(const ItemList *items, const SkipList *skips, const char *root) {
    Buffer out = {0};
    int ok = buf_put(&out, "{\"ok\":true,") &&
             emit_items(&out, items, root) &&
             buf_put(&out, ",") &&
             emit_skips(&out, skips) &&
             buf_put(&out, "}\n");
    if (!ok) {
        free(out.data);
        put_error("output_too_large");
        return 65;
    }
    fputs(out.data, stdout);
    free(out.data);
    return 0;
}

static int emit_survey(const ItemList *items, const SkipList *skips) {
    TypeCounts counts = {0};
    Buffer out = {0};
    size_t i;
    int ok;
    for (i = 0; i < items->len; ++i)
        if (!type_count_add(&counts, items->items[i].media_type, items->items[i].size))
            return 70;
    qsort(counts.items, counts.len, sizeof(counts.items[0]), type_count_cmp);
    ok = buf_printf(&out, "{\"ok\":true,\"total\":%zu,\"skipped_count\":%zu,\"by_type\":{",
                    items->len, skips->len);
    for (i = 0; ok && i < counts.len; ++i) {
        ok = (!i || buf_put(&out, ",")) &&
             buf_json_string(&out, counts.items[i].media_type) &&
             buf_printf(&out, ":{\"count\":%zu,\"bytes\":%llu}",
                        counts.items[i].count, counts.items[i].bytes);
    }
    ok = ok && buf_put(&out, "},") && emit_skips(&out, skips) && buf_put(&out, "}\n");
    free(counts.items);
    if (!ok) {
        free(out.data);
        put_error("output_too_large");
        return 65;
    }
    fputs(out.data, stdout);
    free(out.data);
    return 0;
}

static int emit_metadata(const FileItem *it) {
    Buffer out = {0};
    int ok = buf_put(&out, "{\"ok\":true,\"path\":") &&
             buf_json_string(&out, it->path) &&
             buf_put(&out, ",\"name\":") &&
             buf_json_string(&out, it->name) &&
             buf_put(&out, ",\"media_type\":") &&
             buf_json_string(&out, it->media_type) &&
             buf_put(&out, ",\"input_sha256\":") &&
             buf_json_string(&out, it->hash) &&
             buf_printf(&out, ",\"size\":%lld,\"mtime\":%.9f}\n",
                        (long long)it->size, it->mtime);
    if (!ok) {
        free(out.data);
        put_error("output_too_large");
        return 65;
    }
    fputs(out.data, stdout);
    free(out.data);
    return 0;
}

static void fsync_parent_dir(const char *path) {
    char tmp[PATH_MAX];
    char *slash;
    int fd;
    if (!make_absolute_path(tmp, sizeof(tmp), path)) return;
    slash = strrchr(tmp, '/');
    if (!slash) return;
    if (slash == tmp) slash[1] = '\0';
    else *slash = '\0';
    fd = open(tmp, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

static int atomic_no_clobber_move(const char *src, const char *dst, const char **reason) {
    struct stat src_st, dst_st;
    if (lstat(dst, &dst_st) == 0) {
        *reason = "dest_exists";
        return 0;
    }
    if (errno != ENOENT) {
        *reason = "dest_stat_failed";
        return 0;
    }
    if (link(src, dst) != 0) {
        if (errno == EEXIST) *reason = "dest_exists";
        else if (errno == EXDEV) *reason = "cross_device";
        else *reason = "link_failed";
        return 0;
    }
    if (stat(src, &src_st) != 0 || stat(dst, &dst_st) != 0 ||
        src_st.st_ino != dst_st.st_ino || src_st.st_dev != dst_st.st_dev) {
        (void)unlink(dst);
        *reason = "inode_mismatch";
        return 0;
    }
    if (unlink(src) != 0) {
        (void)unlink(dst);
        *reason = "unlink_failed";
        return 0;
    }
    fsync_parent_dir(dst);
    fsync_parent_dir(src);
    return 1;
}

static int validate_move_source(const char *src, off_t expected_size, int have_size,
                                double expected_mtime, int have_mtime,
                                unsigned long long expected_dev, int have_dev,
                                unsigned long long expected_ino, int have_ino,
                                const char *expected_hash, const char **reason) {
    struct stat path_st, st;
    int flags = O_RDONLY;
    int fd;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (lstat(src, &path_st) != 0) {
        *reason = "cannot_open_src";
        return 0;
    }
    if (S_ISLNK(path_st.st_mode)) {
        *reason = "cannot_open_src";
        return 0;
    }
    fd = open(src, flags);
    if (fd < 0) {
        *reason = "cannot_open_src";
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_dev != path_st.st_dev || st.st_ino != path_st.st_ino) {
        close(fd);
        *reason = "not_regular_file";
        return 0;
    }
    if ((have_size && st.st_size != expected_size) ||
        (have_mtime && (stat_mtime(&st) - expected_mtime > 0.0001 ||
                        expected_mtime - stat_mtime(&st) > 0.0001)) ||
        (have_dev && (unsigned long long)st.st_dev != expected_dev) ||
        (have_ino && (unsigned long long)st.st_ino != expected_ino)) {
        close(fd);
        *reason = "changed_since_scan";
        return 0;
    }
    if (expected_hash) {
        Sha256 sha;
        unsigned char digest[32];
        char hex[65];
        unsigned char block[1 << 20];
        sha256_init(&sha);
        for (;;) {
            ssize_t n = read(fd, block, sizeof(block));
            if (n < 0) {
                close(fd);
                *reason = "cannot_read_src";
                return 0;
            }
            if (n == 0) break;
            sha256_update(&sha, block, (size_t)n);
        }
        sha256_final(&sha, digest);
        sha256_hex(digest, hex);
        if (strcmp(hex, expected_hash) != 0) {
            close(fd);
            *reason = "changed_since_scan";
            return 0;
        }
    }
    close(fd);
    return 1;
}

static int emit_move_result(int ok, const char *reason) {
    Buffer out = {0};
    int good = buf_put(&out, "{\"ok\":true,\"moved\":") &&
               buf_put(&out, ok ? "true" : "false");
    if (!ok) {
        good = good && buf_put(&out, ",\"reason\":") &&
               buf_json_string(&out, reason ? reason : "unknown");
    }
    good = good && buf_put(&out, "}\n");
    if (!good) {
        free(out.data);
        put_error("output_too_large");
        return 65;
    }
    fputs(out.data, stdout);
    free(out.data);
    return 0;
}

/* Copy receipts are hard links outside the destination folder, under the
   authorized root. They prove ownership across a crash between publication
   and the gateway journal update. Undo checks both identity and full content;
   a later edit/replacement is preserved. No directory component is followed. */
static int action_parent(int root_fd, const char *relative, int create,
                         char leaf[PATH_MAX]) {
    if (!relative || !*relative || *relative == '/' || strlen(relative) >= PATH_MAX) return -1;
    char parts[PATH_MAX]; snprintf(parts, sizeof(parts), "%s", relative);
    int directory = dup(root_fd);
    char *cursor = parts;
    while (directory >= 0) {
        char *slash = strchr(cursor, '/');
        if (slash) *slash = 0;
        if (!*cursor || !strcmp(cursor, ".") || !strcmp(cursor, "..")) { close(directory); return -1; }
        if (!slash) { snprintf(leaf, PATH_MAX, "%s", cursor); return directory; }
        if (create && mkdirat(directory, cursor, 0700) != 0 && errno != EEXIST) { close(directory); return -1; }
        int next = openat(directory, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(directory); directory = next; cursor = slash + 1;
    }
    return -1;
}

static int action_hash_fd(int fd, const char *expected) {
    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    Sha256 sha; unsigned char digest[32], block[65536]; char hash[65];
    sha256_init(&sha);
    for (;;) {
        ssize_t got = read(fd, block, sizeof(block));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return 0;
        if (!got) break;
        sha256_update(&sha, block, (size_t)got);
    }
    sha256_final(&sha, digest); sha256_hex(digest, hash);
    return !strcmp(hash, expected);
}

static int emit_copy_result(int applied, int already, const char *reason) {
    Buffer out = {0};
    int ok = buf_printf(&out, "{\"ok\":true,\"applied\":%s,\"already_completed\":%s",
                        applied ? "true" : "false", already ? "true" : "false");
    if (reason) ok = ok && buf_put(&out, ",\"reason\":") && buf_json_string(&out, reason);
    ok = ok && buf_put(&out, "}\n");
    if (ok) fputs(out.data, stdout);
    free(out.data); return ok ? 0 : 65;
}

static int command_copy(const char *root, const char *src, const char *dst,
                        const char *operation_id, const char *expected_hash,
                        off_t expected_size, int have_size,
                        double expected_mtime, int have_mtime,
                        unsigned long long expected_dev, int have_dev,
                        unsigned long long expected_ino, int have_ino, int undo) {
    char root_abs[PATH_MAX], root_input[PATH_MAX], source_abs[PATH_MAX], destination_abs[PATH_MAX];
    if (!root || !src || !dst || !operation_id || !*operation_id ||
        strlen(operation_id) > 100 || !expected_hash || strlen(expected_hash) != 64 ||
        !have_size || !have_mtime || !have_dev || !have_ino ||
        has_dotdot_component(src) || has_dotdot_component(dst)) return emit_copy_result(0, 0, "bad_args");
    for (const char *p = operation_id; *p; p++)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-')) return emit_copy_result(0, 0, "bad_operation_id");
    if (!realpath(root, root_abs) || !make_absolute_path(root_input, sizeof(root_input), root) ||
        !make_absolute_path(source_abs, sizeof(source_abs), src) ||
        !make_absolute_path(destination_abs, sizeof(destination_abs), dst) ||
        (!inside_root_abs(root_abs, source_abs) && !inside_root_abs(root_input, source_abs)) ||
        (!inside_root_abs(root_abs, destination_abs) && !inside_root_abs(root_input, destination_abs)) ||
        !strcmp(source_abs, destination_abs)) return emit_copy_result(0, 0, "outside_jail");
    const char *src_base = inside_root_abs(root_abs, source_abs) ? root_abs : root_input;
    const char *dst_base = inside_root_abs(root_abs, destination_abs) ? root_abs : root_input;
    const char *source_rel = source_abs + strlen(src_base) + (strcmp(src_base, "/") != 0);
    const char *destination_rel = destination_abs + strlen(dst_base) + (strcmp(dst_base, "/") != 0);
    if (!strncmp(destination_rel, ".samosa-actions", 15) ||
        !strncmp(source_rel, ".samosa-actions", 15)) return emit_copy_result(0, 0, "reserved_path");
    int root_fd = open(root_abs, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root_fd < 0) return emit_copy_result(0, 0, "folder_unavailable");
    char anchor_rel[160], anchor_leaf[PATH_MAX], dst_leaf[PATH_MAX], src_leaf[PATH_MAX];
    snprintf(anchor_rel, sizeof(anchor_rel), ".samosa-actions/%s", operation_id);
    int anchor_dir = action_parent(root_fd, anchor_rel, !undo, anchor_leaf);
    int dst_dir = action_parent(root_fd, destination_rel, !undo, dst_leaf);
    int src_dir = -1, source = -1, anchor = -1, stage = -1, lock = -1;
    int applied = 0, already = 0;
    const char *reason = "cannot_open_destination";
    char lock_name[160], temporary[160];
    snprintf(lock_name, sizeof(lock_name), "%s.lock", operation_id);
    snprintf(temporary, sizeof(temporary), "%s-%ld.partial", operation_id, (long)getpid());
    if (anchor_dir < 0 || dst_dir < 0) goto done;
    lock = openat(anchor_dir, lock_name, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX) != 0) { reason = "lock_failed"; goto done; }
    struct stat dst_st, anchor_st, before, after;
    int destination_exists = fstatat(dst_dir, dst_leaf, &dst_st, AT_SYMLINK_NOFOLLOW) == 0;
    anchor = openat(anchor_dir, anchor_leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (anchor >= 0) {
        if (fstat(anchor, &anchor_st) != 0 || !S_ISREG(anchor_st.st_mode) ||
            !action_hash_fd(anchor, expected_hash)) { reason = "copy_receipt_changed"; goto done; }
        if (destination_exists) {
            if (!S_ISREG(dst_st.st_mode) || dst_st.st_dev != anchor_st.st_dev || dst_st.st_ino != anchor_st.st_ino) {
                reason = "destination_replaced"; goto done;
            }
            if (undo) {
                if (unlinkat(dst_dir, dst_leaf, 0) != 0 || fsync(dst_dir) != 0) { reason = "unlink_failed"; goto done; }
                if (unlinkat(anchor_dir, anchor_leaf, 0) != 0 || fsync(anchor_dir) != 0) { reason = "receipt_cleanup_failed"; goto done; }
            } else already = 1;
            applied = 1; reason = NULL; goto done;
        }
        if (undo) {
            /* Also completes undo interrupted after destination removal. */
            if (unlinkat(anchor_dir, anchor_leaf, 0) == 0 && fsync(anchor_dir) == 0) { applied = 1; already = 1; reason = NULL; }
            else reason = "receipt_cleanup_failed";
            goto done;
        }
    } else if (undo) {
        if (!destination_exists) { applied = 1; already = 1; reason = NULL; }
        else reason = "copy_receipt_missing";
        goto done;
    }
    if (destination_exists) { reason = "dest_exists"; goto done; }
    if (anchor < 0) {
        src_dir = action_parent(root_fd, source_rel, 0, src_leaf);
        source = src_dir >= 0 ? openat(src_dir, src_leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK) : -1;
        if (source < 0 || fstat(source, &before) != 0 || !S_ISREG(before.st_mode) ||
            before.st_size != expected_size || (unsigned long long)before.st_dev != expected_dev ||
            (unsigned long long)before.st_ino != expected_ino ||
            stat_mtime(&before) - expected_mtime > 0.0001 || expected_mtime - stat_mtime(&before) > 0.0001) {
            reason = "changed_since_scan"; goto done;
        }
        stage = openat(anchor_dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (stage < 0) { reason = "cannot_create_copy"; goto done; }
        Sha256 sha; unsigned char digest[32], block[65536]; char hash[65]; sha256_init(&sha);
        off_t copied = 0;
        for (;;) {
            ssize_t got = read(source, block, sizeof(block));
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) { reason = "cannot_read_src"; goto done; }
            if (!got) break;
            sha256_update(&sha, block, (size_t)got); copied += got;
            size_t sent = 0;
            while (sent < (size_t)got) {
                ssize_t n = write(stage, block + sent, (size_t)got - sent);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { reason = "cannot_write_copy"; goto done; }
                sent += (size_t)n;
            }
        }
        sha256_final(&sha, digest); sha256_hex(digest, hash);
        if (copied != expected_size || strcmp(hash, expected_hash) || fstat(source, &after) != 0 ||
            after.st_size != before.st_size || stat_mtime(&after) != stat_mtime(&before)) {
            reason = "changed_during_copy"; goto done;
        }
        if (fchmod(stage, before.st_mode & 0777) != 0 || fsync(stage) != 0 ||
            renameat(anchor_dir, temporary, anchor_dir, anchor_leaf) != 0 || fsync(anchor_dir) != 0) {
            reason = "copy_publish_failed"; goto done;
        }
    }
    if (linkat(anchor_dir, anchor_leaf, dst_dir, dst_leaf, 0) != 0) {
        reason = errno == EEXIST ? "dest_exists" : "copy_publish_failed"; goto done;
    }
    if (fsync(dst_dir) != 0) { reason = "copy_sync_failed"; goto done; }
    applied = 1; reason = NULL;
done:
    if (stage >= 0) { close(stage); unlinkat(anchor_dir, temporary, 0); }
    if (source >= 0) close(source);
    if (src_dir >= 0) close(src_dir);
    if (anchor >= 0) close(anchor);
    if (lock >= 0) { flock(lock, LOCK_UN); close(lock); }
    if (anchor_dir >= 0) close(anchor_dir);
    if (dst_dir >= 0) close(dst_dir);
    close(root_fd);
    return emit_copy_result(applied, already, reason);
}

static int command_move(const char *root, const char *src, const char *dst,
                        off_t expected_size, int have_size,
                        double expected_mtime, int have_mtime,
                        unsigned long long expected_dev, int have_dev,
                        unsigned long long expected_ino, int have_ino,
                        const char *expected_hash) {
    char root_abs[PATH_MAX], src_abs[PATH_MAX], dst_abs[PATH_MAX], src_real[PATH_MAX];
    const char *reason = NULL;
    if (!root || !src || !dst || has_dotdot_component(src) || has_dotdot_component(dst)) {
        put_error("bad_args");
        return 64;
    }
    if (!realpath(root, root_abs)) {
        put_error("folder_unavailable");
        return 65;
    }
    if (expected_hash && access(src, F_OK) != 0 && errno == ENOENT &&
        realpath(dst, dst_abs) && inside_root_abs(root_abs, dst_abs) &&
        validate_move_source(dst_abs, expected_size, have_size, expected_mtime,
                             have_mtime, expected_dev, have_dev, expected_ino,
                             have_ino, expected_hash, &reason)) return emit_move_result(1, NULL);
    if (!realpath(src, src_real) || !make_absolute_path(src_abs, sizeof(src_abs), src) ||
        !canonicalize_parent_path(dst_abs, sizeof(dst_abs), dst)) {
        return emit_move_result(0, "cannot_open_src");
    }
    if (!inside_root_abs(root_abs, src_real) || !inside_root_abs(root_abs, dst_abs))
        return emit_move_result(0, "outside_jail");
    if (!validate_move_source(src_abs, expected_size, have_size, expected_mtime,
                              have_mtime, expected_dev, have_dev,
                              expected_ino, have_ino, expected_hash, &reason))
        return emit_move_result(0, reason);
    if (!ensure_parent_dirs(dst_abs, root_abs))
        return emit_move_result(0, "mkdir_failed");
    if (!atomic_no_clobber_move(src_abs, dst_abs, &reason))
        return emit_move_result(0, reason);
    return emit_move_result(1, NULL);
}

static int command_undo(const char *root, const char *src, const char *dst,
                        off_t expected_size, int have_size,
                        double expected_mtime, int have_mtime,
                        unsigned long long expected_dev, int have_dev,
                        unsigned long long expected_ino, int have_ino, const char *expected_hash) {
    struct stat st;
    char root_abs[PATH_MAX], src_abs[PATH_MAX], dst_abs[PATH_MAX], dst_real[PATH_MAX];
    const char *reason = NULL;
    if (!root || !src || !dst || has_dotdot_component(src) || has_dotdot_component(dst)) {
        put_error("bad_args");
        return 64;
    }
    if (!realpath(root, root_abs)) {
        put_error("folder_unavailable");
        return 65;
    }
    if (expected_hash && access(dst, F_OK) != 0 && errno == ENOENT &&
        realpath(src, src_abs) && inside_root_abs(root_abs, src_abs) &&
        validate_move_source(src_abs, expected_size, have_size, expected_mtime,
                             have_mtime, expected_dev, have_dev, expected_ino,
                             have_ino, expected_hash, &reason)) return emit_move_result(1, NULL);
    if (!realpath(dst, dst_real) || !canonicalize_parent_path(src_abs, sizeof(src_abs), src) ||
        !make_absolute_path(dst_abs, sizeof(dst_abs), dst))
        return emit_move_result(0, "dest_missing");
    if (!inside_root_abs(root_abs, src_abs) || !inside_root_abs(root_abs, dst_real))
        return emit_move_result(0, "outside_jail");
    if (lstat(dst_abs, &st) != 0)
        return emit_move_result(0, "dest_missing");
    if (!S_ISREG(st.st_mode) ||
        (have_size && st.st_size != expected_size) ||
        (have_mtime && (stat_mtime(&st) - expected_mtime > 0.0001 ||
                        expected_mtime - stat_mtime(&st) > 0.0001)) ||
        (have_dev && (unsigned long long)st.st_dev != expected_dev) ||
        (have_ino && (unsigned long long)st.st_ino != expected_ino))
        return emit_move_result(0, "destination_changed_since_move");
    if (expected_hash && !validate_move_source(dst_abs, expected_size, have_size,
            expected_mtime, have_mtime, expected_dev, have_dev, expected_ino, have_ino,
            expected_hash, &reason)) return emit_move_result(0, reason);
    if (!ensure_parent_dirs(src_abs, root_abs))
        return emit_move_result(0, "mkdir_failed");
    if (!atomic_no_clobber_move(dst_abs, src_abs, &reason))
        return emit_move_result(0, reason);
    return emit_move_result(1, NULL);
}

static void usage(void) {
    fputs("usage: samosa-fs survey [--recursive] [--max-file-bytes N] ROOT\n"
          "       samosa-fs list [--recursive] [--max-file-bytes N] ROOT\n"
          "       samosa-fs metadata [--max-file-bytes N] PATH\n"
          "       samosa-fs move --root ROOT [--size N] [--mtime T] [--dev N] [--ino N] [--sha256 H] SRC DST\n"
          "       samosa-fs copy|undo-copy --root ROOT --operation-id ID --size N --mtime T --dev N --ino N --sha256 H SRC DST\n"
          "       samosa-fs undo --root ROOT [--size N] [--mtime T] [--dev N] [--ino N] SRC DST\n"
          "       samosa-fs chutni-inventory --root ROOT [--include-hidden]\n"
          "                          [--cross-filesystems] [--exclude NAME]...\n"
          "                          [--max-depth N] [--max-files N]\n"
          "                          [--max-directories N] [--max-seconds N]\n"
          "                          [--max-file-bytes N] [--max-eligible-bytes N]\n"
          "       samosa-fs chutni-hash --root ROOT PATH\n"
          "       samosa-fs --version\n", stderr);
}

static int parse_size_arg(const char *text, size_t *out) {
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0)
        return 0;
    *out = (size_t)parsed;
    return 1;
}

static int parse_ull_arg(const char *text, unsigned long long *out) {
    char *end = NULL;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !end || *end) return 0;
    *out = parsed;
    return 1;
}

int main(int argc, char **argv) {
    const char *cmd;
    const char *path = NULL;
    const char *root = NULL;
    const char *src = NULL;
    const char *dst = NULL;
    const char *expected_hash = NULL;
    const char *operation_id = NULL;
    size_t max_bytes = DEFAULT_MAX_FILE_BYTES;
    unsigned long long max_files = 100000;
    unsigned long long max_directories = 20000;
    unsigned long long max_seconds = 60;
    unsigned long long max_depth = 64;
    unsigned long long max_inventory_file_bytes = 64ull * 1024ull * 1024ull;
    unsigned long long max_eligible_bytes = 2ull * 1024ull * 1024ull * 1024ull;
    unsigned long long expected_dev = 0, expected_ino = 0;
    off_t expected_size = 0;
    double expected_mtime = 0.0;
    int have_size = 0;
    int have_mtime = 0;
    int have_dev = 0, have_ino = 0;
    int recursive = 0;
    int include_hidden = 0;
    int cross_filesystems = 0;
    char **exclude_names = NULL;
    size_t exclude_count = 0, exclude_cap = 0;
    int i;
    ItemList items = {0};
    SkipList skips = {0};

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("samosa-fs 1");
        return 0;
    }
    if (argc < 3) {
        usage();
        return 64;
    }
    if (!set_limits()) {
        put_error("sandbox_limit_unavailable");
        return 70;
    }
    cmd = argv[1];
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
        } else if (strcmp(argv[i], "--max-file-bytes") == 0 && i + 1 < argc) {
            if (strcmp(cmd, "chutni-inventory") == 0) {
                if (!parse_ull_arg(argv[++i], &max_inventory_file_bytes) ||
                    !max_inventory_file_bytes || max_inventory_file_bytes > (1ull << 40)) {
                    usage(); return 64;
                }
            } else if (!parse_size_arg(argv[++i], &max_bytes)) {
                usage();
                return 64;
            }
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            expected_size = (off_t)strtoll(argv[++i], &end, 10);
            if (errno || !end || *end || expected_size < 0) {
                usage();
                return 64;
            }
            have_size = 1;
        } else if (strcmp(argv[i], "--mtime") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            expected_mtime = strtod(argv[++i], &end);
            if (errno || !end || *end) {
                usage();
                return 64;
            }
            have_mtime = 1;
        } else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            expected_dev = strtoull(argv[++i], &end, 10);
            if (errno || !end || *end) { usage(); return 64; }
            have_dev = 1;
        } else if (strcmp(argv[i], "--ino") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            expected_ino = strtoull(argv[++i], &end, 10);
            if (errno || !end || *end) { usage(); return 64; }
            have_ino = 1;
        } else if (strcmp(argv[i], "--operation-id") == 0 && i + 1 < argc) {
            operation_id = argv[++i];
        } else if (strcmp(argv[i], "--sha256") == 0 && i + 1 < argc) {
            expected_hash = argv[++i];
        } else if (strcmp(argv[i], "--include-hidden") == 0) {
            include_hidden = 1;
        } else if (strcmp(argv[i], "--cross-filesystems") == 0) {
            cross_filesystems = 1;
        } else if (strcmp(argv[i], "--max-files") == 0 && i + 1 < argc) {
            if (!parse_ull_arg(argv[++i], &max_files) || !max_files || max_files > 1000000) { usage(); return 64; }
        } else if (strcmp(argv[i], "--max-directories") == 0 && i + 1 < argc) {
            if (!parse_ull_arg(argv[++i], &max_directories) || !max_directories || max_directories > 100000) { usage(); return 64; }
        } else if (strcmp(argv[i], "--max-seconds") == 0 && i + 1 < argc) {
            if (!parse_ull_arg(argv[++i], &max_seconds) || !max_seconds || max_seconds > 3600) { usage(); return 64; }
        } else if (strcmp(argv[i], "--max-depth") == 0 && i + 1 < argc) {
            if (!parse_ull_arg(argv[++i], &max_depth) || max_depth > 64) { usage(); return 64; }
        } else if (strcmp(argv[i], "--max-eligible-bytes") == 0 && i + 1 < argc) {
            if (!parse_ull_arg(argv[++i], &max_eligible_bytes) || !max_eligible_bytes || max_eligible_bytes > (1ull << 44)) { usage(); return 64; }
        } else if (strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) {
            char **next;
            if (exclude_count == exclude_cap) {
                size_t cap = exclude_cap ? exclude_cap * 2 : 8;
                next = realloc(exclude_names, cap * sizeof(*next));
                if (!next) {
                    put_error("out_of_memory");
                    return 70;
                }
                exclude_names = next;
                exclude_cap = cap;
            }
            exclude_names[exclude_count++] = argv[++i];
        } else if (strcmp(cmd, "move") == 0 || strcmp(cmd, "undo") == 0 ||
                   strcmp(cmd, "copy") == 0 || strcmp(cmd, "undo-copy") == 0) {
            if (!src) src = argv[i];
            else if (!dst) dst = argv[i];
            else {
                usage();
                return 64;
            }
        } else if (!path) {
            path = argv[i];
        } else {
            usage();
            return 64;
        }
    }
    if (strcmp(cmd, "chutni-inventory") == 0) {
        ChutniPolicy policy = {0};
        policy.include_hidden = include_hidden;
        policy.cross_filesystems = cross_filesystems;
        policy.max_depth = (unsigned int)max_depth;
        policy.max_files = max_files;
        policy.max_directories = max_directories;
        policy.max_seconds = max_seconds;
        policy.max_file_bytes = max_inventory_file_bytes;
        policy.max_eligible_bytes = max_eligible_bytes;
        unsigned long long now = chutni_monotonic_ms();
        policy.deadline_ms = now ? now + max_seconds * 1000ull : 0;
        policy.exclude_names = exclude_names;
        policy.exclude_count = exclude_count;
        signal(SIGINT, chutni_on_signal);
        signal(SIGTERM, chutni_on_signal);
        return command_chutni_inventory(root, &policy);
    }
    if (strcmp(cmd, "chutni-hash") == 0) {
        signal(SIGINT, chutni_on_signal);
        signal(SIGTERM, chutni_on_signal);
        return command_chutni_hash(root, path);
    }

    if (!path) {
        if (strcmp(cmd, "copy") == 0 || strcmp(cmd, "undo-copy") == 0)
            return command_copy(root, src, dst, operation_id, expected_hash,
                                expected_size, have_size, expected_mtime, have_mtime,
                                expected_dev, have_dev, expected_ino, have_ino,
                                strcmp(cmd, "undo-copy") == 0);
        if (strcmp(cmd, "move") == 0)
            return command_move(root, src, dst, expected_size, have_size,
                                expected_mtime, have_mtime, expected_dev, have_dev,
                                expected_ino, have_ino, expected_hash);
        if (strcmp(cmd, "undo") == 0)
            return command_undo(root, src, dst, expected_size, have_size,
                                expected_mtime, have_mtime, expected_dev, have_dev,
                                expected_ino, have_ino, expected_hash);
        usage();
        return 64;
    }

    if (strcmp(cmd, "survey") == 0 || strcmp(cmd, "list") == 0) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            put_error("folder_unavailable");
            return 65;
        }
        if (!scan_root(path, recursive, max_bytes, &items, &skips)) {
            put_error("scan_failed");
            return 65;
        }
        return strcmp(cmd, "survey") == 0 ? emit_survey(&items, &skips) : emit_list(&items, &skips, path);
    }
    if (strcmp(cmd, "metadata") == 0) {
        FileItem item;
        if (!process_file(path, max_bytes, 1, NULL, &skips, NULL, &item)) {
            put_error("metadata_failed");
            return 65;
        }
        if (skips.len) {
            Buffer out = {0};
            int ok = buf_put(&out, "{\"ok\":false,\"error\":") &&
                     buf_json_string(&out, skips.items[0].reason) &&
                     buf_put(&out, "}\n");
            if (!ok) {
                free(out.data);
                put_error("output_too_large");
                return 65;
            }
            fputs(out.data, stdout);
            free(out.data);
            return 65;
        }
        return emit_metadata(&item);
    }
    usage();
    return 64;
}
