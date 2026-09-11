#include "samosa_html.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HTML_TITLE_MAX_BYTES 4096u
#define HTML_SKIP_DEPTH 32

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    int limited;
} HtmlBuffer;

static int ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int ascii_prefix_ci(const unsigned char *data, size_t length,
                           size_t at, const char *prefix) {
    size_t n = strlen(prefix);
    if (at > length || n > length - at) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = data[at + i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)prefix[i]) return 0;
    }
    return 1;
}

static size_t find_bytes(const unsigned char *data, size_t length, size_t at,
                         const char *needle) {
    size_t n = strlen(needle);
    if (!n || at > length) return length;
    for (size_t i = at; i + n <= length; ++i)
        if (!memcmp(data + i, needle, n)) return i;
    return length;
}

int samosa_html_sniff(const unsigned char *data, size_t length) {
    if (!data || !length) return 0;
    size_t at = length >= 3 && !memcmp(data, "\xef\xbb\xbf", 3) ? 3 : 0;
    for (;;) {
        while (at < length && ascii_space(data[at])) ++at;
        if (!ascii_prefix_ci(data, length, at, "<!--")) break;
        size_t end = find_bytes(data, length, at + 4, "-->");
        if (end == length) return 0;
        at = end + 3;
    }
    return ascii_prefix_ci(data, length, at, "<!doctype html") ||
           ascii_prefix_ci(data, length, at, "<html") ||
           ascii_prefix_ci(data, length, at, "<head") ||
           ascii_prefix_ci(data, length, at, "<body");
}

static int valid_utf8(const unsigned char *data, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = data[i++];
        int continuation;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) continuation = 1;
        else if (c >= 0xe0 && c <= 0xef) continuation = 2;
        else if (c >= 0xf0 && c <= 0xf4) continuation = 3;
        else return 0;
        if ((size_t)continuation > length - i) return 0;
        if (c == 0xe0 && data[i] < 0xa0) return 0;
        if (c == 0xed && data[i] >= 0xa0) return 0;
        if (c == 0xf0 && data[i] < 0x90) return 0;
        if (c == 0xf4 && data[i] >= 0x90) return 0;
        while (continuation--)
            if ((data[i++] & 0xc0) != 0x80) return 0;
    }
    return 1;
}

static size_t utf8_sequence_width(const unsigned char *data, size_t length,
                                  size_t at) {
    unsigned char c = data[at];
    size_t width;
    uint32_t code;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) { width = 2; code = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { width = 3; code = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { width = 4; code = c & 0x07; }
    else return 0;
    if (width > length - at) return 0;
    for (size_t i = 1; i < width; ++i) {
        if ((data[at + i] & 0xc0) != 0x80) return 0;
        code = (code << 6) | (data[at + i] & 0x3f);
    }
    if ((width == 2 && code < 0x80) ||
        (width == 3 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff))) ||
        (width == 4 && (code < 0x10000 || code > 0x10ffff))) return 0;
    return width;
}

int samosa_html_text_valid(const unsigned char *data, size_t length,
                           const char **error) {
    if (error) *error = NULL;
    if (!data || !length || !valid_utf8(data, length)) {
        if (error) *error = "html_invalid_utf8";
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = data[i];
        if ((c < 0x20 && !ascii_space(c)) || c == 0x7f) {
            if (error) *error = "html_binary_control";
            return 0;
        }
    }
    return 1;
}

static int buffer_reserve(HtmlBuffer *buffer, size_t extra) {
    if (extra > buffer->limit || buffer->len > buffer->limit - extra) {
        buffer->limited = 1;
        return 0;
    }
    size_t need = buffer->len + extra + 1;
    if (need <= buffer->cap) return 1;
    size_t cap = buffer->cap ? buffer->cap : 1024;
    while (cap < need) {
        if (cap > (buffer->limit + 1) / 2) {
            cap = buffer->limit + 1;
            break;
        }
        cap *= 2;
    }
    char *next = realloc(buffer->data, cap);
    if (!next) return 0;
    buffer->data = next;
    buffer->cap = cap;
    return 1;
}

static int buffer_putn(HtmlBuffer *buffer, const char *text, size_t length) {
    if (!buffer_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->len, text, length);
    buffer->len += length;
    buffer->data[buffer->len] = '\0';
    return 1;
}

static int buffer_put(HtmlBuffer *buffer, const char *text) {
    return buffer_putn(buffer, text, strlen(text));
}

static int title_putn(HtmlBuffer *buffer, const char *text, size_t length) {
    if (buffer->len >= buffer->limit) return 1;
    if (length > buffer->limit - buffer->len)
        length = buffer->limit - buffer->len;
    return buffer_putn(buffer, text, length);
}

