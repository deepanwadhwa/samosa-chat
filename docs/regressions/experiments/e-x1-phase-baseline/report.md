# E-X1 phase telemetry and baseline attempt

Date: 2026-07-17.  Branch: `experiments/e-x1-phase-baseline`.
Model resolution was verified from the product launcher: the active release is
`~/.samosa/current` and the snapshot is `~/.samosa/current/model`; the paired
tokenizer is `~/.samosa/current/tokenizer_qwen36.json`.

## Result: baseline aborted for swap safety

The short real-model smoke run verified that `SAMOSA_PHASE_STATS=1` emits one
prefill/decode `[phase]` line and does not alter the model path.  It is not a
baseline workload and must not be used as a performance claim.  Its full output
is in [raw_sanity.log](raw_sanity.log).

The first W-DECODE setup step, which prefilled the committed controlled context
to create a resumed-session seed, crossed the mandatory swap-safety bound while
still running: swap used moved from 246.06 MB to 1,477.00 MB.  The process was
interrupted immediately.  Twenty seconds after exit, swap use remained at
1,477.00 MB.  This violates the E-X1 requirement that swap-used delta be
approximately zero, so the 2T/4T workload sweep, overhead comparison, phase
table, and quality baseline were not run.  Exact command and readings are in
[raw_decode_seed_abort.log](raw_decode_seed_abort.log).

`pmset -g therm` showed no thermal or performance warning before or after the
attempt.  `powermetrics` could not run noninteractively (`sudo -n true` failed),
so package power and joules/token are **not measured**.

## W-DECODE server-path attempt (not a clean baseline)

The W-DECODE fixture tokenized to a 949-token seed prompt; its saved session
contained 950 tokens.  Because the CLI destroys the engine LRU at the end of
every invocation, the real HTTP server path was used to preserve the LRU through
the warm-up and three measured legs.  Each request restored the same session
before generating.  The model naturally stopped at 234 output tokens, so the
reported decode rate uses its 233 stepped tokens.

| Threads | Warm-up tok/s | Measured tok/s | Median | Result |
|---:|---:|---|---:|---|
| 2 | 6.28 | 6.33, 6.11, 5.89 | 6.11 | misses the 12–15 gate |
| 4 | 7.55 | 7.69, 7.62, 7.56 | 7.62 | misses the 12–15 gate |

At the median 4T leg, the phase breakdown was 13.36 ms/token attention, 4.68
router, 25.47 dense resident work, 24.06 expert matmul, **55.07 expert-disk**,
7.65 head/sampler, and 0.94 other: 131.23 ms/token total.  The phase sum agrees
with the 30.575 s / 233-token decode wall time.  This is strong evidence that
warm expert I/O remains the first lever to investigate, but it is not yet a
claim about a clean, idle-machine baseline.

Safety during these legs: AC, no `pmset` thermal/performance warning, swap
used fell from 1,429 MB to 1,373 MB, and pageouts rose about 11.7 MB.  This
passes the per-leg swap/pageout bounds relative to the server start.  It remains
non-baseline because the host was not otherwise idle and `powermetrics` was not
available.  The engine's existing `[stats]` expert hit/disk/mm totals are
cumulative across persistent server turns; the new `[phase]` counters reset
per turn, so only the latter are used as per-run phase evidence.

See [raw_w_decode_server.log](raw_w_decode_server.log) for commands and all
captured `[stats]`/`[phase]` lines.

## Required next condition

Do not rerun E-X1 until the owner confirms the machine is otherwise idle and
swap has returned to the pre-run level.  Start a privileged `powermetrics
--samplers cpu_power,thermal -i 1000` capture in another terminal before the
next attempt.  Only then run the one warm-up plus three measured runs for each
2T/4T workload leg.
