# Upstream thinking-control pilot — 2026-07-14

## Why this control exists

The first group-32 release gate combined a 256-token thinking budget with a
512-token overall completion ceiling. It reached the correct arithmetic inside
reasoning, then forced `</think>` and returned an incomplete final answer.
Because there was no upstream arm, that cell could not distinguish model
damage from an undersized budget or an incorrect forced-close protocol.

The old cell is retained as historical data, but it is invalid as evidence
that group-32 quantization caused the failure.

## Protocol verification

Samosa previously injected only the `</think>` token. Qwen's published
thinking-budget example instead appends this transition and then continues
normal generation:

> Considering the limited time by the user, I have to give the solution based
> on the thinking directly now.

The transition is followed by `</think>` and two newlines. Qwen's documentation
also says that the demonstration's very small budget should not be used in
practice and recommends tuning to accepted latency, above 1,024 tokens for
meaningful improvements.

Samosa now tokenizes and appends the complete published transition. A focused
C test proves that injection begins only at the budget and continues through
the trailing tokens after `</think>`. Natural closure bypasses the transition.

Official reference:
<https://github.com/QwenLM/Qwen3/blob/main/docs/source/getting_started/quickstart.md#thinking-budget>

## OpenRouter arm

- Model: `qwen/qwen3.6-35b-a3b`
- Provider pinned: AkashML; fallback disabled
- Provider-declared quantization: FP8
- Sampling matched to Samosa general thinking: temperature 1.0, top-p .95,
  top-k 20, presence penalty 1.5
- Overall ceiling: 8,192 tokens
- No `reasoning.max_tokens` was sent; the experiment measures natural behavior
  inside the outer ceiling rather than imposing another forced thinking cap.
- Prompts: arithmetic variants A/B; seeds 11, 29, and 47
- Cost: $0.00412958 total

OpenRouter exposes the exact model slug and reasoning output, but its available
providers currently declare FP8 rather than BF16. It also does not pin the
upstream Git revision in the completion response. This is therefore an
upstream-compatible behavioral control, not an unquantized numerical oracle.

| Variant | Seed | Reasoning tokens | Completion tokens | Natural stop | Correct |
|---|---:|---:|---:|---|---|
| A | 11 | 388 | 693 | yes | yes |
| B | 11 | 370 | 605 | yes | yes |
| A | 29 | 383 | 665 | yes | yes |
| B | 29 | 353 | 587 | yes | yes |
| A | 47 | 616 | 928 | yes | yes |
| B | 47 | 376 | 610 | yes | yes |

All six upstream runs exceeded the old 256-token thinking budget, and three
total completions exceeded the old 512-token outer ceiling. All six closed
naturally and answered correctly. Pilot p50 reasoning length is 376 tokens;
nearest-rank p90 is 616. With only six observations from one small task family,
that p90 is descriptive pilot data, not a product-wide budget estimate.

An earlier identical seed-11 pilot returned 595 reasoning tokens and 1,016
completion tokens. The later pinned-provider run returned 388 and 693. Seeds
through this hosted stack are not a trajectory-identical oracle. Compare
distributions and pass rates, not byte-identical output, across runtimes.

The initial exact-substring scorer also marked three semantically correct
answers wrong because they used `Red balls: 7` rather than `7 red`. The runner
now supports task-specific regex assertions that accept equivalent word order
while still requiring red=7, blue=4, and total=11 in the final answer.

Structured records are in `docs/regressions/openrouter-control/` and the
reusable secret-safe runner is `tools/run_openrouter_control.py`. `.env` is now
ignored by Git, and the API key is passed to curl over stdin rather than argv
or a result file.

## Recalibrated local arm

The same group-32 artifact was rerun on variant A/seed 11 with a 1,024-token
thinking budget and a bounded 2,048-token overall ceiling. The latter exceeds
every upstream pilot completion while limiting a pathological local run.

- Result: pass
- Closure: natural; the early-stop transition was not used
- Generated: 933 tokens; model end-of-turn
- Correct final: 7 red, 4 blue, 11 total, with a valid check
- Decode: 192.161 seconds, 4.85 tok/s; 198.003 seconds total
- Expert traffic: 376.77 GB read, 217.28 GB avoided
- Peak RSS: 3.23 GB
- Safety: unchanged swapouts, zero throttled pages, no pressure or macOS
  thermal/performance warning

This does not certify group-32 broadly. It does directly overturn the old
claim that this prompt/seed demonstrated group-32 instability. The observed
failure was caused by a confounded, undersized gate.

## Revised release method

1. Calibrate each task family against an upstream behavioral control first.
   Record natural reasoning length, total completion length, natural-stop
   rate, correctness, and repetition across variants and repeated requests.
2. Choose a family-specific pilot budget from the upstream distribution plus
   answer reserve. Keep the 8,192 outer safety ceiling and Qwen transition as
   containment, not as evidence of quality.
3. Run the bounded local arm and compare rates with upstream. Do not demand
   token-identical trajectories across different samplers/providers.
4. Escalate to a rented BF16 control for strict checkpoint-level attribution
   when FP8 behavioral parity is insufficient.
5. Authorize a new weight artifact only after a matched upstream/local gap
   remains. The mixed q8-down conversion is paused rather than presumed to be
   the next fix.

## Activation-aware candidate, if a real parity gap remains

Round-to-nearest geometry is not the only available method. AWQ uses offline
activation statistics and equivalent channel scaling to protect salient
channels without mixed-precision storage. For this MoE MLP, an equivalent
candidate can scale an `up` output row and inversely scale the matching `down`
input column before quantization, preserving the unquantized function while
redistributing q4 error. It should be evaluated before accepting q8-down's
permanent 20.83% storage increase.

The current teacher stream records logits and selected full-logit calibration
positions; it does **not** yet record per-expert `silu(gate) * up` channel
statistics. A correct prototype therefore needs a new activation-statistics
hook and representative expert coverage—preferably from BF16—not merely a
different rounding call in the converter.

References:

- AWQ: <https://arxiv.org/abs/2306.00978>
- SmoothQuant's related equivalent-scaling formulation:
  <https://arxiv.org/abs/2211.10438>
