# E-X10 M4.0 — authoritative reuse and operand capture

Date: 2026-07-18.  This is the decision gate for the reuse-aware hybrid
experiment (M4: Metal takes high-reuse expert groups, OMP4 takes
singleton/low-reuse plus the shared expert, exact SSD misses consumed as
they arrive). The full M4 specification card was removed in the 2026-07-19
cleanup (see the note at the end); this report is the surviving record.

## Conditions

- checkpoint: `/Users/deepanwadhwa/.samosa/current/model`
- corpus: `../e-x10-m3-spec-verify/raw/k2_corpus.json` (3 sequences, 180 positions)
- baseline routing: full checkpoint top-K (`K=8`); no `MOE_K`, mass, entropy, or gap policy
- verifier shape: `SAMOSA_TEACHER_BATCH=4`, `OMP_NUM_THREADS=4`, `OMP_DYNAMIC=FALSE`
- engine: `qwen36b` built with `make omp`

The capture took 20.397 s.  The existing cache and ordinary `pread` path were
left in place; this is not a Metal speed result.

## Route-reuse result

`tools/hybrid_route_analysis.py` was run on
`raw/fullk_b4_routes.jsonl`.  Its metadata reports 40 layers, 256 experts,
`selected_k=8`, policy off, and router hash `0250571082e4954e`.

| Contiguous window | Weight-load reuse | Repeated jobs | Jobs in multiplicity >= 3 groups | Mean distinct experts/window |
|---:|---:|---:|---:|---:|
| S=2 | 19.15% | 38.30% | 0.00% | 12.94 |
| **S=4** | **34.84%** | **56.57%** | **31.59%** | **20.85** |
| S=8 | 48.37% | 70.10% | 48.68% | 33.04 |

The S=4 non-overlapping result is the authoritative verifier-shaped result:
56,320 expert-row jobs reduce to 36,697 distinct `(layer, expert)` loads.
The S=8 calculation is useful planning information, but is not a measured S=8
teacher execution.

**Decision: GO.** The 31.59% S=4 multiplicity-3-or-higher job share clears the
M4.0 25% threshold.  A grouped shader can amortize expert decoding and command
overhead over real row groups; dispatching one kernel per `(row, expert)`
cannot test this opportunity.

## Exact operand artifact

The second, fresh full-K8 S=4 teacher run produced the matching pair:

- `raw/fullk_b4_routes_with_inputs.jsonl` — 7,200 route records
- `raw/fullk_b4_activations.sm4a` — checksummed normalized MoE inputs

`tools/verify_m4_activation_trace.py` validated the activation stream:

- 1,840 batch records (`46` teacher batches × `40` layers)
- 14,745,600 float32 values = `180 × 40 × 2,048`
- 56.25 MiB payload, router hash `0250571082e4954e`
- SHA-256 trailer and all `(sequence, position, layer, rows)` frames valid

`SAMOSA_M4_ACTIVATIONS=<path>` is deliberately opt-in and requires
`ROUTE_TRACE=<path>`.  It captures the input to `mlp_moe` before routing,
never prompt text, refuses to overwrite an artifact, writes to a temporary
file, and atomically publishes only after writing a checksum trailer.

## M4.1 grouped kernel and mixed CPU/GPU result

Implemented an opt-in grouped backend in `src/metal_expert.m`:

- one no-copy expert slab per unique group;
- one q4 gate/up and down dispatch that iterates over up to four member rows;
- one unweighted output vector per member job; C applies route weights in the
  original order;
- `SAMOSA_METAL_GROUPED=1` enables it; the old compact-pair backend remains
  the default control;
- `SAMOSA_HYBRID_GROUP_MIN=3` assigns only multiplicity-3/4 groups to Metal.
  All other groups execute on the existing OMP4 CPU kernel while the Metal
  command is in flight.

The GPU and CPU group sets are disjoint; `hybrid_complete[]` prevents the
legacy loop from recomputing a completed group. If submission or completion
fails, only GPU-owned groups fall through to the CPU path.

| S=4 teacher path | Elapsed | Greedy argmax agreement vs CPU |
|---|---:|---:|
| CPU OMP4 (`fullk_b4_with_inputs.qtf`) | 20.397 s | 180/180 |
| Grouped Metal, all groups | 23.290 s | 172/180 |
| Grouped Metal + OMP4 CPU, `min=3` | 22.615 s | 172/180 |

**M4.1 verdict: PERFORMANCE NO-GO; IDENTITY FAIL.** The grouped dispatch is
real and the CPU/GPU split is functional, but it does not yet offset command,
activation-quantization, or synchronization costs. It is also not suitable
for lossless speculative verification: the current int8 activation bridge and
different reductions change greedy results.

Raw outputs: `raw/grouped_b4.qtf`, `raw/hybrid_m3_b4.qtf`, and their
`*_compare.json` reports. `tools/compare_teacher_streams.py` performs the
teacher-stream comparison.

## Next experiment

Do not tune SSD or scheduler flags around this kernel: the measured mixed path
is already slower. The remaining credible GPU direction is a numerically
tighter grouped kernel (avoid the int8 intermediate quantization) benchmarked
against the captured exact operands before further live-engine integration.

## Cleanup note (2026-07-19, owner request)

**Summary of the whole M4 line for skimmers: M4.0 reuse gate GO (34.84%
weight-load reuse at S=4); M4.1 grouped Metal + hybrid CPU/GPU split
implemented and measured — PERFORMANCE NO-GO, IDENTITY FAIL. The probe was
retired.**

The owner requested that this branch keep only this results document. The
following uncommitted evidence and probe material was therefore deleted and
is not recoverable; every number above stands as recorded but is not
re-runnable without rebuilding the probes:

- `raw/` captures here and under `../e-x10-m3-spec-verify/` (route traces,
  activation artifact, teacher streams, `.qtf`/compare outputs)
- the probe implementation in `src/qwen36b.c` / `src/metal_expert.[hm]`
  (`SAMOSA_METAL_GROUPED`, `SAMOSA_HYBRID_GROUP_MIN`, `SAMOSA_M4_ACTIVATIONS`,
  teacher-batch capture) — reverted, consistent with the M4.1 NO-GO
- `tools/hybrid_route_analysis.py`, `tools/compare_teacher_streams.py`,
  `tools/verify_m4_activation_trace.py`, `tests/test_hybrid_route_analysis.py`,
  the `qwen36b-sched-runtime` binary, and the M4 specification card
  (`docs/E-X10-M4-HYBRID-SCHEDULER.md`)
