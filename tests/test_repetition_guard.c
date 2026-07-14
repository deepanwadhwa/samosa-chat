#include <stdio.h>
#include <stdlib.h>

#include "repetition_guard.h"

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "repetition guard: %s\n", message);
        exit(1);
    }
}

int main(void) {
    int unique[600];
    for (int i = 0; i < 600; ++i) unique[i] = i;
    require(repeated_tail_period(unique, 600) == 0, "unique tail was rejected");

    int single[16];
    for (int i = 0; i < 16; ++i) single[i] = 42;
    require(repeated_tail_period(single, 15) == 0, "stopped before sixteen repeats");
    require(repeated_tail_period(single, 16) == 1, "missed one-token cycle");

    int patterned[64];
    for (int i = 0; i < 64; ++i) patterned[i] = 100 + i % 4;
    require(repeated_tail_period(patterned, 64) == 4, "missed four-token cycle");

    int prefixed[80];
    for (int i = 0; i < 16; ++i) prefixed[i] = i;
    for (int i = 16; i < 80; ++i) prefixed[i] = 200 + (i - 16) % 4;
    require(repeated_tail_period(prefixed, 80) == 4, "missed cycle after coherent prefix");

    puts("repetition guard tests: ok");
    return 0;
}
