#!/usr/bin/env python3
"""Score one frozen decision-model run against the Samosa gold JSONL."""

import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


REQUIRED_MEASUREMENTS = (
    "input_tokens", "latency_ms", "peak_rss_bytes", "swap_delta_bytes",
    "thermal_celsius", "thermal_observed", "memory_pressure",
)
PRESSURE_ORDER = {"normal": 0, "warn": 1, "critical": 2}


def read_jsonl(path):
    rows = []
    with Path(path).open(encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            if not line.strip():
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{number}: invalid JSON: {exc}") from exc
    return rows


def percentile(values, percentile_value):
    ordered = sorted(values)
    if not ordered:
        return None
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentile_value / 100
    lower = math.floor(position)
    upper = math.ceil(position)
    fraction = position - lower
    return ordered[lower] * (1 - fraction) + ordered[upper] * fraction


def validate_gold(gold):
    by_id = {}
    for index, row in enumerate(gold, 1):
        case_id = row.get("case_id")
        options = row.get("options")
        if not case_id or case_id in by_id:
            raise ValueError(f"gold row {index}: missing or duplicate case_id {case_id!r}")
        if not isinstance(row.get("task"), str) or not row["task"]:
            raise ValueError(f"gold row {index}: task is required for comparable metrics")
        if not isinstance(options, list) or len(options) < 2 or len(set(options)) != len(options):
            raise ValueError(f"gold row {index}: options must contain at least two unique IDs")
        if row.get("gold") not in options:
            raise ValueError(f"gold row {index}: expected label is absent from options")
        by_id[case_id] = row
    if not by_id:
        raise ValueError("gold set is empty")
    return by_id


def validate_predictions(gold_by_id, predictions, token_budget):
    by_id = {}
    revision = None
    for index, row in enumerate(predictions, 1):
        case_id = row.get("case_id")
        if case_id not in gold_by_id:
            raise ValueError(f"prediction row {index}: unknown case_id {case_id!r}")
        if case_id in by_id:
            raise ValueError(f"prediction row {index}: duplicate case_id {case_id!r}")
        if not isinstance(row.get("model_revision"), str) or not row["model_revision"]:
            raise ValueError(f"prediction {case_id}: model_revision is required")
        revision = revision or row["model_revision"]
        if row["model_revision"] != revision:
            raise ValueError("one run must use a single pinned model_revision")
        if row.get("token_budget") != token_budget:
            raise ValueError(f"prediction {case_id}: token_budget does not match this run")
        options = gold_by_id[case_id]["options"]
        selected = row.get("selected_option")
        if selected not in options:
            raise ValueError(f"prediction {case_id}: selected_option is not an allowed option")
        scores = row.get("scores")
        if not isinstance(scores, dict) or set(scores) != set(options):
            raise ValueError(f"prediction {case_id}: scores must contain every allowed option exactly once")
        values = []
        for option in options:
            score = scores[option]
            if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score) or not 0 <= score <= 1:
                raise ValueError(f"prediction {case_id}: score for {option!r} must be finite and in [0,1]")
            values.append(score)
        if abs(sum(values) - 1) > 0.02:
            raise ValueError(f"prediction {case_id}: scores must sum to 1 within 0.02")
        if not isinstance(row.get("abstained"), bool):
            raise ValueError(f"prediction {case_id}: abstained must be boolean")
        if row["abstained"] and not str(row.get("abstention_reason", "")).strip():
            raise ValueError(f"prediction {case_id}: abstention_reason is required when abstaining")
        for field in REQUIRED_MEASUREMENTS:
            value = row.get(field)
            if field == "memory_pressure":
                if value not in PRESSURE_ORDER:
                    raise ValueError(f"prediction {case_id}: memory_pressure must be normal, warn, or critical")
                continue
            if field == "thermal_observed":
                if not isinstance(value, bool):
                    raise ValueError(f"prediction {case_id}: thermal_observed must be boolean")
                if value != (row.get("thermal_celsius") is not None):
                    raise ValueError(f"prediction {case_id}: thermal_observed must match thermal_celsius availability")
                continue
            if field == "thermal_celsius" and value is None and row.get("thermal_observed") is False:
                continue
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError(f"prediction {case_id}: {field} must be a finite non-negative number")
            if field == "input_tokens" and value > token_budget:
                raise ValueError(f"prediction {case_id}: input_tokens exceeds token_budget")
        by_id[case_id] = row
    missing = sorted(set(gold_by_id) - set(by_id))
    if missing:
        raise ValueError(f"missing predictions for {len(missing)} cases: {', '.join(missing)}")
    return by_id, revision


