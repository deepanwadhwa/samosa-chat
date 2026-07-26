# E-JI1 / E-JI2 — 50-file real Ornith gate (2026-07-23)

The owner reduced this synthetic gate from 500 files to **50 files**. The
fixture contains 25 CamScanner-named image decoys, 15 IMG image decoys, seven
unrelated text notes, and three planted files. It was run in an isolated
gateway on ports 8862/8863; the owner's gateway was not used.

## Result: PASS

Cold goal: *Find all files related to the Poličar 2019 research.*

- 50 filenames triaged in four compact 16-file-or-smaller batches.
- 50 first-page skims completed; the scan was read through OCR.
- The classifier shortlisted two files.
- Both targets were verified and returned with page-1 evidence:
  `policar_research_2019.txt` and `CamScanner_03-15-2024.png`.
- Cold wall time: **182.144 s**.
- Observed SSE phase-boundary wall time: triage through 43.398 s; skim
  increment 56.826 s; classify increment 38.033 s; verify increment 43.888 s.
- Cold OCR invocations: **41** (one per image file).

Warm goal: *Find Titli's rabies certificate specifically.*

- 50 skims completed from the same read cache.
- OCR invocations: **0**, measured by a counting wrapper around the actual
  `samosa-ocr` executable.
- The single verified result was `titli_rabies_certificate.txt` with its
  certificate text as evidence.
- Warm wall time: **174.125 s**.
- Observed SSE phase-boundary wall time: triage through 69.994 s; skim
  increment 0.037 s; classify increment 57.303 s; verify increment 46.792 s.

The generated SSE evidence and machine-readable result are adjacent:
[`cold.sse`](cold.sse), [`warm.sse`](warm.sse), and
[`result.json`](result.json).

## Compact-response calibration

The earlier gate revealed that explanation-heavy batch responses could expand
to thousands of tokens. Phase A and C now request only fixed-shape coded
verdicts (no reasons, paths, prose, or Markdown), with at most 16 input files
per call. The first 16-file real triage response used 37 generated tokens.

The read-only sampler observed a gateway-plus-backend peak of **6,211,984 KiB
(~5.92 GiB)**. macOS reported **365.31 MiB swap in use** at the largest sampled
point (a machine-wide observation, not a claim that this run created it).
Thermal telemetry was not sampled because privileged `powermetrics` was not
available; no thermal conclusion is claimed.
