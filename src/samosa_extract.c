/*
 * samosa-extract -- short-lived document text extractor.
 *
 * The portable build handles UTF-8 text, HTML, and bounded DOCX packages. A
 * release can additionally link PDFium outside qwen36b for PDF text/rendering.
 * It holds one opened descriptor for the parser's lifetime (no pathname
 * TOCTOU) and writes one JSON object to stdout. A caller must additionally
 * impose a wall-clock timeout before spawning it; this program enforces
 * CPU/address limits itself so malformed documents cannot take down the
 * resident model.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SAMOSA_EXTRACT_NO_PDFIUM
#include "fpdf_edit.h"
#include "fpdf_text.h"
#include "fpdfview.h"
#endif
#include "samosa_docx.h"
#include "samosa_html.h"
#include "tok.h"

/* Bump this whenever the extraction contract changes (page batch cap, text
   normalization, page-object shape, etc.) -- the gateway's read-cache
   fingerprint (T0.3, docs/TASKS_UI_CHUTNI.md) includes this string so a
   contract change invalidates old cache entries automatically. */
#ifdef SAMOSA_EXTRACT_NO_PDFIUM
#define EXTRACT_VERSION "samosa-extract 0 (reader-v6-large-pdf;no-pdfium)"
#else
#define EXTRACT_VERSION "samosa-extract 0 (reader-v6-large-pdf;pdfium)"
#endif

#define DEFAULT_MAX_BYTES (20UL * 1024UL * 1024UL)
#define MAX_PDF_BYTES (4ULL << 30)
#define MAX_PAGE_CHARS 2000000
#define MAX_JSON_BYTES (16UL * 1024UL * 1024UL)
#define MAX_NATIVE_TEXT_BYTES (7UL * 1024UL * 1024UL)
#define RENDER_LONG_EDGE 768
#define MAX_RENDER_PIXELS (RENDER_LONG_EDGE * RENDER_LONG_EDGE)
#define CPU_SECONDS 15
#define WALL_SECONDS 20

typedef struct {
    int fd;
    unsigned long length;
} InputFile;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static void put_error(const char *code) {
    printf("{\"ok\":false,\"error\":\"%s\"}\n", code);
}

static void on_alarm(int ignored) {
    static const char message[] = "{\"ok\":false,\"error\":\"wall_timeout\"}\n";
    (void)ignored;
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
    _Exit(124);
}

static int set_limits(void) {
    struct rlimit limit;
    limit.rlim_cur = limit.rlim_max = 512UL * 1024UL * 1024UL;
    /* Darwin rejects a finite RLIMIT_AS (EINVAL); RLIMIT_DATA is its usable
     * heap/data-segment fallback on systems that implement it. Linux takes the
     * stricter whole-address-space limit. Some Darwin kernels reject both;
     * extraction stays isolated and the controller's process watchdog remains
     * the memory backstop there. */
    (void)setrlimit(RLIMIT_AS, &limit);
    (void)setrlimit(RLIMIT_DATA, &limit);
    limit.rlim_cur = limit.rlim_max = CPU_SECONDS;
    if (setrlimit(RLIMIT_CPU, &limit) != 0)
        return 0;
    return 1;
}

static unsigned long long max_input_bytes(unsigned long long format_limit) {
    const char *value = getenv("SAMOSA_EXTRACT_MAX_BYTES");
    char *end = NULL;
    unsigned long long parsed;
    if (!value || !*value)
        return format_limit;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > format_limit)
        return format_limit;
    return parsed;
}

static int open_input(const char *path, InputFile *input, const char **error) {
    struct stat path_st, st;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (lstat(path, &path_st) != 0) {
        *error = "file_unavailable";
        return 0;
    }
    if (S_ISLNK(path_st.st_mode)) {
        *error = "symlink_not_allowed";
        return 0;
    }
    input->fd = open(path, flags);
    if (input->fd < 0) {
        *error = (errno == ELOOP) ? "symlink_not_allowed" : "file_unavailable";
        return 0;
    }
    if (fstat(input->fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_dev != path_st.st_dev || st.st_ino != path_st.st_ino) {
        close(input->fd);
        *error = "not_regular_file";
        return 0;
    }
    /* PDFium reads bounded blocks from this descriptor on demand. Unlike
       native/container readers it does not need an input-sized allocation.
       Match the attachment upload ceiling without relaxing native limits. */
    char signature[5];
    int is_pdf = pread(input->fd, signature, sizeof(signature), 0) == sizeof(signature) &&
                 memcmp(signature, "%PDF-", sizeof(signature)) == 0;
    if (st.st_size == 0 || (uintmax_t)st.st_size > max_input_bytes(is_pdf ? MAX_PDF_BYTES : DEFAULT_MAX_BYTES) ||
        (uintmax_t)st.st_size > ULONG_MAX) {
        close(input->fd);
        *error = st.st_size == 0 ? "file_empty" : "file_too_large";
        return 0;
    }
    input->length = (unsigned long)st.st_size;
    return 1;
}

static int read_block(void *opaque, unsigned long position, unsigned char *out,
                      unsigned long size) {
    InputFile *input = opaque;
    size_t done = 0;
    if (position > input->length || size > input->length - position)
        return 0;
    while (done < size) {
        ssize_t n = pread(input->fd, out + done, size - done,
                           (off_t)(position + done));
        if (n <= 0)
            return 0;
        done += (size_t)n;
    }
    return 1;
}

static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length) {
        ssize_t written = write(fd, cursor, length);
        if (written <= 0)
            return 0;
        cursor += written;
        length -= (size_t)written;
    }
    return 1;
}

