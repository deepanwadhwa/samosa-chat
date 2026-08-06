#!/bin/sh
set -eu

tool="${1:-./pagecache-residency}"
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/samosa-pagecache-test.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

# The OS page size varies by platform (16 KiB on Apple Silicon macOS, usually
# 4 KiB on Linux) — size the fixture from the real runtime value rather than
# hardcoding one platform's size, which previously asserted an exact page
# count that only held true on the reference Mac.
page_size=$(getconf PAGESIZE)
fixture="$tmpdir/experts.bin"
dd if=/dev/zero of="$fixture" bs="$page_size" count=2 status=none
"$tool" --json "$fixture" | python3 -c '
import json
import sys

page_size = '"$page_size"'
result = json.load(sys.stdin)
assert result["schema"] == 1
assert result["page_bytes"] == page_size
assert result["file_bytes"] == page_size * 2
assert result["pages"] == 2
assert 0 <= result["resident_pages"] <= result["pages"]
assert result["resident_bytes"] == result["resident_pages"] * result["page_bytes"]
assert 0.0 <= result["resident_percent"] <= 100.0
'
