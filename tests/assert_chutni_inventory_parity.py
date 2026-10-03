#!/usr/bin/env python3
"""Compare a samosa-fs Chutni preview with paths admitted by Chutni scan."""
import json
import os
import sys

preview_path, scan_path, sources_path, root = sys.argv[1:]
with open(preview_path, encoding="utf-8") as stream:
    preview = [json.loads(line) for line in stream if line.strip()]
with open(scan_path, encoding="utf-8") as stream:
    scan = json.load(stream)
with open(sources_path, encoding="utf-8") as stream:
    sources = json.load(stream)["sources"]

expected = {row["rel_path"] for row in preview if row.get("type") == "file"}
done = next(row for row in preview if row.get("type") == "done")
assert scan["scan"]["partial"] is True, scan
assert scan["scan"]["limiting_reason"] == done["limiting_reason"], {
    "preflight_reason": done.get("limiting_reason"),
    "scanner_reason": scan["scan"].get("limiting_reason"),
}
actual = {
    os.path.relpath(row["display_path"], root)
    for row in sources if row.get("state") == "present"
}
assert actual == expected, {"preflight": sorted(expected), "indexed": sorted(actual)}
