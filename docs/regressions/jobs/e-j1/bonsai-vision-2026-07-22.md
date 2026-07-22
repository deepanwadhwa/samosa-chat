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

## Open / follow-ups

- **Coverage of optional fields** — tune the extraction prompt (or make the JSON
  grammar require all schema keys) so authors/year are emitted.
- **Routing.** Image jobs require a vision backend to be the *active* one
  (bonsai/qwen). Auto-routing image units to a vision backend when the active one
  is text-only (ornith) is a separate enhancement.
- **Packaging.** The 0.63 GB mmproj is not yet in the HF release; it can ship as
  an opt-in vision pack the same way PDFium does (owner-gated).
- Multi-image / per-page image reduction still not built.
