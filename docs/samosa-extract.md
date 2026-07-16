# `samosa-extract` protocol

`samosa-extract` is the document-sidecar boundary: it links PDFium while the
resident `qwen36b` server remains dependency-free. It takes one PDF and writes
exactly one JSON object to standard output.

```sh
samosa-extract --json file.pdf
```

A successful response has this shape:

```json
{
  "ok": true,
  "text_layer": true,
  "pages": [{"index": 1, "text_chars": 42, "has_raster_figure": false, "text": "..."}],
  "text": "...",
  "tokens_estimate": 8
}
```

`tokens_estimate` is intentionally a whitespace-token estimate. The future
ingestion caller must calculate the exact model-token count with its loaded
tokenizer before deciding whether input fits context; it must not treat this
estimate as a budget authority.

For the scanned-page/vision seam, the sidecar can render one page to a bounded
PPM image that the existing `stb_image` decoder accepts:

```sh
samosa-extract --render-ppm file.pdf 3 /secure/job-temp/page-3.ppm
```

The page number is one-based. The output must not already exist; it is created
mode `0600` and capped to a 768-pixel long edge (at most 768² pixels). Successful
rendering reports a small JSON acknowledgement on standard output. The caller
owns the temporary directory and must delete the rendered image after inference.

Failures are also JSON, for example `{"ok":false,"error":"pdf_encrypted"}`.
The stable failure classes include unavailable/invalid input, encrypted or
malformed PDFs, page/text/output limits, and the wall timeout.

## Safety boundary

The sidecar accepts only regular, non-symlink files up to 20 MiB (override down,
never up, with `SAMOSA_EXTRACT_MAX_BYTES`). It keeps that opened descriptor
behind PDFium's custom-file API, avoiding a path replacement race. It requests
a 512 MiB `RLIMIT_AS` limit (and `RLIMIT_DATA` fallback): Linux enforces the
address-space limit, while this macOS kernel rejects finite values for both.
It also sets a 15-second CPU limit and a 20-second last-resort alarm. Its
process contains no networking code. The parent watchdog is therefore the
required memory backstop on macOS until a portable OS sandbox adapter lands.

The extractor fixture is also run under macOS's `sandbox-exec` with
`(deny network*)` when that tool is present. That verifies that the supported
extraction path needs no network grant; production spawning must still apply
the equivalent OS policy rather than relying on a development test.

The spawning HTTP/document controller must still set and enforce its own
wall-clock timeout, kill the process group on expiry, and preserve this JSON
boundary. Resource limits protect the model process only because extraction is
kept out of it; they are not a substitute for a parent watchdog or an OS
sandbox policy.

## Development build

PDFium is not part of `make` or `make test`. Build the optional sidecar with an
unpacked, SHA-verified PDFium artifact:

```sh
PDFIUM_DIR=/path/to/pdfium make extract-test
```

The release installer will fetch the platform-specific, manifest-pinned
artifact. That packaging work is deliberately separate from this development
build so the normal engine binary gains no PDFium dependency.