static int read_prefix(InputFile *input, unsigned char *out, size_t capacity,
                       size_t *length) {
    size_t want = input->length < capacity ? input->length : capacity;
    ssize_t got = pread(input->fd, out, want, 0);
    if (got < 0)
        return 0;
    *length = (size_t)got;
    return 1;
}

static int has_ascii_prefix(const unsigned char *data, size_t length,
                            const char *prefix) {
    size_t i, prefix_length = strlen(prefix);
    if (length < prefix_length)
        return 0;
    for (i = 0; i < prefix_length; ++i) {
        unsigned char c = data[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)prefix[i])
            return 0;
    }
    return 1;
}

static int buf_reserve(Buffer *buf, size_t extra) {
    size_t need;
    char *next;
    if (extra > MAX_JSON_BYTES || buf->len > MAX_JSON_BYTES - extra)
        return 0;
    need = buf->len + extra + 1;
    if (need <= buf->cap)
        return 1;
    if (!buf->cap)
        buf->cap = 4096;
    while (buf->cap < need) {
        if (buf->cap > MAX_JSON_BYTES / 2)
            buf->cap = MAX_JSON_BYTES + 1;
        else
            buf->cap *= 2;
    }
    if (buf->cap > MAX_JSON_BYTES + 1)
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
    if (!buf_putn(buf, "\"", 1))
        return 0;
    for (; *p; ++p) {
        char escaped[7];
        switch (*p) {
        case '\\': if (!buf_put(buf, "\\\\")) return 0; break;
        case '\"': if (!buf_put(buf, "\\\"")) return 0; break;
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
    return buf_putn(buf, "\"", 1);
}

static int utf8_put(Buffer *out, uint32_t cp) {
    char bytes[4];
    size_t n;
    if (cp <= 0x7f) { bytes[0] = (char)cp; n = 1; }
    else if (cp <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (cp >> 6)); bytes[1] = (char)(0x80 | (cp & 0x3f)); n = 2;
    } else if (cp <= 0xffff) {
        bytes[0] = (char)(0xe0 | (cp >> 12)); bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (cp & 0x3f)); n = 3;
    } else {
        bytes[0] = (char)(0xf0 | (cp >> 18)); bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); bytes[3] = (char)(0x80 | (cp & 0x3f)); n = 4;
    }
    return buf_putn(out, bytes, n);
}

static int utf16_to_utf8(const unsigned short *input, int count, Buffer *out) {
    int i;
    for (i = 0; i < count && input[i]; ++i) {
        uint32_t cp = input[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < count &&
            input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (input[++i] - 0xdc00);
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        if (!utf8_put(out, cp))
            return 0;
    }
    return 1;
}

static int valid_utf8(const unsigned char *data, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = data[i++];
        int continuation = 0;
        if (c == 0)
            return 0;
        if (c < 0x80)
            continue;
        if (c >= 0xc2 && c <= 0xdf) continuation = 1;
        else if (c >= 0xe0 && c <= 0xef) continuation = 2;
        else if (c >= 0xf0 && c <= 0xf4) continuation = 3;
        else return 0;
        if ((size_t)continuation > length - i)
            return 0;
        if (c == 0xe0 && data[i] < 0xa0) return 0;
        if (c == 0xed && data[i] >= 0xa0) return 0;
        if (c == 0xf0 && data[i] < 0x90) return 0;
        if (c == 0xf4 && data[i] >= 0x90) return 0;
        while (continuation--) {
            if ((data[i++] & 0xc0) != 0x80)
                return 0;
        }
    }
    return 1;
}

static int text_has_binary_control(const unsigned char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char c = data[i];
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f') ||
            c == 0x7f)
            return 1;
    }
    return 0;
}

static unsigned long utf8_char_count(const char *text) {
    unsigned long count = 0;
    for (; *text; ++text)
        if (((unsigned char)*text & 0xc0) != 0x80)
            ++count;
    return count;
}

static unsigned long estimate_tokens(const char *text);

static int extract_native_text(InputFile *input, Buffer *text, const char **error) {
    unsigned char *raw;
    size_t i;
    if (input->length > MAX_NATIVE_TEXT_BYTES) {
        *error = "native_text_output_limit";
        return 0;
    }
    raw = malloc((size_t)input->length);
    if (!raw) {
        *error = "out_of_memory";
        return 0;
    }
    if (!read_block(input, 0, raw, input->length) ||
        !valid_utf8(raw, input->length)) {
        free(raw);
        *error = "text_invalid_utf8";
        return 0;
    }
    if (text_has_binary_control(raw, input->length)) {
        free(raw);
        *error = "text_binary_control";
        return 0;
    }
    for (i = 0; i < input->length; ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < input->length && raw[i + 1] == '\n')
                ++i;
            if (!buf_putn(text, "\n", 1)) {
                free(raw);
                *error = "output_too_large";
                return 0;
            }
        } else if (!buf_putn(text, (const char *)raw + i, 1)) {
            free(raw);
            *error = "output_too_large";
            return 0;
        }
    }
    free(raw);
    return 1;
}

static int extract_html(InputFile *input, SamosaHtmlResult *result,
                        const char **error) {
    unsigned char *raw;
    if (input->length > MAX_NATIVE_TEXT_BYTES) {
        *error = "html_output_limit";
        return 0;
    }
    raw = malloc((size_t)input->length);
    if (!raw) {
        *error = "out_of_memory";
        return 0;
    }
    if (!read_block(input, 0, raw, input->length)) {
        free(raw);
        *error = "file_unavailable";
        return 0;
    }
    int ok = samosa_html_extract(raw, (size_t)input->length,
                                 MAX_NATIVE_TEXT_BYTES, result, error);
    free(raw);
    return ok;
}

