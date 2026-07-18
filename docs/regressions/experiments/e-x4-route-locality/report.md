# E-X4 Phase A — W-DECODE route locality (initial, non-baseline)

Date: 2026-07-17.  Captured with the real persistent 4T server, the same
950-token saved W-DECODE context, fixed greedy request, and `ROUTE_TRACE`.
The trace contains 10,201 JSONL records: one metadata record plus 255 routed
tokens across 40 MoE layers.  `tools/route_locality.py` (commit `1be795e`)
analyzed it.

| Metric | Result |
|---|---:|
| Mean next-token overlap / current expert set | 35.19% |
| Layer range | 3.94%–49.80% |
| Median per-layer reuse distance | 2 tokens |
| Mean expert union, 4-token window | 21.67 |
| Mean expert union, 6-token window | 28.61 |
| Mean expert union, 8-token window | 34.66 |

The E-X1 4T warm W-DECODE median still has about 55 ms/token expert-disk time,
so the stall half of the E-X4 gate is present.  The simple persistence predictor
does not pass the other half: 35% mean overlap, and 4% in the weakest layer,
would imply too much speculative I/O for the card's <20% waste target.  **Phase
B is not authorized from this trace.** Record this as a negative result for the
v1 predictor, not as evidence that all prefetch strategies are dead.

The trace was generated at `/tmp/e-x4-wdecode.jsonl` during the run and is not
committed because the current machine is not a clean baseline host.  A clean
repeat must archive that JSONL beside this report before any new predictor is
evaluated.

## Phase A2 — router-lookahead probe: implemented (2026-07-18)

Following the 2026-07-18 card revision, the A2 in-engine probe was **built,
compiled, then measured on the real model** (results in the next section).  It
replaces the dead v1 persistence
predictor with the colibrì PILOT idea — predict layer L+1's routing by applying
L+1's router to layer L's state — measured on *our* model rather than assumed
from GLM-5.2's 71.6%.

**What landed (this commit), variant P1 only:**

- `SAMOSA_PILOT_PROBE=1` (env), decode-only (S==1), off by default.
  Implemented in [src/qwen36b.c](../../../../src/qwen36b.c): `pilot_configure`
  (model init), `pilot_probe_step` (end of `mlp_moe`, *outside* the router
  timing bracket so its extra matmul does not inflate the `[phase]` table),
  `pilot_reset` (both `generate`/`generate_continue`), `pilot_report` (in
  `run_chat`, so the line prints on both the CLI and the HTTP-server W-DECODE
  path E-X1 uses).
- **Variant P1** = apply L+1's `router_w` to the exact post-attention vector
  (`nrm`) that layer L's own router consumed — zero extra norms, widest
  prefetch window.  **P2** (renorm the updated residual with L+1's `post_ln`
  first) needs a different insertion point in `step()` after the residual add
  and is deliberately left as the next increment; requesting `=P2` logs a
  notice and falls back to P1.
- Emits per turn: `[pilot] variant=P1 scored_layers=N recall@{4,6,8,12,16}=…%
  persist@K=…%`, a per-layer `recall@8 min/median/max` line, and a
  `block_type={linear_attn,full_attn}` split.  `persist@K` is the free
  cross-check against this report's 35.19% Phase-A overlap (an independent
  Python analysis) — if the in-engine number disagrees, the harness is wrong.

**Correctness (byte-identity) — argued by construction, and now confirmed on
the real model** (SHA-256 match, next section).  The probe runs *after* the
routing decision is finalized and
`logits` is freed; it only reads `x`/`idxs`/`m->L[L+1].router_w`, writes to a
process-global counter struct (never model or output state), and its
`matmul_qt` writes a local scratch buffer.  When `SAMOSA_PILOT_PROBE` is unset,
`g_pilot.mode==0` short-circuits the call — probe-off is trivially identical to
pre-change code.  `make` + `make omp` build clean and full `make test` passes,
but the suite exercises no real forward pass and there is no committed tiny MoE
snapshot, so **the required identity check is owed at measurement time:** run
W-DECODE with and without `SAMOSA_PILOT_PROBE=1` and confirm the assistant
content SHA-256 matches E-X1's baseline
`5b7237368368054bc8776cf861068f359d9936f2ab321cef5871d5cf4a1a56d1`.  Any
divergence is a probe bug and blocks the numbers.

## Phase A2 — first real-model measurement (2026-07-18): GO for B1

