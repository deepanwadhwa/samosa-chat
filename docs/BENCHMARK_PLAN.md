# Benchmark plan

There is no single benchmark suite used identically by every model company.
Scores are comparable only when the prompt format, sampling, context limit,
tool scaffold, dataset revision, and scoring implementation match.

The upstream [Qwen3.6-35B-A3B model card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)
reports, among others, MMLU-Pro, MMLU-Redux, GPQA, HLE, LiveCodeBench v6,
SWE-bench Verified/Multilingual/Pro, Terminal-Bench 2.0, and several internal
agent/WebDev sets. Internal Qwen sets cannot be reproduced from public data,
and agent scores cannot be attributed to the base model without reproducing
the stated scaffold.

## Samosa evaluation ladder

Every task family starts with an upstream control using the same prompt,
template, sampler, and a non-binding outer ceiling. Record the natural
reasoning and completion length distributions before choosing the local
thinking budget. OpenRouter FP8 is a low-cost behavioral control; strict
checkpoint attribution still requires a revision-pinned BF16 run.

The local release matrix is staged rather than launched as one 60-run job. Start
with two prompt variants and two seeds per task family; stop and fix any failed
cell. Expand passing families to five variants and three seeds only after the
runtime gate and machine-safety telemetry pass. At current decode rates and
expert-cache churn, the full matrix is not a cheap test on this laptop.

### 0. Runtime and stability gate (run on every release)

- Same-seed determinism for each product profile.
- Local/upstream parity at a budget calibrated from the upstream task-family
  distribution; do not treat hosted seeds as byte-identical trajectories.
- Natural versus forced `</think>` closure, structural completion, and answer
  correctness recorded as separate fields; forced closure is never an
  automatic pass.
- Model end-of-turn versus token-ceiling stop recorded separately.
- Global, tail, line-run, and online token-cycle repetition checks.
- Task-specific markers/tests (for example `</html>`, compiled code, or an
  exact numeric answer). Accept semantically equivalent formatting; do not use
  brittle word-order substrings as a correctness oracle.
- Two-core default safety telemetry: RSS, swap delta, thermal/performance
  warning, expert blob size, cache hits, expert bytes read/avoided, total bytes
  read/written, and decode speed.

### 1. Public model benchmarks

- MMLU-Pro and GPQA for knowledge/reasoning.
- LiveCodeBench for code generation, using its pinned public revision.
- IFEval-style instruction-following checks.
- A public long-context retrieval/aggregation suite with lengths capped to a
  machine-safe range.

Every result must publish the exact dataset commit, prompts/template,
sampling profile, seed count, token ceiling, scorer, and Samosa commit.
Samosa scores should be compared with upstream only when those settings match.

### 2. Agent benchmarks

- SWE-bench Verified and Terminal-Bench require a reproducible shell/editing
  scaffold, isolated environments, dependency installation, and long timeouts.
- Start with a disclosed stratified subset. A full run on this fanless 16 GB
  laptop would take a long time and generate extreme expert-store read volume;
  it should be scheduled only after the subset and safety gates pass.

### 3. Product-quality WebDev set

QwenWebBench is internal, so Samosa must not present a local substitute as the
same benchmark. Maintain a public prompt set and score:

- required-section and HTML/CSS validity;
- browser render success and console errors;
- responsive layout at pinned viewport sizes;
- accessibility basics;
- screenshot-based visual review with the generated source and seed retained.

## Machine-safety policy

- Run sequentially at the cooler two-core default; four-core tests are short,
  explicit performance controls.
- Stop on any macOS thermal/performance warning, meaningful swap growth,
  unbounded RSS, repeated-token guard, or unexpected disk writes.
- Record cumulative expert bytes read. Reduce cache churn before attempting
  large full-suite runs.
- Use small stratified smoke sets before full datasets. Do not turn a 16 GB
  personal machine into a days-long benchmark worker by default.

## Artifact and performance sequencing

- Treat the groupwise-q4 conversion as the weight-quality baseline. Do not
  remove the think-code float-activation containment or claim a speed recovery
  until prompt-variant/seed tests show that the new weights pass without it.
- Do not authorize mixed q8-down or another larger artifact from NRMSE alone.
  First demonstrate a matched local/upstream behavioral gap. If one remains,
  compare a same-size activation-aware q4 transform against mixed q8-down;
  report quality, bytes per expert, cache traffic, RSS, and speed together.
- Changed-container upgrades now stage a complete versioned release, verify
  declared byte sizes and SHA-256 digests, compile and smoke-test it while
  inactive, then atomically switch one `current` symlink. A corrupt-payload
  integration test proves the active pointer remains unchanged on failure.
  `samosa doctor` reports the manifest quantization format, flags the legacy
  whole-row store, and discloses retained rollback/legacy directories. The
  inactive release requires enough side-by-side disk space; the installer
  calculates this from `release-manifest.tsv` before downloading.
- Route recording/replay exists, but a co-activation-derived physical expert
  layout is a separate experiment. Layout alone does not reduce cache misses
  in the current individual-`pread` path. Require a trace analyzer, a
  coalesced-read/readahead implementation, and a same-trace A/B showing lower
  expert bytes read and equal outputs before baking a layout into a release.
- Whole-layer sequential prefill reads (`SEQ_PREFILL=1`) are already
  implemented and measured. On the 2026-07-12 cold-cache control, scattered
  reads consumed 2.6 seconds of a 108-second prefill while the sequential path
  consumed 5.5 seconds; prefill was about 97% compute-bound. Keep that path
  experimental and pursue kernel/batching compute improvements instead.
- A repetition-triggered thinking close is a salvage experiment, not an
  automatic pass. Report the trigger separately and require answer-correctness
  improvement over a clean stop before enabling it by default.
