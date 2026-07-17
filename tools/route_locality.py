#!/usr/bin/env python3
"""Report temporal expert-route locality from a qwen36 route-v2 JSONL trace."""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path


def read_trace(path: Path) -> tuple[dict, dict[int, list[list[int]]]]:
    meta = None
    layers: dict[int, list[list[int]]] = collections.defaultdict(list)
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("type") == "meta":
                if meta is not None:
                    raise ValueError("duplicate trace metadata")
                meta = record
                continue
            if meta is None or record.get("type") != "route":
                raise ValueError(f"invalid trace record at line {line_number}")
            selected = record["ids"][:int(record.get("effective_k", meta["selected_k"]))]
            if len(selected) != len(set(selected)):
                raise ValueError(f"duplicate selected expert at line {line_number}")
            layers[int(record["layer"])].append([int(item) for item in selected])
    if meta is None:
        raise ValueError("trace has no metadata")
    return meta, dict(layers)


def percentile(values: list[int], quantile: float) -> float | None:
    if not values:
        return None
    values = sorted(values)
    return values[round((len(values) - 1) * quantile)]


def layer_metrics(records: list[list[int]]) -> dict:
    references = [expert for selected in records for expert in selected]
    last: dict[int, int] = {}
    reuse: list[int] = []
    cold = 0
    for token, selected in enumerate(records):
        for expert in selected:
            if expert in last:
                reuse.append(token - last[expert])
            else:
                cold += 1
            last[expert] = token
    overlaps = [len(set(left) & set(right)) for left, right in zip(records, records[1:])]
    counts = collections.Counter(references)
    needed = 0
    covered = 0
    target = 0.9 * len(references)
    for _, count in counts.most_common():
        needed += 1
        covered += count
        if covered >= target:
            break
    windows = {}
    for width in (4, 6, 8):
        unions = [len(set().union(*records[start:start + width]))
                  for start in range(max(0, len(records) - width + 1))]
        windows[str(width)] = {
            "samples": len(unions), "mean": sum(unions) / len(unions) if unions else None,
            "p50": percentile(unions, .50), "p95": percentile(unions, .95),
        }
    return {
        "tokens": len(records), "references": len(references), "cold_references": cold,
        "reuse_distance": {"samples": len(reuse), "p50": percentile(reuse, .50),
                           "p95": percentile(reuse, .95), "mean": sum(reuse) / len(reuse) if reuse else None},
        "next_token_overlap": {"samples": len(overlaps), "mean_experts": sum(overlaps) / len(overlaps) if overlaps else None,
                               "mean_fraction_of_current": sum(overlap / len(records[i]) for i, overlap in enumerate(overlaps)) / len(overlaps) if overlaps else None},
        "hotset": {"unique_pairs": len(counts), "pairs_for_90pct": needed,
                    "fraction_pairs_for_90pct": needed / len(counts) if counts else None},
        "union_windows": windows,
    }


def analyze(meta: dict, layers: dict[int, list[list[int]]]) -> dict:
    return {"schema": 1, "trace_metadata": meta,
            "layers": {str(layer): layer_metrics(records) for layer, records in sorted(layers.items())}}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    meta, layers = read_trace(args.trace)
    text = json.dumps(analyze(meta, layers), indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
