import os
import sys
import json
import subprocess
import tempfile
import time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading

# Thread-safe printing and progress tracking
print_lock = threading.Lock()
progress_lock = threading.Lock()
completed_runs = 0

def log(msg):
    with print_lock:
        print(msg, flush=True)

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

def get_key():
    env_file = Path("/Users/deepanwadhwa/Documents/samosa-chat/.env")
    value = os.environ.get("OPENROUTER_API_KEY") or load_env_value(env_file, "OPENROUTER_API_KEY")
    if not value:
        raise RuntimeError("OPENROUTER_API_KEY is missing")
    return value

def curl_post(payload: dict, key: str, timeout: int = 60) -> dict:
    API_URL = "https://openrouter.ai/api/v1/chat/completions"
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump(payload, f)
        req_path = f.name
    
    curl_config = (
        f'url = "{API_URL}"\n'
        'silent\nshow-error\nfail-with-body\n'
        'header = "Content-Type: application/json"\n'
        f'header = "Authorization: Bearer {key}"\n'
        f'data-binary = "@{req_path}"\n'
        f'max-time = {timeout}\n'
    )
    
    try:
        completed = subprocess.run(
            ["curl", "--config", "-"], input=curl_config, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
        )
    finally:
        os.unlink(req_path)
        
    try:
        response = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        detail = completed.stderr.strip() or completed.stdout[:500]
        raise RuntimeError(f"OpenRouter returned non-JSON output: {detail}") from error
        
    if completed.returncode != 0 or "error" in response:
        error = response.get("error", response)
        raise RuntimeError(f"OpenRouter request failed: {error}")
        
    return response

def evaluate_response(case: dict, response: dict) -> dict:
    choice = response["choices"][0]
    msg = choice.get("message", {})
    tool_calls = msg.get("tool_calls")
    content = msg.get("content") or ""
    
    malformed = False
    wrong_tool = False
    hallucinated_args = False
    tool_called_name = None
    args_parsed = None
    
    # 1. Check if tool calls exist
    if not tool_calls:
        if "<tool_call>" in content or "<function=" in content:
            malformed = True
        else:
            malformed = True
    else:
        t_call = tool_calls[0]
        func = t_call.get("function", {})
        tool_called_name = func.get("name")
        args_str = func.get("arguments") or ""
        
        try:
            args_parsed = json.loads(args_str)
        except json.JSONDecodeError:
            malformed = True
            
        if tool_called_name != case["expected_tool"]:
            wrong_tool = True
            
        if args_parsed is not None:
            for req in case["required_args"]:
                if req not in args_parsed:
                    hallucinated_args = True
                elif not str(args_parsed[req]).strip():
                    hallucinated_args = True
                    
    passed = (not malformed) and (not wrong_tool) and (not hallucinated_args)
    return {
        "passed": passed,
        "malformed": malformed,
        "wrong_tool": wrong_tool,
        "hallucinated_args": hallucinated_args,
        "tool_called": tool_called_name,
        "arguments": args_parsed
    }

def run_single_task(task_args) -> dict:
    global completed_runs
    case, seed, config, key = task_args
    
    payload = {
        "model": config["model"],
        "provider": {"order": [config["provider"]], "allow_fallbacks": False},
        "messages": [{"role": "user", "content": case["prompt"]}],
        "tools": config["tools"],
        "stream": False,
        "temperature": config["defaults"]["temperature"],
        "top_p": config["defaults"]["top_p"],
        "top_k": config["defaults"]["top_k"],
        "presence_penalty": config["defaults"]["presence_penalty"],
        "seed": seed,
        "reasoning": {"enabled": True, "exclude": False}
    }
    
    try:
        started = time.monotonic()
        resp = curl_post(payload, key)
        elapsed = time.monotonic() - started
        
        eval_res = evaluate_response(case, resp)
        result = {
            "case_id": case["id"],
            "seed": seed,
            "prompt": case["prompt"],
            "expected_tool": case["expected_tool"],
            "elapsed_s": elapsed,
            "eval": eval_res,
            "usage": resp.get("usage", {})
        }
    except Exception as e:
        result = {
            "case_id": case["id"],
            "seed": seed,
            "prompt": case["prompt"],
            "expected_tool": case["expected_tool"],
            "error": str(e),
            "eval": {"passed": False, "malformed": True, "wrong_tool": False, "hallucinated_args": False, "tool_called": None, "arguments": None}
        }
        
    with progress_lock:
        completed_runs += 1
        current_progress = completed_runs
        
    log(f"[{current_progress}/60] Finished case '{case['id']}' seed {seed}. Result: passed={result['eval']['passed']}, malformed={result['eval']['malformed']}")
    return result

