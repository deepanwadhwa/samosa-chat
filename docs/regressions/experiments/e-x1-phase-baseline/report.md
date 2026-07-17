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

## Required next condition

Do not rerun E-X1 until the owner confirms the machine is otherwise idle and
swap has returned to the pre-run level.  Start a privileged `powermetrics
--samplers cpu_power,thermal -i 1000` capture in another terminal before the
next attempt.  Only then run the one warm-up plus three measured runs for each
2T/4T workload leg.