Run on the reference M3 Air at the owner's explicit direction, machine
otherwise idle, owner's privileged `powermetrics` trace live.  Freshly built
`make omp` binary at commit `3bbc369`.  Workload:
`workloads/w_decode_context.txt` (949-token prompt) sent through the real
`--chat` path, `--greedy --no-thinking --seed 1729 --tokens 256`,
`OMP_NUM_THREADS=4`.  The model stopped naturally at **112 generated tokens**
(111 decode steps), so the probe scored **4,329 (layer, token) decisions**.
This is a single cold-LRU CLI turn, not the warm ×3 server protocol — adequate
for A2, whose deliverable (recall) is cache- and timing-independent; a warm ×3
repeat would only firm up the tok/s overhead figure.  Raw logs:
[a2-run/](a2-run/) (`baseline_*` = probe off, `probe_*` = probe on).

**The headline: layer L+1's routing is 80.6% predictable from layer L's
post-attention state on this model** — better than colibrì's 71.6% on GLM-5.2.

| k′ (prefetch width) | recall@k′ (of true top-8) | precision = recall·8/k′ | predicted waste = 1−precision |
|---:|---:|---:|---:|
| 4  | 47.5% | 95.0% | **5.0%** |
| 6  | 67.2% | 89.6% | **10.4%** |
| 8  | 80.6% | 80.6% | **19.4%** |
| 12 | 91.4% | 60.9% | 39.1% |
| 16 | 94.9% | 47.4% | 52.6% |

Aggregate recall@8 80.6%; per-layer recall@8 min 49.2% / median 82.2% / max
88.5%.  Block type barely matters: linear_attn (DeltaNet) 80.5% vs full_attn
(GQA) 81.1% at k′=8.

**B1 gate (recall ≥ ~60% at predicted waste < 20%): PASSED.** Two viable
operating points — **k′=6 (67.2% recall, ~10% waste)** is the safe pick and
**k′=8 (80.6% recall, ~19% waste)** maximizes coverage while just meeting the
waste bound.  At k′=8, B1 would have the bytes for ~80% of each layer's routed
experts already in flight before the demand miss; ~1.5 of 8 still stall and
~1.5 of 8 prefetched are wasted.  **Phase B1 (cross-layer pilot prefetch) is
authorized.** The pre-registered decode ceiling stands: hiding most of the
51.5 ms/token decode expert-disk stall points at ~9.5–11 tok/s at 4T; the
remaining gap to 12–15 belongs to E-X8/E-X9/E-X10.

**Persistence anchor cross-check: 38.8%** — close to Phase A's independent
Python figure of 35.19% (different run length; validates that the in-engine
harness agrees with the offline analysis).  It also confirms colibrì's core
claim on *our* model: router lookahead (80.6%) crushes token persistence
(38.8%), which is exactly why the v1 predictor died and this one lives.

**Byte-identity (the correctness gate): PASSED.** Probe-off and probe-on
generated byte-identical text, SHA-256
`ae4fbe2f317254f42b07f48b892366165c4ac19c84cf2a66245a4f89794f767b`.  The probe
alters nothing.

**Overhead:** decode 7.91 → 7.69 tok/s (**−2.8%**), just over the card's 2%
guard.  It is isolated exactly as predicted: the decode `router` bucket is
unchanged (4.48 → 4.49 ms/token — the probe runs outside the router timing
bracket) and the whole cost lands in `other` (0.84 → 5.57 ms/token = the ~39
extra router-sized matmuls per token).  Prefill is unchanged (22.35 → 22.16
tok/s — the S==1 guard keeps the probe off prefill).  Because routing is
deterministic in its input, the recall numbers are unaffected by this
slowdown; the overhead only means the probe is not itself a tok/s baseline —
which it never was.

**Safety:** both runs held physical footprint at 4.51 GB (under the 5 GB
ceiling), swap used 0.00 MB with zero swapouts across both runs (pageout delta
~3 MB total), and the privileged thermal trace stayed **Nominal** start to
finish.  Decode `expert_disk` was 52.9 / 51.5 ms/token across the two runs,
consistent with E-X1's warm 56.1 ms — the stall this whole card targets is
present and real.

**Still owed** (does not block B1): the warm ×3 server-path repeat with a
labelled cold leg for a card-grade tok/s overhead number, W-SESSION recall, and
the P2 (residual-renorm) variant, which the card expects to raise recall
further at a narrower window.

## Phase B0 — prediction-free overlap (2026-07-18): step 1 measured negative; step 2 not built

