#!/bin/sh
set -eu

EXTRACTOR=${SAMOSA_EXTRACT:-./samosa-extract}
FIXTURE=tests/fixtures/documents/hello.pdf
TEXT_FIXTURE=tests/fixtures/documents/notes.txt

if [ ! -x "$EXTRACTOR" ]; then
  echo "samosa-extract is missing" >&2
  exit 1
fi

EXTRACT_VERSION=$($EXTRACTOR --version)
PDFIUM_ENABLED=0
case "$EXTRACT_VERSION" in
  *';pdfium)'*) PDFIUM_ENABLED=1 ;;
esac

if [ "$PDFIUM_ENABLED" = 1 ] && [ "$(uname -s)" = "Darwin" ]; then
  otool -l "$EXTRACTOR" | grep -F 'path @loader_path' >/dev/null || {
    echo "samosa-extract is missing its loader-relative PDFium rpath" >&2
    exit 1
  }
fi

if [ "$PDFIUM_ENABLED" = 1 ]; then
  out=$("$EXTRACTOR" --json "$FIXTURE")
  printf '%s' "$out" | grep -F '"ok":true' >/dev/null
  printf '%s' "$out" | grep -F 'Hello PDFium' >/dev/null
  printf '%s' "$out" | grep -F '"text_layer":true' >/dev/null
  printf '%s' "$out" | grep -F '"index":1' >/dev/null

  pages_out=$("$EXTRACTOR" --json-pages "$FIXTURE" 1 5)
  printf '%s' "$pages_out" | grep -F '"ok":true' >/dev/null
  printf '%s' "$pages_out" | grep -F '"page_count":1' >/dev/null
  printf '%s' "$pages_out" | grep -F '"page_start":1' >/dev/null
  printf '%s' "$pages_out" | grep -F '"page_end":1' >/dev/null
  printf '%s' "$pages_out" | grep -F 'Hello PDFium' >/dev/null

  if "$EXTRACTOR" --json-pages "$FIXTURE" 1 6 >/dev/null 2>&1; then
    echo "samosa-extract accepted a page range larger than five" >&2
    exit 1
  fi
  if "$EXTRACTOR" --json-pages "$FIXTURE" 2 1 >"${TMPDIR:-/tmp}/samosa-extract-pages-error.$$" 2>&1; then
    echo "samosa-extract accepted a page beyond the document" >&2
    exit 1
  fi
  grep -F 'page_out_of_range' "${TMPDIR:-/tmp}/samosa-extract-pages-error.$$" >/dev/null
  rm -f "${TMPDIR:-/tmp}/samosa-extract-pages-error.$$"
fi

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
    "$EXTRACTOR" --json "$TEXT_FIXTURE")
  printf '%s' "$network_out" | grep -F '"ok":true' >/dev/null
fi

error_file=$(mktemp "${TMPDIR:-/tmp}/samosa-extract-error.XXXXXX")
bad_file=$(mktemp "${TMPDIR:-/tmp}/samosa-extract-bad.XXXXXX")
link_file="$bad_file.link"
render_dir=$(mktemp -d "${TMPDIR:-/tmp}/samosa-extract-render.XXXXXX")
render_file="$render_dir/page.ppm"
docx_file="$render_dir/good.docx"
trap 'rm -rf "$error_file" "$bad_file" "$link_file" "$render_dir"' EXIT HUP INT TERM

if [ "$PDFIUM_ENABLED" = 1 ]; then
  render_out=$("$EXTRACTOR" --render-ppm "$FIXTURE" 1 "$render_file")
  printf '%s' "$render_out" | grep -F '"format":"image/x-portable-pixmap"' >/dev/null
  [ "$(dd if="$render_file" bs=2 count=1 2>/dev/null)" = P6 ]
  [ "$(wc -c <"$render_file" | tr -d ' ')" -gt 1000 ]
  if "$EXTRACTOR" --render-ppm "$FIXTURE" 1 "$render_file" >"$error_file" 2>&1; then
    echo "samosa-extract overwrote an existing rendered page" >&2
    exit 1
  fi
  grep -F 'output_exists' "$error_file" >/dev/null
else
  if "$EXTRACTOR" --json "$FIXTURE" >"$error_file" 2>&1; then
    echo "portable samosa-extract accepted a PDF without PDFium" >&2
    exit 1
  fi
  grep -F 'pdf_extractor_unavailable' "$error_file" >/dev/null
fi

if "$EXTRACTOR" --json /dev/null >"$error_file" 2>&1; then
  echo "samosa-extract accepted a non-regular file" >&2
  exit 1
fi
grep -F 'not_regular_file' "$error_file" >/dev/null

if [ "$PDFIUM_ENABLED" = 1 ]; then
  printf '%%PDF-1.7\nbroken' >"$bad_file"
  if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
    echo "samosa-extract accepted malformed input" >&2
    exit 1
  fi
  grep -F 'pdf_malformed' "$error_file" >/dev/null
fi

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
grep -F 'docx_malformed' "$error_file" >/dev/null

python3 - "$docx_file" <<'PY'
import sys, zipfile

document = '''<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Portable DOCX text</w:t></w:r></w:p><w:p><w:r><w:t>DOCX_CLI_SENTINEL</w:t></w:r></w:p></w:body></w:document>'''
content_types = '''<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'''
rels = '''<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'''
with zipfile.ZipFile(sys.argv[1], "w", compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("[Content_Types].xml", content_types)
    archive.writestr("_rels/.rels", rels)
    archive.writestr("word/document.xml", document)
PY
docx_probe=$("$EXTRACTOR" --probe "$docx_file")
printf '%s' "$docx_probe" | grep -F 'application/vnd.openxmlformats-officedocument.wordprocessingml.document' >/dev/null
docx_out=$("$EXTRACTOR" --json "$docx_file")
printf '%s' "$docx_out" | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert result["ok"] is True
assert result["input_type"] == "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
assert "Portable DOCX text" in result["text"]
assert "DOCX_CLI_SENTINEL" in result["text"]
'

printf '<!doctype html><html><head><title>Extractor &amp; HTML</title><style>STYLE_POISON</style></head><body><h1>Readable report</h1><ul><li>alpha</li><li>HTML_LATE_SENTINEL</li></ul><script>SCRIPT_POISON</script><template>TEMPLATE_POISON</template></body></html>' >"$bad_file"
html_out=$("$EXTRACTOR" --json "$bad_file")
printf '%s' "$html_out" | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert result["ok"] is True
assert result["input_type"] == "text/html"
assert result["title"] == "Extractor & HTML"
assert result["page_count"] == result["page_start"] == result["page_end"] == 1
assert "Readable report" in result["text"]
assert "HTML_LATE_SENTINEL" in result["text"]
assert "SCRIPT_POISON" not in result["text"]
assert "STYLE_POISON" not in result["text"]
assert "TEMPLATE_POISON" not in result["text"]
assert "<h1>" not in result["text"]
'

printf '<html>bad\300\257</html>' >"$bad_file"
if "$EXTRACTOR" --json "$bad_file" >"$error_file" 2>&1; then
  echo "samosa-extract accepted invalid UTF-8 HTML" >&2
  exit 1
fi
grep -F 'html_invalid_utf8' "$error_file" >/dev/null

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
