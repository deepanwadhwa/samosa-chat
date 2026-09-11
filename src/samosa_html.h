#ifndef SAMOSA_HTML_H
#define SAMOSA_HTML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable, dependency-free HTML readable-text extraction shared by public
 * web reads and local document attachments. The caller owns both returned
 * strings and releases them with samosa_html_result_free(). */
typedef struct {
    char *text;
    char *title;
    size_t text_bytes;
    unsigned long text_chars;
} SamosaHtmlResult;

/* Byte sniffing is deliberately independent of filenames and declared MIME
 * types. A UTF-8 BOM, leading ASCII whitespace, and leading comments are
 * allowed before a normal HTML document root. */
int samosa_html_sniff(const unsigned char *data, size_t length);

/* HTML attachments must be valid UTF-8 and may contain only ordinary text
 * whitespace controls. This validation does not allocate. */
int samosa_html_text_valid(const unsigned char *data, size_t length,
                           const char **error);

/* Extract readable text with script/style/template/noscript/SVG contents
 * removed. max_text_bytes is a hard allocation/output bound. */
int samosa_html_extract(const unsigned char *data, size_t length,
                        size_t max_text_bytes, SamosaHtmlResult *result,
                        const char **error);

/* Public web pages historically tolerate encoding damage. This variant
 * replaces malformed UTF-8 and binary controls with U+FFFD before invoking
 * the same parser. Local attachments must use the strict function above. */
int samosa_html_extract_replacing_invalid(
    const unsigned char *data, size_t length, size_t max_text_bytes,
    SamosaHtmlResult *result, const char **error);

void samosa_html_result_free(SamosaHtmlResult *result);

#ifdef __cplusplus
}
#endif

#endif
