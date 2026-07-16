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

#define DEFAULT_MAX_BYTES (20UL * 1024UL * 1024UL)
#define MAX_PAGE_CHARS 2000000
#define MAX_JSON_BYTES (16UL * 1024UL * 1024UL)
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

static void usage(void) {
    fputs("usage: samosa-extract --json FILE.pdf\n", stderr);
}

int main(int argc, char **argv) {
    InputFile input = { .fd = -1, .length = 0 };
    FPDF_FILEACCESS access;
    FPDF_DOCUMENT document = NULL;
    Buffer pages = {0}, document_text = {0}, output = {0};
    const char *error = NULL;
    int page_count, page_index, text_layer = 0;

    if (argc != 3 || strcmp(argv[1], "--json") != 0) {
        usage();
        return 64;
    }
    if (!set_limits()) {
        put_error("sandbox_limit_unavailable");
        return 70;
    }
    if (!open_input(argv[2], &input, &error)) {
        put_error(error);
        return 65;
    }
    signal(SIGALRM, on_alarm);
    alarm(WALL_SECONDS);

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
        return 65;
    }
    page_count = FPDF_GetPageCount(document);
    if (page_count < 0 || page_count > 10000) {
        put_error("page_count_limit");
        FPDF_CloseDocument(document);
        FPDF_DestroyLibrary();
        close(input.fd);
        return 65;
    }
    if (!buf_put(&pages, "[")) error = "output_too_large";
    for (page_index = 0; !error && page_index < page_count; ++page_index) {
        FPDF_PAGE page = FPDF_LoadPage(document, page_index);
        FPDF_TEXTPAGE text_page;
        unsigned short *utf16 = NULL;
        Buffer page_text = {0};
        int chars = 0, written = 0, has_raster;
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
        has_raster = page_has_raster_figure(page);
        if (!error && !buf_printf(&pages, "%s{\"index\":%d,\"text_chars\":%d,\"has_raster_figure\":%s,\"text\":",
                                  page_index ? "," : "", page_index + 1, chars,
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
    if (!error && (!buf_printf(&output, "{\"ok\":true,\"text_layer\":%s,\"pages\":", text_layer ? "true" : "false") ||
                   !buf_putn(&output, pages.data, pages.len) ||
                   !buf_put(&output, ",\"text\":") ||
                   !buf_json_string(&output, document_text.data ? document_text.data : "") ||
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
    return error ? 65 : 0;
}
