# Regression ledger

This ledger records release-gate outcomes separately from the directional
mechanism experiments in `THINKING_DIAGNOSIS.md`. A release pass requires
natural thinking closure, a non-empty and correct final answer after
`</think>`, model end-of-turn, repetition checks, and machine-safety checks.
Forced closure never passes automatically.

## Superseded group-32 fail-fast smoke

Artifact: `GROUP32_BASELINE.md`

| Case | Variant | Seed | Result | Closure | Correct final | Decode | Expert read | Safety |
|---|---:|---:|---|---|---|---:|---:|---|
| arithmetic counts | A | 11 | **INVALID GATE** | forced at 256 | no | 6.54 tok/s | 114.94 GB | pass |
| arithmetic counts | B | 11 | NOT RUN | fail-fast | not evaluated | - | 0 GB | protected |

Variant A reached the correct calculation inside reasoning but did not close
naturally. Its post-closure answer was incomplete. Stopping variant B was the
correct action under the declared fail-fast rule, but the rule itself was
confounded: it allowed only 256 thinking tokens and 512 total tokens without an
upstream control. This cell cannot attribute failure to group-32.

## Upstream-calibrated correction

Six OpenRouter Qwen3.6 FP8 controls used 353--616 reasoning tokens and 587--928
completion tokens. All stopped naturally and answered correctly. The same
local group-32 variant A/seed 11 then ran with a 1,024 thinking budget and
2,048 outer ceiling:

| Case | Variant | Seed | Result | Closure | Correct final | Decode | Expert read | Safety |
|---|---:|---:|---|---|---|---:|---:|---|
| arithmetic counts | A | 11 | **PASS** | natural at 933 total tokens | yes | 4.85 tok/s | 376.77 GB | pass |

The corrected result invalidates the old release-blocking interpretation for
this prompt. It does not establish broad release stability; expansion must now
be gated on task-family upstream calibration and local/upstream parity.

The reusable runner is `tools/run_regression_gate.py`; the two-case definition
is `tests/regression_cases_group32_smoke.json`. Each future run writes stdout,
stderr, a structured result, pre/post swap and memory state, macOS thermal
state, and disk margin. It terminates a run on thermal/performance warnings,
throttled pages, low disk/memory margin, or meaningful swap growth.

The recalibrated case is `tests/regression_cases_group32_recalibrated.json`.
Upstream protocol, limitations, results, and structured record paths are in
`UPSTREAM_CONTROL_2026-07-14.md`.
