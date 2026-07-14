#ifndef SAMOSA_THINKING_BUDGET_H
#define SAMOSA_THINKING_BUDGET_H

/* Qwen's published budget mechanism does not force a bare control token. It
 * appends this trained transition, then continues normal generation. Keep the
 * text byte-for-byte aligned with the upstream Qwen3 example. */
#define QWEN_THINKING_EARLY_STOP_TEXT \
    "\n\n Considering the limited time by the user, I have to give the solution " \
    "based on the thinking directly now.\n</think>\n\n"

#define THINKING_EARLY_STOP_MAX_TOKENS 96

typedef struct {
    int tokens[THINKING_EARLY_STOP_MAX_TOKENS];
    int count;
    int position;
} ThinkingBudgetTransition;

/* Return one injected transition token when the budget has just been reached,
 * or while a previously started transition is still in progress. Continuing
 * after </think> matters because the published transition includes trailing
 * newlines before normal answer generation resumes. */
static int thinking_budget_next(ThinkingBudgetTransition *transition,
                                int thinking_open, int budget, int generated,
                                int *token) {
    int continuing = transition && transition->position > 0 &&
                     transition->position < transition->count;
    int starting = transition && transition->count > 0 &&
                   transition->position == 0 && thinking_open && budget > 0 &&
                   generated >= budget;
    if (!continuing && !starting) return 0;
    *token = transition->tokens[transition->position++];
    return 1;
}

#endif
