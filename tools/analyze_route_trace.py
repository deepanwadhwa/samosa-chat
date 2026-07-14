#!/usr/bin/env python3
"""Analyze deterministic MoE traces without changing the expert artifact."""

from __future__ import annotations

import argparse
import collections
import itertools
import json
from pathlib import Path


def read_trace(path: Path) -> tuple[dict, dict[int, list[list[int]]]]:
    metadata = None
    layers: dict[int, list[list[int]]] = collections.defaultdict(list)
    with path.open(encoding="utf-8") as handle:
        for line_number, raw in enumerate(handle, 1):
            if not raw.strip():
                continue
            record = json.loads(raw)
            if record.get("type") == "meta":
                if metadata is not None:
                    raise ValueError("duplicate trace metadata")
                metadata = record
                continue
            if record.get("type") != "route" or metadata is None:
                raise ValueError(f"invalid trace record at line {line_number}")
            effective_k = int(record.get("effective_k", metadata["selected_k"]))
            selected = [int(item) for item in record["ids"][:effective_k]]
            if len(selected) != len(set(selected)):
                raise ValueError(f"duplicate selected expert at line {line_number}")
            layers[int(record["layer"])].append(selected)
    if metadata is None:
        raise ValueError("trace has no metadata")
    return metadata, dict(layers)


def greedy_order(records: list[list[int]], experts: int) -> list[int]:
    frequency = collections.Counter(itertools.chain.from_iterable(records))
    pairs: collections.Counter[tuple[int, int]] = collections.Counter()
    for selected in records:
        for left, right in itertools.combinations(sorted(selected), 2):
            pairs[(left, right)] += 1
    remaining = set(range(experts))
    first = min(remaining, key=lambda item: (-frequency[item], item))
    order = [first]
    remaining.remove(first)
    while remaining:
        previous = order[-1]
        candidate = min(
            remaining,
            key=lambda item: (-pairs[tuple(sorted((previous, item)))],
                              -frequency[item], item),
        )
        order.append(candidate)
        remaining.remove(candidate)
    return order


def layout_metrics(records: list[list[int]], order: list[int]) -> dict[str, float]:
    position = {expert: index for index, expert in enumerate(order)}
    adjacency = spans = selected_total = 0
    for selected in records:
        positions = sorted(position[item] for item in selected)
        adjacency += sum(right == left + 1 for left, right in zip(positions, positions[1:]))
        spans += positions[-1] - positions[0] + 1
        selected_total += len(positions)
    count = max(1, len(records))
    return {
        "records": len(records),
        "mean_selected": selected_total / count,
        "mean_adjacent_selected_pairs": adjacency / count,
        "mean_layout_span": spans / count,
        "mean_span_over_selected": spans / max(1, selected_total),
    }


def analyze(metadata: dict, layers: dict[int, list[list[int]]]) -> dict:
    experts = int(metadata["experts"])
    current = list(range(experts))
    output = {"schema": 1, "trace_metadata": metadata, "layers": {}}
    weighted_current = weighted_candidate = total_records = 0.0
    for layer in sorted(layers):
        records = layers[layer]
        candidate = greedy_order(records, experts)
        current_metrics = layout_metrics(records, current)
        candidate_metrics = layout_metrics(records, candidate)
        output["layers"][str(layer)] = {
            "current_numeric": current_metrics,
            "greedy_coactivation": candidate_metrics,
            "candidate_order": candidate,
        }
        n = len(records)
        total_records += n
        weighted_current += current_metrics["mean_adjacent_selected_pairs"] * n
        weighted_candidate += candidate_metrics["mean_adjacent_selected_pairs"] * n
    output["aggregate"] = {
        "records": int(total_records),
        "current_mean_adjacent_selected_pairs": weighted_current / max(1, total_records),
        "candidate_mean_adjacent_selected_pairs": weighted_candidate / max(1, total_records),
    }
    output["limitations"] = [
        "A route-derived order is workload-specific and is not a release layout.",
        "Adjacency can reduce syscall/readahead cost only with a matching coalesced-read path.",
        "Physical order does not reduce logical cache misses or requested expert bytes by itself.",
    ]
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    metadata, layers = read_trace(args.trace)
    result = analyze(metadata, layers)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
