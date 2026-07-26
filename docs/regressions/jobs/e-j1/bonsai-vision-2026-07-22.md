# E-J1 — fast local vision via Bonsai mmproj (2026-07-22)

The image path worked on Qwen but at **8+ minutes per inference** on the 16 GiB
host (docs/regressions/jobs/e-j1/qwen-image-thinking-fix-2026-07-22.md). Bonsai
(the Qwen3.6-27B ternary/1-bit model already installed here, served by the
`prism-llama.cpp` fork) ships an **optional vision tower** as a separate mmproj
pack. Wiring it gives a vision backend that is ~35× faster.

## What was missing vs. what was here

- **Runtime was already capable.** `~/.samosa/backends/prism-llama.cpp/build/bin/
  llama-server` (v9596) has `--mmproj`, `--mmproj-url`, `--mmproj-auto` and the
  `mtmd`/clip vision code compiled in (`clip.cpp.o` present).
- **The vision pack was not on disk.** Only `Bonsai-27B-Q1_0.gguf` (text) was
  present. The 1-bit repo `prism-ml/Bonsai-27B-gguf` publishes
  `Bonsai-27B-mmproj-Q8_0.gguf` (0.63 GB, HQQ 4-bit) and a BF16 reference; the
  mmproj is a separate vision encoder over the same Qwen3.6-27B base, so it pairs
  with the 1-bit weights. Downloaded the Q8_0 pack (600 MB, GGUF v3) into
  `~/.samosa/models/bonsai-27b-1bit/`.
- **The gateway did not wire it.** It hardcoded `bonsai → supports_images:false`
  and launched llama-server with no `--mmproj`.

## Gateway changes (src/samosa_gateway.c)

- New config `bonsai_mmproj` (`SAMOSA_BONSAI_MMPROJ`, default
  `models/bonsai-27b-1bit/Bonsai-27B-mmproj-Q8_0.gguf`).
- New `backend_supports_images(g, name)`: qwen always (built-in tower); **bonsai
  iff the mmproj file exists**; ornith never. Drives both `/healthz` and
  `/v1/backends` (no more hardcoded flags).
- `backend_start` now adds `--mmproj <pack>` for the bonsai backend when the pack
  is present (dynamic argv + execv); text-only serving and Ornith skip it.
- Regression test: `tests/test_compiled_gateway.sh` drops a fixture mmproj and
  asserts `/v1/backends` reports bonsai `supports_images:true` (ornith false).

## Live result — fast, passing, correct

Bonsai backend, real mmproj, full-resolution rendered JSS page (`v109i03` p1,
543×768), `/v1/jobs/definition/run`:

```
healthz: backend=bonsai  supports_images=true   (ready in 4 s)
model_call_seconds = 14.371   wall = 15 s   swap used = 0.00M
{"status":"passed","extracted":{
  "title":"openTSNE: A Modular Python Library for t-SNE Dimensionality Reduction and Embedding",
  "journal":"Journal of Statistical Software"}}
```

- **title + journal read correctly from the image.**
- **~14 s vs Qwen's 8+ min** — a genuinely usable local vision path.
- authors/year came back absent — Bonsai emitted only the required fields
  (title/journal). Populating the optional fields is a prompt/schema nuance, not
  a structural blocker.
- `make jobs-test` and `make test` green.

## Follow-ups done (2026-07-22, same session)

- **All schema fields now emitted (was the real cause of empty authors/year).**
  `schema_fields_prompt` listed only the *required* keys and said "no other
  keys", so the model was told to omit optional fields. Fixed to list **all**
  declared properties with "use null when not present". Re-verified on Bonsai:
  the record now carries `title`, `journal`, `authors`, `year` (~18 s).
  **Remaining inaccuracy is OCR, not structure:** the 1-bit Bonsai misread the
  diacritic author names ("Poličar"→"Goljaric") and the year (2024→2021).
  Improving that means the BF16 mmproj (0.93 GB) or the 2-bit Ternary variant —
  a model-quality choice, not a code fix.
- **Vision-backend guard (the safe half of "routing").** An image unit on a
  text-only active backend (ornith) no longer gets sent to a blind model; it is
  queued `review_required` with reason `vision_backend_required`. Tested offline
  (guard fires on ornith; the same job passes after selecting bonsai). Full
  auto-switching to a vision backend mid-job (two live backends) is deliberately
  not done on the 16 GiB host — selecting bonsai/qwen for image jobs is the
  supported flow.

## Still open

- **OCR accuracy of fine print** — try the BF16 mmproj or 2-bit Ternary Bonsai.
- **Packaging.** The 0.63 GB mmproj is not yet in the HF release; it can ship as
  an opt-in vision pack the same way PDFium does (owner-gated).
- Multi-image / per-page image reduction still not built.
