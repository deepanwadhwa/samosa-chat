# `samosa-extract` protocol

`samosa-extract` is the document-sidecar boundary: it links PDFium while the
resident `qwen36b` server remains dependency-free. It takes one PDF and writes
exactly one JSON object to standard output.

```sh
samosa-extract --json file.pdf
```

UTF-8 plain-text input is also handled natively. Its line endings are normalized
to LF and it receives the same one-page JSON shape. The extractor sniffs bytes,
not extensions: invalid UTF-8/binary input is rejected rather than silently
treated as text. DOCX, HTML, and RTF receive explicit unavailable/unsupported
errors until their portable extractors land; no macOS host tool is used.

## Current format scope

| Input | Status | Behavior |
| --- | --- | --- |
| PDF | Implemented | PDFium text, per-page metadata, optional exact token counts, bounded page rendering |
| UTF-8 text / Markdown / source | Implemented | Native extraction with line-ending normalization |
| DOCX | Deferred | Requires the planned vendored miniz ZIP reader plus XML text strip; returns `docx_extractor_unavailable` today |
| HTML | Deferred to #4 | Will use the shared portable web extractor; returns `html_extractor_unavailable` today |
| RTF | Unsupported | Returns `rtf_unsupported` |

This is the explicit #5 boundary for now. It does not claim full document-format
support before those remaining portable extractors are implemented and tested.

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

The controller can request exact counts from its trusted, release-provided Qwen
tokenizer:

```sh
samosa-extract --json file.pdf --tokenizer tokenizer_qwen36.json
```

That adds exact `tokens` fields for the whole document and each page. The
tokenizer path is not a user document; callers must pass only their installed,
verified model tokenizer.

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
artifact whenever the release manifest includes it. `tools/package_hf.py`
accepts `--pdfium-dir`; that directory must contain the reviewed macOS-arm64,
Linux-x64, and Linux-arm64 archives before it will package any of them. The
installer verifies the archive through the release manifest, unpacks it inside
the inactive release, compiles `samosa-extract`, and stores its shared library
under that release's `lib/` directory. The normal engine binary gains no PDFium
dependency. An older release manifest without the artifact simply has no PDF
capability; it never substitutes a host PDF tool.

For the real installer path on macOS arm64, run:

```sh
PDFIUM_MAC_ARM64_ARCHIVE=/path/to/pdfium-mac-arm64.tgz make document-installer-test
```

It builds a tiny verified release fixture, runs the actual installer, checks the
relative library path by extracting a PDF from the installed release, and checks
that `qwen36b` did not gain a PDFium dependency.
