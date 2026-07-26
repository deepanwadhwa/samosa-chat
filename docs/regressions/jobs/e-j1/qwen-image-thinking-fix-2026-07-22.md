# E-J1 Qwen image — two structural fixes, single-image extraction now passes (2026-07-22)

Continues the other agent's Qwen image smoke
([qwen-image-smoke-2026-07-22/report.md](qwen-image-smoke-2026-07-22/report.md)),
which found the image job returned a JSON scalar (`extracted:0`,
`invalid_model_output`) instead of a schema object. Two independent bugs; both
fixed; the image job now returns a **passing** object on a real rendered JSS page.

## Bug 1 — Qwen ran with reasoning on (regression)

`model_extract` disabled reasoning only the llama.cpp way
(`chat_template_kwargs:{enable_thinking:false}`). The **Qwen C engine ignores
that**; it only honors a top-level `"thinking"` field
(`qwen36b.c`: `json_get(root,"thinking")` → `"off"`). The image-plumbing rewrite
had dropped the top-level `"thinking":"off"` the original code sent, so Qwen
burned its budget reasoning. Restored it (kept the llama.cpp field for
Ornith/Bonsai). **Confirmed on Qwen text:** `thinking:"off"` → clean
`{"ok": true}`, `finish_reason:stop`, 6 tokens, ~8 s.

## Bug 2 — fenced JSON was not recovered (the real image blocker)

Even with reasoning off, the image job still recorded `0`. Captured Qwen's raw
output on the rendered page — it returns a **correct object wrapped in a
markdown fence**:

```
```json
{ "title": "openTSNE: A Modular Python Library for t-SNE Dimensionality
   Reduction and Embedding", "journal": "Journal of Statistical Software", ... }
```
```

`definition_record` called `json_parse()` **directly** on that content, which
fails on the leading ```` ```json ````, so the record fell to
`review_required`. Ornith/llama-server return bare JSON, which is why the PDF
batch passed and only Qwen images broke. This is the J1.5 recovery contract the C
port was missing.

**Fix:** added `first_json_object()` — a string-aware scanner that returns the
first balanced `{…}` (braces inside strings don't miscount), so fenced or
prose-wrapped output is recovered. Applied in `definition_record`. This hardens
**every** backend, not just Qwen. Regression test: a new "Fenced JSON probe" case
in `tests/fake_openai_backend.c` returns a ```` ```json ````-wrapped object and
`tests/test_compiled_gateway.sh` asserts the record is `passed` (not
`invalid_model_output`).

## Verification — the image job now passes

Real rendered JSS page (`v109i03` page 1), Qwen vision, fixed gateway,
`/v1/jobs/definition/run`:

```json
{"status":"passed","extracted":{
  "title":"openTSNE: A Modular Python Library for t-SNE Dimensionality Reduction and Embedding",
  "journal":"Journal of Statistical Software","authors":"…","year":"…"}}
```

- **title correct**, **journal correct** — read from the image.
- authors/year were hallucinated because the test image was downscaled to 384 px
  (small text unreadable) to keep inference tractable; a plain "what text do you
  see" prompt on the same image returned the correct title + journal in prose, so
  vision works. Full-resolution field accuracy needs a full-res image, which runs
  **8+ min per inference** on the 16 GiB host (cold expert streaming) — measured,
  not iterable here.
- Safety: `vm.swapusage` used 0.00M throughout.

`make jobs-test` and `make test` green.

## Status

- **Fixed + tested:** Qwen thinking propagation; fenced/prose JSON recovery
  (regression-tested offline). Single-image extraction produces passing records.
- **Still open:** full-resolution field accuracy (hardware-bound: minutes per
  vision inference) and multi-image / per-page image reduction. `docs/TASKS_JOBS.md`
  status updated accordingly.
