#!/usr/bin/env python3
"""E-J1 harness: run a labeled Samosa Jobs corpus against a live local serve.

The harness deliberately does not launch or stop the model server.  Keeping
server lifecycle explicit prevents a benchmark from interrupting an owner's
interactive session.  It records the exact jobs commands, host-safety samples,
Jobs event timing, and strict field-by-field comparison with the supplied
labels.  Input fixtures are intentionally separate from the harness so a
real-image/PDF corpus can replace the starter text corpus without code changes.

With no ``--job``, this runs the bundled text-receipt starter corpus.  Pass a
validated Jobs definition with ``--job`` and its expected-record mapping with
``--labels`` to evaluate a representative PDF or image corpus.  The harness
copies that definition into its results directory and redirects its job state
and merged output there; it never changes the supplied definition or existing
Jobs artifacts.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUTS = ROOT / "tests" / "fixtures" / "jobs" / "e_j1_text"
DEFAULT_LABELS = ROOT / "tests" / "fixtures" / "jobs" / "e_j1_labels.json"


def command_output(command: list[str]) -> str:
    try:
        return subprocess.run(command, check=False, text=True,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT).stdout.strip()
    except OSError as error:
        return f"unavailable: {error}"


def vm_stat() -> dict[str, int]:
    result: dict[str, int] = {}
    for line in command_output(["vm_stat"]).splitlines():
        name, separator, value = line.partition(":")
        if separator and name in {"Pages throttled", "Swapins", "Swapouts"}:
            result[name.lower().replace(" ", "_")] = int(value.strip().rstrip("."))
    return result


def serve_json(url: str, path: str) -> dict[str, object] | None:
    try:
        with urllib.request.urlopen(f"{url}{path}", timeout=5) as response:
            value = json.loads(response.read())
        return value if isinstance(value, dict) else None
    except Exception:
        return None


def safety_sample(results: Path, serve_url: str) -> dict[str, object]:
    usage = shutil.disk_usage(results)
    return {
        "timestamp": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "disk_free_gb": round(usage.free / 1_000_000_000, 3),
        "vm": vm_stat(),
        "memory_pressure": command_output(["memory_pressure", "-Q"]),
        "thermal": command_output(["pmset", "-g", "therm"]),
        "power": command_output(["pmset", "-g", "batt"]),
        "serve_status": serve_json(serve_url, "/internal/v1/status"),
        "serve_health": serve_json(serve_url, "/healthz"),
    }


def make_job(job_id: str, inputs: Path, output: Path) -> dict[str, object]:
    return {
        "schema_version": 1,
        "job_id": job_id,
        "name": "E-J1 labeled text corpus",
        "input": {
            "folder": str(inputs.resolve()),
            "recursive": False,
            "types": ["text/plain"],
            "max_file_bytes": 26_214_400,
        },
        "unit": "auto",
        "instruction": (
            "Extract receipt fields exactly as written. Normalize dates to "
            "YYYY-MM-DD when unambiguous. Use null for a field not shown."
        ),
        "reduce": {"mode": "deterministic", "model_fields": []},
        "inference": {
            "thinking": "off", "seed": 11, "temperature": 0,
            "max_tokens": 256, "timeout_s": None,
        },
        "output_schema": {
            "type": "object",
            "required": ["merchant", "date", "subtotal", "tax", "total", "currency"],
            "properties": {
                "merchant": {"type": ["string", "null"]},
                "date": {"type": ["string", "null"]},
                "subtotal": {"type": ["number", "null"]},
                "tax": {"type": ["number", "null"]},
                "total": {"type": ["number", "null"]},
                "currency": {"type": ["string", "null"], "maxLength": 3},
            },
        },
        "validation": {"domain_rules": ["subtotal + tax ~= total"]},
        "output": {"dir": str(output.resolve()), "format": "jsonl"},
        "resources": {
            "max_attempts": 2, "run_on_battery": False,
            "pause_when_user_active": True, "min_free_gb": 10,
        },
    }


def load_experiment_job(job_path: Path, inputs: Path | None, output: Path,
                        job_id: str | None) -> tuple[dict[str, object], Path]:
    """Load a user-supplied job and redirect only experiment-local fields."""
    try:
        job = json.loads(job_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read job: {error}") from error
    if not isinstance(job, dict) or not isinstance(job.get("input"), dict):
        raise ValueError("job must be a JSON object with an input object")

    source_inputs = inputs or Path(str(job["input"].get("folder", "")))
    if not source_inputs.is_dir():
        raise ValueError(f"input folder is missing: {source_inputs}")
    job["input"]["folder"] = str(source_inputs.resolve())
    job["output"] = dict(job.get("output", {}))
    job["output"]["dir"] = str(output.resolve())
    job["job_id"] = job_id or str(job.get("job_id", "e-j1"))
    return job, source_inputs


def run_command(command: list[str], env: dict[str, str], output: Path) -> dict[str, object]:
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=ROOT, env=env, check=False,
                               text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    output.write_text(completed.stdout, encoding="utf-8")
    return {
        "command": command,
        "returncode": completed.returncode,
        "wall_seconds": round(time.perf_counter() - started, 3),
        "output_file": output.name,
    }


def load_records(path: Path) -> list[dict[str, object]]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def values_equal(actual: object, expected: object) -> bool:
    if isinstance(actual, (int, float)) and not isinstance(actual, bool) and \
            isinstance(expected, (int, float)) and not isinstance(expected, bool):
        return abs(float(actual) - float(expected)) <= 0.01
    return actual == expected


def evaluate(records: list[dict[str, object]], labels: dict[str, dict[str, object]]) -> dict[str, object]:
    records_by_name = {Path(str(r.get("input_path", ""))).name: r for r in records}
    cases = []
    field_total = field_correct = 0
    for name, expected in sorted(labels.items()):
        actual = records_by_name.get(name)
        fields: dict[str, bool] = {}
        for field, value in expected.items():
            fields[field] = actual is not None and values_equal(actual.get(field), value)
            field_total += 1
            field_correct += int(fields[field])
        cases.append({"input": name, "record_present": actual is not None,
                      "fields": fields, "actual": actual})
    return {
        "records": len(records),
        "labeled_inputs": len(labels),
        "field_correct": field_correct,
        "field_total": field_total,
        "field_accuracy": round(field_correct / field_total, 4) if field_total else None,
        "cases": cases,
    }


def active_inference_seconds(events: list[dict[str, object]], items_dir: Path) -> float:
    total = 0.0
    timed_units: set[str] = set()
    for event in events:
        seconds = event.get("model_call_seconds")
        if isinstance(seconds, (int, float)) and not isinstance(seconds, bool):
            total += seconds
            if isinstance(event.get("unit_id"), str):
                timed_units.add(event["unit_id"])
    for event in events:
        if event.get("type") != "item_complete":
            continue
        unit_id = event.get("unit_id")
        if not isinstance(unit_id, str) or unit_id in timed_units:
            continue
        provenance = items_dir / f"{unit_id.replace('#', '_').replace('/', '_')}.provenance.json"
        try:
            seconds = json.loads(provenance.read_text()).get("wall_seconds")
            if isinstance(seconds, (int, float)) and not isinstance(seconds, bool):
                total += seconds
        except (OSError, json.JSONDecodeError):
            pass
    return round(total, 3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job", type=Path,
                        help="existing Jobs definition; defaults to the text starter job")
    parser.add_argument("--inputs", type=Path,
                        help="override the job input folder")
    parser.add_argument("--labels", type=Path,
                        help="expected-record JSON; defaults to the bundled text labels")
    parser.add_argument("--results", type=Path, required=True,
                        help="new directory for this run's evidence")
    parser.add_argument("--serve-url", default="http://127.0.0.1:8642")
    parser.add_argument("--engine", type=Path, default=ROOT / "qwen36b")
    parser.add_argument("--tokenizer", type=Path, default=ROOT / "tokenizer_qwen36.json")
    parser.add_argument("--job-id",
                        help="experiment-local job id (defaults to the supplied job id)")
    args = parser.parse_args()

    if args.results.exists():
        parser.error(f"results directory already exists: {args.results}")
    if serve_json(args.serve_url, "/healthz") is None:
        parser.error(f"serve is not healthy at {args.serve_url}")
    if not args.engine.is_file() or not args.tokenizer.is_file():
        parser.error("engine or tokenizer is missing")

    args.results.mkdir(parents=True)
    jobs_root = args.results / "jobs"
    output_dir = args.results / "output"
    job_path = args.results / "job.json"
    try:
        if args.job:
            labels_path = args.labels or args.job.with_name(
                f"{args.job.stem}.expected.json")
            job, inputs = load_experiment_job(args.job, args.inputs, output_dir,
                                              args.job_id)
        else:
            inputs = args.inputs or DEFAULT_INPUTS
            labels_path = args.labels or DEFAULT_LABELS
            if not inputs.is_dir():
                raise ValueError(f"input folder is missing: {inputs}")
            job = make_job(args.job_id or "e-j1-text", inputs, output_dir)
        if not labels_path.is_file():
            raise ValueError(f"labels file is missing: {labels_path}")
        labels = json.loads(labels_path.read_text(encoding="utf-8"))
        if not isinstance(labels, dict) or not labels:
            raise ValueError("labels must be a non-empty JSON object")
    except (ValueError, json.JSONDecodeError) as error:
        shutil.rmtree(args.results)
        parser.error(str(error))

    job_path.write_text(json.dumps(job, indent=2) + "\n", encoding="utf-8")

    env = os.environ.copy()
    env.update({
        "SAMOSA_JOBS_DIR": str(jobs_root),
        "SAMOSA_SERVE_URL": args.serve_url,
        "SAMOSA_ENGINE": str(args.engine.resolve()),
        "TOKENIZER": str(args.tokenizer.resolve()),
    })
    runner = [sys.executable, str(ROOT / "dist" / "samosa_jobs.py")]
    first_name = sorted(labels)[0]
    first_input = inputs / first_name
    if not first_input.is_file():
        shutil.rmtree(args.results)
        parser.error(f"labeled input is missing from input folder: {first_name}")
    before = safety_sample(args.results, args.serve_url)
    preview = run_command(runner + ["preview", str(job_path), "--file", str(first_input)], env,
                          args.results / "preview.log")
    run = run_command(runner + ["run", str(job_path)], env, args.results / "run.log")
    after = safety_sample(args.results, args.serve_url)

    event_path = jobs_root / args.job_id / "events.jsonl"
    events = [json.loads(line) for line in event_path.read_text().splitlines() if line.strip()]
    records = load_records(output_dir / "output.jsonl")
    comparison = evaluate(records, labels)
    terminal = [event for event in events if event["type"] in {
        "item_complete", "item_review_required", "item_failed"}]
    report = {
        "schema_version": 1,
        "scope": ("text-only starter corpus; image/PDF and interlock acceptance remain pending"
                  if not args.job else "user-supplied labeled Jobs corpus; interactive-interlock acceptance remains pending"),
        "inputs": str(inputs.resolve()),
        "labels": str(labels_path.resolve()),
        "preview": preview,
        "run": run,
        "safety_before": before,
        "safety_after": after,
        "active_inference_seconds": active_inference_seconds(
            events, jobs_root / args.job_id / "results" / "items"),
        "terminal_units": len(terminal),
        "review_required": sum(e["type"] == "item_review_required" for e in terminal),
        "failed": sum(e["type"] == "item_failed" for e in terminal),
        "comparison": comparison,
    }
    (args.results / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.results / "report.md").write_text(
        "# E-J1 labeled Jobs run\n\n"
        f"- Preview wall time: {preview['wall_seconds']} s\n"
        f"- Run wall time: {run['wall_seconds']} s\n"
        f"- Active inference time: {report['active_inference_seconds']} s\n"
        f"- Field accuracy: {comparison['field_correct']}/{comparison['field_total']}\n"
        f"- Review required: {report['review_required']}; failed: {report['failed']}\n"
        f"- Scope: {report['scope']}\n"
    )
    print(json.dumps(report, indent=2))
    return 0 if preview["returncode"] == 0 and run["returncode"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
