#!/usr/bin/env python3
"""Assert a folder_report's selected count and skips match shared inventory."""
import json
import sys
from collections import Counter


inventory_path, report_path = sys.argv[1:]
inventory = [json.loads(line) for line in open(inventory_path, encoding="utf-8") if line.strip()]
report_lines = [line.strip() for line in open(report_path, encoding="utf-8") if line.strip()]
report_rows = [json.loads(line.removeprefix("data: ")) for line in report_lines if line.startswith("data: {")]
done = next(row for row in inventory if row.get("type") == "done")
report = next(row for row in report_rows if row.get("type") == "report")
skip_counts = Counter(row["reason"] for row in inventory if row.get("type") == "skip")
assert report["total"] == done["files"], (report["total"], done["files"])
assert report["partial"] == done["partial"], (report["partial"], done["partial"])
assert report["limiting_reason"] == done["limiting_reason"], (report["limiting_reason"], done["limiting_reason"])
assert report["skip_reasons"] == dict(skip_counts), (report["skip_reasons"], dict(skip_counts))
print(json.dumps({"files": report["total"], "partial": report["partial"], "limiting_reason": report["limiting_reason"], "skip_reasons": report["skip_reasons"]}, sort_keys=True))
