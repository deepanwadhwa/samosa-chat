#!/usr/bin/env python3
"""Run bounded Samosa regressions with structural and machine-safety gates."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_thinking_output import evaluate as evaluate_thinking  # noqa: E402


STATS_RE = re.compile(
    r"\[stats\] prompt=(?P<prompt>\d+) generated=(?P<generated>\d+) "
    r"stop=(?P<stop>\S+) thinking=(?P<thinking>\S+) "
    r"prefill=(?P<prefill_s>[0-9.]+)s \((?P<prefill_tps>[0-9.]+) tok/s\) "
    r"decode=(?P<decode_s>[0-9.]+)s \((?P<decode_tps>[0-9.]+) tok/s\) "
    r"total=(?P<total_s>[0-9.]+)s .*peak_rss=(?P<peak_rss_gb>[0-9.]+) GB"
)
ECACHE_RE = re.compile(
    r"\[ecache\].*bytes_read=(?P<bytes_read_gb>[0-9.]+) GB "
    r"bytes_avoided=(?P<bytes_avoided_gb>[0-9.]+) GB .*"
    r"pressure_warn=(?P<pressure_warn>\d+) pressure_critical=(?P<pressure_critical>\d+)"
)


def command_output(*command: str) -> str:
    try:
        return subprocess.run(command, check=False, text=True,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT).stdout.strip()
    except OSError as error:
        return f"unavailable: {error}"


def vm_stat() -> dict[str, int]:
    text = command_output("vm_stat")
    result: dict[str, int] = {}
    for key in ("Pages throttled", "Swapins", "Swapouts"):
        match = re.search(rf"^{key}:\s+(\d+)\.", text, re.MULTILINE)
        if match:
            result[key.lower().replace(" ", "_")] = int(match.group(1))
    return result


def memory_free_percent() -> int | None:
    text = command_output("memory_pressure", "-Q")
    match = re.search(r"System-wide memory free percentage:\s*(\d+)%", text)
    return int(match.group(1)) if match else None


def safety_sample(path: Path) -> dict[str, object]:
    usage = shutil.disk_usage(path)
    return {
        "time": time.time(),
        "vm": vm_stat(),
        "memory_free_percent": memory_free_percent(),
        "thermal": command_output("pmset", "-g", "therm"),
        "disk_free_gb": usage.free / 1e9,
    }


def thermal_is_safe(text: str) -> bool:
    lowered = text.lower()
    return ("no thermal warning level has been recorded" in lowered and
            "no performance warning level has been recorded" in lowered)


def parse_engine_metrics(stderr: str) -> dict[str, object]:
    metrics: dict[str, object] = {}
    stats = STATS_RE.search(stderr)
    if stats:
        for key, value in stats.groupdict().items():
            metrics[key] = value if key in {"stop", "thinking"} else float(value)
        metrics["prompt"] = int(float(metrics["prompt"]))
        metrics["generated"] = int(float(metrics["generated"]))
    cache = ECACHE_RE.search(stderr)
    if cache:
        for key, value in cache.groupdict().items():
            metrics[key] = (int(value) if key.startswith("pressure_")
                            else float(value))
    return metrics


def response_text(stdout: str) -> str:
    return stdout.split("--- risposta ---", 1)[1].strip() \
        if "--- risposta ---" in stdout else stdout.strip()


def evaluate_case(case: dict[str, object], stdout: str,
                  stderr: str) -> dict[str, object]:
    profile = str(case["profile"])
    required = tuple(str(item) for item in case.get("require", []))
    required_patterns = tuple(str(item) for item in case.get("require_regex", []))
    metrics = parse_engine_metrics(stderr)
    response = response_text(stdout)
    if profile == "direct":
        final = response
        checks: dict[str, object] = {
            "final_answer_nonempty": bool(final),
            "required_substrings_present": all(item in final for item in required),
            "model_stopped": metrics.get("stop") == "model",
            "no_pressure_events": (metrics.get("pressure_warn", 0) == 0 and
                                   metrics.get("pressure_critical", 0) == 0),
        }
        structural = {"passed": all(checks.values()), "checks": checks}
    else:
        structural = evaluate_thinking(
            response, float(case.get("repetition_limit", 0.45)), required,
            required_patterns)
        structural["checks"]["natural_closure"] = (
            metrics.get("thinking") == "model-controlled")
        structural["checks"]["model_stopped"] = metrics.get("stop") == "model"
        structural["checks"]["no_pressure_events"] = (
            metrics.get("pressure_warn", 0) == 0 and
            metrics.get("pressure_critical", 0) == 0)
        structural["passed"] = bool(structural["passed"] and
                                    structural["checks"]["natural_closure"] and
                                    structural["checks"]["model_stopped"] and
                                    structural["checks"]["no_pressure_events"])
    return {"passed": structural["passed"], "checks": structural["checks"],
            "metrics": metrics}


def engine_command(case: dict[str, object], engine: Path,
                   tokenizer: Path) -> list[str]:
    command = [str(engine.resolve()), "--chat", str(case["prompt"]), "--tokens",
               str(case["max_tokens"]), "--tokenizer", str(tokenizer.resolve()),
               "--seed", str(case["seed"]), "--stream"]
    profile = str(case["profile"])
    if profile == "direct":
        command.append("--no-thinking")
    elif profile == "think-code":
        command.append("--thinking-code")
    elif profile != "think":
        raise ValueError(f"unsupported profile: {profile}")
    if profile != "direct":
        command += ["--thinking-budget", str(case["thinking_budget"])]
    return command


def safety_violation(before: dict[str, object], current: dict[str, object],
                     min_free_gb: float, min_memory_percent: int,
                     max_swapout_mb: float) -> str | None:
    if float(current["disk_free_gb"]) < min_free_gb:
        return f"disk free below {min_free_gb} GB"
    memory = current["memory_free_percent"]
    if memory is not None and int(memory) < min_memory_percent:
        return f"memory free below {min_memory_percent}%"
    if not thermal_is_safe(str(current["thermal"])):
        return "macOS thermal/performance warning"
    old_swap = int(before.get("vm", {}).get("swapouts", 0))
    new_swap = int(current.get("vm", {}).get("swapouts", old_swap))
    swap_mb = max(0, new_swap - old_swap) * 16384 / 1e6
    if swap_mb > max_swapout_mb:
        return f"swapouts grew by {swap_mb:.1f} MB"
    if int(current.get("vm", {}).get("pages_throttled", 0)) > 0:
        return "throttled pages reported"
    return None


def run_case(case: dict[str, object], args: argparse.Namespace) -> dict[str, object]:
    result_dir = args.results / str(case["id"])
    result_dir.mkdir(parents=True, exist_ok=True)
    stdout_path, stderr_path = result_dir / "stdout.txt", result_dir / "stderr.txt"
    before = safety_sample(args.results)
    command = engine_command(case, args.engine, args.tokenizer)
    env = os.environ.copy()
    env.update({"SNAP": str(args.model.resolve()), "OMP_NUM_THREADS": str(args.threads),
                "VECLIB_MAXIMUM_THREADS": str(args.threads)})
    violation = None
    with stdout_path.open("w", encoding="utf-8") as stdout_file, \
            stderr_path.open("w", encoding="utf-8") as stderr_file:
        process = subprocess.Popen(command, stdout=stdout_file, stderr=stderr_file,
                                   text=True, env=env)
        while process.poll() is None:
            time.sleep(args.poll_seconds)
            current = safety_sample(args.results)
            violation = safety_violation(before, current, args.min_free_gb,
                                         args.min_memory_percent,
                                         args.max_swapout_mb)
            if violation:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                break
    after = safety_sample(args.results)
    stdout = stdout_path.read_text(errors="replace")
    stderr = stderr_path.read_text(errors="replace")
    evaluation = evaluate_case(case, stdout, stderr)
    if violation:
        evaluation["passed"] = False
        evaluation["checks"]["machine_safety"] = False
    else:
        evaluation["checks"]["machine_safety"] = True
    result = {
        "schema": 1,
        "case": case,
        "command": command,
        "returncode": process.returncode,
        "safety_violation": violation,
        "safety_before": before,
        "safety_after": after,
        **evaluation,
    }
    result["passed"] = bool(result["passed"] and process.returncode == 0)
    (result_dir / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cases", type=Path)
    parser.add_argument("--engine", type=Path, default=Path("./qwen36b"))
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, required=True)
    parser.add_argument("--results", type=Path, default=Path("regression-results"))
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    parser.add_argument("--min-free-gb", type=float, default=15.0)
    parser.add_argument("--min-memory-percent", type=int, default=25)
    parser.add_argument("--max-swapout-mb", type=float, default=64.0)
    parser.add_argument("--no-fail-fast", action="store_true")
    args = parser.parse_args()
    payload = json.loads(args.cases.read_text())
    args.results.mkdir(parents=True, exist_ok=True)
    summary = []
    for case in payload["cases"]:
        result = run_case(case, args)
        summary.append({"id": case["id"], "passed": result["passed"],
                        "result": str(args.results / str(case["id"]) / "result.json")})
        print(json.dumps(summary[-1], sort_keys=True), flush=True)
        if not result["passed"] and not args.no_fail_fast:
            break
    (args.results / "summary.json").write_text(
        json.dumps({"schema": 1, "runs": summary}, indent=2, sort_keys=True) + "\n")
    return 0 if summary and all(item["passed"] for item in summary) else 1


if __name__ == "__main__":
    raise SystemExit(main())