static int extract_docx(InputFile *input, SamosaDocxResult *result,
                        const char **error) {
    unsigned char *raw = malloc((size_t)input->length);
    if (!raw) {
        *error = "out_of_memory";
        return 0;
    }
    if (!read_block(input, 0, raw, input->length)) {
        free(raw);
        *error = "file_unavailable";
        return 0;
    }
    int ok = samosa_docx_extract(raw, (size_t)input->length,
                                 MAX_NATIVE_TEXT_BYTES, result, error);
    free(raw);
    return ok;
}

static int count_model_tokens(Tok *tokenizer, const char *text, unsigned long *count) {
    size_t length = strlen(text);
    int *ids;
    if (length > INT_MAX)
        return 0;
    ids = calloc(length ? length : 1, sizeof(*ids));
    if (!ids)
        return 0;
    *count = (unsigned long)tok_encode(tokenizer, text, (int)length, ids, (int)length);
    free(ids);
    return 1;
}

static int emit_single_text(const char *input_type, const char *contents,
                            const char *title, Buffer *output, Tok *tokenizer) {
    unsigned long chars = utf8_char_count(contents);
    unsigned long exact_tokens = 0;
    if (tokenizer && !count_model_tokens(tokenizer, contents, &exact_tokens))
        return 0;
    return buf_put(output, "{\"ok\":true,\"input_type\":") &&
           buf_json_string(output, input_type) &&
           (!title || !*title || (buf_put(output, ",\"title\":") &&
                                  buf_json_string(output, title))) &&
           buf_put(output, ",\"text_layer\":true,\"page_count\":1,\"page_start\":1,\"page_end\":1,\"pages\":[{\"index\":1,\"text_chars\":") &&
           buf_printf(output, "%lu", chars) &&
           (!tokenizer || buf_printf(output, ",\"tokens\":%lu", exact_tokens)) &&
           buf_put(output, ",\"has_raster_figure\":false,\"text\":") &&
           buf_json_string(output, contents) &&
           buf_put(output, "}],\"text\":") &&
           buf_json_string(output, contents) &&
           (!tokenizer || buf_printf(output, ",\"tokens\":%lu", exact_tokens)) &&
           buf_printf(output, ",\"tokens_estimate\":%lu}\n", estimate_tokens(contents));
}

static int emit_native_text(Buffer *text, Buffer *output, Tok *tokenizer) {
    return emit_single_text("text/plain", text->data ? text->data : "", NULL,
                            output, tokenizer);
}

#ifndef SAMOSA_EXTRACT_NO_PDFIUM
/* Cheap, bounded page inspection. Geometry is normalized in rendered-page
   coordinates, including CropBox/rotation and nested form transforms. These
   are routing signals, not a claim to recognize arbitrary image contents. */
#define INSPECT_GRID 32
#define INSPECT_IMAGES 128
typedef struct { double x0, y0, x1, y1; } InspectRect;
typedef struct {
    FPDF_PAGE page;
    InspectRect images[INSPECT_IMAGES];
    int image_count, objects, incomplete, useful, bad;
    unsigned char image_cells[INSPECT_GRID * INSPECT_GRID];
    unsigned char text_cells[INSPECT_GRID * INSPECT_GRID];
    double coverage, quality;
    int blank, needs_ocr, region;
    InspectRect crop;
    const char *kind, *reason;
} PageInspection;

static FS_MATRIX inspect_matrix(FS_MATRIX p, FS_MATRIX q) {
    FS_MATRIX r = {p.a*q.a+p.c*q.b, p.b*q.a+p.d*q.b,
                   p.a*q.c+p.c*q.d, p.b*q.c+p.d*q.d,
                   p.a*q.e+p.c*q.f+p.e, p.b*q.e+p.d*q.f+p.f};
    return r;
}

static int inspect_rect(FPDF_PAGE page, FS_MATRIX parent, double l, double b,
                        double r, double t, InspectRect *out) {
    out->x0 = out->y0 = 1; out->x1 = out->y1 = 0;
    for (int i = 0; i < 4; ++i) {
        double x = i & 1 ? r : l, y = i & 2 ? t : b;
        double px = parent.a*x+parent.c*y+parent.e, py = parent.b*x+parent.d*y+parent.f;
        int dx, dy;
        if (!isfinite(px) || !isfinite(py) ||
            !FPDF_PageToDevice(page, 0, 0, 4096, 4096, 0, px, py, &dx, &dy)) return 0;
        double nx = dx / 4096.0, ny = dy / 4096.0;
        if (nx < out->x0) out->x0 = nx;
        if (nx > out->x1) out->x1 = nx;
        if (ny < out->y0) out->y0 = ny;
        if (ny > out->y1) out->y1 = ny;
    }
    if (out->x0 < 0) out->x0 = 0;
    if (out->y0 < 0) out->y0 = 0;
    if (out->x1 > 1) out->x1 = 1;
    if (out->y1 > 1) out->y1 = 1;
    return out->x1 > out->x0 && out->y1 > out->y0;
}

