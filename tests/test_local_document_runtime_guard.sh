#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-document-runtime-guard.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
guard="$ROOT/tools/validate_local_document_runtime.sh"

extractor() {
  printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$2" >"$1"
  chmod +x "$1"
}

portable="$TMP/portable"
pdfium="$TMP/pdfium"
library="$TMP/libpdfium.dylib"
extractor "$portable" 'samosa-extract 0 (reader-test;no-pdfium)'
extractor "$pdfium" 'samosa-extract 0 (reader-test;pdfium)'
printf 'fixture\n' >"$library"

"$guard" "$portable" ""
"$guard" "$pdfium" "$library"
if "$guard" "$portable" "$library" >"$TMP/out" 2>&1; then
  echo "FAIL: portable extractor was accepted beside a PDFium runtime" >&2
  exit 1
fi
grep -F 'was built without PDFium' "$TMP/out" >/dev/null
if "$guard" "$pdfium" "" >"$TMP/out" 2>&1; then
  echo "FAIL: PDFium extractor was accepted without its runtime library" >&2
  exit 1
fi
grep -F 'has no runtime library' "$TMP/out" >/dev/null

echo "local document runtime guard: PASS"
