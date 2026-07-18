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

## Phase A2 — router-lookahead probe: implemented, not yet measured (2026-07-18)

Following the 2026-07-18 card revision, the A2 in-engine probe is **built and
compiled; no measurement has been run.**  It replaces the dead v1 persistence
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

**Correctness (byte-identity) — argued by construction, not yet run
end-to-end.**  The probe runs *after* the routing decision is finalized and
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

**Measurement still owed (per the A2 card):** P1 recall@k′ curve on W-DECODE and
W-SESSION (warm, ×3 + warm-up), the overhead guard (router-bucket and tok/s
deltas <2% under `SAMOSA_PHASE_STATS=1`; note the probe adds one router-sized
matmul per MoE layer per token, so this is the number to watch — recall itself
is timing-independent), and the B1 go/no-go read off the curve (recall ≥ ~60%
at predicted waste < 20%).  Not run here: the real model was not driven, per the
standing machine-safety rule and the E-X1 idle-host protocol.