static size_t tag_end(const unsigned char *data, size_t length, size_t at) {
    unsigned char quote = 0;
    for (size_t i = at; i < length; ++i) {
        unsigned char c = data[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return length;
}

static int tag_name(const unsigned char *data, size_t length, size_t at,
                    char out[20], int *closing) {
    *closing = 0;
    while (at < length && ascii_space(data[at])) ++at;
    if (at < length && data[at] == '/') {
        *closing = 1;
        ++at;
        while (at < length && ascii_space(data[at])) ++at;
    }
    if (at >= length || !isalpha(data[at])) {
        out[0] = '\0';
        return 0;
    }
    size_t n = 0;
    while (at < length &&
           (isalnum(data[at]) || data[at] == '-' || data[at] == ':')) {
        if (n + 1 < 20) {
            unsigned char c = data[at];
            out[n++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        }
        ++at;
    }
    out[n] = '\0';
    return n != 0;
}

static int skip_tag(const char *tag) {
    return !strcmp(tag, "script") || !strcmp(tag, "style") ||
           !strcmp(tag, "svg") || !strcmp(tag, "noscript") ||
           !strcmp(tag, "template") || !strcmp(tag, "iframe") ||
           !strcmp(tag, "object");
}

static int block_tag(const char *tag) {
    static const char *const tags[] = {
        "address", "article", "aside", "blockquote", "dd", "div", "dl",
        "dt", "figcaption", "figure", "footer", "form", "h1", "h2",
        "h3", "h4", "h5", "h6", "header", "hr", "main", "nav", "ol",
        "p", "pre", "section", "table", "tr", "ul", NULL
    };
    for (size_t i = 0; tags[i]; ++i)
        if (!strcmp(tag, tags[i])) return 1;
    return 0;
}

static size_t utf8_encode(uint32_t code, char out[4]) {
    if (code <= 0x7f) { out[0] = (char)code; return 1; }
    if (code <= 0x7ff) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        return 2;
    }
    if (code <= 0xffff && !(code >= 0xd800 && code <= 0xdfff)) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        return 3;
    }
    if (code <= 0x10ffff) {
        out[0] = (char)(0xf0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[3] = (char)(0x80 | (code & 0x3f));
        return 4;
    }
    return 0;
}

static int entity_value(const unsigned char *name, size_t length,
                        char out[8], size_t *out_length) {
    struct Named { const char *name; const char *value; };
    static const struct Named named[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""},
        {"apos", "'"}, {"nbsp", " "}, {"copy", "\xc2\xa9"},
        {"reg", "\xc2\xae"}, {"mdash", "\xe2\x80\x94"},
        {"ndash", "\xe2\x80\x93"}, {"hellip", "\xe2\x80\xa6"},
        {"laquo", "\xc2\xab"}, {"raquo", "\xc2\xbb"}, {NULL, NULL}
    };
    if (length && name[0] == '#') {
        size_t at = 1;
        unsigned base = 10;
        if (at < length && (name[at] == 'x' || name[at] == 'X')) {
            base = 16;
            ++at;
        }
        if (at == length) return 0;
        uint32_t code = 0;
        for (; at < length; ++at) {
            unsigned digit;
            if (name[at] >= '0' && name[at] <= '9') digit = name[at] - '0';
            else if (base == 16 && name[at] >= 'a' && name[at] <= 'f') digit = name[at] - 'a' + 10;
            else if (base == 16 && name[at] >= 'A' && name[at] <= 'F') digit = name[at] - 'A' + 10;
            else return 0;
            if (code > (0x10ffffu - digit) / base) return 0;
            code = code * base + digit;
        }
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            (code >= 0xd800 && code <= 0xdfff)) return 0;
        *out_length = utf8_encode(code, out);
        return *out_length != 0;
    }
    char lower[20];
    if (!length || length >= sizeof(lower)) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = name[i];
        lower[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    lower[length] = '\0';
    for (size_t i = 0; named[i].name; ++i) {
        if (!strcmp(lower, named[i].name)) {
            *out_length = strlen(named[i].value);
            memcpy(out, named[i].value, *out_length);
            return 1;
        }
    }
    return 0;
}

static void normalize_whitespace(HtmlBuffer *buffer) {
    if (!buffer->data) return;
    char *read = buffer->data;
    char *write = buffer->data;
    while (*read) {
        if (ascii_space((unsigned char)*read)) {
            int newline = 0;
            while (*read && ascii_space((unsigned char)*read)) {
                if (*read == '\n' || *read == '\r') newline = 1;
                ++read;
            }
            if (write > buffer->data && *read)
                *write++ = newline ? '\n' : ' ';
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    buffer->len = (size_t)(write - buffer->data);
}

static unsigned long utf8_chars(const char *text) {
    unsigned long count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xc0) != 0x80) ++count;
    return count;
}

void samosa_html_result_free(SamosaHtmlResult *result) {
    if (!result) return;
    free(result->text);
    free(result->title);
    memset(result, 0, sizeof(*result));
}

int samosa_html_extract(const unsigned char *data, size_t length,
                        size_t max_text_bytes, SamosaHtmlResult *result,
                        const char **error) {
    HtmlBuffer text = {.limit = max_text_bytes};
    HtmlBuffer title = {.limit = HTML_TITLE_MAX_BYTES};
    char skip[HTML_SKIP_DEPTH][20];
    int skip_depth = 0;
    int skip_overflow = 0;
    int in_title = 0;
    int in_head = 0;
    size_t at = 0;
    if (error) *error = NULL;
    if (!result || !max_text_bytes) {
        if (error) *error = "html_output_limit";
        return 0;
    }
    memset(result, 0, sizeof(*result));
    if (!samosa_html_text_valid(data, length, error)) return 0;
    if (length >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) at = 3;

    while (at < length) {
        if (data[at] == '<') {
            if (ascii_prefix_ci(data, length, at, "<!--")) {
                size_t end = find_bytes(data, length, at + 4, "-->");
                at = end == length ? length : end + 3;
                continue;
            }
            size_t end = tag_end(data, length, at + 1);
            if (end == length) {
                if (skip_depth) break;
                if (!(in_title ? title_putn(&title, "<", 1)
                               : buffer_putn(&text, "<", 1))) goto failed;
                ++at;
                continue;
            }
            char tag[20];
            int closing = 0;
            if (!tag_name(data, end, at + 1, tag, &closing)) {
                if (!skip_depth && at + 1 < length && data[at + 1] != '!' &&
                    data[at + 1] != '?') {
                    if (!(in_title ? title_putn(&title, "<", 1)
                                   : in_head ? 1 : buffer_putn(&text, "<", 1)))
                        goto failed;
                    ++at;
                    continue;
                }
                at = end + 1;
                continue;
            }
            if (skip_depth) {
                if (closing && skip_overflow && skip_tag(tag)) {
                    --skip_overflow;
                } else if (closing && !strcmp(tag, skip[skip_depth - 1])) {
                    --skip_depth;
                } else if (!closing && skip_tag(tag)) {
                    if (skip_depth < HTML_SKIP_DEPTH)
                        strcpy(skip[skip_depth++], tag);
                    else
                        ++skip_overflow;
                }
                at = end + 1;
                continue;
            }
            if (!closing && skip_tag(tag)) {
                strcpy(skip[skip_depth++], tag);
            } else if (!strcmp(tag, "title")) {
                in_title = !closing;
            } else if (!strcmp(tag, "head")) {
                in_head = !closing;
            } else if (!in_title) {
                if (!strcmp(tag, "br") || !strcmp(tag, "hr") || block_tag(tag)) {
                    if (!buffer_putn(&text, "\n", 1)) goto failed;
                }
                if (!closing && !strcmp(tag, "li")) {
                    if (!buffer_put(&text, "\n- ")) goto failed;
                } else if (closing && (!strcmp(tag, "td") || !strcmp(tag, "th"))) {
                    if (!buffer_put(&text, " | ")) goto failed;
                }
            }
            at = end + 1;
            continue;
        }
        if (skip_depth || (in_head && !in_title)) { ++at; continue; }
        if (data[at] == '&') {
            size_t semi = at + 1;
            while (semi < length && semi - at <= 32 && data[semi] != ';' &&
                   data[semi] != '<' && !ascii_space(data[semi])) ++semi;
            if (semi < length && data[semi] == ';') {
                char decoded[8];
                size_t decoded_length = 0;
                if (entity_value(data + at + 1, semi - at - 1,
                                 decoded, &decoded_length)) {
                    if (!(in_title ? title_putn(&title, decoded, decoded_length)
                                   : buffer_putn(&text, decoded, decoded_length))) goto failed;
                    at = semi + 1;
                    continue;
                }
            }
        }
        if (!(in_title ? title_putn(&title, (const char *)data + at, 1)
                       : buffer_putn(&text, (const char *)data + at, 1))) goto failed;
        ++at;
    }
    if (!text.data && !buffer_putn(&text, "", 0)) goto failed;
    if (!title.data && !buffer_putn(&title, "", 0)) goto failed;
    normalize_whitespace(&text);
    normalize_whitespace(&title);
    result->text = text.data;
    result->title = title.data;
    result->text_bytes = text.len;
    result->text_chars = utf8_chars(text.data);
    return 1;

failed:
    if (error) *error = text.limited ? "html_output_limit" : "out_of_memory";
    free(text.data);
    free(title.data);
    return 0;
}

int samosa_html_extract_replacing_invalid(
    const unsigned char *data, size_t length, size_t max_text_bytes,
    SamosaHtmlResult *result, const char **error) {
    HtmlBuffer repaired = {0};
    if (length > (SIZE_MAX - 1) / 3) {
        if (error) *error = "html_input_limit";
        return 0;
    }
    repaired.limit = length * 3;
    for (size_t at = 0; at < length;) {
        size_t width = utf8_sequence_width(data, length, at);
        if (width == 1 && data[at] < 0x20 && !ascii_space(data[at])) width = 0;
        if (width == 1 && data[at] == 0x7f) width = 0;
        if (width) {
            if (!buffer_putn(&repaired, (const char *)data + at, width)) goto failed;
            at += width;
        } else {
            if (!buffer_putn(&repaired, "\xef\xbf\xbd", 3)) goto failed;
            ++at;
        }
    }
    {
        int ok = samosa_html_extract((const unsigned char *)repaired.data,
                                     repaired.len, max_text_bytes,
                                     result, error);
        free(repaired.data);
        return ok;
    }

failed:
    if (error) *error = repaired.limited ? "html_input_limit" : "out_of_memory";
    free(repaired.data);
    return 0;
}