def main():
    cases_file = Path("/Users/deepanwadhwa/Documents/samosa-chat/tests/tool_call_cases.json")
    out_dir = Path("/Users/deepanwadhwa/Documents/samosa-chat/docs/regressions/tool-call-validation")
    out_dir.mkdir(parents=True, exist_ok=True)
    
    key = get_key()
    config = json.loads(cases_file.read_text(encoding="utf-8"))
    
    seeds = [11, 29, 47]
    tasks = []
    for case in config["cases"]:
        for seed in seeds:
            tasks.append((case, seed, config, key))
            
    log(f"Starting E-I1 Parallel Tool-call JSON Reliability Evaluation (8 workers).")
    log(f"Total tasks: {len(tasks)}.")
    
    results = []
    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = {executor.submit(run_single_task, t): t for t in tasks}
        for future in as_completed(futures):
            results.append(future.result())
            
    # Sort results by case_id then seed for deterministic output
    results.sort(key=lambda x: (x["case_id"], x["seed"]))
    
    # Write summary
    total = len(results)
    passed_cnt = sum(1 for r in results if r["eval"]["passed"])
    malformed_cnt = sum(1 for r in results if r["eval"]["malformed"])
    wrong_tool_cnt = sum(1 for r in results if r["eval"]["wrong_tool"])
    hallucinated_args_cnt = sum(1 for r in results if r["eval"]["hallucinated_args"])
    
    malformed_rate = malformed_cnt / total
    wrong_tool_rate = wrong_tool_cnt / total
    hallucinated_args_rate = hallucinated_args_cnt / total
    success_rate = passed_cnt / total
    
    log("\n--- E-I1 Final Results ---")
    log(f"Total Runs: {total}")
    log(f"Success Rate: {success_rate:.2%}")
    log(f"Malformed-JSON Rate: {malformed_rate:.2%}")
    log(f"Wrong-Tool Rate: {wrong_tool_rate:.2%}")
    log(f"Hallucinated-Arguments Rate: {hallucinated_args_rate:.2%}")
    
    # Save details JSON
    with open(out_dir / "results.json", "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
        
    # Write markdown report
    report_path = out_dir / "report.md"
    with open(report_path, "w", encoding="utf-8") as f:
        f.write("# E-I1: Tool-Call JSON Reliability Evaluation Report\n\n")
        f.write(f"Analyzed on: 2026-07-15\n")
        f.write(f"Model evaluated: `{config['model']}` (Upstream FP8 on `{config['provider']}`)\n\n")
        
        f.write("## Summary Statistics\n\n")
        f.write(f"| Metric | Count | Rate |\n")
        f.write(f"|---|---|---|\n")
        f.write(f"| **Total Runs** | {total} | 100.0% |\n")
        f.write(f"| **Successful Tool Calls** | {passed_cnt} | {success_rate:.2%} |\n")
        f.write(f"| **Malformed Tool Calls / JSON** | {malformed_cnt} | {malformed_rate:.2%} |\n")
        f.write(f"| **Wrong Tool Selected** | {wrong_tool_cnt} | {wrong_tool_rate:.2%} |\n")
        f.write(f"| **Hallucinated / Missing Arguments** | {hallucinated_args_cnt} | {hallucinated_args_rate:.2%} |\n\n")
        
        # Check 20% limit gate
        f.write("## Verdict\n\n")
        if malformed_rate > 0.20:
            f.write("> [!WARNING]\n")
            f.write(f"> **FAIL/NO-GO**: The malformed-JSON rate ({malformed_rate:.2%}) exceeds the 20% limit threshold. Model-initiated tool calls (A3.3) are disqualified from integration. We should proceed with user-initiated URL ingestion (A3.1) and web search (A3.2) only.\n")
        else:
            f.write("> [!TIP]\n")
            f.write(f"> **PASS**: The malformed-JSON rate ({malformed_rate:.2%}) is within the 20% threshold. We can proceed to design and scope the C engine support for model-initiated tool calling (A3.3).\n")
            
        f.write("\n## Run Details\n\n")
        f.write("| Case ID | Seed | Expected Tool | Tool Called | Passed | Malformed | Wrong Tool | Hallucinated Args |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for r in results:
            f.write(f"| `{r['case_id']}` | {r['seed']} | `{r['expected_tool']}` | `{r['eval']['tool_called']}` | {r['eval']['passed']} | {r['eval']['malformed']} | {r['eval']['wrong_tool']} | {r['eval']['hallucinated_args']} |\n")
            
    log(f"Report written to {report_path}")

if __name__ == "__main__":
    main()
