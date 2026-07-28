#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "samosa_gigatoken.h"

int main(void) {
    const char *adapter = getenv("SAMOSA_GIGATOKEN_ADAPTER");
    const char *tokenizer = getenv("SAMOSA_TOKENIZER");
    if (!adapter || !tokenizer) return 2;
    unsigned char sha[32] = {0};
    unsigned char policy[] = "supervisor-test";
    SamosaGigatoken gateway;
    assert(samosa_gigatoken_start(&gateway, adapter, tokenizer,
                                  "34a1599f0c0ae7d7cd0d1c530e6522320158b360",
                                  "qwen36b", "test", sha, 100000, policy, strlen((char *)policy)));
    assert(samosa_gigatoken_health(&gateway, 2000));
    assert(samosa_gigatoken_cancel(&gateway, 999));
    assert(samosa_gigatoken_shutdown(&gateway, 2000));
    puts("test_gigatoken_supervisor: PASS");
    return 0;
}
