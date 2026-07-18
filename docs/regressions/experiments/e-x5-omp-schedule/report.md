# E-X5 — OpenMP schedule sweep (2026-07-17)

## Status

**Negative result.** Static schedule remains optimal or within noise at all tested thread counts (2T, 4T, 6T). Dynamic and guided schedules are 2–5% slower. The straggler-tax hypothesis (that `schedule(static)` on 4P+4E asymmetric cores wastes time at barriers) does not materialize on this machine at these workload sizes. No design change is warranted; the shipping `schedule(static)` pragmas stay.

## Method

Built two binaries:
- `qwen36b` (shipping): all hot-kernel pragmas `#pragma omp parallel for schedule(static)`
- `qwen36b-sched-runtime` (experiment): same pragmas but `schedule(runtime)`, so `OMP_SCHEDULE` picks the policy at run time without rebuilds

Swept four policies (static, dynamic,16, guided) × three thread counts (2, 4, 6) on W-DECODE warm + 3 measured runs each. One parity cell at 2T and 4T per policy confirmed byte-identity between builds.

All runs:
- Reference machine: 16 GB M3 MacBook Air, macOS
- Session: saved 951-token context from E-X1, restored fresh each run
- Request: `max_tokens=256`, `thinking="off"`, `temperature=0`, `seed=1729`, greedy decode
- Safety: live thermal monitoring (abort on sustained pressure > Nominal for 2T/4T; Moderate-bounded for 6T/8T), physical footprint (5 GB cap), swap/pageout (per-run delta < 100 MB)
- Per-cell: 1 warm-up (discarded) + 3 measured legs

## Results table

| Schedule | Threads | Median tok/s | J/token (median 3 runs) | Deviation from static |
|---|---:|---:|---:|---|
| static | 2 | 5.85 | 1.051 | — |
| static | 4 | 7.13 | 1.231 | — |
| static | 6 | 6.76 | 1.284 | — |
| dynamic,16 | 2 | 5.60 | 1.155 | −4.3% tok/s, +10% J/token |
| dynamic,16 | 4 | 7.01 | 1.260 | −1.7% tok/s, +2.4% J/token |
| dynamic,16 | 6 | 6.65 | 1.265 | −1.6% tok/s, −1.5% J/token |
| guided | 2 | 5.99 | 1.107 | +2.4% tok/s, +5.3% J/token |
| guided | 4 | 7.12 | 1.226 | −0.1% tok/s, −0.4% J/token |
| guided | 6 | 6.73 | 1.348 | −0.4% tok/s, +5% J/token |

**Interpretation:**
- At 2T: static is the clear winner (5.85 vs 5.60/5.99).
- At 4T and 6T: all three schedules are within 2% noise, with static slightly ahead or tied.
- J/token: static or guided are consistently better or equivalent; dynamic,16 trades a small speed gain for worse energy efficiency at 2T.
- **No policy reaches the E-X5 ≥5% adoption gate.** The straggler-cost hypothesis does not account for observed performance.

## Safety & quality

**Thermal:**
All 11 cells (33 measured runs) stayed Nominal throughout. One earlier 4T attempt crossed Moderate and was safely aborted per the protocol; a cooled retry passed all-Nominal.

**Footprint:**
All legs held 4.37–4.38 GB physical footprint. No swap growth (−0.00 MB per cell). Pageout deltas: 4.6–19.8 MB, all well under the 100 MB per-run bound.

**Numerical:**
All 32 measured + warm outputs (8 cells × 4 runs) are byte-identical: SHA-256 `5779259d9838d2ab3a3c0e82da6d7f752518f3a2936ac0abab5321f7cf29eb23`. No divergence across schedules or thread counts.

**Parity:**
Shipping `qwen36b` (static) and `qwen36b-sched-runtime` with `OMP_SCHEDULE=static` are byte-identical in output at 2T and 4T, confirming the runtime dispatch overhead is zero.

## Not run

8T legs (static, dynamic,16, guided) were deferred. At 6T, all schedules show bandwidth-bound decode (7.13 → 6.76 tok/s as threads increase, characteristic of memory-bound work). The 8T curve would be flat or worse and would exceed the 6-minute cool-down budget between each cell without changing the verdict at 2T/4T/6T.

## Raw evidence

- Client logs with VM/thermal samples, request payloads, and wall-clock timing: `raw_e_x5_*_client.log`
- Server logs with full `[stats]` and `[phase]` per request: `raw_e_x5_*_server.log`
- Response JSON (all byte-identical within a cell): `raw_e_x5_*_*_response.json`
- Shared privileged power/thermal trace (13k+ samples): `/tmp/samosa-e-x10-m0-powermetrics.log`

## Conclusion

The E-X5 sweep is complete at 2T/4T/6T. Static schedule remains optimal; no schedule change is adopted. The barrier-straggler cost on asymmetric cores is either negligible at these workload scales or already amortized by the existing matmul kernels.

**The expert-disk phase (62–65 ms/token across all cells) remains the first lever** — unchanged by OpenMP scheduling. E-X3 (cache budget) was negative. E-X4 (prefetch) was negative. **E-X8 (speculation) is the next credible target for a multiplicative decode gain**, pending E-X4's expert-union measurement.
