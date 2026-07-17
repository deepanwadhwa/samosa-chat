/*
 * samosa-extract -- short-lived PDF text extractor.
 *
 * This deliberately links PDFium outside qwen36b.  It accepts one local PDF,
 * holds the opened descriptor for PDFium's lifetime (no pathname TOCTOU), and
 * writes one JSON object to stdout.  A caller must additionally impose a
 * wall-clock timeout before spawning it; this program enforces CPU/address
 * limits itself so a malformed document cannot take down the resident model.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#include "fpdf_edit.h"
#include "fpdf_text.h"
#include "fpdfview.h"
#include "tok.h"

#define DEFAULT_MAX_BYTES (20UL * 1024UL * 1024UL)
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

static unsigned long max_input_bytes(void) {
    const char *value = getenv("SAMOSA_EXTRACT_MAX_BYTES");
    char *end = NULL;
    unsigned long parsed;
    if (!value || !*value)
        return DEFAULT_MAX_BYTES;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > DEFAULT_MAX_BYTES)
        return DEFAULT_MAX_BYTES;
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
    if (st.st_size < 5 || (uintmax_t)st.st_size > max_input_bytes() ||
        (uintmax_t)st.st_size > ULONG_MAX) {
        close(input->fd);
        *error = "file_too_large";
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
    if (!read_block(input, 0, raw, input->length) || !valid_utf8(raw, input->length)) {
        free(raw);
        *error = "text_invalid_utf8";
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

static int emit_native_text(Buffer *text, Buffer *output, Tok *tokenizer) {
    unsigned long chars = utf8_char_count(text->data ? text->data : "");
    unsigned long exact_tokens = 0;
    const char *contents = text->data ? text->data : "";
    if (tokenizer && !count_model_tokens(tokenizer, contents, &exact_tokens))
        return 0;
    return buf_put(output, "{\"ok\":true,\"input_type\":\"text/plain\",\"text_layer\":true,\"pages\":[{\"index\":1,\"text_chars\":") &&
           buf_printf(output, "%lu", chars) &&
           (!tokenizer || buf_printf(output, ",\"tokens\":%lu", exact_tokens)) &&
           buf_put(output, ",\"has_raster_figure\":false,\"text\":") &&
           buf_json_string(output, contents) &&
           buf_put(output, "}],\"text\":") &&
           buf_json_string(output, contents) &&
           (!tokenizer || buf_printf(output, ",\"tokens\":%lu", exact_tokens)) &&
           buf_printf(output, ",\"tokens_estimate\":%lu}\n", estimate_tokens(contents));
}

static int page_has_raster_figure(FPDF_PAGE page) {
    int i, count = FPDFPage_CountObjects(page);
    for (i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
        if (object && FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_IMAGE)
            return 1;
    }
    return 0;
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

static unsigned long estimate_tokens(const char *text) {
    unsigned long count = 0;
    int in_word = 0;
    for (; *text; ++text) {
        if ((unsigned char)*text <= ' ') in_word = 0;
        else if (!in_word) { ++count; in_word = 1; }
    }
    return count;
}

static int render_ppm(FPDF_DOCUMENT document, int page_number,
                      const char *output_path, const char **error) {
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
    scale = (float)RENDER_LONG_EDGE / (page_width > page_height ? page_width : page_height);
    width = (int)(page_width * scale + 0.5f);
    height = (int)(page_height * scale + 0.5f);
    if (width < 1 || height < 1 || width > RENDER_LONG_EDGE || height > RENDER_LONG_EDGE ||
        (uintmax_t)width * (uintmax_t)height > MAX_RENDER_PIXELS) {
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
    header_len = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width, height);
    if (header_len < 0 || !write_all(fd, header, (size_t)header_len)) {
        *error = "output_write_failed";
        goto done;
    }
    for (y = 0; y < height; ++y) {
        const unsigned char *row = pixels + (size_t)y * stride;
        for (x = 0; x < width; ++x) {
            rgb[x * 3] = row[x * 4 + 2];
            rgb[x * 3 + 1] = row[x * 4 + 1];
            rgb[x * 3 + 2] = row[x * 4];
        }
        if (!write_all(fd, rgb, (size_t)width * 3)) {
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

static void usage(void) {
    fputs("usage: samosa-extract --json FILE\n"
          "       samosa-extract --json FILE --tokenizer tokenizer.json\n"
          "       samosa-extract --render-ppm FILE.pdf PAGE OUTPUT.ppm\n", stderr);
}

int main(int argc, char **argv) {
    InputFile input = { .fd = -1, .length = 0 };
    FPDF_FILEACCESS access;
    FPDF_DOCUMENT document = NULL;
    Buffer pages = {0}, document_text = {0}, output = {0};
    const char *error = NULL, *input_path, *render_path = NULL;
    int page_count, page_index, text_layer = 0;
    unsigned long document_tokens = 0;
    int render_page = 0;
    char *end = NULL;
    unsigned char prefix[64];
    size_t prefix_length = 0;
    Tok tokenizer;
    Tok *tokenizer_ptr = NULL;
    const char *tokenizer_path = NULL;

    if (argc == 3 && strcmp(argv[1], "--json") == 0) {
        input_path = argv[2];
    } else if (argc == 5 && strcmp(argv[1], "--json") == 0 &&
               strcmp(argv[3], "--tokenizer") == 0) {
        input_path = argv[2];
        tokenizer_path = argv[4];
    } else if (argc == 5 && strcmp(argv[1], "--render-ppm") == 0) {
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
    if (!render_page && !has_ascii_prefix(prefix, prefix_length, "%pdf-")) {
        if (has_ascii_prefix(prefix, prefix_length, "pk\003\004")) {
            put_error("docx_extractor_unavailable");
        } else if (has_ascii_prefix(prefix, prefix_length, "<html") ||
                   has_ascii_prefix(prefix, prefix_length, "<!doctype html")) {
            put_error("html_extractor_unavailable");
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
    if (render_page && !has_ascii_prefix(prefix, prefix_length, "%pdf-")) {
        put_error("not_pdf");
        close(input.fd);
        if (tokenizer_ptr) tok_free(tokenizer_ptr);
        return 65;
    }

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
    if (render_page) {
        if (!render_ppm(document, render_page, render_path, &error)) {
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
    for (page_index = 0; !error && page_index < page_count; ++page_index) {
        FPDF_PAGE page = FPDF_LoadPage(document, page_index);
        FPDF_TEXTPAGE text_page;
        unsigned short *utf16 = NULL;
        Buffer page_text = {0};
        int chars = 0, written = 0, has_raster;
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
        has_raster = page_has_raster_figure(page);
        if (!error && !buf_printf(&pages, "%s{\"index\":%d,\"text_chars\":%d",
                                  page_index ? "," : "", page_index + 1, chars)) error = "output_too_large";
        if (!error && tokenizer_ptr && !buf_printf(&pages, ",\"tokens\":%lu", page_tokens)) error = "output_too_large";
        if (!error && !buf_printf(&pages, ",\"has_raster_figure\":%s,\"text\":",
                                  has_raster ? "true" : "false")) error = "output_too_large";
        if (!error && !buf_json_string(&pages, page_text.data ? page_text.data : "")) error = "output_too_large";
        if (!error && !buf_put(&pages, "}")) error = "output_too_large";
        if (!error && page_text.len) text_layer = 1;
        if (!error && page_index && !buf_put(&document_text, "\n\n")) error = "output_too_large";
        if (!error && !buf_putn(&document_text, page_text.data ? page_text.data : "", page_text.len)) error = "output_too_large";
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
    if (!error && (!buf_printf(&output, "{\"ok\":true,\"text_layer\":%s,\"pages\":", text_layer ? "true" : "false") ||
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
}
