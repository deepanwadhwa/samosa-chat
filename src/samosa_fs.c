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
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
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
                        expected_mtime - stat_mtime(&st) > 0.0001))) {
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

static int command_move(const char *root, const char *src, const char *dst,
                        off_t expected_size, int have_size,
                        double expected_mtime, int have_mtime,
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
    if (!realpath(src, src_real) || !make_absolute_path(src_abs, sizeof(src_abs), src) ||
        !canonicalize_parent_path(dst_abs, sizeof(dst_abs), dst)) {
        return emit_move_result(0, "cannot_open_src");
    }
    if (!inside_root_abs(root_abs, src_real) || !inside_root_abs(root_abs, dst_abs))
        return emit_move_result(0, "outside_jail");
    if (!validate_move_source(src_abs, expected_size, have_size, expected_mtime,
                              have_mtime, expected_hash, &reason))
        return emit_move_result(0, reason);
    if (!ensure_parent_dirs(dst_abs, root_abs))
        return emit_move_result(0, "mkdir_failed");
    if (!atomic_no_clobber_move(src_abs, dst_abs, &reason))
        return emit_move_result(0, reason);
    return emit_move_result(1, NULL);
}

static int command_undo(const char *root, const char *src, const char *dst) {
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
    if (!realpath(dst, dst_real) || !canonicalize_parent_path(src_abs, sizeof(src_abs), src) ||
        !make_absolute_path(dst_abs, sizeof(dst_abs), dst))
        return emit_move_result(0, "dest_missing");
    if (!inside_root_abs(root_abs, src_abs) || !inside_root_abs(root_abs, dst_real))
        return emit_move_result(0, "outside_jail");
    if (lstat(dst_abs, &st) != 0)
        return emit_move_result(0, "dest_missing");
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
          "       samosa-fs move --root ROOT [--size N] [--mtime T] [--sha256 H] SRC DST\n"
          "       samosa-fs undo --root ROOT SRC DST\n"
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

int main(int argc, char **argv) {
    const char *cmd;
    const char *path = NULL;
    const char *root = NULL;
    const char *src = NULL;
    const char *dst = NULL;
    const char *expected_hash = NULL;
    size_t max_bytes = DEFAULT_MAX_FILE_BYTES;
    off_t expected_size = 0;
    double expected_mtime = 0.0;
    int have_size = 0;
    int have_mtime = 0;
    int recursive = 0;
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
            if (!parse_size_arg(argv[++i], &max_bytes)) {
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
        } else if (strcmp(argv[i], "--sha256") == 0 && i + 1 < argc) {
            expected_hash = argv[++i];
        } else if (strcmp(cmd, "move") == 0 || strcmp(cmd, "undo") == 0) {
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
    if (!path) {
        if (strcmp(cmd, "move") == 0)
            return command_move(root, src, dst, expected_size, have_size,
                                expected_mtime, have_mtime, expected_hash);
        if (strcmp(cmd, "undo") == 0)
            return command_undo(root, src, dst);
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
