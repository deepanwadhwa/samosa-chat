# E-J1 text-only baseline — 2026-07-16

This is a measured **starter baseline**, not E-J1 acceptance. It adds a
source-controlled ten-input text corpus plus a harness that records Jobs
artifacts, per-field labels, event timing, and machine-safety snapshots. It
does not substitute synthetic text receipts for the required labeled image/PDF
inputs or interactive-chat interlock trial.

## Environment

- Reference machine: 16 GB M3 MacBook Air, macOS Darwin 25.5.0, arm64.
- Power: AC; battery 100% charged.
- Model: local `qwen36_group32_i8` groupwise-q4 experts (group size 32).
- Corpus: [`tests/fixtures/jobs/e_j1_text`](../../../../tests/fixtures/jobs/e_j1_text)
  with ten hand-labeled text receipts and
  [`e_j1_labels.json`](../../../../tests/fixtures/jobs/e_j1_labels.json).
- Runner: [`tools/run_e_j1.py`](../../../../tools/run_e_j1.py).

## Commands

```sh
make omp

SNAP=/Users/deepanwadhwa/Documents/samosa-models/qwen36_group32_i8 \
TOKENIZER=/Users/deepanwadhwa/Documents/samosa-chat/tokenizer_qwen36.json \
OMP_NUM_THREADS=2 ./qwen36b --serve --port 8642 \
  --tokenizer /Users/deepanwadhwa/Documents/samosa-chat/tokenizer_qwen36.json

python3 tools/run_e_j1.py \
  --results /tmp/samosa-e-j1-text-baseline \
  --serve-url http://127.0.0.1:8642 \
  --engine /Users/deepanwadhwa/Documents/samosa-chat/qwen36b \
  --tokenizer /Users/deepanwadhwa/Documents/samosa-chat/tokenizer_qwen36.json
```

The first full corpus run was made before the OpenMP rebuild, so its server
correctly reported `threads: 1`; it is a portable/single-thread baseline. The
subsequent OpenMP server reported `threads: 2` and was verified with one
bounded preview.

## Results

| Measurement | Result |
|---|---:|
| Labeled text inputs | 10 |
| Correct fields | 60 / 60 (100%) |
| `review_required` / failed | 0 / 0 |
| Single-thread preview wall time | 52.717 s |
| Single-thread ten-item run wall time | 504.257 s |
| Single-thread active inference time | 502.310 s |
| OpenMP two-thread preview wall time | 30.325 s |
| OpenMP two-thread prefill / decode | 20.537 s / 9.788 s |
| OpenMP two-thread decode rate | 6.13 tok/s |

The ten-item output validated every schema and domain rule. The two-thread
preview of `r01_coffee.txt` also produced the expected six fields and recorded
non-null provenance timing (`input_tokens: 236`, `output_tokens: 61`).

## Safety observations

For the full single-thread corpus run, free disk was 118.650 → 118.690 GB;
`Swapouts` stayed at **188362**; `Pages throttled` stayed at **0**; memory free
was 66% → 65%; and macOS reported neither a thermal nor a performance warning.
Serve RSS was 2.93 GiB fresh and 4.41 GiB after the run. The two-thread preview
also completed with zero new swapouts, zero throttled pages, and no thermal or
performance warning.

## Still required for E-J1 acceptance

- A 10–20-item **representative image + text** labeled corpus; PDF/multi-page
  coverage remains unavailable until the #5 extractor lands.
- A multi-page image/document reduction run when that input path exists.
- An interactive-chat interlock trial that records `job_paused` then
  `job_resumed` while a real job is running.
- A full ten-item OpenMP/two-thread corpus run (the two-thread result above is
  deliberately only a bounded preview).

