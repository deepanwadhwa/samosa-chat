# Samosa document reader contract

The document reader uses PDFium for PDF extraction/rendering and native
Tesseract for OCR. The custom detector, recognizer, handwriting classifier,
weight format, exporters, NumPy reference and tensor tests have been removed.
See [OCR_TESSERACT.md](OCR_TESSERACT.md) for build/install details and the
measured limits of the current implementation.

## Reading documents

`doc.read` supplies document evidence to Chat and Jobs. Native PDF text and
OCR evidence must retain their separate `STRUCTURED TEXT:` and `OCR:` labels.
Embedded images must also be read when a native text layer exists. A scanned
PDF has an empty structured-text field and text recovered through Tesseract.
The required processing is recorded in [PDF_READING_PLAN.md](PDF_READING_PLAN.md).

PDF extraction uses bounded page windows. A window is a transport limit, not
a limit on how many pages an entire document may contain. The direct
`samosa ocr PDFPATH` command renders and recognizes every page sequentially,
printing each completed page immediately and reusing the Tesseract instance.
That CLI command has no OCR timeout. Gateway cancellation and watchdogs are
separate controls.

## Native sidecar

`src/samosa_ocr.c` adapts Tesseract's C API to Samosa's process/JSON interface.
It implements `read IMAGE`, `detect IMAGE`, `recognize IMAGE --box ...`, and
the native `pdf PDFPATH EXTRACTOR` command. It contains no inference engine.
The JSON result contains text, page dimensions, line boxes, normalized
confidence, and reader provenance (`tesseract`). Optional crop export supports
existing downstream review workflows. Script classification is `uncertain`;
there is no custom handwriting classifier or secondary recognizer.

The default language is English. Language data is configured through
`SAMOSA_TESSDATA` and `SAMOSA_OCR_LANG`. `--check` initializes that data; the
gateway uses this check for readiness. The existing `/healthz.ocr.pack_ready`
field is retained for API compatibility and now reports language readiness.

## Cache and Jobs

Read results use the central content-addressed cache, not companion files in
user folders. Cache identity includes the sidecar version; ranged results do
not populate the full-document cache. See [src/read_cache.h](../src/read_cache.h).

Failures must remain failures. Missing OCR data, process failure, timeout or
cancellation must never become a claim that requested text is absent. Jobs
can park uncertain work in `review_required`. Existing gateway review and
vision escalation remain separate from the OCR library. Tesseract confidence
must not be advertised as a calibrated guarantee of transcription accuracy.

## Verification

- `make ocr-test`: native Tesseract interface and missing-language failure.
- `make test-pdf-ocr-runtime`: scanned/mixed PDFs, dense text, multiple pages,
  failure handling, immediate page output, and a runtime with Python disabled.
- `make read-cache-test`: central cache integrity and concurrency.
- `make doc-read-test`, `make motto-test`, `make tier2-test`: Jobs/cache and
  review orchestration with deterministic reader fixtures.
- `tests/test_tesseract_installer.py`: bundled native OCR and clean installation.
