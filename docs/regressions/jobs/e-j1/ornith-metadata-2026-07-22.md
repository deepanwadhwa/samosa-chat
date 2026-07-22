# E-J1 via Ornith — memory resolved + accurate extraction (2026-07-22)

**Superseded later on 2026-07-22 by the full four-PDF compiled-gateway run:**
[`jss-pdf-ornith-2026-07-22/`](jss-pdf-ornith-2026-07-22/). That run used the
reviewed PDFium archive, rebuilt/installed document support, completed all four
labeled JSS PDFs through `/v1/jobs/definition/*`, produced 4/4 `passed` records,
and scored 46/48 labeled fields with zero swap/throttled pages. This file is
kept because it records the earlier memory finding and the integration bugs that
led to the final fixes.

Runs the Jobs metadata-extraction pipeline on the real JSS PDFs through the
**Ornith 1.0 9B** backend (llama-server, Metal `-ngl 99`), on the 16 GiB M3 Air.
Qwen is not required — the architecture is model-agnostic (Models→Tools→Jobs);
Ornith is far lighter and faster than the 24 GB Qwen engine. This directly
retires the open E-J1 memory-safety concern and gives a first real accuracy
data point. Two separate bugs surfaced and are recorded.

## Result

**1. Memory pressure — RESOLVED.** The bounded pipeline held `llama-server` RSS
flat at ~5.4 GiB across the whole run; free pages and compressor pages did not
move; no swap. This is the definitive answer to the 2026-07-16 abort
([pdf-preview-aborted-2026-07-16.md](pdf-preview-aborted-2026-07-16.md)): that
was a *whole-file 20k-token* prefill at 8 threads on the 24 GB model; the current
path reads in bounded chunks and never approaches pressure.

**2. Accuracy — 2/2 correct, both `passed`.** Feeding the real JSS page-1 text,
Ornith extracted (35 s wall for both docs, temperature 0):

| Doc | title | journal | authors | year |
|---|---|---|---|---|
| v109i02 | scikit-fda: A Python Package for Functional Data Analysis | JSS Journal of Statistical Software | Ramos-Carreño, Torrecilla, Carbajo-Berrocal, Marcos, Suárez | 2024 |
| v109i03 | openTSNE: A Modular Python Library for t-SNE Dimensionality Reduction and Embedding | JSS Journal of Statistical Software | Poličar, Stražar, Zupan | 2024 |

Both correct against the source; `status:"passed"`. (v109i03's title/journal
match the earlier one-page Qwen smoke.)

## Two bugs this run surfaced (neither is memory or model capability)

**A. The installed `samosa-extract` is stale — no `--json-pages`.** The gateway's
PDF path shells out `samosa-extract --json-pages FILE 1 5`
([samosa_gateway.c:2095](../../../../src/samosa_gateway.c#L2095),
[:1702](../../../../src/samosa_gateway.c#L1702)), but the binary in every
`~/.samosa/releases/*/bin` only supports `--json` / `--render-ppm` and prints
usage + non-zero for `--json-pages`. So **PDF jobs return no text and every unit
becomes `review_required:invalid_model_output`** on the installed release. It
can't be rebuilt here — `make samosa-extract` needs `PDFIUM_DIR` (headers), and
no PDFium archive/headers exist on this machine (only the compiled
`libpdfium.dylib`). Workaround used for this run: extract whole-doc text via the
working `--json` mode and run the text path. **This is a real release-integration
bug** — the shipped gateway depends on an extractor feature the shipped extractor
doesn't have.

**B. Reasoning backends need an adequate token budget; `thinking:"off"` is not
propagated to llama-server.** Ornith is a reasoning model: it emits its chain
into `reasoning_content` and the answer into `content` only after it finishes.
With the Jobs default token budget it spent the whole budget reasoning and
returned **empty `content`** (`finish_reason:"length"`, 0 answer tokens) →
false `invalid_model_output`. Setting `inference.max_tokens: 1024` on the job
gave it room and both docs extracted cleanly. Separately, sending
`thinking:"off"` did **not** stop the reasoning (still emitted, content still
empty) — the flag isn't reaching the llama-server backend. Fixes worth doing:
raise the Jobs default `max_tokens` (or derive it) for reasoning backends, and
actually propagate `thinking:"off"` to llama-server.

## Honest scope

- Two documents, page-1 metadata, text path (not the PDF-page path, which is
  blocked by bug A). Not the full labeled four-PDF batch; not image-bearing
  pages. But it is real inference on a real model with correct, validated output
  and zero memory pressure — the E-J1 memory concern is closed; a full accuracy
  batch is unblocked once bug A (extractor) is fixed.

## Branch follow-up after this run

- `dist/install.sh` now smoke-tests the staged `samosa-extract --json-pages`
  interface whenever document support is enabled, before activating the release.
- `tools/install_local_dev.sh` now stages `samosa-jobsd` and applies the same
  extractor interface smoke, so a local dev release cannot silently install a
  gateway/extractor mismatch.
- `src/samosa_gateway.c` now honors `job.inference.max_tokens` for definition
  extraction, with a 1024-token default. This keeps reasoning backends from
  exhausting the whole completion budget before content appears.
- `tools/run_e_j1.py` now drives the compiled
  `/v1/jobs/definition/preview` and `/v1/jobs/definition/run` routes rather than
  the removed Python Jobs runner.

Supersession note: the reviewed-PDFium rebuild/install and full labeled
four-PDF batch were completed later on 2026-07-22 in
[`jss-pdf-ornith-2026-07-22/`](jss-pdf-ornith-2026-07-22/). Remaining E-J1
coverage is image/multi-image input on a vision-capable backend and a live
interactive-chat pause/resume run. A later compiled regression also added
active inference telemetry and offline interlock coverage; see
[`compiled-interlock-telemetry-2026-07-22.md`](compiled-interlock-telemetry-2026-07-22.md).
