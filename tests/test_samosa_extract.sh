#!/bin/sh
set -eu

EXTRACTOR=${SAMOSA_EXTRACT:-./samosa-extract}
FIXTURE=tests/fixtures/documents/hello.pdf
TEXT_FIXTURE=tests/fixtures/documents/notes.txt

if [ ! -x "$EXTRACTOR" ]; then
  echo "samosa-extract: SKIP (build with PDFIUM_DIR=... make samosa-extract)"
  exit 0
fi

out=$("$EXTRACTOR" --json "$FIXTURE")
printf '%s' "$out" | grep -F '"ok":true' >/dev/null
printf '%s' "$out" | grep -F 'Hello PDFium' >/dev/null
printf '%s' "$out" | grep -F '"text_layer":true' >/dev/null
printf '%s' "$out" | grep -F '"index":1' >/dev/null

text_out=$("$EXTRACTOR" --json "$TEXT_FIXTURE")
printf '%s' "$text_out" | grep -F '"input_type":"text/plain"' >/dev/null
printf '%s' "$text_out" | grep -F 'Ada Lovelace' >/dev/null
printf '%s' "$text_out" | grep -F 'ada@example.test' >/dev/null

if [ -n "${SAMOSA_EXTRACT_TOKENIZER:-}" ]; then
  exact_out=$("$EXTRACTOR" --json "$TEXT_FIXTURE" --tokenizer "$SAMOSA_EXTRACT_TOKENIZER")
  printf '%s' "$exact_out" | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert isinstance(result["tokens"], int) and result["tokens"] > 0
assert result["tokens"] == result["pages"][0]["tokens"]
'
fi

# macOS's sandbox-exec checks the controller's required no-network policy. The
# Linux sandbox adapter is a separate packaging task, so its absence is fine.
if command -v sandbox-exec >/dev/null 2>&1; then
  network_out=$(sandbox-exec -p '(version 1) (allow default) (deny network*)' \
    "$EXTRACTOR" --json "$FIXTURE")
  printf '%s' "$network_out" | grep -F '"ok":true' >/dev/null
fi

error_file=$(mktemp "${TMPDIR:-/tmp}/samosa-extract-error.XXXXXX")
bad_file=$(mktemp "${TMPDIR:-/tmp}/samosa-extract-bad.XXXXXX")
link_file="$bad_file.link"
render_dir=$(mktemp -d "${TMPDIR:-/tmp}/samosa-extract-render.XXXXXX")
render_file="$render_dir/page.ppm"
trap 'rm -rf "$error_file" "$bad_file" "$link_file" "$render_dir"' EXIT HUP INT TERM

render_out=$("$EXTRACTOR" --render-ppm "$FIXTURE" 1 "$render_file")
printf '%s' "$render_out" | grep -F '"format":"image/x-portable-pixmap"' >/dev/null
[ "$(dd if="$render_file" bs=2 count=1 2>/dev/null)" = P6 ]
[ "$(wc -c <"$render_file" | tr -d ' ')" -gt 1000 ]
if "$EXTRACTOR" --render-ppm "$FIXTURE" 1 "$render_file" >"$error_file" 2>&1; then
  echo "samosa-extract overwrote an existing rendered page" >&2
  exit 1
fi
grep -F 'output_exists' "$error_file" >/dev/null

if "$EXTRACTOR" --json /dev/null >"$error_file" 2>&1; then
  echo "samosa-extract accepted a non-regular file" >&2
  exit 1
fi
grep -F 'not_regular_file' "$error_file" >/dev/null

printf '%%PDF-1.7\nbroken' >"$bad_file"
if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
  echo "samosa-extract accepted malformed input" >&2
  exit 1
fi
grep -F 'pdf_malformed' "$error_file" >/dev/null

printf 'not\000text' >"$bad_file"
if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
  echo "samosa-extract accepted binary non-PDF input" >&2
  exit 1
fi
grep -F 'text_invalid_utf8' "$error_file" >/dev/null

printf 'first\r\nsecond\rthird\n' >"$bad_file"
normalized_out=$("$EXTRACTOR" --json "$bad_file")
printf '%s' "$normalized_out" | python3 -c '
import json, sys
assert json.load(sys.stdin)["text"] == "first\nsecond\nthird\n"
'

printf 'PK\003\004x' >"$bad_file"
if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
  echo "samosa-extract silently treated a ZIP/DOCX as text" >&2
  exit 1
fi
grep -F 'docx_extractor_unavailable' "$error_file" >/dev/null

printf '<html><body>not plain text</body></html>' >"$bad_file"
if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
  echo "samosa-extract silently treated HTML as text" >&2
  exit 1
fi
grep -F 'html_extractor_unavailable' "$error_file" >/dev/null

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
