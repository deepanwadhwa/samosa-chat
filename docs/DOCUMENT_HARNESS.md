# Model-directed document reading

The latest WeatherNext large-PDF correction and installed-build verification
are recorded in [OCR_REVIEW_FIXES.md](OCR_REVIEW_FIXES.md). See
[OCR_READER_HANDOFF.md](OCR_READER_HANDOFF.md) for the implementation details,
verification results and future hardening limits.

The four review corrections are implemented: strict OCR validation/cache
recovery, uncertainty propagation, text-aware aggregate image routing and
active document cancellation. See [OCR_REVIEW_FIXES.md](OCR_REVIEW_FIXES.md)
for the implementation and verification record.

Document chat, including the browser's default `analysis_depth=fast`, asks the
selected chat model what evidence the question needs before invoking a reader.
Each source starts with its filename, media type, byte size, and attachment ID.
The planner also receives a bounded recent conversation window for follow-ups.

The model can finish without reading, select PDF pages, select a UTF-8 text byte
range, search extracted non-PDF text, or explicitly request a full document read.
After a bounded read, it receives the actual result and chooses again. This is
not an author-name keyword heuristic: metadata answers, irrelevant sources,
opening-page checks, and jumps to later sections use the same action contract.
Planner-generated prose is never substituted for extracted evidence.

PDF ranges constrain extraction and OCR themselves. The reader preserves
absolute page numbers and total page count. Short digital title pages use their
existing text layer. Full and ranged cache entries have separate identities;
different ranges can coexist. Plain text ranges use bounded file I/O. HTML and
DOCX need container parsing first, but only selected text enters the prompt.
Image/video specialist selection and audio transcription retain their existing
modality routing; this loop governs document evidence.

Control limits are eight decisions, twelve PDF pages per model-selected window,
8,000 bytes per text read, and 24,000 evidence bytes per source. A window is
executed in extractor batches of at most five pages without intervening model
calls. Planning output is limited to 112 tokens, with a short public purpose.
Repeated identical actions do not execute again; two repeated decisions without
new evidence stop the loop. Already-read overlap at range boundaries is trimmed.
Invalid plans permit one bounded opening read, never an
implicit full extraction. Exhausted budgets and partial reads are labelled as
incomplete evidence. Hard reader failures preserve their error code, stop the
read loop, and surface as errors instead of unsupported model answers.
Cancellation stops planning without initiating fallback
reading. A full-read action remains available for tasks requiring broad coverage;
the existing full-document/retrieval context budgets still apply afterward.

Metadata-only and selected-range results do not become complete-document cache
entries. Follow-ups receive the same source's previously inspected passages
before planning, so they can answer from them or request different evidence.
Compaction preserves the
saved evidence snapshot without starting new document reading. Final answers
must distinguish filename inferences from verified content and cite only the
supplied ranges.

Verification: `make test-document-harness` drives the compiled gateway with a
deterministic model and an extractor spy. To test real PDFium extraction on the
generated 100-page fixture, run:

```sh
SAMOSA_REAL_DOCUMENT_EXTRACT="$PWD/build/samosa-extract" python3 tests/test_document_harness.py
```

The tests inspect model requests and actual extractor invocations for zero-read
answers, title pages, adaptive page jumps, cache isolation, follow-ups,
compaction, invalid/repeated plans, full reads, text ranges, multiple files, and
cancellation. They validate orchestration; real-model decision quality requires
separate acceptance checks.

Each accepted read publishes its actual page range and short task purpose over
the existing `file_activity` SSE channel before extraction. Completed reads,
read failures, evidence reuse, and answer preparation have distinct statuses.
Unknown-duration stages are indeterminate. The UI shows the action above the
filename and avoids duplicating it in a second waiting indicator. Status text
uses text nodes, so a filename or purpose cannot introduce HTML.

## Local model acceptance, 2026-09-07

`python3 tests/test_document_harness_live.py` passed against the installed
`dev-31ceed08af97` build with Ornith 9B. Both fixtures contain 100 pages:

- `The Quiet Orchard - Jane Doe.pdf`: no content read; the answer identified
  Jane Doe and explicitly attributed the inference to the filename.
- `book.pdf`: only pages 1-2 entered evidence; the answer identified Jane Doe
  from the first page and cited `book.pdf (Page 1)`.

The check inspects each conversation's saved evidence as well as the final
answer. The initial live run exposed unnecessary title-page confirmation for
a clear filename; a later run exposed incorrect filename attribution for a
page-derived answer. The final policy and source-attribution instructions
correct both behaviors. This acceptance covers Ornith; other installed models
were not switched in or tested during this change.

## Decision progress and batching acceptance

The decision-progress update was exercised with Ornith 9B on the user's
506-page Indica PDF in an isolated conversation. The successful run used a
warm extractor cache:

- At 0.08 seconds: checking the filename and choosing what to read.
- At 14.76 seconds: `Reading PDF pages 1–8 to find the first chapter name…`
- At 14.87 seconds: read complete, checking whether the pages answer the question.
- At 26.65 seconds: evidence ready; answer preparation begins.
- At 49.57 seconds: correct answer, “Why on Earth,” citing PDF page 7.
- A repeated question reused the saved page evidence, performed no new reading,
  and completed in 41.53 seconds. Model inference remains the dominant cost.

The new integration checks assert the purpose/range events, their ordering,
bounded extractor batches without intervening model calls, trimmed overlap,
and reuse. The UI fixture checks status/filename separation, text-safe labels,
indeterminate visibility, and clearing activity when the answer begins. No
browser connection was available for visual inspection in this session.

The PDF reader now adds page-level inspection before OCR: selectable-text
quality, suspicious Unicode, image count/coverage, blank-page preview, mixed
image/text regions, and explicit OCR routing. `tests/test_pdf_ocr_routing.py`
verifies digital, blank, scanned and mixed fixtures, crop rendering and actual
OCR recovery of a generated sentinel when native Tesseract and English language data are available.

The original author acceptance also passed again on the installed
`dev-8f9d6a17a61b` update: the named file required zero content reads (24.79
seconds), while `book.pdf` used only the opening pages and correctly attributed
the author to page 1 (34.04 seconds).

A historical live acceptance against `dev-77260c01b8e8` used the 506-page
Indica PDF. It reported inspection decisions live, OCR'd pages 1–2, read pages
3–8 directly, and returned “Why on Earth” from page 7 in 69.99 seconds cold. A
repeat turn reused the evidence without new reading/OCR in 42.73 seconds. Those
timings describe that historical release; the later `dev-23918e2d0d7f`
release was verified with deterministic and real-extractor harnesses and has
not been assigned those live-model timings.

The current `dev-f740fa6ddcb0` release was separately accepted on the user's
22 MB WeatherNext paper: all 25 authors from page 1, no OCR, followed by evidence
reuse. It took 90.15 s end to end (about 0.16 s for reading), and 90.63 s for the
repeat answer. Details and the exact reproducible command are in
[OCR_REVIEW_FIXES.md](OCR_REVIEW_FIXES.md).

An opt-in reproducible live check is available as
`tests/test_document_decisions_live.py --attachment ID --question QUESTION --expect ANSWER`.
