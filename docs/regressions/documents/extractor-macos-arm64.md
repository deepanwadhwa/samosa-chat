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
malformed input, symlink rejection, and the configurable downward-only input
size limit.

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

## Limit result

This macOS kernel returned `EINVAL` when setting finite `RLIMIT_AS` and
`RLIMIT_DATA`. The sidecar does enforce its 15-second CPU limit and 20-second
last-resort alarm. A controller-side memory/wall watchdog is required on macOS;
Linux's `RLIMIT_AS` remains the intended hard address-space limit. This is an
open platform-sandbox task, not a claim that macOS memory capping was verified.