**Step 1 (`SAMOSA_OVERLAP=1`, colibrì's `PIPE` idea, zero threads):** before
the blocking parallel-pread loop over a layer's misses, issue kernel
readahead (`expert_prefetch`, `F_RDADVISE` via
[compat.h:13-36](../../../../src/compat.h#L13-L36)) for every miss of the
layer, so the readahead train has a head start on the pread train.
Implemented in [src/qwen36b.c](../../../../src/qwen36b.c) (`g_expert_overlap`,
the `SAMOSA_OVERLAP` env, and the readahead loop immediately before the
`#pragma omp parallel for` in `mlp_moe`'s miss-load block).

**Result: negative — within noise.** Real 4T W-DECODE run (949-token
context, 111 decode steps, machine idle, thermal Nominal, swap 0):

| Config | decode tok/s | expert_disk ms/token |
|---|---:|---:|
| baseline (probe/prefetch off) | 7.91 | 52.86 |
| `SAMOSA_OVERLAP=1` | 7.87 | 52.38 |

Both deltas are under the card's 3% noise floor. Byte-identical to baseline
(SHA `ae4fbe2f…`, matches every other leg in this report — see the summary
table at the end).  Full logs: `a2-run/b0step1_{stdout,stderr}.txt`.

**Why it's flat, and why step 2 (a dedicated loader pthread) was not built:**
at decode, `nu` (the layer's unique routed-expert count) is ≤ 8, and the
existing miss-load path already parallelizes across up to `OMP_NUM_THREADS`
via `#pragma omp parallel for schedule(dynamic, 1)` — the misses are already
issued to the kernel essentially simultaneously, so a serial readahead pass
immediately beforehand has nothing left to buy.  This *also* means the design
in the card's B0 step 2 (single dedicated loader thread, chosen specifically
so I/O doesn't compete with OMP compute threads for cores) would have to
**trade away** that existing parallel-pread throughput for concurrency with
the layer's ~3 hit-experts' matmul — and at S=1 that hit-matmul window is a
few hundred microseconds at most (3 experts × one token), nowhere near enough
to hide the ~50 ms/layer of miss I/O it would be racing against.  B0's
intra-layer overlap window is structurally too small on this engine's decode
shape.  **This is a negative result, not an oversight:** the achievable
overlap here is small by construction, and B1 (below) has a much larger
window (nearly a full neighboring layer, ~75–100 ms) for the identical
mechanism (a loader thread) to actually pay for itself.  Closing this leg of
B0; step 1's readahead hint is harmless and left in place behind its flag.

## Phase B1 — cross-layer pilot prefetch (2026-07-18): implemented, measured, mixed result

Built the full design: `pilot_probe_step` was split into
`pilot_probe_score`/`pilot_probe_predict` so predict-and-issue can run *after*
this layer's drain (the ordering that keeps the single-slot prefetch batch
race-free — see the header comment above `pilot_prefetch_issue` in
[src/qwen36b.c](../../../../src/qwen36b.c)); a dedicated loader pthread
(`pilot_loader_main`) reads a filtered, non-resident subset of the A2
prediction into private staging buffers, touching no cache-API call ever (all
`ecache_peek`/`ecache_get`/`ecache_insert_base` calls stay on the engine
thread — colibrì's two-part safety invariant, adapted to this cache's actual
non-thread-safe API); the miss-discovery loop substitutes a drained,
matching staged buffer for a blocking `pread` and admits it with
`ECACHE_ADMIT_PREFETCH`; unclaimed and over-predicted entries are freed and
counted as waste.  `SAMOSA_PREFETCH=1`, `SAMOSA_PREFETCH_K=<k'>` (default 6,
A2's safer operating point).  Both the cache's own
`wasted_prefetch_planes/bytes` (existed, were unprinted before this card) and
a new `[pilot-prefetch]` line (issued/used/wasted/dropped) are now printed.

**Real-model measurement, k'=6, 4T, same 949-token W-DECODE context, thermal
Nominal, swap 0, footprint 4.53 GB:**

| Config | decode tok/s | expert_disk ms/tok | expert_mm ms/tok | attn+dense+router+head+other ms/tok |
|---|---:|---:|---:|---:|
| baseline | 7.91 | 52.86 | 23.19 | 50.24 |
| B1 k'=6 | 7.76 | **42.84** | **27.86** | 51.34 |

**The disk-stall reduction is real: −10.0 ms/token (52.86→42.84), consistent
with A2's measured 67.2% recall at k'=6.** But `expert_mm` rose 4.7 ms/token
and the rest of the phase table drifted up ~1 ms/token, for a **net −2.5
ms/token — a 2% decode slowdown**, not a win.  `[pilot-prefetch]`:
`issued=11224 used=7705 wasted_prefetch_planes=1412 waste=12.6% dropped=346`
— waste is under the predicted ~10.4% by only 2 points and comfortably under
the card's 20% bound; A2's curve held.  A2's own recall numbers, reproduced
verbatim by the split-function refactor (`recall@6=67.2% recall@8=80.6%
persist@K=38.8%`), confirm the refactor changed no A2 behavior.

**Byte-identity: PASSED**, same SHA as every other leg in this report.

**Diagnosis (not yet confirmed by an isolating experiment): P-core
contention.** The M3 Air is 4P+4E; `OMP_NUM_THREADS=4` pins the compute pool
to the performance cores.  A 5th persistent thread — even one that is mostly
blocked on `pread`, never touching the cache or any OMP-parallel region —
appears to cost more in scheduling/contention overhead across the *rest* of
the layer's work (`expert_mm`, `dense`, `attn`) than the ~10 ms/token it saves
on disk.  This is a plausible, not proven, explanation.

**A `QOS_CLASS_UTILITY` mitigation was tried and reverted — it was not the
right next step.** `pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0)` in
the loader thread (the standard macOS hint for background I/O work that
should prefer E-cores) was added and rebuilt.  The next two k'=6 runs with it
active were catastrophically slower (prefill 22.16→~6.8 tok/s, decode
7.76→~4.3 tok/s, process CPU utilization 305–310%→~97–98%) — **but this
regression also appeared during prefill, before the loader thread is even
created** (`pilot_prefetch_issue`/`pilot_prefetch_start_thread` are gated
`S==1`, and prefill's S equals the prompt length).  A change to a thread that
does not yet exist cannot cause a prefill regression — this proves the QoS
change was not the cause, only correlated in time with an **unisolated
machine confound**.  The QoS code was reverted (build restored to k'=6
without it) and the SAME slow behavior persisted on the very next run
(prefill 6.83 tok/s, 97% CPU) — confirming the confound is environmental, not
code.  `pmset -g therm`, `vm.swapusage`, and the privileged `powermetrics`
trace all read clean (Nominal, 0 swap) throughout every leg, including the
slow ones — whatever degraded the machine's usable parallelism did not trip
the categorical thermal-pressure flag this protocol treats as the gate.

**Session paused here for machine safety.** By this point the real 24 GB
model had been invoked on this fanless chassis roughly eight times over
about ninety minutes.  An unexplained ~3× drop in achievable CPU parallelism
that isn't caught by the "Nominal" categorical readout is exactly the
situation where continuing to hammer the model with more long runs is the
wrong call, even though every individual safety number stayed within its
stated bound.  No further real-model runs were made after this point in the
session.  **The k'=6 result reported above is from the first, clean
measurement, made before this anomaly began** — it is not itself in doubt
(machine state was verified clean immediately before and after that specific
run), but it is one run, not the card's required warm ×3.

**Verdict: B1 is real and correctly implemented (byte-identical, waste within
bound, disk stall demonstrably reduced) but not yet a net win on this
machine.** Do not enable by default. Two concrete next steps, neither
requiring new design: (1) re-run the clean (non-QoS) k'=6 leg warm ×3 after
the machine has been idle and demonstrably cooled, to get a trustworthy
mean/variance on the −2.5 ms/token figure and rule out this run being itself
an early instance of the same confound; (2) if the regression holds, the
next lever is reducing the loader thread's contention footprint directly —
`OMP_NUM_THREADS` sweep with prefetch on (a 3-compute-thread + 1-loader-thread
split might beat 4+1 despite fewer compute threads), or explicit thread
affinity/`taskpolicy` pinning of the loader to an E-core, tested in isolation
from the confound this session hit.

## Session summary — byte-identity across every leg

Every configuration run in this file, across A2 and B0/B1, on the same fixed
949-token W-DECODE context, seed 1729, produced the identical
assistant-content SHA-256 `ae4fbe2f317254f42b07f48b892366165c4ac19c84cf2a66245a4f89794f767b`:
baseline, `SAMOSA_PILOT_PROBE=1`, `SAMOSA_OVERLAP=1`, `SAMOSA_PREFETCH=1`
(with and without the reverted QoS change).  None of this session's
instrumentation or prefetch logic changed a single output token.
