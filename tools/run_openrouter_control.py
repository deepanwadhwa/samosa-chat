#!/usr/bin/env python3
"""Run a bounded upstream Qwen control through OpenRouter.

The API key is read from the environment or a local .env file. It is passed to
curl through stdin and is never written to the result directory or command
line. Results retain the model's reasoning/content so closure and correctness
can be audited, but redact request/response identifiers from the summary.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any


API_URL = "https://openrouter.ai/api/v1/chat/completions"


def load_env_value(path: Path, name: str) -> str | None:
    if not path.exists():
        return None
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip() != name:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        return value
    return None


def api_key(env_file: Path) -> str:
    value = os.environ.get("OPENROUTER_API_KEY") or load_env_value(
        env_file, "OPENROUTER_API_KEY")
    if not value:
        raise RuntimeError("OPENROUTER_API_KEY is missing")
    return value


def request_payload(config: dict[str, Any], case: dict[str, Any]) -> dict[str, Any]:
    defaults = config.get("defaults", {})
    payload: dict[str, Any] = {
        "model": config["model"],
        "messages": [{"role": "user", "content": case["prompt"]}],
        "stream": False,
        "max_tokens": case.get("max_tokens", defaults.get("max_tokens", 8192)),
        "temperature": case.get("temperature", defaults.get("temperature", 1.0)),
        "top_p": case.get("top_p", defaults.get("top_p", 0.95)),
        "top_k": case.get("top_k", defaults.get("top_k", 20)),
        "presence_penalty": case.get(
            "presence_penalty", defaults.get("presence_penalty", 1.5)),
        "seed": case["seed"],
        # Do not set reasoning.max_tokens: this arm measures natural behavior
        # within the overall completion ceiling, not a second forced budget.
        "reasoning": {"enabled": True, "exclude": False},
    }
    provider = config.get("provider")
    if provider:
        payload["provider"] = {"order": [provider], "allow_fallbacks": False}
    return payload


def curl_post(payload: dict[str, Any], key: str, timeout: int) -> dict[str, Any]:
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False,
                                     encoding="utf-8") as request_file:
        json.dump(payload, request_file)
        request_path = Path(request_file.name)
    # curl configuration arrives on stdin so the bearer token is not visible
    # in argv/process listings. The request file contains no secret.
    escaped_path = str(request_path).replace('"', '\\"')
    escaped_key = key.replace('\\', '\\\\').replace('"', '\\"')
    curl_config = (
        f'url = "{API_URL}"\n'
        'silent\nshow-error\nfail-with-body\n'
        'header = "Content-Type: application/json"\n'
        f'header = "Authorization: Bearer {escaped_key}"\n'
        f'data-binary = "@{escaped_path}"\n'
        f'max-time = {timeout}\n'
    )
    try:
        completed = subprocess.run(
            ["curl", "--config", "-"], input=curl_config, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    finally:
        request_path.unlink(missing_ok=True)
    try:
        response = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        detail = completed.stderr.strip() or completed.stdout[:500]
        raise RuntimeError(f"OpenRouter returned non-JSON output: {detail}") from error
    if completed.returncode != 0 or "error" in response:
        error = response.get("error", response)
        raise RuntimeError(f"OpenRouter request failed: {error}")
    return response


def reasoning_text(message: dict[str, Any]) -> str:
    for key in ("reasoning", "reasoning_content"):
        value = message.get(key)
        if isinstance(value, str):
            return value
    blocks = message.get("reasoning_details") or []
    return "\n".join(
        str(block.get("text", "")) for block in blocks
        if isinstance(block, dict) and block.get("type") == "reasoning.text"
    )


def evaluate_output(case: dict[str, Any], reasoning: str, content: str,
                    finish_reason: str | None) -> dict[str, bool]:
    markers = [str(item) for item in case.get("require", [])]
    patterns = [str(item) for item in case.get("require_regex", [])]
    lowered = content.casefold()
    return {
        "natural_model_stop": finish_reason == "stop",
        "final_answer_nonempty": bool(content.strip()),
        "required_substrings_present": all(
            marker.casefold() in lowered for marker in markers),
        "required_patterns_present": all(
            re.search(pattern, content, flags=re.IGNORECASE | re.DOTALL)
            for pattern in patterns),
        "reasoning_returned": bool(reasoning.strip()),
    }


def normalize_response(case: dict[str, Any], response: dict[str, Any],
                       elapsed_s: float) -> dict[str, Any]:
    choice = response["choices"][0]
    message = choice.get("message", {})
    reasoning = reasoning_text(message)
    content = message.get("content") or ""
    usage = response.get("usage", {})
    details = usage.get("completion_tokens_details") or {}
    checks = evaluate_output(case, reasoning, content, choice.get("finish_reason"))
    return {
        "schema": 1,
        "case_id": case["id"],
        "seed": case["seed"],
        "model": response.get("model"),
        "provider": response.get("provider"),
        "finish_reason": choice.get("finish_reason"),
        "native_finish_reason": choice.get("native_finish_reason"),
        "reasoning": reasoning,
        "content": content,
        "usage": {
            "prompt_tokens": usage.get("prompt_tokens"),
            "completion_tokens": usage.get("completion_tokens"),
            "reasoning_tokens": details.get("reasoning_tokens"),
            "cost": usage.get("cost"),
        },
        "elapsed_s": elapsed_s,
        "checks": checks,
        "passed": all(checks.values()),
    }


def percentile_nearest_rank(values: list[int], percentile: float) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, int((percentile * len(ordered) + 0.999999)))
    return ordered[min(rank, len(ordered)) - 1]


def summarize(results: list[dict[str, Any]], config: dict[str, Any]) -> dict[str, Any]:
    reasoning_tokens = [
        int(result["usage"]["reasoning_tokens"])
        for result in results if result["usage"]["reasoning_tokens"] is not None
    ]
    return {
        "schema": 1,
        "control": "OpenRouter upstream-compatible provider control",
        "requested_model": config["model"],
        "requested_provider": config.get("provider"),
        "declared_provider_quantization": config.get("provider_quantization"),
        "not_bf16": config.get("provider_quantization") != "bf16",
        "runs": len(results),
        "natural_stop_rate": (
            sum(result["checks"]["natural_model_stop"] for result in results)
            / len(results) if results else None),
        "correct_final_rate": (
            sum(result["checks"]["required_substrings_present"] and
                result["checks"].get("required_patterns_present", True)
                for result in results)
            / len(results) if results else None),
        "reasoning_tokens": reasoning_tokens,
        "reasoning_tokens_min": min(reasoning_tokens) if reasoning_tokens else None,
        "reasoning_tokens_max": max(reasoning_tokens) if reasoning_tokens else None,
        "reasoning_tokens_p50_nearest_rank": percentile_nearest_rank(
            reasoning_tokens, 0.50),
        "reasoning_tokens_p90_nearest_rank": percentile_nearest_rank(
            reasoning_tokens, 0.90),
        "p90_is_pilot_only": len(reasoning_tokens) < 30,
        "total_cost": sum(
            float(result["usage"]["cost"] or 0) for result in results),
        "results": [
            {"case_id": result["case_id"], "seed": result["seed"],
             "path": f'{result["case_id"]}.json', "passed": result["passed"]}
            for result in results
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cases", type=Path)
    parser.add_argument("--env-file", type=Path, default=Path(".env"))
    parser.add_argument("--results", type=Path,
                        default=Path("docs/regressions/openrouter-control"))
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--limit", type=int, default=0,
                        help="Run only the first N cases; zero runs all cases")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--rescore", action="store_true",
                        help="re-evaluate existing result JSON without API calls")
    args = parser.parse_args()

    config = json.loads(args.cases.read_text(encoding="utf-8"))
    cases = config["cases"][:args.limit or None]
    args.results.mkdir(parents=True, exist_ok=True)
    if args.dry_run:
        print(json.dumps([request_payload(config, case) for case in cases],
                         indent=2, sort_keys=True))
        return 0

    if args.rescore:
        results = []
        for case in cases:
            path = args.results / f'{case["id"]}.json'
            result = json.loads(path.read_text(encoding="utf-8"))
            result["checks"] = evaluate_output(
                case, result.get("reasoning", ""), result.get("content", ""),
                result.get("finish_reason"))
            result["passed"] = all(result["checks"].values())
            path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
            results.append(result)
        summary = summarize(results, config)
        (args.results / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0

    key = api_key(args.env_file)
    results: list[dict[str, Any]] = []
    for case in cases:
        payload = request_payload(config, case)
        started = time.monotonic()
        response = curl_post(payload, key, args.timeout)
        result = normalize_response(case, response, time.monotonic() - started)
        path = args.results / f'{case["id"]}.json'
        path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
        results.append(result)
        print(json.dumps({"case_id": result["case_id"],
                          "seed": result["seed"],
                          "reasoning_tokens": result["usage"]["reasoning_tokens"],
                          "finish_reason": result["finish_reason"],
                          "passed": result["passed"]}, sort_keys=True), flush=True)
    summary = summarize(results, config)
    (args.results / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if results else 1


if __name__ == "__main__":
    raise SystemExit(main())
