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
