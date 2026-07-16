#!/bin/sh
set -eu

EXTRACTOR=${SAMOSA_EXTRACT:-./samosa-extract}
FIXTURE=tests/fixtures/documents/hello.pdf

if [ ! -x "$EXTRACTOR" ]; then
  echo "samosa-extract: SKIP (build with PDFIUM_DIR=... make samosa-extract)"
  exit 0
fi

out=$("$EXTRACTOR" --json "$FIXTURE")
printf '%s' "$out" | grep -F '"ok":true' >/dev/null
printf '%s' "$out" | grep -F 'Hello PDFium' >/dev/null
printf '%s' "$out" | grep -F '"text_layer":true' >/dev/null
printf '%s' "$out" | grep -F '"index":1' >/dev/null

error_file=$(mktemp "${TMPDIR:-/tmp}/samosa-extract-error.XXXXXX")
bad_file=$(mktemp "${TMPDIR:-/tmp}/samosa-extract-bad.XXXXXX")
link_file="$bad_file.link"
trap 'rm -f "$error_file" "$bad_file" "$link_file"' EXIT HUP INT TERM

if "$EXTRACTOR" --json /dev/null >"$error_file" 2>&1; then
  echo "samosa-extract accepted a non-regular file" >&2
  exit 1
fi
grep -F 'not_regular_file' "$error_file" >/dev/null

printf 'not a PDF' >"$bad_file"
if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
  echo "samosa-extract accepted malformed input" >&2
  exit 1
fi
grep -F 'pdf_malformed' "$error_file" >/dev/null

ln -s "$bad_file" "$link_file"
if "$EXTRACTOR" --json "$link_file" >"$error_file" 2>&1; then
  echo "samosa-extract followed a symlink" >&2
  exit 1
fi
grep -F 'symlink_not_allowed' "$error_file" >/dev/null

if SAMOSA_EXTRACT_MAX_BYTES=10 "$EXTRACTOR" --json "$FIXTURE" >"$error_file" 2>&1; then
  echo "samosa-extract ignored its input-size limit" >&2
  exit 1
fi
grep -F 'file_too_large' "$error_file" >/dev/null

echo "samosa-extract: PASS"
