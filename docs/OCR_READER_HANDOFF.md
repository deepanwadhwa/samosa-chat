# PDF reader efficiency and OCR routing — implementation handoff

Updated: 2026-09-09 (America/New_York).

## Checkpoint: WeatherNext regression follow-up

The earlier checks missed PDFs above the reader's 20 MiB limit. The user's
22,047,900-byte `weathernext_3.pdf` exposed that gap. The subsequent correction
and exact-file live acceptance are recorded at the top of
[OCR_REVIEW_FIXES.md](OCR_REVIEW_FIXES.md). Do not treat the historical OCR
passes as evidence that this particular upload worked before that correction.
The current installed release is `dev-f740fa6ddcb0`; the real Ornith 9B chat
returned all 25 authors using only native text on page 1, and reused that
evidence on the follow-up. End-to-end answers took about 90 seconds despite
the subsecond native read; no OCR ran. See the review record for exact timings,
artifact hashes, commands and scope of verification.

## Earlier OCR checkpoint

The review defects are fixed and recorded in [OCR_REVIEW_FIXES.md](OCR_REVIEW_FIXES.md):
strict OCR validation/cache recovery, uncertainty-preserving evidence,
text-aware aggregate image routing, and interruptible document subprocesses.
The verified release at that earlier checkpoint was `dev-23918e2d0d7f`.

- Workspace: `/Users/deepanwadhwa/Documents/samosa-chat`.
- Installed release at that earlier checkpoint: `dev-23918e2d0d7f`.
- `/healthz` reports `ready:true`, `supports_documents:true`, PDFium enabled and
  OCR `pack_ready:true`.
- Corrective OCR work is uncommitted in `src/samosa_extract.c`, `src/samosa_gateway.c`,
  `Makefile`, `tools/install_local_dev.sh`, the OCR docs and the document-reader
  regression tests. This document is also new.
- The worktree was already extensively dirty. Most of the overall `git diff`
  is pre-existing work (including HTML/DOCX, multimodal, audio, installer and UI
  changes). Do not reset, revert, stage or overwrite unrelated changes.
- No agent was spawned. This document is intended for a subsequent agent.

## User objective and prior accepted behavior

The user uploaded a long PDF and asked for its author. The old harness read
the document unnecessarily even though the author was in its filename. The
earlier fix lets the selected chat model choose no reading, selected pages,
text excerpts/search, or a full read. The next complaint was a slow chapter
lookup with an opaque “Deciding…” status. That led to bounded batch reads,
evidence reuse, repeated-action guards and streamed public decisions.

The current request is to make file reading efficient **and accurate** by
improving the software's OCR decision, not hardcoding answers or asking the
model to inspect every page. The user explicitly accepted this direction:

1. Cheap inspection of only the requested pages.
2. Prefer usable embedded text; do not OCR short title pages merely because
   they contain a logo.
3. Detect blank pages and avoid unnecessary OCR.
4. Detect substantial image regions that embedded text does not cover,
   including mixed digital/scanned pages.
5. Check text quality, not just character count.
6. OCR likely missing text regions when possible, retain good native text,
   and label uncertainty/failure honestly.
7. Tell the user what is actually happening, e.g. “Page 7 has an image region
   without enough selectable text; running OCR on that region…”.

This is deterministic reader routing. The LLM still decides **which evidence
to request**; the reader decides **how to recover text**. Geometry and Unicode
checks are heuristics, not proof that a page is scanned or that an image has
no text. Do not present them as a perfect document classifier.

See [DOCUMENT_HARNESS.md](DOCUMENT_HARNESS.md) for the earlier completed feature,
its limits and actual live timings. Its reported test passes predate this OCR
patch and must not be treated as verification of this patch.

## Current implementation map

Use symbols rather than relying on line numbers, which will move.

### `src/samosa_extract.c`

- Added `<math.h>` for finite-number validation.
- Originally bumped extractor version to `reader-v5-page-inspection`, retaining
  `;pdfium)` / `;no-pdfium)` suffixes. The version change invalidates old
  extraction caches after the routing and output-contract changes.
  This changes the reader fingerprint and invalidates old extraction caches.
- Replaced the top-level boolean `page_has_raster_figure()` helper with
  `PageInspection` and `inspect_*` helpers.
- `inspect_object()` traverses nested PDF form objects with composed
  transforms; collects visible image bounds and counts; bounds traversal to
  depth 16, 4,096 objects, 128 images.
- `inspect_rect()` maps PDF-space bounds through `FPDF_PageToDevice` into
  normalized rendered-page coordinates (top-left origin), accounting for page
  rotation/CropBox. **Nested/rotated/cropped cases still need tests.**
- A 32x32 occupancy grid estimates union image coverage and spatial coverage
  by useful text. Text boxes receive one grid-cell padding.