def summarize(gold_by_id, predicted_by_id, revision, token_budget, bins=10):
    total = len(gold_by_id)
    correct = 0
    tp = fp = fn = 0
    confidence_pairs = []
    grouping = defaultdict(lambda: [0, 0])
    decision_groups = defaultdict(lambda: {"gold": [], "predicted": []})
    rows = []
    for case_id, gold in gold_by_id.items():
        prediction = predicted_by_id[case_id]
        expected = gold["gold"]
        selected = prediction["selected_option"]
        is_correct = selected == expected
        correct += int(is_correct)
        class_count = str(len(gold["options"]))
        decision_group = gold["task"] + ":" + class_count + "_options"
        decision_groups[decision_group]["gold"].append(expected)
        decision_groups[decision_group]["predicted"].append(selected)
        abstain_expected = bool(gold.get("review_expected", False))
        abstained = prediction["abstained"]
        tp += int(abstained and abstain_expected)
        fp += int(abstained and not abstain_expected)
        fn += int(not abstained and abstain_expected)
        grouping[class_count][0] += int(is_correct)
        grouping[class_count][1] += 1
        if gold.get("placement") in ("begin", "middle", "end"):
            grouping["position:" + gold["placement"]][0] += int(is_correct)
            grouping["position:" + gold["placement"]][1] += 1
        confidence_pairs.append((float(prediction["scores"][selected]), float(is_correct)))
        rows.append(prediction)

    task_metrics = {}
    task_macro_f1s = []
    for group_id, values in sorted(decision_groups.items()):
        labels = set(values["gold"]) | set(values["predicted"])
        f1s = []
        for label in labels:
            true_positive = sum(g == label and p == label
                                for g, p in zip(values["gold"], values["predicted"]))
            false_positive = sum(g != label and p == label
                                 for g, p in zip(values["gold"], values["predicted"]))
            false_negative = sum(g == label and p != label
                                 for g, p in zip(values["gold"], values["predicted"]))
            denominator = 2 * true_positive + false_positive + false_negative
            f1s.append(2 * true_positive / denominator if denominator else 0.0)
        group_accuracy = sum(g == p for g, p in
                             zip(values["gold"], values["predicted"])) / len(values["gold"])
        group_macro_f1 = statistics.mean(f1s) if f1s else 0.0
        task_macro_f1s.append(group_macro_f1)
        task_metrics[group_id] = {
            "cases": len(values["gold"]),
            "accuracy": group_accuracy,
            "macro_f1": group_macro_f1,
        }
    ece = 0.0
    for bin_index in range(bins):
        low, high = bin_index / bins, (bin_index + 1) / bins
        members = [(confidence, outcome) for confidence, outcome in confidence_pairs
                   if low <= confidence < high or (bin_index == bins - 1 and confidence == 1)]
        if members:
            mean_confidence = statistics.mean(item[0] for item in members)
            mean_accuracy = statistics.mean(item[1] for item in members)
            ece += len(members) / total * abs(mean_confidence - mean_accuracy)

    def safe_ratio(numerator, denominator):
        return numerator / denominator if denominator else None

    metrics = {
        "cases": total,
        "model_revision": revision,
        "token_budget": token_budget,
        "accuracy": correct / total,
        "macro_f1": statistics.mean(task_macro_f1s) if task_macro_f1s else 0.0,
        "metrics_by_task_and_class_count": task_metrics,
        "expected_calibration_error_10_bins": ece,
        "abstention_precision": safe_ratio(tp, tp + fp),
        "abstention_recall": safe_ratio(tp, tp + fn),
        "latency_ms_p50": percentile([row["latency_ms"] for row in rows], 50),
        "latency_ms_p95": percentile([row["latency_ms"] for row in rows], 95),
        "peak_rss_bytes_max": max(row["peak_rss_bytes"] for row in rows),
        "swap_delta_bytes_total": sum(row["swap_delta_bytes"] for row in rows),
        "thermal_celsius_max": max(
            (row["thermal_celsius"] for row in rows if row["thermal_celsius"] is not None),
            default=None),
        "thermal_observed_cases": sum(row["thermal_observed"] for row in rows),
        "memory_pressure_counts": {
            level: sum(row["memory_pressure"] == level for row in rows)
            for level in PRESSURE_ORDER
        },
        "memory_pressure_worst": max(
            (row["memory_pressure"] for row in rows),
            key=lambda level: PRESSURE_ORDER[level]),
        "accuracy_by_class_count_and_position": {
            group: {"correct": values[0], "cases": values[1],
                    "accuracy": safe_ratio(values[0], values[1])}
            for group, values in sorted(grouping.items())
        },
        "scope_note": "Offline hand-authored English development set; this report alone does not qualify a shipped model.",
    }
    return metrics


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gold", help="frozen gold JSONL")
    parser.add_argument("predictions", help="one-model-run prediction JSONL")
    parser.add_argument("--token-budget", type=int, required=True,
                        choices=(512, 1024, 2048, 4096, 8192))
    args = parser.parse_args(argv)
    try:
        gold_by_id = validate_gold(read_jsonl(args.gold))
        predicted_by_id, revision = validate_predictions(
            gold_by_id, read_jsonl(args.predictions), args.token_budget)
        print(json.dumps(summarize(gold_by_id, predicted_by_id, revision,
                                   args.token_budget), indent=2, sort_keys=True))
    except (OSError, ValueError) as exc:
        print(f"evaluate.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