static void inspect_mark(unsigned char *grid, InspectRect r, int padding) {
    int l = (int)(r.x0 * INSPECT_GRID) - padding, t = (int)(r.y0 * INSPECT_GRID) - padding;
    int right = (int)(r.x1 * INSPECT_GRID) + padding, bottom = (int)(r.y1 * INSPECT_GRID) + padding;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (right >= INSPECT_GRID) right = INSPECT_GRID - 1;
    if (bottom >= INSPECT_GRID) bottom = INSPECT_GRID - 1;
    for (int y = t; y <= bottom; ++y)
        for (int x = l; x <= right; ++x) grid[y * INSPECT_GRID + x] = 1;
}

static void inspect_object(PageInspection *s, FPDF_PAGEOBJECT object,
                           FS_MATRIX parent, int depth) {
    if (!object || depth > 16 || ++s->objects > 4096) { s->incomplete = 1; return; }
    int type = FPDFPageObj_GetType(object);
    if (type == FPDF_PAGEOBJ_FORM) {
        FS_MATRIX local;
        int count = FPDFFormObj_CountObjects(object);
        if (count < 0 || !FPDFPageObj_GetMatrix(object, &local)) { s->incomplete = 1; return; }
        FS_MATRIX transform = inspect_matrix(parent, local);
        for (int i = 0; i < count && s->objects <= 4096; ++i)
            inspect_object(s, FPDFFormObj_GetObject(object, (unsigned long)i), transform, depth + 1);
    } else if (type == FPDF_PAGEOBJ_IMAGE) {
        float l, b, r, t;
        InspectRect box;
        if (!FPDFPageObj_GetBounds(object, &l, &b, &r, &t)) { s->incomplete = 1; return; }
        if (!inspect_rect(s->page, parent, l, b, r, t, &box)) return;
        if (s->image_count >= INSPECT_IMAGES) { s->incomplete = 1; return; }
        s->images[s->image_count++] = box;
        inspect_mark(s->image_cells, box, 0);
    }
}

/* Only a completely white preview is classified blank. A failed preview or
   any visible mark stays eligible for OCR. Never render healthy digital text
   merely to classify it. */
static int inspect_blank(FPDF_PAGE page) {
    FPDF_BITMAP bitmap = FPDFBitmap_Create(256, 256, 0);
    if (!bitmap) return 0;
    FPDFBitmap_FillRect(bitmap, 0, 0, 256, 256, FPDF_ARGB(255,255,255,255));
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, 256, 256, 0, FPDF_ANNOT);
    const unsigned char *pixels = FPDFBitmap_GetBuffer(bitmap);
    int stride = FPDFBitmap_GetStride(bitmap), blank = pixels && stride >= 1024;
    for (int y = 0; blank && y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const unsigned char *p = pixels + (size_t)y * stride + x * 4;
            if (p[0] < 253 || p[1] < 253 || p[2] < 253) { blank = 0; break; }
        }
    FPDFBitmap_Destroy(bitmap);
    return blank;
}

static int inspect_space(unsigned int c) {
    return c == 9 || c == 10 || c == 13 || c == 32 || c == 0xa0 ||
           (c >= 0x2000 && c <= 0x200b) || c == 0x3000;
}