- `inspect_page()` samples at most 50,000 character records, treating missing
  Unicode, replacement characters, controls and BMP private-use characters as
  suspicious. Whitespace does not count as useful text. This is deliberately
  not an English-only dictionary or alphabet check.
- Provisional thresholds: text quality >= 0.85; individual image area >= 0.12
  of the page; an image is considered covered when padded text covers >= 0.45
  of its grid cells and the page has >= 50 useful characters.
- Healthy digital pages do not undergo preview rendering. Pages with no
  useful or suspicious characters get a 256x256 white-background preview;
  only previews with every RGB channel >= 253 are classified blank.
- `emit_inspection()` adds the following page JSON fields under `inspection`:
  `version`, `image_count`, `image_coverage`, `text_quality`, `usable_chars`,
  `suspicious_chars`, `blank`, `incomplete`, `kind`, `reason`, `needs_ocr`,
  `ocr_region`, `ocr_bounds`.
- Existing `text`, `text_chars`, `has_raster_figure`, page indices/counts and
  token fields remain. `--json-pages` still has a hard five-page batch cap.
- Added `--render-ocr-ppm FILE.pdf PAGE OUTPUT.ppm [LEFT TOP RIGHT BOTTOM]`.
  Bounds are finite normalized top-left coordinates in [0,1]. It renders a
  maximum 2,000-pixel page long edge and emits the selected crop with a small
  margin. Existing `--render-ppm` remains 768 pixels for its other consumers.
  It currently renders the full bitmap before writing only the crop; it does
  **not** avoid full-page rasterization. OCR gets only the cropped pixels.
  Existing exclusive/no-follow output-file handling is retained.

### `src/samosa_gateway.c`

- Introduced `DocumentReadProgress`, `doc_read_progress()` and
  `doc_read_with_progress()`. `doc_read_handler()` is now a compatibility
  wrapper calling the latter with a NULL progress argument.
- `attachment_document_json()` and selected range reads now receive the
  per-turn progress callback; inspection/OCR statuses reach the chat UI.
- New reader uses `inspection.needs_ocr` when available, and falls back to the
  previous sparse-text/image rule for older extractors/test doubles.
- Added cache-reuse, batch-inspection, direct-text, blank-page, OCR-start and
  OCR-complete status messages. Cancellation is checked before batch/page work,
  after child completion, before fallback/publication, and before launching OCR
  after rendering. The owned process group is force-killed if a cancelled group
  leader exits while a descendant remains.
- New inspected pages use `--render-ocr-ppm`; valid region bounds become its
  four optional CLI arguments. Legacy sidecars still use `--render-ppm`.
- Avoids launching OCR if the render process fails.
- Region OCR results keep the native text as an additional line with
  `reader=text_layer`, and use source `ocr_with_text_layer`. Existing OCR lines
  follow. No duplicate-line filtering has been implemented.
- Empty OCR results set `needs_review=true`. OCR confidence reporting remains.
- `emit_text_layer_page()` now marks `text_layer_ocr_unavailable` fallback as
  needing review rather than pretending complete success.
- `reshape_doc_read_result()` now preserves the page's `inspection` object.
- Reader cache contract is `reader-v3` for full reads and
  `reader-v3-pages-<start>-<count>` for selected ranges. Retryable, malformed,
  cancelled and incomplete results are rejected by the shared cache policy;
  the outer full-attachment cache applies the same policy. A planner
  `refresh:true` argument bypasses an otherwise valid range cache when a caller
  explicitly retries; the public activity says which pages are being retried.
- OCR framing is checked before the generic recursive JSON parser, so deeply
  nested malformed input is rejected at a bounded depth. Incomplete page
  inspection is carried through `needs_review`, a fixed evidence warning, and
  top-level `retryable:true` rather than being presented as clean completion.

## Future hardening and limitations

### 1. Remaining correctness and robustness limits

- **Tiled scans:** aggregate image coverage now triggers OCR for multiple small
  tiles, but more complex overlapping/tiled layouts still need broader fixtures.
- **Illustrations versus scans:** a large photo without embedded text can
  trigger region OCR. That is conservative for accuracy, but not semantic
  image classification. Decide whether the existing OCR detector can cheaply
  reject non-text regions; do not silently skip arbitrary image text based
  solely on native text elsewhere. Record limits clearly.
- **Blank preview:** downsampling and the near-white threshold can miss faint
  or very tiny marks. Add a faint-text fixture and adopt a conservative
  uncertain/second-check path if necessary. Blank is currently a heuristic,
  not proof. Account for annotations/widgets and vector-only text.
- **Unicode/geometry:** test CJK/other scripts, hidden OCR layers, malformed
  Unicode maps, text outside CropBox, nested/transformed forms and page
  rotation. PUA characters can be legitimate in specialized documents; mark
  uncertainty rather than asserting corruption with certainty.
