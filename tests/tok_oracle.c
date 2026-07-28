#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "tok.h"

static int read_full(void *dst, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n) {
        size_t got = fread(p, 1, n, stdin);
        if (!got) return 0;
        p += got; n -= got;
    }
    return 1;
}

static int write_full(const void *src, size_t n) {
    return fwrite(src, 1, n, stdout) == n;
}

int main(int argc, char **argv) {
    Tok tokenizer;
    if (argc < 2 || argc > 3) return 2;
    tok_load(&tokenizer, argv[1]);
    int allow_added_tokens = argc == 2 || atoi(argv[2]) != 0;
    for (;;) {
        uint32_t len;
        if (fread(&len, 1, sizeof(len), stdin) != sizeof(len)) break;
        if (len > 2u * 1024u * 1024u) return 3;
        unsigned char *text = malloc((size_t)len + 1);
        int *ids = malloc(((size_t)len * 2 + 1024) * sizeof(*ids));
        if (!text || !ids) return 4;
        if (!read_full(text, len)) return 5;
        int count = tok_encode_policy(&tokenizer, (const char *)text, (int)len,
                                      ids, (int)((size_t)len * 2 + 1024), allow_added_tokens);
        uint32_t output_count = (uint32_t)count;
        if (!write_full(&output_count, sizeof(output_count)) ||
            !write_full(ids, (size_t)count * sizeof(*ids))) return 6;
        fflush(stdout);
        free(text); free(ids);
    }
    tok_free(&tokenizer);
    return 0;
}
