# Native Tesseract OCR

The default `samosa-ocr` executable links directly to Tesseract's C API and
Leptonica. There is no Python process, Python wrapper, or custom neural-network
inference in the OCR runtime. The previous custom OCR implementation, weight
exporters, NumPy reference, and tensor tests have been removed. Only the native
Tesseract adapter remains.

`samosa ocr PDFPATH` calls the native PDF command. It initializes Tesseract once,
uses PDFium to render each page, recognizes that page, prints and flushes its
text, and then continues. It has no OCR timeout. Temporary page images are
removed between pages and on normal exit or interruption. The chat reader uses
the same Tesseract adapter through `read IMAGE`, with line boxes and confidence
values in the existing JSON interface. Chat's caller-side watchdog and planning
sequence are separate from this change.

English is the default. Set `SAMOSA_OCR_LANG`, such as `eng+deu`, and provide
the matching traineddata files using `SAMOSA_TESSDATA`. `TESSDATA_PREFIX` is also
honored. Installed releases include English data under `share/tessdata`.
`samosa-ocr --check` actually initializes the configured language data; Settings
uses that result to report actual OCR readiness.

## Build and installation

- Source-build dependencies on macOS: `brew install tesseract pkgconf`.
- Debian build dependencies: `libtesseract-dev libleptonica-dev tesseract-ocr-eng pkg-config`.
- Build with `make samosa-ocr`; install normally with `make install`.
- macOS release packaging includes the compiled OCR executable, native libraries,
  and English language data in the verified manifest. New installs download and
  validate those files; recipients need neither Homebrew nor Tesseract development
  packages. Packaging and local source builds use `tools/stage_tesseract_runtime.sh`
  to copy dependencies, rewrite library paths and sign the binaries.
- Linux source installs require the distribution libraries listed above.
- The OCR version was bumped so a restarted gateway uses a new reader cache
  fingerprint. An already-running gateway must restart to refresh that fingerprint.

## Measurements and verification (2026-09-13)

On this 8-core Apple Silicon machine, using the same retained 1414×2000 page
raster, Tesseract 5.5.3 with English data took 2.57 seconds through its own CLI.
The native Samosa adapter took 2.49 seconds inside the image-read operation,
2.93 seconds including process startup. These are individual observations,
not a one-second-per-page guarantee or a benchmark of an entire document.

The earlier custom implementation took 15.85 seconds while returning only
138 characters and 30 empty lines out of 45 detected boxes. Its fixed-width
preprocessing damaged long lines. Counting boxes was not an accuracy check.
Tesseract returned about 3,900 characters from this page. That character count
alone is also not proof of perfect transcription: the English model misreads
the accented name in the tiny legacy fixture.

`make ocr-test` checks the native interface and missing language data.
`make test-pdf-ocr-runtime` exercises independent scanned and mixed PDFs,
dense text with >97% normalized text similarity, two-page output, blank images,
invalid symlinks, line boxes, crop export, and failure propagation. It blocks the
second page's renderer until the first page's text has been observed, and runs
the PDF command with Python executables deliberately disabled.

`make test-tesseract-installer` packages the real native OCR runtime, installs
into an empty Samosa home with host OCR tools and Python disabled, and reads
a two-page scanned PDF. It also verifies that missing language data prevents
activation of an incomplete release. Packaging fixture tests and the atomic
installer gate cover release manifests and upgrade failure behavior.

Tesseract's official [API examples](https://tesseract-ocr.github.io/tessdoc/APIExample.html)
describe direct native integration and text, confidence, and bounding-box access.

## Release validation (2026-09-14)

The following checks passed locally on macOS ARM64:

- Portable and OpenMP builds, and the complete `make test` suite.
- Document harness, audio attachments, Molmo2 gateway and processor tests.
- Composer UI/performance and session-token UI tests.
- `make ocr-test test-pdf-ocr-runtime`, including dense-text accuracy and
  observable page-by-page output with Python disabled.
- `make test-tesseract-installer` with the pinned PDFium SDK, real bundled
  Tesseract libraries and English data, and host OCR/Python tools disabled.

The pre-merge gates also require `make ci-debian`, `make ci-ubuntu-full`, and
the PR's macOS/Linux CI checks to pass. Their final results are recorded in the
PR. Ubuntu's Docker gate includes the real OCR/PDF tests; macOS CI additionally
checks the bundled clean install. These are independent fixtures and do not
measure a complete live-model chat's latency or answer quality.