- **Character cap:** >50,000 chars forces full-page OCR now. Confirm this is a
  useful bounded fallback, and do not throw away plentiful good native text
  merely because inspection was capped.
- **Native text preservation:** mixed and whole-page OCR paths now retain
  available native text, but mixed-source line ordering and duplicate text need
  explicit treatment; do not deduplicate away unique text.
- **OCR confidence remains heuristic:** low confidence is retained as an
  explicit warning, but it is not a proof that the recovered text is wrong.
  A caller can use `refresh:true` to retry a valid but uncertain range.
- **Cancellation coverage:** the deterministic contract test covers a silent
  extractor that ignores SIGTERM. Add separate render- and OCR-specific
  cancellation fixtures if those subprocesses gain independent wrappers.
- **Rendering failures/temp paths:** rendering uses a PID/page-derived PPM path
  and the old unconditional unlink pattern. Preserve exclusive file creation;
  preferably use a unique per-call temporary directory and unlink only files
  created by that call. Failed rendering must not consume a stale image.
- **Crop provenance:** OCR bounding boxes refer to crop pixels, unlike page
  geometry. Retain crop/coordinate-space metadata or map them appropriately;
  do not imply they are normalized page boxes. Progress should describe the
  validated actual operation (currently emitted before bounds validation).
- Add a clear policy/contract version wherever needed to invalidate stale
  routing decisions if thresholds or output semantics change again.

### 2. Add OCR-specific tests before live acceptance

Use deterministic fixtures and assert actual reader/OCR invocations, not just
final model prose. Suggested test matrix:

| Fixture | Expected behavior |
| --- | --- |
| Short digital title page | Native text; no preview/OCR escalation |
| Title plus small logo | Native text; no OCR |
| Dense digital page | Native text; no OCR |
| Empty page / whitespace-only page | Blank classification; no OCR |
| All-white scanned image | Conservative blank handling; no unnecessary OCR |
| Full scanned text page | OCR; recovered sentinel text |
| Scan plus short footer | OCR; footer alone is not complete evidence |
| Scan plus >50 unrelated digital characters | OCR missing image region; keep native text |
| Mixed native text plus scanned inset | Crop OCR and retain both unique sentinels |
| Full image with a good, spatially covering OCR layer | Reuse embedded text |
| Page tiled with small scan images | Aggregate detection; not mistaken for digital |
| Nested form / transformed image | Correct image bounds and OCR crop |
| Rotated page / non-default CropBox | Correct region and recovered text |
| Faint text / vector-only text / annotations | Not falsely “blank” |
| Broken Unicode mapping | OCR recovery or honest uncertainty |
| Non-Latin native text | No English-only rejection |
| Invalid OCR JSON / render failure / unavailable OCR | Explicit incomplete result; retry not poisoned by cache |
| Cancellation at inspection / render / OCR boundary | No later I/O or cache publication |
| Repeated identical range | Cache reuse, no new OCR |

Added `tests/test_pdf_ocr_routing.py`, a reproducible ReportLab/Pillow fixture
runner. It verifies digital, blank, full-scan and mixed-page inspection, crop
render dimensions, native-text preservation and the OCR sidecar response. It
does not replace broader production-document coverage, but the installed OCR
pack also recovered the generated scanned sentinel in live execution.
The document-reader harness now includes a controlled OCR sequence (malformed
zero-exit output followed by repaired text), low-confidence output, incomplete
inspection and explicit refresh assertions. Keep those fixtures isolated from
the real-extractor mode; do not alter the user's production model/runtime just
to run tests.

The PDF skill was read completely at:
`/Users/deepanwadhwa/.codex/plugins/cache/openai-primary-runtime/pdf/26.727.11326/skills/pdf/SKILL.md`.
It calls for rendering/visually inspecting generated PDF fixtures. The generated
scan fixture was rendered with Poppler and visually inspected; its two sentinel
lines were legible and correctly recovered by the OCR pack.

### 3. Regression commands and tools

Run from the workspace root. Avoid running scripts that rebuild the same
executables concurrently with integration tests using those executables.

PDFium SDK available locally:
`/Users/deepanwadhwa/Documents/samosa-pdfium-artifacts/chromium-7961/mac-arm64-unpacked`.

```sh
make samosa-extract PDFIUM_DIR=/Users/deepanwadhwa/Documents/samosa-pdfium-artifacts/chromium-7961/mac-arm64-unpacked
make samosa-gateway test_fake_openai_backend
SAMOSA_EXTRACT="$PWD/build/samosa-extract" sh tests/test_samosa_extract.sh
python3 tests/test_document_harness.py
SAMOSA_REAL_DOCUMENT_EXTRACT="$PWD/build/samosa-extract" python3 tests/test_document_harness.py
sh tests/test_doc_read_pdf_paging.sh
sh tests/test_attachments.sh
sh tests/test_document_context_prefix.sh
node tests/test_composer_perf.mjs
node tests/test_composer_ui.mjs
git diff --check
```

