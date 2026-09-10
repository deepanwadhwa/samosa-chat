#include "samosa_docx.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 64;
    char *end = NULL;
    unsigned long limit = 7u * 1024u * 1024u;
    if (argc == 3) {
        errno = 0;
        limit = strtoul(argv[2], &end, 10);
        if (errno || !end || *end || !limit) return 64;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 66;
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET)) return 66;
    unsigned char *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) return 66;
    fclose(file);
    SamosaDocxResult result;
    const char *error = NULL;
    if (!samosa_docx_extract(data, (size_t)size, (size_t)limit,
                             &result, &error)) {
        printf("error:%s\n", error ? error : "unknown");
        free(data);
        return 65;
    }
    printf("ok:%u:%zu:%lu\n%s\n", result.entry_count, result.text_bytes,
           result.text_chars, result.text);
    samosa_docx_result_free(&result);
    free(data);
    return 0;
}