static void inspect_page(FPDF_PAGE page, FPDF_TEXTPAGE text_page, int chars,
                         PageInspection *s) {
    memset(s, 0, sizeof(*s)); s->page = page;
    s->crop = (InspectRect){0,0,1,1};
    FS_MATRIX identity = {1,0,0,1,0,0};
    int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count && s->objects <= 4096; ++i)
        inspect_object(s, FPDFPage_GetObject(page, i), identity, 0);
    if (count < 0 || chars > 50000) s->incomplete = 1;
    for (int i = 0; i < chars && i < 50000; ++i) {
        unsigned int c = FPDFText_GetUnicode(text_page, i);
        if (inspect_space(c)) continue;
        if (c < 32 || c == 0xfffd || (c >= 0xe000 && c <= 0xf8ff) ||
            (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff || (c >= 0x7f && c < 0xa0)) { s->bad++; continue; }
        s->useful++;
        double l, r, b, t;
        InspectRect box;
        if (FPDFText_GetCharBox(text_page, i, &l, &r, &b, &t) &&
            inspect_rect(page, identity, l, b, r, t, &box)) inspect_mark(s->text_cells, box, 1);
    }
    s->quality = s->useful + s->bad ? (double)s->useful / (s->useful + s->bad) : 0;
    int image_cells = 0;
    for (int i = 0; i < INSPECT_GRID * INSPECT_GRID; ++i) image_cells += s->image_cells[i];
    s->coverage = image_cells / (double)(INSPECT_GRID * INSPECT_GRID);
    int healthy = s->useful > 0 && s->quality >= 0.85;
    s->blank = !s->incomplete && !s->useful && !s->bad && inspect_blank(page);
    s->kind = s->blank ? "blank" : healthy ? "digital_text" : "visual_only";
    s->reason = s->blank ? "blank_preview" : healthy ? "usable_text_layer" : "no_usable_text";
    s->needs_ocr = !s->blank && !healthy;
    if (s->bad && !healthy) { s->kind = "damaged_text"; s->reason = "unreliable_unicode"; }
    InspectRect uncovered = {1,1,0,0};
    for (int i = 0; healthy && i < s->image_count; ++i) {
        InspectRect box = s->images[i];
        double area = (box.x1-box.x0)*(box.y1-box.y0);
        if (area < 0.12) continue; /* Small logos do not justify page OCR. */
        unsigned char cells[INSPECT_GRID * INSPECT_GRID] = {0};
        inspect_mark(cells, box, 0);
        int covered = 0, total = 0;
        for (int j = 0; j < INSPECT_GRID * INSPECT_GRID; ++j)
            if (cells[j]) { total++; covered += s->text_cells[j]; }
        if (total && (double)covered / total >= 0.45 && s->useful >= 50) continue;
        if (box.x0 < uncovered.x0) uncovered.x0 = box.x0;
        if (box.y0 < uncovered.y0) uncovered.y0 = box.y0;
        if (box.x1 > uncovered.x1) uncovered.x1 = box.x1;
        if (box.y1 > uncovered.y1) uncovered.y1 = box.y1;
        s->needs_ocr = 1; s->kind = "mixed"; s->reason = "image_region_without_text";
    }
    /* Several smaller scan tiles are still a scan, but total image coverage is
       not enough evidence by itself: a tiled white background can sit behind
       a completely usable native text layer. Measure the union of image cells
       that are not covered by usable text before escalating. */
    int aggregate_total = 0, aggregate_text_covered = 0;
    for (int j = 0; j < INSPECT_GRID * INSPECT_GRID; ++j) {
        if (s->image_cells[j]) {
            aggregate_total++;
            if (s->text_cells[j]) aggregate_text_covered++;
        }
    }
    double aggregate_text_ratio = aggregate_total
        ? (double)aggregate_text_covered / aggregate_total : 0.0;
    if (healthy && !s->needs_ocr && s->coverage >= 0.35 && s->image_count > 1 &&
        aggregate_total > 0 && aggregate_text_ratio < 0.45) {
        s->needs_ocr = 1; s->kind = "mixed"; s->reason = "aggregate_image_region_without_text";
        uncovered = (InspectRect){1,1,0,0};
        for (int j = 0; j < INSPECT_GRID * INSPECT_GRID; ++j) {
            if (!s->image_cells[j] || s->text_cells[j]) continue;
            double x0 = (double)(j % INSPECT_GRID) / INSPECT_GRID;
            double y0 = (double)(j / INSPECT_GRID) / INSPECT_GRID;
            double x1 = (double)(j % INSPECT_GRID + 1) / INSPECT_GRID;
            double y1 = (double)(j / INSPECT_GRID + 1) / INSPECT_GRID;
            if (x0 < uncovered.x0) uncovered.x0 = x0;
            if (y0 < uncovered.y0) uncovered.y0 = y0;
            if (x1 > uncovered.x1) uncovered.x1 = x1;
            if (y1 > uncovered.y1) uncovered.y1 = y1;
        }
        if (uncovered.x1 <= uncovered.x0 || uncovered.y1 <= uncovered.y0)
            uncovered = (InspectRect){0,0,1,1};
    }
    if (healthy && s->needs_ocr && uncovered.x1 > uncovered.x0) {
        s->region = 1; s->crop = uncovered;
    }
    if (s->incomplete) {
        s->needs_ocr = 1; s->blank = 0; s->region = 0;
        s->reason = "inspection_limit";
    }
}

static int emit_inspection(Buffer *out, const PageInspection *s) {
    return buf_printf(out,
        ",\"inspection\":{\"version\":1,\"image_count\":%d,\"image_coverage\":%.4f,"
        "\"text_quality\":%.4f,\"usable_chars\":%d,\"suspicious_chars\":%d,"
        "\"blank\":%s,\"incomplete\":%s,\"kind\":\"%s\",\"reason\":\"%s\","
        "\"needs_ocr\":%s,\"ocr_region\":%s,\"ocr_bounds\":[%.5f,%.5f,%.5f,%.5f]}",
        s->image_count, s->coverage, s->quality, s->useful, s->bad,
        s->blank ? "true" : "false", s->incomplete ? "true" : "false", s->kind, s->reason,
        s->needs_ocr ? "true" : "false", s->region ? "true" : "false",
        s->crop.x0, s->crop.y0, s->crop.x1, s->crop.y1);
}

static const char *pdf_error(unsigned long error) {
    switch (error) {
    case FPDF_ERR_FILE: return "pdf_file_error";
    case FPDF_ERR_FORMAT: return "pdf_malformed";
    case FPDF_ERR_PASSWORD: return "pdf_encrypted";
    case FPDF_ERR_SECURITY: return "pdf_unsupported_security";
    case FPDF_ERR_PAGE: return "pdf_page_error";
    default: return "pdf_load_failed";
    }
}
#endif

static unsigned long estimate_tokens(const char *text) {
    unsigned long count = 0;
    int in_word = 0;
    for (; *text; ++text) {
        if ((unsigned char)*text <= ' ') in_word = 0;
        else if (!in_word) { ++count; in_word = 1; }
    }
    return count;
}

