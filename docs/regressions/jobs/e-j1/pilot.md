# E-J1 (pilot) — the J1 runner works end-to-end on the real 24 GB model

Ran on: 2026-07-16
Machine: reference **16 GB M3 MacBook Air (macOS, arm64)**.
Branch: `issue-7-jobs` (foundation `1c42f4b` + planner `7fbbf7d`).
Model: `~/Documents/samosa-models/qwen36_group32_i8` (experts.bin 20.9 GB +
resident.safetensors 3.0 GB), tokenizer `tokenizer_qwen36.json`.

## Scope (honest)

This is a **pilot**, not the full E-J1. It runs the whole J1 pipeline on the
**real model** over **3 hand-labeled text receipts**, with a **portable `make`
build (single thread)**. **Not yet done:** image/vision inputs, the 10–20-item
labeled set, multi-thread (`make omp`), and the swap-delta wear measurement.
Those remain for the full E-J1.

## What ran

`make` → `./qwen36b tokenize --count` → `./qwen36b --serve` (real model) →
`samosa_jobs.py validate → preview → run (interrupted + resumed) → status`.

## Results

**Engine additions (both verified against the real engine):**
- `tokenize --count r1.txt` → **92** (exact tokenizer count, no model load).
- `GET /internal/v1/status` → `{"interactive_active":false,"last_interactive_ts":
  null,"queue_depth":0,"inference_busy":false,"threads":1}`.

**Extraction correctness — 3/3 correct vs. hand labels:**

| file | merchant | date | subtotal | tax | total | currency | verdict |
|---|---|---|---|---|---|---|---|
| r1 | JOE'S COFFEE HOUSE | 2026-07-10 | 7.75 | 0.62 | 8.37 | USD | ✅ all correct |
| r2 | TECH MART | 2026-06-28 | 37.49 | 3.0 | 40.49 | USD | ✅ all correct |
| r3 | Cafe Bleu | 07/15/2026 | 5.5 | null | 5.5 | EUR | ✅ (subtotal=total inferred; no explicit subtotal/tax on the receipt — defensible) |

All 3 validated `status:passed` (no errors/warnings); the `subtotal + tax ~= total`
domain rule held on r1/r2 and was correctly skipped on r3 (tax null).

**Cost (single thread, portable build):**
- `preview` (1 unit): **54.4 s** wall.
- `run`: ~**50–56 s/unit** (per-unit prompt 216–264 tokens, output 51–61 tokens).
  A portable build runs 1 thread; `make omp` / multi-thread not measured here.

**Idempotency (J1.8):** the first `run` was interrupted after 2 units (2-min shell
cap); the resume reported **"3 inputs found, 1 new, 2 already processed"** and
finished only the 3rd. This is the "100 more tomorrow" behavior, confirmed.

**Recovery (J1.7):** the interrupted unit (SIGTERM mid-inference) left an
`item_running` with no terminal event; on resume it was reset to READY and
re-run (events show 4 `item_running` / 4 `item_ingested` for 3 units, 3
`item_complete`, 1 `job_complete`). Correct.

**Footprint / safety:** system memory free went 80% → 66% (~2.2 GB for the
resident model, consistent with the ~2.5 GB expectation); 112 GB disk free
throughout. No OOM, no thermal issue on this small run.

## Bugs found (E-J1's job)

- **B1 — merged output ignores `job.output.dir`.** `write_merged_output`
  ([dist/samosa_jobs.py:1273](../../../dist/samosa_jobs.py)) writes to
  `<job_dir>/results/output.jsonl` and never reads `job['output']['dir']` (it
  reads only `format`). The user's configured output directory is silently
  unused. The records *are* produced (in the job dir), so this is a location bug,
  not data loss. **FIXED** `c85ed3e` (regression test `TestMergedOutput`).
- **B2 — provenance timing is null.** `prefill_seconds`/`decode_seconds` are
  `None` in every provenance record. Serve's `usage` carries token counts
  (captured: `input_tokens`/`output_tokens`) but no timing, and the runner does
  not fall back to wall-clock as the card specifies. Cost numbers above are shell
  `time`, not from provenance. **FIXED (offline)** — `derive_timing()` now records
  the runner's measured `wall_seconds` on every call and, when serve reports
  `samosa.tokens_per_second`, splits it into `decode_seconds` (from the decode
  rate) and `prefill_seconds` (the wall-clock remainder; loopback overhead is
  negligible against real prefill). When the rate is absent, prefill/decode stay
  `null` rather than being fabricated. Unit-tested (`TestDeriveTiming`,
  `TestCallServe`); the **real-model** provenance timing is still to be confirmed
  in the full E-J1 run.

## Verdict

**The J1 pipeline works on the real model for text extraction** — correctness is
excellent on this labeled set, and idempotency, crash recovery, validation, and
both engine additions all function end-to-end. Two minor bugs (B1 output
location, B2 provenance timing). The full E-J1 (vision inputs, larger labeled
set, multi-thread, swap-delta wear) is still to run.
