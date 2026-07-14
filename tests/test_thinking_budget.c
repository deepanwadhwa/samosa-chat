#include <assert.h>
#include <stdio.h>

#include "thinking_budget.h"

int main(void) {
    ThinkingBudgetTransition transition = {
        .tokens = {101, 102, 103}, .count = 3, .position = 0
    };
    int token = -1;
    assert(!thinking_budget_next(&transition, 1, 256, 255, &token));
    assert(thinking_budget_next(&transition, 1, 256, 256, &token));
    assert(token == 101);
    /* The remainder must continue even if 102 represented </think>. */
    assert(thinking_budget_next(&transition, 0, 256, 257, &token));
    assert(token == 102);
    assert(thinking_budget_next(&transition, 0, 256, 258, &token));
    assert(token == 103);
    assert(!thinking_budget_next(&transition, 0, 256, 259, &token));

    ThinkingBudgetTransition naturally_closed = {
        .tokens = {201}, .count = 1, .position = 0
    };
    assert(!thinking_budget_next(&naturally_closed, 0, 256, 300, &token));
    puts("thinking budget transition: ok");
    return 0;
}