#ifndef SAMOSA_EXTRACT_NO_PDFIUM
static int render_ppm(FPDF_DOCUMENT document, int page_number,
                      const char *output_path, int ocr, const double *crop,
                      const char **error) {
    FPDF_PAGE page = NULL;
    FPDF_BITMAP bitmap = NULL;
    unsigned char *rgb = NULL;
    unsigned char *pixels;
    float page_width, page_height, scale;
    int width, height, stride, fd = -1, y, x;
    char header[64];
    int header_len;
    int output_created = 0;

    if (page_number < 1 || page_number > FPDF_GetPageCount(document)) {
        *error = "page_out_of_range";
        return 0;
    }
    page = FPDF_LoadPage(document, page_number - 1);
    if (!page) {
        *error = "pdf_page_error";
        return 0;
    }
    page_width = FPDF_GetPageWidthF(page);
    page_height = FPDF_GetPageHeightF(page);
    if (page_width <= 0 || page_height <= 0) {
        *error = "pdf_page_error";
        goto done;
    }
    int long_edge = ocr ? 2000 : RENDER_LONG_EDGE;
    scale = (float)long_edge / (page_width > page_height ? page_width : page_height);
    width = (int)(page_width * scale + 0.5f);
    height = (int)(page_height * scale + 0.5f);
    if (width < 1 || height < 1 || width > long_edge || height > long_edge ||
        (uintmax_t)width * (uintmax_t)height > (uintmax_t)long_edge * long_edge) {
        *error = "render_size_limit";
        goto done;
    }
    bitmap = FPDFBitmap_Create(width, height, 0);
    if (!bitmap) {
        *error = "out_of_memory";
        goto done;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, FPDF_ARGB(255, 255, 255, 255));
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT);
    int left = 0, top = 0, right = width, bottom = height;
    if (crop) {
        /* A two-pixel margin protects glyphs at detected region edges. */
        left = (int)(crop[0] * width) - 2; top = (int)(crop[1] * height) - 2;
        right = (int)(crop[2] * width + 0.999) + 2; bottom = (int)(crop[3] * height + 0.999) + 2;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > width) right = width;
        if (bottom > height) bottom = height;
    }
    pixels = FPDFBitmap_GetBuffer(bitmap);
    stride = FPDFBitmap_GetStride(bitmap);
    if (!pixels || stride < width * 4) {
        *error = "render_failed";
        goto done;
    }
    rgb = malloc((size_t)width * 3);
    if (!rgb) {
        *error = "out_of_memory";
        goto done;
    }
    fd = open(output_path, O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
              , 0600);
    if (fd < 0) {
        *error = (errno == EEXIST) ? "output_exists" : "output_unavailable";
        goto done;
    }
    output_created = 1;
    header_len = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", right - left, bottom - top);
    if (header_len < 0 || !write_all(fd, header, (size_t)header_len)) {
        *error = "output_write_failed";
        goto done;
    }
    for (y = top; y < bottom; ++y) {
        const unsigned char *row = pixels + (size_t)y * stride;
        for (x = left; x < right; ++x) {
            rgb[(x - left) * 3] = row[x * 4 + 2];
            rgb[(x - left) * 3 + 1] = row[x * 4 + 1];
            rgb[(x - left) * 3 + 2] = row[x * 4];
        }
        if (!write_all(fd, rgb, (size_t)(right - left) * 3)) {
            *error = "output_write_failed";
            goto done;
        }
    }
done:
    if (fd >= 0 && close(fd) != 0 && !*error)
        *error = "output_write_failed";
    if (*error && output_created)
        unlink(output_path);
    free(rgb);
    if (bitmap)
        FPDFBitmap_Destroy(bitmap);
    if (page)
        FPDF_ClosePage(page);
    return !*error;
}
#endif

static void usage(void) {
    fputs("usage: samosa-extract --json FILE\n"
          "       samosa-extract --json FILE --tokenizer tokenizer.json\n"
          "       samosa-extract --probe FILE\n"
          "       samosa-extract --json-pages FILE.pdf START COUNT\n"
          "       samosa-extract --render-ppm FILE.pdf PAGE OUTPUT.ppm\n"
          "       samosa-extract --render-ocr-ppm FILE.pdf PAGE OUTPUT.ppm [LEFT TOP RIGHT BOTTOM]\n"
          "       samosa-extract --version\n", stderr);
}

