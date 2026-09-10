#ifndef SAMOSA_DOCX_H
#define SAMOSA_DOCX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *text;
    size_t text_bytes;
    unsigned long text_chars;
    unsigned entry_count;
} SamosaDocxResult;

/* Extract a validated Office Open XML word-processing package from memory.
 * The ZIP reader accepts only stored/Deflate entries and the implementation
 * enforces entry, expansion, ratio, path, XML, and output bounds before any
 * document text is returned. */
int samosa_docx_extract(const unsigned char *data, size_t length,
                        size_t max_text_bytes, SamosaDocxResult *result,
                        const char **error);

void samosa_docx_result_free(SamosaDocxResult *result);

#ifdef __cplusplus
}
#endif

#endif
