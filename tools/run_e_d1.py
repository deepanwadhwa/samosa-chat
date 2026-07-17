#!/usr/bin/env python3
"""Run the reproducible local PDF portion of document experiment E-D1.

Inputs are never copied into the repository. The report has only basenames,
SHA-256 digests, and extractor metadata so a user can retain private source
documents while recording reproducible evidence.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import platform
import subprocess
import sys
import time
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run_one(extractor: str, tokenizer: str | None, path: Path) -> dict[str, object]:
    command = [extractor, "--json", str(path)]
    if tokenizer:
        command.extend(["--tokenizer", tokenizer])
    started = time.monotonic()
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    elapsed_ms = round((time.monotonic() - started) * 1000, 1)
    record: dict[str, object] = {
        "file": path.name,
        "sha256": sha256(path),
        "bytes": path.stat().st_size,
        "wall_ms": elapsed_ms,
        "exit_code": completed.returncode,
    }
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError:
        record["error"] = "non_json_output"
        record["stderr"] = completed.stderr[:500]
        return record
    record["ok"] = payload.get("ok", False)
    if not record["ok"]:
        record["error"] = payload.get("error", "unknown")
        return record
    pages = payload.get("pages", [])
    record.update({
        "pages": len(pages),
        "text_layer": payload.get("text_layer"),
        "tokens": payload.get("tokens"),
        "tokens_estimate": payload.get("tokens_estimate"),
        "pages_with_raster_figures": sum(bool(page.get("has_raster_figure")) for page in pages),
        "page_tokens": [page.get("tokens") for page in pages] if tokenizer else None,
    })
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--extractor", required=True)
    parser.add_argument("--tokenizer", help="trusted installed Qwen tokenizer for exact counts")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    if not Path(args.extractor).is_file() or not Path(args.extractor).stat().st_mode & 0o111:
        parser.error(f"extractor is not executable: {args.extractor}")
    missing = [str(path) for path in args.inputs if not path.is_file()]
    if missing:
        parser.error("input is not a regular file: " + ", ".join(missing))
    report = {
        "experiment": "E-D1 PDF extraction",
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "platform": {"system": platform.system(), "release": platform.release(), "machine": platform.machine()},
        "extractor": Path(args.extractor).name,
        "tokenizer": Path(args.tokenizer).name if args.tokenizer else None,
        "results": [run_one(args.extractor, args.tokenizer, path) for path in args.inputs],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    failures = [entry for entry in report["results"] if not entry.get("ok")]
    print(f"E-D1: {len(report['results']) - len(failures)}/{len(report['results'])} extracted -> {args.out}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