Also test a no-PDFium build in a separate build directory so it cannot replace
the PDFium-capable binary intended for installation:

```sh
make samosa-extract BUILD_DIR=build/ocr-portable-check
SAMOSA_EXTRACT="$PWD/build/ocr-portable-check/samosa-extract" sh tests/test_samosa_extract.sh
```

`pdftoppm` is available at `/opt/homebrew/bin/pdftoppm`. The repo `.venv` already
had Pillow; this task installed ReportLab 5.0.1 there using:
`uv pip install --python .venv/bin/python reportlab pillow`.
Use `.venv/bin/python` for ReportLab fixture generation, and `tmp/pdfs/` for
temporary rendered fixtures. No project dependency manifest was changed.

### 4. Live acceptance and installation

The deterministic suite and the real OCR sentinel check passed. Keep the
following commands for future changes; they exercise the actual OCR runtime,
not just a fake sidecar.

The existing real-model checks (opt-in, potentially slow) are:

```sh
python3 tests/test_document_harness_live.py
python3 tests/test_document_decisions_live.py --attachment a13254153fd12b7d0a147c3ccc52a64b24beb9715331b93f423ddf7d5443b309 --expect 'Why on Earth'
```

They target the running app, so make sure you know which release they exercise.
The attachment is the user's 506-page Indica PDF, already uploaded locally.
Use isolated test conversations, not the user's original conversation. Do not
print UI tokens. Existing scripts read `~/.samosa/run/ui-token` privately.

For a future reader change, update `docs/DOCUMENT_HARNESS.md`,
`docs/samosa-extract.md` and this checkpoint with actual results/remaining
limits. Rebuild any other shipped gateway variant (e.g. `samosa-jobsd`) as
appropriate before local installation. `tools/install_local_dev.sh` rebuilds
the gateway and jobs daemon from the checkout before staging a release, unless
an external packaging workflow sets `SAMOSA_INSTALL_SKIP_BUILD=1` after building
the exact artifacts.
Use the established local dev installer only after verification, restart the
local service, confirm `/healthz` readiness and installed artifact identities.
Do not publish a release, change models, or modify external repositories.

## Verification actually performed

- PDFium extractor build: **passed**, with `-Wall -Wextra -Werror`.
- Smoke read of `tests/fixtures/documents/hello.pdf`: **passed**. Returned
  `digital_text`, `usable_chars=11`, `text_quality=1`, `image_count=0`,
  `needs_ocr=false`, and preserved `Hello PDFium`.
- `git diff --check`: **passed**.
- Gateway build: **passed**, with `-Wall -Wextra -Werror`.
- New deterministic OCR routing test: **passed** with PDFium.
- Existing document harness, PDF paging, attachment, and diff checks: **passed**.
  The isolated missing-PDFium gate also behaved correctly with its expected
  nonzero (exit `2`) result. The deterministic harness uses its extractor spy for
  fault injection; its separate `SAMOSA_REAL_DOCUMENT_EXTRACT=...` run passed
  the PDFium-backed orchestration checks and intentionally skips injected OCR
  failures.
- Actual neural OCR recovery of a generated scanned sentinel: **passed**.
- Document-reader contract (strict JSON, NUL termination, deep-input rejection,
  empty-OCR uncertainty, cancellation before cache publication, and silent
  SIGTERM-ignoring descendant cancellation): **passed** in the compiled gateway
  harness; no orphan remained. The test requires the descendant PID marker, so
  a helper that never starts cannot produce a false pass. Browser visual checks
  were not performed.
- Installation and artifact identity checks: **passed** (`dev-23918e2d0d7f`).
  Gateway, extractor and jobs-daemon SHA-256 values match their `build/`
  counterparts, and the restarted `/healthz` endpoint reported ready with
  PDFium and OCR pack readiness.

The previously recorded live acceptance against `dev-77260c01b8e8` passed on
the 506-page Indica PDF:
the first turn selected pages 1–8, OCR'd pages 1–2 because they lacked reliable
selectable text, read pages 3–8 directly from the text layer, and answered
“Why on Earth” from page 7 in 69.99 seconds. The repeat turn reused saved
evidence with no new document/OCR read and answered in 42.73 seconds. The
stream included routing, inspection, OCR, OCR-complete, text-layer, checking,
and answer-preparation statuses.

## Copyable next-agent task

> Continue from `docs/OCR_REVIEW_FIXES.md`. Preserve unrelated dirty-worktree
> changes. If changing the reader contract, update both cache fingerprints and
> the targeted gateway/PDF routing regressions before installing a new local
> release. Keep the public reader warnings and cancellation ownership intact.
