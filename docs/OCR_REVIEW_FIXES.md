# OCR reader corrective handoff

Review date: 2026-09-09. The earlier OCR checks below did not cover the user's
22 MB WeatherNext upload and were insufficient evidence of end-to-end success.
See the WeatherNext regression record below for the subsequent fix and acceptance.

This document records the corrective implementation and its verification. It
supersedes earlier statements that the OCR work was complete before these four
defects were addressed.

## WeatherNext large-PDF regression (2026-09-09, America/New_York)

The exact uploaded `weathernext_3.pdf` is 22,047,900 bytes, SHA-256
`12563b139a446fa2c8ac0f1e72753ce5ab9029bc505ef08237cd9a7ffca7b726`.
The installed reader rejected it with `{"ok":false,"error":"file_too_large"}`
and exit 65: uploads allow 4 GiB, but the extractor imposed 20 MiB on every
format. This was a reader input-limit failure, not missing document text.
Its 43-page PDF has all 25 authors as usable native text on page 1; inspection
reports `digital_text`, `usable_text_layer`, `needs_ocr:false`.
The PDF skill's page-render inspection independently verified the author block.

Changes:

- `samosa_extract.c`: PDF signatures checked on the held file descriptor;
  PDF input ceiling now matches 4 GiB uploads. PDFium's existing custom block
  reader avoids an input-sized allocation. Native/container formats retain
  their 20 MiB limit, and CPU, wall, output and page limits remain unchanged.
  Environment overrides may only lower the format ceiling. Extractor contract
  is now `reader-v6-large-pdf`, invalidating prior reader fingerprints.
- `doc_read_with_progress()`: parse structured extractor errors even when
  the child exits nonzero; retain the real error code with JSON escaping.
- `document_task_json()`: abort a failed read with a `read_failed` activity
  carrying the actual error. Do not retry the same failed source or synthesize
  a successful evidence result from an operation failure.
- `attachment_augment()`: return an explicit reader failure instead of letting
  the fast path convert it to an unsupported answer. Oversized documents return
  HTTP 413; other failures return 422. Streaming requests emit error events.
  Recoverable partial OCR results still preserve native text and uncertainty.
- Planner guidance starts title-page lookups at page 1, expanding only when
  needed. This is model guidance, not a filename/author keyword branch in C.
- Regression tests now include a valid >20 MiB PDF with a large unreferenced
  resource, bounded reads through HTTP, format/override limits, nonzero-exit
  structured errors in streaming/nonstreaming paths, and recovery without a
  poisoned cache. The live acceptance can require every author, no OCR and a
  maximum requested page.

Two intermediate runs were deliberately cancelled because the model still
requested pages 1–3, triggering irrelevant OCR on page 2. Generic advice alone
did not change this model's decision; adding a concrete one-page authorship
action example did. Those cancelled runs are not passing acceptance.

Final runtime for acceptance: `dev-f740fa6ddcb0`, extractor
`reader-v6-large-pdf;pdfium`, selected backend Ornith 9B. Installed gateway,
jobs daemon and extractor SHA-256 values match their `build/` binaries:

```text
gateway  ecff05ccce468124939a6a4a6cc1e68b5155962f51cc55d46ed92e797c0bc301
jobsd    929775eb1c072d3555df8202ef2ae98859240b2cfb770a24d7efe1e20316da95
extract  d3409ee16d935949cb50dbcaa5b216699f7fabffd690b22a15aace32dd7576f7
```

The actual uploaded paper's cold first turn in isolated conversation
`harness-decisions-07b91c5770144441` returned all 25 authors, with no extra names,
and cited PDF page 1. Activity timestamps: selected page 1 at 10.96 s; native
read completed at 11.12 s; evidence ready at 30.01 s; full answer at 90.15 s.
No page beyond page 1 and no OCR was requested. Reading took about 0.16 s;
**90.15 s is the end-to-end latency**, not subsecond answering. Most time was
in the selected model's planning and generation. The follow-up passed at
90.63 s, returning the same 25 names and citation using saved source evidence;
it issued no new read/search and no OCR. The two-turn live test exited zero.

