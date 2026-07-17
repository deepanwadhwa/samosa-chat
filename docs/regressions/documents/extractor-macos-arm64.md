# PDFium extractor — macOS arm64 development evidence

Date: 2026-07-16

## Environment

```text
Darwin MacBook-Air.local 25.5.0 Darwin Kernel Version 25.5.0: Mon Apr 27 20:41:19 PDT 2026; root:xnu-12377.121.6~2/RELEASE_ARM64_T8122 arm64
```

Development-only PDFium artifact: `pdfium-mac-arm64.tgz`, Chromium `7947`
(`PDFium 152.0.7947.0`), SHA-256:

```text
aa9739354fc7bc8f200f3f3c9532bd5233298203051e094820272ccd9c997a77
```

It is intentionally not committed or shipped by this increment. Release
packaging must download a platform artifact through the existing verified
manifest before claiming support.

## Commands and results

```sh
PDFIUM_DIR=/tmp/pdfium-mac-arm64.45AEFQ/unpacked make -B extract-test
```

```text
samosa-extract: PASS
```

The test covers successful text extraction, non-regular input rejection,
malformed input, symlink rejection, the configurable downward-only input-size
limit, successful execution under `sandbox-exec` with `(deny network*)`, and
PPM rendering with no overwrite of an existing output file.

Native UTF-8 text extraction was added with a committed metadata fixture. It
normalizes CRLF/CR to LF (covered by a direct assertion); binary non-PDF input,
ZIP/DOCX input, and HTML all return specific errors rather than falling through
as plain text.

With the local verified `tokenizer_qwen36.json`, exact token output was also
verified: the metadata text fixture produced 18 tokens and the PDF fixture
produced 3. `tokens_estimate` remains only an explicitly non-authoritative
fallback when no tokenizer is supplied.

Manual render check:

```sh
./samosa-extract --render-ppm v109i02.pdf 1 /tmp/page-1.ppm
file /tmp/page-1.ppm
```

```text
{"ok":true,"page":1,"format":"image/x-portable-pixmap"}
Netpbm image data, size = 543 x 768, rawbits, pixmap
```

The output was mode `0600`, 1.2 MiB. It is within the existing vision decoder's
4 MiB HTTP-body cap and its PNM/PPM decoder support.

The four user-provided JSS PDFs were evaluated locally but are not committed:

```text
file          ok    pages  text layer  estimated tokens  pages with image objects
v109i02.pdf   true  37     true        14271             2
v109i03.pdf   true  30     true        11281             1
v110i01.pdf   true  24     true        8939              5
v110i02.pdf   true  26     true        9448              2
```

```sh
make test
```

Passed on this machine. The optional PDFium target is intentionally outside
`make test`, so a normal source checkout remains dependency-free.

`otool -L samosa-extract` shows `@rpath/libpdfium.dylib`; it is linked only by
the short-lived sidecar. The resident engine was not rebuilt or linked against
PDFium.

`tests/test_package_pdfium.py` verifies that release packaging refuses a partial
platform set and records every supported artifact in `release-manifest.tsv`.

```sh
PDFIUM_MAC_ARM64_ARCHIVE=/tmp/pdfium-mac-arm64.45AEFQ/pdfium-mac-arm64.tgz \
  make document-installer-test
```

```text
document installer: PASS
```

This created a minimal checksum-verified release, ran the real installer,
extracted the committed PDF fixture with the installed sidecar through its
relative rpath, and confirmed `qwen36b` had no `libpdfium` load command.

## Limit result

This macOS kernel returned `EINVAL` when setting finite `RLIMIT_AS` and
`RLIMIT_DATA`. The sidecar does enforce its 15-second CPU limit and 20-second
last-resort alarm. A controller-side memory/wall watchdog is required on macOS;
Linux's `RLIMIT_AS` remains the intended hard address-space limit. This is an
open platform-sandbox task, not a claim that macOS memory capping was verified.

## Deferred format scope

DOCX is deliberately deferred until the portable vendored-miniz + XML-strip
implementation can be added and tested. HTML is deferred to #4's shared
extractor, and RTF remains explicitly unsupported. The implemented E-D1 scope
for this branch is therefore PDF plus native UTF-8 text—not a claim that every
document format has landed.