int main(int argc, char **argv) {
    InputFile input = { .fd = -1, .length = 0 };
#ifndef SAMOSA_EXTRACT_NO_PDFIUM
    FPDF_FILEACCESS access;
    FPDF_DOCUMENT document = NULL;
    Buffer pages = {0};
#endif
    Buffer document_text = {0}, output = {0};
    const char *error = NULL, *input_path, *render_path = NULL;
#ifndef SAMOSA_EXTRACT_NO_PDFIUM
    int page_count, page_index, text_layer = 0;
    int page_end = 0, emitted_pages = 0;
    unsigned long document_tokens = 0;
#endif
    int page_start = 1, page_limit = 0;
    int render_page = 0;
    int render_ocr = 0, crop_set = 0;
    double crop[4] = {0,0,1,1};
    int probe_only = 0;
    char *end = NULL;
    unsigned char prefix[4096];
    size_t prefix_length = 0;
    Tok tokenizer;
    Tok *tokenizer_ptr = NULL;
    const char *tokenizer_path = NULL;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts(EXTRACT_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--json") == 0) {
        input_path = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--probe") == 0) {
        input_path = argv[2];
        probe_only = 1;
    } else if (argc == 5 && strcmp(argv[1], "--json-pages") == 0) {
        long start, count;
        errno = 0;
        start = strtol(argv[3], &end, 10);
        if (errno || !end || *end || start < 1 || start > INT_MAX) {
            usage();
            return 64;
        }
        errno = 0;
        count = strtol(argv[4], &end, 10);
        if (errno || !end || *end || count < 1 || count > 5) {
            usage();
            return 64;
        }
        input_path = argv[2];
        page_start = (int)start;
        page_limit = (int)count;
    } else if (argc == 5 && strcmp(argv[1], "--json") == 0 &&
               strcmp(argv[3], "--tokenizer") == 0) {
        input_path = argv[2];
        tokenizer_path = argv[4];
    } else if ((argc == 5 && strcmp(argv[1], "--render-ppm") == 0) ||
               ((argc == 5 || argc == 9) && strcmp(argv[1], "--render-ocr-ppm") == 0)) {
        long page;
        errno = 0;
        page = strtol(argv[3], &end, 10);
        if (errno || !end || *end || page < 1 || page > INT_MAX) {
            usage();
            return 64;
        }
        input_path = argv[2];
        render_page = (int)page;
        render_path = argv[4];
        render_ocr = !strcmp(argv[1], "--render-ocr-ppm");
        if (argc == 9) {
            for (int i = 0; i < 4; ++i) {
                errno = 0; crop[i] = strtod(argv[i + 5], &end);
                if (errno || end == argv[i + 5] || *end || !isfinite(crop[i]) || crop[i] < 0 || crop[i] > 1) {
                    usage(); return 64;
                }
            }
            if (crop[2] <= crop[0] || crop[3] <= crop[1]) { usage(); return 64; }
            crop_set = 1;
        }
    } else {
        usage();
        return 64;
    }
    if (!set_limits()) {
        put_error("sandbox_limit_unavailable");
        return 70;
    }
    if (!open_input(input_path, &input, &error)) {
        put_error(error);
        return 65;
    }
    if (tokenizer_path) {
        tok_load(&tokenizer, tokenizer_path);
        tokenizer_ptr = &tokenizer;
    }
    signal(SIGALRM, on_alarm);
    alarm(WALL_SECONDS);

    if (!read_prefix(&input, prefix, sizeof(prefix), &prefix_length)) {
        put_error("file_unavailable");
        close(input.fd);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 65;
    }
    if (!render_page && !page_limit && !has_ascii_prefix(prefix, prefix_length, "%pdf-")) {
        if (has_ascii_prefix(prefix, prefix_length, "pk\003\004") ||
            has_ascii_prefix(prefix, prefix_length, "pk\005\006") ||
            has_ascii_prefix(prefix, prefix_length, "pk\007\008")) {
            SamosaDocxResult docx = {0};
            if (extract_docx(&input, &docx, &error)) {
                int emitted = probe_only
                    ? printf("{\"ok\":true,\"input_type\":\"application/vnd.openxmlformats-officedocument.wordprocessingml.document\",\"text_chars\":%lu,\"entry_count\":%u}\n",
                             docx.text_chars, docx.entry_count) > 0
                    : emit_single_text(
                          "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                          docx.text, NULL, &output, tokenizer_ptr);
                if (emitted && !probe_only) fputs(output.data, stdout);
                samosa_docx_result_free(&docx);
                free(output.data);
                if (emitted) {
                    close(input.fd);
                    alarm(0);
                    if (tokenizer_ptr) tok_free(tokenizer_ptr);
                    return 0;
                }
                error = "output_too_large";
            }
            samosa_docx_result_free(&docx);
            put_error(error ? error : "output_too_large");
        } else if (probe_only) {
            put_error("unsupported_probe_type");
        } else if (samosa_html_sniff(prefix, prefix_length)) {
            SamosaHtmlResult html = {0};
            Buffer readable = {0};
            if (extract_html(&input, &html, &error) &&
                (html.title[0]
                    ? (strlen(html.title) + 2 + html.text_bytes <= MAX_NATIVE_TEXT_BYTES &&
                       buf_put(&readable, html.title) &&
                       buf_put(&readable, "\n\n") &&
                       buf_putn(&readable, html.text, html.text_bytes))
                    : buf_putn(&readable, html.text, html.text_bytes)) &&
                emit_single_text("text/html", readable.data, html.title,
                                 &output, tokenizer_ptr)) {
                fputs(output.data, stdout);
                samosa_html_result_free(&html);
                free(readable.data);
                free(output.data);
                close(input.fd);
                alarm(0);
                if (tokenizer_ptr) tok_free(tokenizer_ptr);
                return 0;
            }
            samosa_html_result_free(&html);
            free(readable.data);
            put_error(error ? error : "output_too_large");
        } else if (has_ascii_prefix(prefix, prefix_length, "{\\rtf")) {
            put_error("rtf_unsupported");
        } else if (extract_native_text(&input, &document_text, &error) &&
                   emit_native_text(&document_text, &output, tokenizer_ptr)) {
            fputs(output.data, stdout);
            free(output.data);
            free(document_text.data);
            close(input.fd);
            alarm(0);
            if (tokenizer_ptr) tok_free(tokenizer_ptr);
            return 0;
        } else {
            put_error(error ? error : "output_too_large");
        }
        free(output.data);
        free(document_text.data);
        close(input.fd);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 65;
    }
    if ((render_page || page_limit) && !has_ascii_prefix(prefix, prefix_length, "%pdf-")) {
        put_error("not_pdf");
        close(input.fd);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 65;
    }

#ifdef SAMOSA_EXTRACT_NO_PDFIUM
    (void)page_start;
    (void)render_path;
    (void)render_ocr; (void)crop_set; (void)crop;
    put_error("pdf_extractor_unavailable");
    close(input.fd);
    if (tokenizer_ptr) tok_free(tokenizer_ptr);
    return 65;
#else
    memset(&access, 0, sizeof(access));
    access.m_FileLen = input.length;
    access.m_GetBlock = read_block;
    access.m_Param = &input;
    FPDF_InitLibrary();
    document = FPDF_LoadCustomDocument(&access, NULL);
    if (!document) {
        put_error(pdf_error(FPDF_GetLastError()));
        FPDF_DestroyLibrary();
        close(input.fd);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 65;
    }
    page_count = FPDF_GetPageCount(document);
    if (page_count < 0 || page_count > 10000) {
        put_error("page_count_limit");
        FPDF_CloseDocument(document);
        FPDF_DestroyLibrary();
        close(input.fd);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 65;
    }
    if (page_limit) {
        if (page_start > page_count) {
            put_error("page_out_of_range");
            FPDF_CloseDocument(document);
            FPDF_DestroyLibrary();
            close(input.fd);
            if (tokenizer_ptr) tok_free(tokenizer_ptr);
            return 65;
        }
        page_end = page_start - 1 + page_limit;
        if (page_end > page_count)
            page_end = page_count;
    } else {
        page_end = page_count;
    }
    if (render_page) {
        if (!render_ppm(document, render_page, render_path, render_ocr, crop_set ? crop : NULL, &error)) {
            put_error(error);
            FPDF_CloseDocument(document);
            FPDF_DestroyLibrary();
            close(input.fd);
            if (tokenizer_ptr) tok_free(tokenizer_ptr);
            return 65;
        }
        printf("{\"ok\":true,\"page\":%d,\"format\":\"image/x-portable-pixmap\"}\n", render_page);
        FPDF_CloseDocument(document);
        FPDF_DestroyLibrary();
        close(input.fd);
        alarm(0);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 0;
    }
    if (!buf_put(&pages, "[")) error = "output_too_large";
    for (page_index = page_limit ? page_start - 1 : 0;
         !error && page_index < page_end; ++page_index) {
        FPDF_PAGE page = FPDF_LoadPage(document, page_index);
        FPDF_TEXTPAGE text_page;
        unsigned short *utf16 = NULL;
        Buffer page_text = {0};
        int chars = 0, written = 0;
        PageInspection inspection;
        unsigned long page_tokens = 0;
        if (!page) { error = "pdf_page_error"; break; }
        text_page = FPDFText_LoadPage(page);
        if (!text_page) { FPDF_ClosePage(page); error = "pdf_page_error"; break; }
        chars = FPDFText_CountChars(text_page);
        if (chars < 0 || chars > MAX_PAGE_CHARS) {
            FPDFText_ClosePage(text_page); FPDF_ClosePage(page); error = "page_text_limit"; break;
        }
        utf16 = calloc((size_t)chars + 1, sizeof(*utf16));
        if (!utf16) { FPDFText_ClosePage(text_page); FPDF_ClosePage(page); error = "out_of_memory"; break; }
        if (chars) written = FPDFText_GetText(text_page, 0, chars, utf16);
        if (written < 0 || !utf16_to_utf8(utf16, written, &page_text)) error = "output_too_large";
        if (!error && tokenizer_ptr && !count_model_tokens(tokenizer_ptr,
                                                             page_text.data ? page_text.data : "",
                                                             &page_tokens))
            error = "token_count_failed";
        inspect_page(page, text_page, chars, &inspection);
        if (!error && !buf_printf(&pages, "%s{\"index\":%d,\"text_chars\":%d",
                                  emitted_pages ? "," : "", page_index + 1, chars)) error = "output_too_large";
        if (!error && tokenizer_ptr && !buf_printf(&pages, ",\"tokens\":%lu", page_tokens)) error = "output_too_large";
        if (!error && !emit_inspection(&pages, &inspection)) error = "output_too_large";
        if (!error && !buf_printf(&pages, ",\"has_raster_figure\":%s,\"text\":",
                                  inspection.image_count ? "true" : "false")) error = "output_too_large";
        if (!error && !buf_json_string(&pages, page_text.data ? page_text.data : "")) error = "output_too_large";
        if (!error && !buf_put(&pages, "}")) error = "output_too_large";
        if (!error && page_text.len) text_layer = 1;
        if (!error && emitted_pages && !buf_put(&document_text, "\n\n")) error = "output_too_large";
        if (!error && !buf_putn(&document_text, page_text.data ? page_text.data : "", page_text.len)) error = "output_too_large";
        emitted_pages++;
        free(page_text.data);
        free(utf16);
        FPDFText_ClosePage(text_page);
        FPDF_ClosePage(page);
    }
    if (!error && !buf_put(&pages, "]")) error = "output_too_large";
    if (!error && tokenizer_ptr && !count_model_tokens(tokenizer_ptr,
                                                         document_text.data ? document_text.data : "",
                                                         &document_tokens))
        error = "token_count_failed";
    if (!error && (!buf_printf(&output, "{\"ok\":true,\"text_layer\":%s,\"page_count\":%d,\"page_start\":%d,\"page_end\":%d,\"pages\":",
                                  text_layer ? "true" : "false", page_count,
                                  page_limit ? page_start : (page_count ? 1 : 0), page_end) ||
                   !buf_putn(&output, pages.data, pages.len) ||
                   !buf_put(&output, ",\"text\":") ||
                   !buf_json_string(&output, document_text.data ? document_text.data : "") ||
                   (tokenizer_ptr && !buf_printf(&output, ",\"tokens\":%lu", document_tokens)) ||
                   !buf_printf(&output, ",\"tokens_estimate\":%lu}\n", estimate_tokens(document_text.data ? document_text.data : ""))))
        error = "output_too_large";
    if (error) put_error(error);
    else fputs(output.data, stdout);
    free(output.data);
    free(document_text.data);
    free(pages.data);
    FPDF_CloseDocument(document);
    FPDF_DestroyLibrary();
    close(input.fd);
    alarm(0);
    if (tokenizer_ptr) tok_free(tokenizer_ptr);
    return error ? 65 : 0;
#endif
}