Recovery after the exact earlier failed assistant answer also passed in isolated
conversation `harness-decisions-06c5c803cbe04052` at 81.77 s. The planner requested
page 1, recovered its verified native-text cache and returned all 25 authors
instead of repeating the prior failure. No later pages or OCR were requested.
This replays the failed answer in a new conversation; it does not modify or
claim to have replayed the user's original stored conversation itself.
To reproduce, append `--turns 1 --prior-answer 'I cannot determine the authors
of this paper because the document content could not be read. The attached
file weathernext_3.pdf has no accessible text or metadata that reveals the
author information.'` to the command below (as one shell argument).

Final `/healthz`: `ready:true`, `generating:false`, backend `ornith`, model
`Ornith-1.0-9B-Q4_K_M.gguf`, PDF documents enabled, OCR runtime and pack ready.
Verification exercised the actual installed streaming chat endpoint and saved
source evidence, not browser clicks or a visual UI regression. Original user
conversations and attachments were preserved. No remote publishing or model
switching was performed.

Rerun the same acceptance against the installed app (do not replace it with a
fake backend or switch the user's model):

```sh
python3 tests/test_document_decisions_live.py \
  --attachment 12563b139a446fa2c8ac0f1e72753ce5ab9029bc505ef08237cd9a7ffca7b726 \
  --question 'Who are the authors of this paper?' --max-page 1 --no-ocr \
  --expect 'Stephan Rasp' --expect 'Boris Babenko' --expect 'Dominic Masters' \
  --expect 'Andrew El-Kadi' --expect 'Samier Merchant' --expect 'Guy Shalev' \
  --expect 'Ilan Price' --expect 'Fred Zyda' --expect 'Remi Lam' \
  --expect 'Sasha Shysheya' --expect 'Matthew Willson' --expect 'Stratis Markou' \
  --expect 'Shreya Agrawal' --expect 'Suhani Vora' --expect 'Mohammed Alewi Hassen' \
  --expect 'Sunny Mak' --expect 'Tom R. Andersson' --expect 'Megan Bela' \
  --expect 'Akib Uddin' --expect 'Nofar Peled Levi' --expect 'Ben Gaiarin' \
  --expect 'Ferran Alet' --expect 'Aaron Bell' --expect 'Peter Battaglia' \
  --expect 'Alvaro Sanchez-Gonzalez'
```

The attachment is private local runtime data, not committed as a test fixture.
On another machine, upload that paper and substitute the resulting attachment
ID. The reusable generated >20 MiB fixture is independent of this private file.

Final-source deterministic checks passed:

```sh
make test-document-reader-contract
python3 tests/test_document_harness.py
SAMOSA_REAL_DOCUMENT_EXTRACT="$PWD/build/samosa-extract" python3 tests/test_document_harness.py
SAMOSA_EXTRACT="$PWD/build/samosa-extract" make test-pdf-ocr-routing
SAMOSA_EXTRACT="$PWD/build/samosa-extract" sh tests/test_samosa_extract.sh
SAMOSA_EXTRACT="$PWD/build/samosa-extract" sh tests/test_doc_read_pdf_paging.sh
SAMOSA_EXTRACT="$PWD/build/samosa-extract" sh tests/test_document_context_prefix.sh
git diff --check
```

## Earlier verified state (before the WeatherNext regression)

- Workspace: `/Users/deepanwadhwa/Documents/samosa-chat`.
- Verified installed release after the fixes: `dev-23918e2d0d7f`.
- The installed gateway and extractor hashes match the final `build/` artifacts;
  the installed extractor reports `reader-v5-page-inspection;pdfium`.
- Existing changes span many unrelated files. Preserve the dirty worktree;
  do not reset it or stage unrelated work. Do not infer that the whole diff is
  part of this task.
- Read [OCR_READER_HANDOFF.md](OCR_READER_HANDOFF.md) for architecture, SDK
  paths and historical verification, and this document for required fixes.
- Keep filename-only answers, model-selected page ranges, direct text reads,
  native-text preservation, evidence reuse and public progress messages.
- No remote publishing or model switching is required.

The “Confirmed behavior” paragraphs below describe the defects observed before
this patch; the implementation and verification records at the end describe the
current behavior.

## 1. Reject invalid OCR results and prevent failed reads poisoning caches

Priority: high, affects correctness and recovery.

Where: `src/samosa_gateway.c`, `doc_read_with_progress()` near lines 4232–4248
and 4347–4349; `attachment_document_json()` near 9869–9893;
`reshape_doc_read_result()`. Find symbols again because line numbers will move.

### Confirmed behavior

An OCR process that exits zero but returns malformed JSON is treated as a
successful empty result: `ok:true`, `text:""`, `needs_review:true`. The inner
reader caches it because its only guard is a substring check for
`text_layer_ocr_unavailable`. On a second read, even when the OCR tool has been
repaired and would return `REPAIRED TEXT`, the cached empty result is returned.
The full-attachment cache's more restrictive guard cannot repair an already
poisoned inner cache. The same inner-cache gap applies to review/incomplete
results beyond this malformed-JSON example.

### Required changes

1. Validate OCR JSON before treating it as evidence: object shape, success
   status, lines array, line text types, finite confidence in [0,1], and any
   bounding boxes used downstream. Reject malformed/error responses even when
   the process exits zero. Treat missing confidence as unknown if compatibility
   requires accepting it; never manufacture confidence 1.
2. Represent failure, incomplete coverage, uncertain recognition and a valid
   empty recognition result separately. Preserve useful native text after an
   OCR failure, but explicitly mark the missing coverage and retryable failure.
3. Introduce a structured cache policy shared by both cache layers. Do not
   search arbitrary source text for a magic substring. Failed, cancelled,
   malformed and retryable partial results must not become successful cache
   entries. If valid low-confidence/empty results are cached, retain their
   uncertainty and provide an explicit refresh/retry path; they must never
   become proof that the source contains no relevant text.
4. Carry the result state through reshaping, source snapshots and all callers.
   Ensure a previously failed saved passage does not suppress an explicit retry.
5. Invalidate old poisoned cache entries using a contract/fingerprint change.
   Do not erase the user's whole cache or conversations. Address both inner
   range/full caches and the outer full-attachment cache.
6. Emit an accurate read-failure/partial status, not “OCR found no readable
   text” when the actual failure was invalid output or an unavailable tool.

### Acceptance tests

- With an isolated cache and fake OCR, return malformed JSON with exit zero,
  then valid `REPAIRED TEXT` on the next invocation. The second request must
  invoke OCR and return the repaired text. Repeat for the selected-range path
  and the full-attachment path; inspect actual invocation counts.
- Cover `ok:false`, missing/wrong-type `lines`, invalid line data, nonzero exit,
  render failure, low confidence, valid empty output, and mixed native+scan
  fallback. Verify distinct result states and their cache behavior.
- A clean successful result should still produce a cache hit on repetition.
- Include source text literally containing `text_layer_ocr_unavailable` to
  prove that document contents do not control caching.

The review reproduction was a temporary C driver including the actual gateway
source, with an isolated `Gateway`, a mutex, fake extractor/OCR executables and
two `doc_read_handler()` calls. First OCR stdout: `malformed OCR result`.
Second OCR stdout: `{"ok":true,"lines":[{"text":"REPAIRED TEXT","conf":0.99,"bbox":[0,0,10,10]}]}`.
Both actual returns remained empty with `ok:true` and `needs_review:true`.

## 2. Preserve uncertainty in evidence sent to the model

Priority: high, affects answer accuracy and honesty.

Where: `document_task_json()` in `src/samosa_gateway.c` near lines 11170–11225;
also inspect the explicit full-read path and `attachment_augment()`.

### Confirmed behavior

The selected-page assembly copies `kind`, `reason`, source label and line text.
It drops `needs_review`, minimum/line confidence, uncertainty counts and other
coverage state. A low-confidence OCR string thus reaches the model as ordinary
text. `read_succeeded` only checks `ok:true` and a string-valued `text`, so an
empty or partial OCR result can also produce a misleading completed-read status.
This was established by inspecting the actual evidence assembly code.

### Required changes

1. Add concise, fixed reader annotations to the exact source evidence for
   uncertain lines, unread regions, OCR failure and valid empty OCR. Preserve
   page citations and native/OCR provenance. Propagate those annotations to
   both the next planning decision and the final answering request.
2. Distinguish “appears blank”, “OCR ran but recognized no text”, and “OCR did
   not succeed”. Absence of recovered text must not imply absence of content.
3. Ensure full reads, selected reads, cache hits and follow-up snapshots retain
   the same important uncertainty/coverage facts. Keep the bounded evidence
   budget; annotate selectively rather than serializing every metric.
4. Report partial progress honestly to the user and let the planner seek more
   evidence or qualify an answer. Do not use planner-generated text as source
   evidence, and keep source content untrusted.

### Acceptance tests

- Drive the compiled gateway with OCR output containing a distinctive answer
  string at confidence 0.30. Inspect captured requests to the fake model and
  verify that both planner and answerer receive an explicit uncertainty marker.
- Repeat through full-read, cache-hit and follow-up flows. Test mixed native
  text plus failed/uncertain OCR and valid empty OCR.
- Assert public statuses do not claim clean completion after incomplete reads.
- Do not settle for testing the raw extractor or OCR response: the relevant
  assertion is what the answering model and user actually receive.

## 3. Fix aggregate image coverage overriding valid text coverage

Priority: medium, directly regresses reading efficiency.

Where: `inspect_page()` in `src/samosa_extract.c`, especially the aggregate
fallback near lines 634–645.

### Confirmed reproduction

Generate two 600x800-point PDFs with identical dense selectable text covering
the page: Helvetica 10, rows every 16 points, repeating
`Selectable native text already covers this image. ` twice per row.

- PDF A: one white background raster filling the page.
- PDF B: two white raster objects, each filling one half of the page.

Both have 4,214 usable characters, text quality 1.0 and image coverage 1.0.
The installed extractor returns `needs_ocr:false` for A, but `needs_ocr:true`,
`kind:mixed`, full-page OCR bounds and reason
`aggregate_image_region_without_text` for B.

The aggregate fallback checks image count/coverage but never checks whether
text already covers that aggregate. It can reverse a correct earlier decision
that every image is adequately covered by native text.

### Required changes

1. Base the aggregate decision on substantial image area lacking usable text,
   not total image coverage alone. Preserve the decision to reuse an adequate
   existing text layer, including already-OCR'd tiled scans.
2. Still catch genuine scans composed of individually small tiles, including
   a digital footer or unrelated native text elsewhere on the page.
3. Choose OCR bounds from the missing regions, and test that the bounds capture
   unique scanned text while keeping useful native evidence.
4. Version the extraction policy so cached old routing decisions are not reused.

### Acceptance tests

- Both covered-background PDFs above bypass OCR, verified at the gateway with
  zero render/OCR calls, not merely `needs_ocr:false` in extractor JSON.
- A tiled scan with no covering text must invoke OCR, even if every tile is
  below the individual image-area threshold.
- A tiled page with a usable covering text layer must reuse it; a footer-only
  layer must not count as covering the scan. Include overlapping tiles to
  verify union coverage rather than double-counted area.

## 4. Make cancellation interrupt active document subprocesses

Priority: medium, affects responsiveness and wasted processing.

Where: `run_capture_mode()` near line 454, `doc_read_with_progress()`, and
`/v1/cancel` near line 22258 in `src/samosa_gateway.c`.

### Confirmed code path

This was the original failure mode: the cancel handler set
`document_cancel_requested`, while capture could remain blocked in a pipe read
and an unbounded `waitpid()`. The current implementation owns a process group
for each document child, polls its pipe, escalates from SIGTERM to SIGKILL
after a bounded grace period, and reaps the child before returning.

### Required changes

1. Track subprocess ownership for the active document request and make its
   wait interruptible. On cancel, terminate and reap that request's extraction,
   rendering or OCR process, escalating after a bounded grace period if needed.
   If using process groups, establish and track the child's group explicitly.
2. Do not kill all `job_pids`, unrelated background jobs, or the resident model
   as a shortcut. The existing capture helper has many non-document consumers.
3. Check cancellation before launches, after child completion, before fallback
   or the next page, and before cache/snapshot publication. Treat cancellation
   as cancellation, not an empty successful OCR result.
4. Use request-owned temporary paths and cleanup only that request's files.
   Ensure cancellation works during output silence and after client disconnect
   as well as an explicit Stop request.

### Acceptance tests

- Use fake extraction/render/OCR children that write a ready marker then block
  for 30 seconds. Cancel after the marker. Require the request to terminate
  within a short deterministic deadline (e.g. 2 seconds), verify the child is
  gone/reaped, and prove no later page, OCR fallback or model call started.
- Test children that emit no stdout and ones that ignore SIGTERM, plus cancel
  during extraction, rendering and OCR. Verify no cache/snapshot publication.
- Run an unrelated dummy job alongside the cancelled request and verify it
  survives. Verify the next document request succeeds normally.

## Tests and build pitfalls

The existing `tests/test_pdf_ocr_routing.py` passed during review. It creates
only four simple PDF types and calls extraction/rendering/OCR directly. It does
not exercise gateway caching, mixed-evidence assembly, SSE or cancellation.
Extend coverage with a compiled-gateway integration suite and controlled
sidecars; keep optional actual OCR accuracy tests separate from deterministic
failure injection. Tests must fail against the reviewed code and pass after
the corresponding correction.

Also correct `make test-pdf-ocr-routing`: the reviewed target depends on
`samosa-extract`, which defaults to a no-PDFium build when `PDFIUM_DIR` is
absent. `make -n` confirmed it would overwrite `build/samosa-extract` with that
build, then the test would report SKIP. Ensure the intended PDF test cannot
silently replace a working PDFium artifact or appear to pass without PDF tests.
Respect `BUILD_DIR` and explicit extractor paths. An optional environment skip
must be distinct from the required release verification gate.

Useful existing checks, after building the appropriate artifacts:

```sh
make samosa-extract PDFIUM_DIR=/Users/deepanwadhwa/Documents/samosa-pdfium-artifacts/chromium-7961/mac-arm64-unpacked
make samosa-gateway samosa-jobsd test_fake_openai_backend
.venv/bin/python tests/test_pdf_ocr_routing.py
python3 tests/test_document_harness.py
SAMOSA_REAL_DOCUMENT_EXTRACT="$PWD/build/samosa-extract" python3 tests/test_document_harness.py
SAMOSA_EXTRACT="$PWD/build/samosa-extract" sh tests/test_samosa_extract.sh
sh tests/test_doc_read_pdf_paging.sh
sh tests/test_attachments.sh
sh tests/test_document_context_prefix.sh
node tests/test_composer_perf.mjs
node tests/test_composer_ui.mjs
git diff --check
```

Do not run rebuilding test scripts concurrently against shared binaries or
fixed ports. Use isolated temporary homes/caches/ports for failure injection.
The review's temporary reproduction files are in
`/tmp/samosa-ocr-review.a4bOJf` (`probe.c`, fake sidecars and
`geometry_probe.py`). They may disappear; the reproduction inputs above are
the durable specification. Port useful reproductions into repository tests.

## Implementation record

- OCR responses now require a successful object, a lines array, text strings,
  finite confidence values in range, and valid bounding boxes. Invalid,
  failed, cancelled, or incomplete reads are retryable and cannot populate the
  bounded-range or full-attachment caches. Both cache contracts were bumped to
  invalidate older poisoned entries.
- Reader warnings (OCR failure, empty recognition, uncertain confidence and
  incomplete inspection) are retained in reshaped results and the exact page
  evidence sent to the planner and answer model. Native text fallback remains
  available but is explicitly labelled as incomplete.
- PDF image routing now measures native coverage before aggregate tiled-image
  escalation. Dense native text behind multiple raster tiles bypasses OCR;
  sparse/footer-only native text and genuine tiled scans still route to OCR.
- Document extraction, rendering and OCR children run in an owned process group.
  `/v1/cancel` terminates only that group, capture waits are interruptible, and
  cancellation is checked before publication or fallback.
- `make test-pdf-ocr-routing` now requires `PDFIUM_DIR` (or an explicit
  `SAMOSA_EXTRACT`) and cannot silently rebuild a portable extractor and report
  a skip.

Additional regressions closed in the final pass:

- OCR framing is checked with `ocr_json_text_complete()` before the generic
  recursive JSON parser runs. A deeply nested adversarial OCR response is
  rejected without a parser stack overflow; the gateway then returns a
  retryable/native-fallback result.
- `inspection.incomplete:true` survives page reshaping and evidence assembly,
  sets `needs_review:true`, adds a fixed warning, and sets top-level
  `retryable:true`. The planner can explicitly send `refresh:true`; that flag
  reaches the reader, bypasses the range cache, and is shown to the user as a
  “Retrying PDF pages …” activity.
- Cancellation kills the owned process group even when the group leader has
  already exited. The contract test forks a SIGTERM-ignoring grandchild and
  verifies that no descendant remains alive.
- The PDF routing target propagates nested `make samosa-extract` failures and
  exits nonzero when PDFium is absent. The local installer rebuilds the current
  gateway and jobs daemon before staging a release, preventing stale OCR code
  from being installed.
- The real-extractor harness mode now reports exactly what it covers: it runs
  PDFium-backed extraction and orchestration checks, while deterministic
  malformed-OCR/incomplete/cancellation fixtures remain in the spy-backed
  mode. It does not claim neural OCR fault injection when the real extractor
  is selected.
- Valid empty OCR now sets page and aggregate `needs_review:true` and emits the
  fixed “OCR recognized no readable text” warning. Cancellation observed at
  the final OCR progress boundary returns `document_cancelled` before cache
  publication; a cancellation racing a cache write removes that entry.
- The descendant-cancellation contract now requires proof that the grandchild
  actually started. Replacing the helper with a process that creates no PID
  marker fails the test instead of producing a false pass.

## Verification record

The build at that earlier checkpoint passed:

```text
make samosa-gateway test_fake_openai_backend
make test-document-reader-contract
make test-document-harness
SAMOSA_REAL_DOCUMENT_EXTRACT=$PWD/build/samosa-extract python3 tests/test_document_harness.py
make test-pdf-ocr-routing PDFIUM_DIR=/Users/deepanwadhwa/Documents/samosa-pdfium-artifacts/chromium-7961/mac-arm64-unpacked
SAMOSA_EXTRACT=$PWD/build/samosa-extract sh tests/test_doc_read_pdf_paging.sh
SAMOSA_EXTRACT=$PWD/build/samosa-extract SAMOSA_OCR=$PWD/build/samosa-ocr sh tests/test_attachments.sh
git diff --check
```

The negative routing-gate check was also run in an isolated build directory;
with `PDFIUM_DIR=/definitely-missing-pdfium` it exited `2` at the nested
extractor build instead of running a portable extractor and reporting a skip.

`test_document_reader_contract` directly checks NUL termination, strict OCR
JSON rejection, the deep-input boundary, empty-OCR warnings, cancellation
before cache publication, and cancellation of a proven silent SIGTERM-ignoring
process group with a descendant. `make test-document-harness`
includes low-confidence and incomplete OCR pages, verifies warning markers in
both planner and answer-model requests, exercises explicit refresh, and tests
malformed zero-exit OCR followed by valid `REPAIRED TEXT 77` without cache
poisoning. It also verifies active extraction cancellation; the contract test
separately verifies that no descendant remains. The real-extractor harness passed the PDFium-backed
metadata, bounded-page, adaptive-jump, cache, follow-up, compaction, full-read,
and text-range checks. The PDF routing test covers scanned pages, mixed
native/scan pages, tiled scans, and tiled raster backgrounds already covered by
dense native text.

At that earlier checkpoint, the verified local release was `dev-23918e2d0d7f`; `build/samosa-gateway` and
`~/.samosa/current/bin/samosa-gateway` have identical SHA-256 hashes, as do the
extractor and jobs-daemon artifacts. The running local gateway was restarted
after installation; `/healthz` reported `ready:true`, `supports_documents:true`,
PDFium enabled, and OCR `pack_ready:true`. No remote publishing or model
switching was done.
