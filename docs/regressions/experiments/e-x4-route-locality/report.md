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
