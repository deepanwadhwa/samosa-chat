/* Backend sizing must be right on machines this repo will never run on.
 * The tier table is pure arithmetic over (RAM, performance cores), so it is
 * tested directly rather than inferred from whatever laptop happens to build. */
#include <stdio.h>
#include <string.h>

/* Mirrors backend_limits() in src/samosa_gateway.c. Kept in step by
 * test_backend_limits_match.sh, which diffs the tier boundaries. */
static void limits_for(double gb, int cores, int *out_ctx, int *out_threads) {
    int ctx;
    if (gb <= 0)         ctx = 8192;
    else if (gb <= 9.0)  ctx = 2048;
    else if (gb <= 17.0) ctx = 8192;
    else if (gb <= 33.0) ctx = 16384;
    else                 ctx = 32768;
    int threads = cores > 1 ? cores - 1 : 0;
    if (threads > 12) threads = 12;
    *out_ctx = ctx; *out_threads = threads;
}

static int failures = 0;
static void expect(const char *what, double gb, int cores, int want_ctx, int want_threads) {
    int ctx = 0, threads = 0;
    limits_for(gb, cores, &ctx, &threads);
    if (ctx != want_ctx || threads != want_threads) {
        printf("  FAIL %s (%.0f GB, %d cores): got -c %d -t %d, want -c %d -t %d\n",
               what, gb, cores, ctx, threads, want_ctx, want_threads);
        failures++;
    }
}

int main(void) {
    /* The reference machine. Its context must not move: it is the only
       configuration that has ever been measured. */
    expect("M3 Air 16 GB", 16, 4, 8192, 3);
    expect("8 GB laptop", 8, 4, 2048, 3);
    expect("32 GB", 32, 10, 16384, 9);
    expect("64 GB", 64, 16, 32768, 12);   /* thread cap applies */
    expect("128 GB", 128, 24, 32768, 12);
    /* Boundaries: a machine reporting slightly under a round number (16 GB
       hardware often reports 15.9) must land in the tier a person would
       expect, not the one below. */
    expect("15.9 GB", 15.9, 4, 8192, 3);
    expect("17 GB edge", 17, 4, 8192, 3);
    expect("17.1 GB", 17.1, 8, 16384, 7);
    /* Degenerate inputs must stay safe rather than producing an argv that
       llama.cpp would reject. */
    expect("unknown RAM", 0, 0, 8192, 0);
    expect("single core", 16, 1, 8192, 0);
    if (failures) { printf("test_backend_limits: %d failure(s)\n", failures); return 1; }
    printf("test_backend_limits: PASS\n");
    return 0;
}
