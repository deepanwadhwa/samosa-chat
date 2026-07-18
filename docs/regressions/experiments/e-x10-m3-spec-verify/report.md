# E-X10 M3 — native MTP + batched verification/prefill

Date: 2026-07-18  
Machine: 16 GB Apple-silicon MacBook Air, Apple M3 GPU  
Status: experimental, opt-in only

## Question

Can Samosa combine its SSD-streamed experts, CPU state/routing work, and the
Apple GPU profitably by reusing expert weights across several token rows?

This experiment tests both plausible consumers of a multi-row expert kernel:

1. native-MTP speculative verification; and
2. ordinary prompt prefill.

Normal `qwen36b` behavior remains unchanged. The native MTP head is loaded
only with `SAMOSA_MTP_PROBE=N`; the Metal path still requires the separate
`qwen36b-metal` build plus `SAMOSA_METAL=1`.

## What was implemented

- Loaded the checkpoint's existing resident int8 MTP tensors and layer-40
  int8 expert shelf.
- Added a shadow MTP mode which makes real proposals but never changes the
  generated output.
- Added an all-row logit path and `SAMOSA_TEACHER_BATCH=1..16` so the full
  model can execute verifier-shaped batches over an exact token corpus.
- Extended the custom Metal expert backend from one token/eight experts to
  multiple token/expert pairs. Expert slabs remain no-copy Metal views over
  the bounded cache; the CPU evaluates the shared expert while the GPU command
  is in flight.
- Tested both union-shaped dispatch and compact `(token, expert)` pairs, plus
  small and collapsed command sizes.

## Native MTP result

Three greedy, no-thinking prompts were decoded in shadow mode with an
eight-draft window:

| Prompt | Windows | Drafted | Compared through first miss | Accepted | Accepted/drafted | Conditional match | Ideal tokens/verify | Draft ms/token |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| tides | 15 | 114 | 47 | 33 | 0.2895 | 0.7021 | 3.200 | 12.338 |
| Python/Fibonacci | 12 | 94 | 47 | 35 | 0.3723 | 0.7447 | 3.917 | 12.885 |
| sheep arithmetic | 15 | 120 | 37 | 23 | 0.1917 | 0.6216 | 2.533 | 11.908 |
| **aggregate** | **42** | **328** | **131** | **91** | **0.2774** | **0.6947** | **3.167** | **12.338** |

This is a real positive finding: the checkpoint's MTP head is correctly
aligned and dramatically better than the earlier top-k-logit drafter. The
MTP proposal itself is cheap enough to remain interesting.

## Full-model verifier result

The exact 180-position corpus in `raw/k2_corpus.json` was teacher-forced with
four CPU threads. Times below exclude model initialization and include the
full transformer state update and logits:

| Verifier path | Rows/pass | Elapsed | ms/position | Relative to CPU S=1 |
|---|---:|---:|---:|---:|
| CPU/OpenMP | 1 | 24.573 s | 136.5 | 1.000x |
| CPU/OpenMP | 2 | 20.670 s | 114.8 | 1.189x |
| CPU/OpenMP | 4 | 20.064 s | 111.5 | 1.225x |
| CPU/OpenMP | 8 | 30.999 s | 172.2 | 0.793x |
| Metal union | 2 | 22.711 s | 126.2 | 1.082x |
| Metal union | 4 | 21.549 s | 119.7 | 1.140x |
| Metal compact pairs | 4 | 21.893 s | 121.6 | 1.123x |
| Metal union | 8 | 20.738 s | 115.2 | 1.185x |

The final fairness rerun changed an important intermediate conclusion: with
the same four-thread OpenMP build, CPU batch-4 is the best verifier path and
is slightly faster than batch-4 Metal. Metal does rescue the batch-8 case,
but it still does not beat the CPU batch-4 sweet spot.

That is not enough for speculative decoding. For example, a three-draft
window needs a four-row verifier pass. Even assuming all three drafts are
accepted, the current measured cost is approximately:

```text
3 × 12.34 ms MTP + 4 × 111.5 ms verifier = 483.0 ms
483.0 / 4 emitted tokens = 120.8 ms/token (absolute best case)
```

Any rejection, recurrent-state rollback, or replay makes it slower. Reaching
10 tok/s with a realistic ~2.5 emitted tokens/pass would require the
four-row verifier to fall to roughly 53 ms per row, about 2.1x faster than
the current best implementation.

## Correctness

Shadow MTP does not alter output. The batch Metal verifier currently does:

- compact-pair Metal and CPU batch-4 agreed on greedy argmax at 176/180
  positions (97.8%);
- complete top-5 order agreed at 79/180 positions.

The discrepancy comes from the accelerated activation quantization and
different reduction order. This is acceptable for an isolated performance
probe, but not for lossless speculative verification: a verifier must define
the model's authoritative distribution.

## Prefill result

Fair comparisons used the OpenMP CPU build with four threads.

| Prompt | Tokens | CPU prefill | Metal small-batch prefill | Result |
|---|---:|---:|---:|---:|
| short | 18 | 2.226 s | 2.633 s | Metal 18.3% slower |
| long | 104 | 6.831 s | 7.945 s | Metal 16.3% slower |

Collapsing the 104-token workload into much larger GPU commands worsened it
to 8.772 s. The CPU's expert-union kernel is already efficient for prefill;
this custom Metal shader does not beat it.

## Decision

The native MTP head and CPU batch-4 weight reuse are real and worth keeping.
The complete speculative path and Metal prefill kernel are not production
wins yet:

- a three-draft verifier is bounded near 8.3 tok/s even under perfect
  acceptance, before rollback/replay;
- prefill is slower than the four-thread CPU path;
- GPU verification is not yet argmax-identical.

The next go/no-go threshold is concrete: do not integrate speculative decode
until an exact (or explicitly quality-gated) four-row verifier is below about
210 ms per pass on this machine. That likely requires a substantially better
matrix kernel or a different division of the dense/shared work, not another
cache or command-queue setting.

## Raw artifacts

All logs, generated text, teacher streams, MTP acceptance runs, and timing
outputs are under `raw/`. The most relevant files are:

- `mtp_w8_p{1,2,3}_omp.log`
- `verify_b{1,2,4,8}_omp.log`
- `verify_b{2,4,8}_metal.log`
- `verify_b4_pairs.log`
- `cpu_omp_prefill_phase.log`
- `metal_pairs_prefill_phase.log`
- `cpu_long_prefill.log`
- `metal_long_prefill.log`
