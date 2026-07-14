#ifndef SAMOSA_REPETITION_GUARD_H
#define SAMOSA_REPETITION_GUARD_H

/* Return the period of an exact token cycle repeated sixteen times at the
 * tail, or zero. The bound is deliberately small and deterministic: it
 * catches runaway generation without treating normal document reuse as a
 * loop. */
static inline int repeated_tail_period(const int *tokens, int count) {
    const int repeats = 16, max_period = 32;
    for (int period = 1; period <= max_period; ++period) {
        int span = period * repeats;
        if (count < span) continue;
        int start = count - span, repeated = 1;
        for (int i = start + period; i < count; ++i) {
            if (tokens[i] != tokens[i - period]) { repeated = 0; break; }
        }
        if (repeated) return period;
    }
    return 0;
}

#endif
