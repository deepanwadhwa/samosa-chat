#!/bin/sh
# Native library smoke test; dense PDFs are covered by test-pdf-ocr-runtime.
set -eu
OCR="${SAMOSA_OCR:-./build/samosa-ocr}"
"$OCR" --version | grep -q tesseract
"$OCR" --check | grep -q '"ok":true'
READ=$("$OCR" read tools/testdata/ocr/tiny.png)
echo "$READ"
echo "$READ" | grep -q '2019'
echo "$READ" | grep -q '"reader":"tesseract"'
if SAMOSA_TESSDATA=/nonexistent/samosa-tessdata "$OCR" --check; then
  echo 'FAIL: missing language data accepted' >&2; exit 1
fi
echo 'ocr-test: PASS'
