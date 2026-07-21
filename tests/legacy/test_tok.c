#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOK_MAX 8192
typedef struct {
    char *vocab[1000000];
    float scores[1000000];
    int vocab_size;
} Tokenizer; // fake struct just to check samosa tokenizer, actually let's just use Python for this test

