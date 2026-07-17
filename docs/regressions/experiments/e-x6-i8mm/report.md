# E-X6 — i8mm implementation preflight (2026-07-17)

## Status

Step 0 is complete; no engine source was changed and no model inference was
run. The dispatch implementation, microbenchmark, bit-exact tests, and
W-PREFILL measurements remain unrun pending the thermal-retry decision.

## Reference-machine capability

The reference M3 reports i8mm support:

    $ sysctl -n hw.optional.arm.FEAT_I8MM
    1

The compiler is Apple clang 21.0.0 (clang-2100.1.1.101), targeting
arm64-apple-darwin25.5.0.

## Per-function target feasibility

A scratch translation unit containing this function compiled cleanly with the
ordinary compiler invocation—no global -march/-mcpu flag:

    __attribute__((target("arch=armv8.6-a+i8mm")))
    int32x4_t i8mm_probe(int32x4_t acc, int8x16_t left, int8x16_t right) {
        return vmmlaq_s32(acc, left, right);
    }

Disassembly of the resulting arm64 object contains smmla at offset 0x34.
Therefore the planned one-binary runtime-dispatch design is feasible on the
reference toolchain: i8mm code can be isolated to target-attributed functions
while the ordinary dot-product path remains runnable on Macs without i8mm.

This is only a compiler and ISA preflight. It establishes neither the
correctness of a tiled i8mm implementation nor its speed, power, quality, or
memory characteristics; those require the card's bit-exact and thermal-safe
runtime measurements.
