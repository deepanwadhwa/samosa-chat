#!/bin/sh
set -eu

tool="${1:-./pagecache-residency}"
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/samosa-pagecache-test.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture="$tmpdir/experts.bin"
dd if=/dev/zero of="$fixture" bs=16384 count=2 status=none
"$tool" --json "$fixture" | python3 -c '
import json
import sys

result = json.load(sys.stdin)
assert result["schema"] == 1
assert result["file_bytes"] == 32768
assert result["pages"] == 2
assert 0 <= result["resident_pages"] <= result["pages"]
assert result["resident_bytes"] == result["resident_pages"] * result["page_bytes"]
assert 0.0 <= result["resident_percent"] <= 100.0
'
