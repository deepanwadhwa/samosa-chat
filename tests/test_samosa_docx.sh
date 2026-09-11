#!/bin/sh
set -eu

RUNNER=${SAMOSA_DOCX_TEST_RUNNER:-./build/test-samosa-docx}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-docx-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

python3 - "$TMP" <<'PY'
import pathlib, sys, zipfile

root = pathlib.Path(sys.argv[1])
content_types = '''<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
</Types>'''
rels = '''<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>'''
document = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
<w:p><w:r><w:t>Hello </w:t></w:r><w:r><w:t>DOCX &amp; world</w:t></w:r></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Key</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Value</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:p><w:del><w:r><w:t>DELETED_POISON</w:t></w:r></w:del><w:r><w:t>DOCX_LATE_SENTINEL</w:t></w:r></w:p>
</w:body></w:document>'''
header = '''<w:hdr xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:p><w:r><w:t>HEADER_SENTINEL</w:t></w:r></w:p></w:hdr>'''

def package(path, doc=document, extra=(), types=content_types, relationships=rels):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", types)
        z.writestr("_rels/.rels", relationships)
        z.writestr("word/document.xml", doc)
        z.writestr("word/header1.xml", header)
        for name, value in extra:
            z.writestr(name, value)

package(root / "good.docx")
package(root / "traversal.docx", extra=(("../escape.xml", "bad"),))
package(root / "doctype.docx", doc='''<!DOCTYPE w:document [<!ENTITY xxe SYSTEM "file:///etc/passwd">]><w:document xmlns:w="x"><w:body><w:p><w:r><w:t>&xxe;</w:t></w:r></w:p></w:body></w:document>''')
package(root / "invalid-utf8.docx", doc=b'<w:document xmlns:w="x"><w:body><w:p><w:r><w:t>bad\xc0\xaf</w:t></w:r></w:p></w:body></w:document>')
package(root / "bad-content-type.docx", types="<Types/>")
package(root / "bad-rels.docx", relationships="<Relationships/>")
package(root / "bomb.docx", doc='<w:document xmlns:w="x"><w:body><w:p><w:r><w:t>' + ('A' * (3 * 1024 * 1024)) + '</w:t></w:r></w:p></w:body></w:document>')
with zipfile.ZipFile(root / "missing.docx", "w", compression=zipfile.ZIP_DEFLATED) as z:
    z.writestr("word/document.xml", document)
with zipfile.ZipFile(root / "many.docx", "w", compression=zipfile.ZIP_STORED) as z:
    z.writestr("[Content_Types].xml", content_types)
    z.writestr("_rels/.rels", rels)
    z.writestr("word/document.xml", document)
    for i in range(2046):
        z.writestr(f"custom/item{i}.xml", "x")
(root / "malformed.docx").write_bytes(b"PK\x03\x04not-a-zip")
PY

OUT=$($RUNNER "$TMP/good.docx")
printf '%s' "$OUT" | grep -F 'Hello DOCX & world' >/dev/null
printf '%s' "$OUT" | grep -F 'DOCX_LATE_SENTINEL' >/dev/null
printf '%s' "$OUT" | grep -F '[word/header1.xml]' >/dev/null
printf '%s' "$OUT" | grep -F 'HEADER_SENTINEL' >/dev/null
if printf '%s' "$OUT" | grep -F 'DELETED_POISON' >/dev/null; then
  echo "DOCX extractor retained deleted revision text" >&2; exit 1
fi

expect_error() {
  file=$1 expected=$2
  if "$RUNNER" "$TMP/$file" >"$TMP/error" 2>&1; then
    echo "DOCX extractor accepted $file" >&2; exit 1
  fi
  grep -F "error:$expected" "$TMP/error" >/dev/null || {
    echo "DOCX extractor returned the wrong error for $file" >&2
    sed -n '1,5p' "$TMP/error" >&2
    exit 1
  }
}

expect_error traversal.docx docx_unsafe_path
expect_error doctype.docx docx_xml_doctype_unsupported
expect_error invalid-utf8.docx docx_xml_invalid_utf8
expect_error bad-content-type.docx docx_content_type_invalid
expect_error bad-rels.docx docx_relationship_invalid
expect_error bomb.docx docx_compression_ratio_limit
expect_error missing.docx docx_required_entry_missing
expect_error many.docx docx_entry_count_limit
expect_error malformed.docx docx_malformed

if "$RUNNER" "$TMP/good.docx" 16 >"$TMP/error" 2>&1; then
  echo "DOCX extractor ignored its text-output bound" >&2; exit 1
fi
grep -F 'error:docx_text_output_limit' "$TMP/error" >/dev/null

echo "test_samosa_docx: PASS"
