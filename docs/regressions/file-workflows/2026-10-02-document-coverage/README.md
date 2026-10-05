# Complete PDF reading and OCR repair — 2026-10-02

The reported user PDF and saved conversation were not opened. All document
tests below use independent generated fixtures.

## Reproduction and changes

`tools/gen_mixed_document_fixture.py` creates a deterministic 40-page harbor
report: 13 native-text pages and 27 scanned/mixed pages. Every page has a unique
approval count. Page 40 is scanned. The original gateway, with an invalid
planner response, supplied only page 1 to the answering model; the test reported
missing facts on pages 2–40.

- Explicit complete-reading/OCR requests now read the whole document without
  asking a planner to choose a starting page. Fast mode retains complete text
  coverage. Explicit single-page questions retain their narrow scope.
- PDFium checks each page; Tesseract reads scanned regions even when the same
  page also has native text. Full evidence now retains absolute page labels.
- Complete reading is not converted into a few ranked snippets after extraction.
  If the selected model cannot hold the text, consecutive sections are reviewed
  and labelled reading notes are used for synthesis. Every section must finish;
  failed/incomplete reviews do not produce a complete-document answer. Notes
  are explicitly distinguished from verbatim source text.
- Detailed text reading no longer automatically selects Molmo2. The UI's detail
  button preserves the original request, requests OCR for scans, and asks for
  reading gaps without adding a visual-model requirement.
- Reader children have a 30-second default deadline, including children that
  close stdout and hang. PDF extraction has a three-minute budget checked between
  pages. Focused planning has a one-minute total budget. Section review has a
  six-minute total budget and a 150-second per-call deadline. Cancellation is
  retained. Failures remain explicit and retryable where appropriate.
- Progress includes the current page and total page count. The model receives
  coverage and OCR uncertainty information. Updated cache fingerprints prevent
  old conversation evidence from bypassing the new coverage contract.

## Checks

- `tests/test_document_mixed_pdf.py`: real PDFium and Tesseract through HTTP chat,
  with a deliberately unhelpful fake planner. All 40 facts reach the model;
  exactly 27 pages invoke OCR. Cold extraction about 30 seconds. Reopening in
  detailed mode reuses extraction, without a second OCR pass.
- The same test at an 8,192-token model context exercises consecutive section
  review. All 40 facts reach the reviewing model. The final model receives
  clearly labelled notes. Failed section review returns an error without final
  synthesis. Fake model timing does not measure real answer speed.
- `tests/test_document_reader_contract.c`: whole-document routing in auto, fast
  and detailed modes; focused-page routing; subprocess deadlines with open and
  closed stdout; missing pages rejected before caching; existing malformed OCR,
  cancellation, cache and cleanup checks. Helper invocations reject unknown
  arguments instead of recursively entering the test suite.
- Existing document harness, attachment integration, conversation prefix reuse,
  real OCR runtime, PDF OCR routing, and UI fixtures pass.
- `tests/test_document_detail_ui.mjs` executes the actual detail-button handler
  for uploads and selected files, verifying its OCR request and original prompt.

## Real-model acceptance

`tests/test_document_mixed_live.py` creates its own PDF and conversation. It asks
Ornith for a summary and the known counts on pages 1, 20, 37, and 40. It reads only
the conversation it created. Results are recorded alongside this file.

An initial live attempt completed OCR in 28 seconds, then hit the old one-minute
section-review deadline. A separate synthetic model probe measured 46 seconds
of prefill for 3,196 tokens and 13 seconds for 66 output tokens. The bounded
review request was shortened and its deadline adjusted using those measurements.
A subsequent probe identified a second failure: the model enumerated every
station and exhausted 320 output tokens before closing its JSON. The section
instruction now requests one concise paragraph focused on the user's question.
The corrected probe completed with `finish_reason=stop`, retaining the requested
page-20 fact; its cold prefill plus generation took 112 seconds. The per-call
deadline allows headroom while preserving the six-minute total review budget.

The complete Ornith run passed. All three sections finished; the answer returned
7101 (page 1), 7120 (page 20), 7137 (page 37), and 7140 (page 40), matching the
fixture manifest. Coverage records identify all 40 processed pages. The cached
extraction run took 397.21 seconds: 306.95 seconds for section review and about
90 seconds for synthesis. Independent cold extraction took about 30 seconds.
This is correctness evidence, not a satisfactory response-time claim. The
answer's unnecessary description of internal sections prompted a short synthesis
instruction distinguishing actual reading gaps from internal processing.
A separate real-Ornith synthesis check with that instruction completed in
68.46 seconds, retained all four correct counts, and omitted section numbering.
The model still added unnecessary commentary about the condensed notes; wording
and response time remain cleanup work. See
[final synthesis check](final-gap-instruction-live.json).

See [real-model log](mixed-40-ornith-live.log), [truncated response](model-review-truncated.json),
and [corrected section response](model-review-concise.json). The development
build is installed; the running release is recorded in `installed-runtime.json`.

## Limits

This repairs the demonstrated page-1 and unnecessary visual-model paths; it does
not establish perfect OCR on arbitrary handwriting, damaged scans or languages
whose trained data is not installed. Condensed reading notes are lossy and must
not be quoted as exact source text. The original source and extraction cache
remain available for focused page questions. Larger documents can exceed the
bounded review budget and then fail explicitly. Browser visual acceptance and
FW-6 remain open; scheduling remains gated.
