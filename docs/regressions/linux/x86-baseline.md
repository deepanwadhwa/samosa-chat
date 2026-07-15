# E-L1 (partial): first real-model run on native x86 — the H2 baseline

Ran on: 2026-07-15, by the project owner on their own hardware.

## Machine

```
ASUS Zenbook, Intel Core i7-1260P (12th gen: 4 P-cores + 8 E-cores, 16 threads)
16 GB RAM  ·  Windows  ·  WSL2 (Ubuntu) with Docker CE — no Docker Desktop
WSL2 VM: 7 GB RAM visible, 954 GB free
CPU reports AVX2: YES
container: --memory=6g, model in a named Docker volume (not a bind mount)
```

Reached via `git clone` → `docker build` → `samosa pull` (24 GB) → `samosa serve`,
exactly as the README documents. Browser on Windows → `http://127.0.0.1:8642`.

## Result — four-architecture parity

Same prompt, same seed (11), thinking off, `max_tokens=16`:

```json
{"content":"The capital of France is **Paris**."}
{"usage":{"prompt_tokens":19,"completion_tokens":9,"total_tokens":28}}
{"samosa":{"tokens_per_second":1.26,"rss_gb":3.84,"thinking_closure":"natural"}}
```

| Platform | Answer | Tokens | rss_gb | tok/s |
|---|---|---|---|---|
| macOS M3 (NEON, native) | Paris | 9 | 3.84 | 5–7 |
| arm64 Linux (Docker, virtiofs) | Paris | 9 | 3.84 | 0.92 |
| x86_64 Linux (QEMU-emulated, virtiofs) | Paris | 9 | 3.85 | 0.16 |
| **x86_64 Windows/WSL2 (native, volume)** | **Paris** | **9** | **3.84** | **1.26** |

**Byte-identical output and an identical footprint across four platforms.** The
x86 scalar path — which had never executed on real hardware before this run —
agrees with NEON.

This is a **partial** E-L1: one prompt, one seed, 9 tokens. The full gate is 256
tokens × 5 seeds, plus a separate arm for AVX512-VNNI. It does establish that the
scalar path is not catastrophically wrong, which was the open risk.

## Why 1.26 tok/s — and why this is the number H2 must beat

The CPU **has AVX2**. The build compiles it out (**G10**): no `-march`, so
`__AVX2__` never defines and `kernels.h` falls through to its scalar remainder.
Measured cost of that path: **7.6x** (NEON 17.09 vs SCALAR 2.26 GFLOP/s).

Decode composition, measured on the M3 the same day:

```
2 threads:  decode 6.158s = expert_disk 4.283s (70%) + expert_mm 1.856s (30%)
4 threads:  decode 5.384s = expert_disk 3.882s (73%) + expert_mm 1.421s (27%)
```

macOS is **storage-bound**: doubling threads bought only 14% (6.33 → 7.24 tok/s).
On x86 the 7.6x scalar penalty inverts this — matmul becomes ~77% of decode, so
the machine is **compute-bound**.

**Prediction for H2 (runtime SIMD dispatch): ~3x, landing near 3.5–4 tok/s**, at
which point x86 returns to the storage-bound regime and further compute work
stops paying. Recorded here so the prediction can be checked rather than
quietly revised afterwards.

Two cheap things not yet tried on this machine:

- **`OMP_NUM_THREADS=8`.** The default targets half the P-cores — tuned for a
  fanless MacBook Air — so this 12-core laptop is likely running **2 threads**.
  Because x86 is compute-bound, threads should help far more here than the 14%
  they bought on the Mac. Predicted ~2x, for one flag.
- A long generation, which would exercise **G9** (the cgroup page-cache pressure
  fix) under sustained streaming in a 6 GB container — a path never tested on
  real hardware.

## Why a GPU is not the answer here

Asked and answered on the same day. Amdahl bounds it: at 70/30 on the Mac, an
infinitely fast, free GPU still cannot go below the disk time — a **~1.4x**
ceiling. And the 24 GB of expert weights do not fit in a 2 GB laptop GPU, so
every token would stream weights over PCIe into a thrashing VRAM working set —
adding a hop rather than removing one.

**H2 is worth ~3x on x86 for a few days of work. Metal is worth ~1.4x on macOS
for weeks.** That comparison is why H2 is the priority, and this run is what
makes it concrete rather than theoretical.
