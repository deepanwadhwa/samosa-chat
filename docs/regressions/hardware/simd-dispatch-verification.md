# H2 SIMD dispatch: implemented, arm64-validated, x86 AVX2/VNNI UNVALIDATED (blocked on hardware)

Ran on: 2026-07-16
Machine: the reference **16 GB M3 MacBook Air (macOS, arm64)** — the only machine
Samosa has ever been measured on.
Verifies: the dynamic SIMD dispatch implementation for **H2** (fixes **G10**),
added on branch `tasks-hardware` (uncommitted in the working tree at the time of
this verification: `src/kernels.h`, `src/qwen36b.c`, `Makefile`, `dist/samosa`,
`tests/test_simd_dispatch.c`).

## TL;DR

The dispatch scaffolding is correct and the **arm64 (NEON) and scalar paths are
validated**. **The x86 AVX2 and AVX512-VNNI kernels have never executed on any
machine**, so their numerical correctness is unverified. H2's mandatory
token-parity gate (scalar vs AVX2 on real x86, ≥256 tokens × 5 seeds; separately
VNNI) **cannot be run here** — the project has no x86 hardware, and Rosetta /
Docker-Desktop x86 emulation on this Mac does not expose AVX2 to
`__builtin_cpu_supports`. **So the x86 AVX2/VNNI path is gated OFF by default
(opt-in via `SAMOSA_SIMD=avx2`/`avx512`): a stock x86 build runs the known-good
scalar path, unchanged from today, and nothing unvalidated ships. Enabling it by
default awaits the parity gate on real x86.**

## What IS verified (arm64 / macOS — commands + output)

- **`make test` exits 0** on this machine (run 2026-07-16, independently — not
  taken on report). The new `test_simd_dispatch` prints:

  ```
  [simd] path=neon
  Starting SIMD dispatch test...
  dot_i8i8 test: PASS
  dot_i4i8 test: PASS
  matmul_q test: PASS
  matmul_i4 test: PASS
  All SIMD dispatch tests passed successfully.
  ```

- **Dispatch structure is sound.** Pointers default to the scalar kernels
  ([kernels.h:94](../../../src/kernels.h#L94), :130, :180, :214, :412, :466), so
  an uninitialized dispatch runs scalar rather than crashing. `#define matmul_q
  g_matmul_q` ([kernels.h:217-220](../../../src/kernels.h#L217-L220)) sits in the
  `#if defined(__x86_64__)` branch; the original inline NEON body is the `#else`
  ([:222+](../../../src/kernels.h#L222)) — opposite branches, no collision. The
  hot path `matmul_qt_impl` → `matmul_q` routes through the pointer on x86
  ([kernels.h:692](../../../src/kernels.h#L692)).
- **The engine calls the initializers** (not only the test):
  `host_profile_init()` and `simd_init()` at
  [qwen36b.c:5234-5235](../../../src/qwen36b.c#L5234-L5235).
- **`SAMOSA_SIMD=scalar` escape hatch** works
  ([kernels.h:475-482](../../../src/kernels.h#L475-L482)); `immintrin.h` is
  `#if defined(__x86_64__)`-guarded.
- **arm64 math is unchanged.** `git diff HEAD -- src/kernels.h` shows no change to
  the NEON arithmetic (only guard lines moved). The one path that has ever
  produced a token is preserved.

## What is NOT verified — and why it cannot be, here

- **The AVX2/VNNI kernels never ran.** The implementing agent's own cross-compile
  logs show **`[simd] path=scalar` on BOTH x86 runs**, including the
  `-mavx2 -mfma` build. Under Rosetta, `__builtin_cpu_supports("avx2")` returns
  false, so `simd_init()` ([kernels.h:492](../../../src/kernels.h#L492)) selects
  scalar and the AVX2 function bodies are never dispatched to. Compiling *with*
  `-mavx2` changes codegen, **not** runtime `cpuid` — it proves the code
  **compiles**, not that it **runs correctly**.
- **The test can only validate the path the host dispatches** (arm64→NEON;
  x86-via-Rosetta→scalar). There is no force-AVX2 mode, so on every reachable
  machine it exercises NEON or scalar, never AVX2/VNNI. `matmul_q test: PASS` on
  x86 was scalar-vs-scalar — meaningless for AVX2.
- **VNNI (AVX-512) is doubly unverified:** no AVX-512 hardware is reachable, and
  `simd_init`'s avx512 branch reuses the AVX2 matmul plus the **untested** VNNI
  dot bodies ([kernels.h:483-491](../../../src/kernels.h#L483-L491)).
- **Partial coverage even for reachable paths:** the test does not exercise
  `matmul_i4_grouped`, `matmul_i2`, or the specific avx2/vnni dot variants.
- **No real 24 GB model run on x86** (the "works" bar).

## The blocker (explicit)

H2 states, in bold: *"H2 requires access to real x86 hardware. Do not ship it
without."* That hardware does not exist in the current setup — Rosetta /
Docker-Desktop x86 emulation on this Mac reports no AVX2 / AVX512 / SSE4.2 to
`cpuid` (consistent with the H2 card's note and E-L1). **The x86 SIMD path is
therefore implemented but unvalidatable on Mac. It is parked until a real x86 box
is available** — a cheap cloud instance for an afternoon suffices (see
[TASKS_HARDWARE.md](../../TASKS_HARDWARE.md) Open questions). Note such a box has
**unknown storage**, so throughput from it is not comparable to the reference
Mac's; it validates *correctness and the SIMD speedup ratio*, not absolute tok/s.

## Gating (how this ships safely — 2026-07-16)

`simd_init()` was changed so AVX2/VNNI are **opt-in, not auto-selected**
([kernels.h simd_init](../../../src/kernels.h)): with no `SAMOSA_SIMD` set, x86
selects **scalar** (`[simd] path=scalar (x86 SIMD gated pending validation…)`),
identical to today's shipped x86 behavior — **zero regression**. `SAMOSA_SIMD=avx2`
or `avx512` enables the corresponding path (logged `(opt-in, UNVALIDATED)`) for
anyone doing the parity run on real x86. This lets the code land on `main` without
shipping unvalidated numerics. **Closing the gate = validate on real x86, then flip
the default back to auto-select.**

## Required to close (unchanged from H2 acceptance)

On real x86 hardware: greedy token sequences **identical between
`SAMOSA_SIMD=scalar` and the AVX2 path for ≥256 tokens across 5 seeds**; repeat
independently for **AVX512-VNNI**; AVX2 ≥ 4× scalar on the `matmul_q` microbench;
a pre-AVX2 CPU selects scalar and runs correctly; one Docker image correct on both
an AVX-512 Xeon and a pre-AVX2 CPU; musl/Alpine builds (no IFUNC). See E-L1 in
[TASKS_LINUX.md](../../TASKS_LINUX.md) for the numerical-parity method.

## Secondary notes

- The work is **uncommitted** (working tree, branch `tasks-hardware`) — never
  through CI.
- The agent's reported "Local Execution Log" (the `make omp` startup `[host]`
  line) was **empty**; the `[host]` profiler line at real startup is unshown here
  and not independently confirmed (verifying it needs the 24 GB model).
- **doctor 9p magic looks wrong:** [dist/samosa:246](../../../dist/samosa#L246)
  matches `01020304` / `28cd3d45` but **not** the canonical V9FS_MAGIC
  `0x01021997`, so real 9p mounts may go undetected; virtiofs (`6a656a63`,
  [:244](../../../dist/samosa#L244)) is correct. Also `stat -f -c %t` is GNU
  syntax and no-ops on a macOS host (fails safe, but the Mac-host case is not
  covered).
