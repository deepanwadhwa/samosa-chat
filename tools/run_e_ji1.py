#!/usr/bin/env python3
"""Run the E-JI1/E-JI2 real-model find gates in an isolated local sandbox."""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOME = Path.home() / ".samosa"
PORT, BACKEND_PORT = 8862, 8863


def request(url: str, body: dict | None = None) -> str:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=7200) as response:
        return response.read().decode()


def wait_ready() -> None:
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        try:
            health = json.loads(request(f"http://127.0.0.1:{PORT}/healthz"))
            if health.get("ready"):
                return
        except (OSError, urllib.error.URLError, json.JSONDecodeError):
            pass
        time.sleep(1)
    raise RuntimeError("isolated Ornith gateway did not become ready within 10 minutes")


def write_fixtures(folder: Path) -> None:
    logo = ROOT / "assets" / "samosa-chat.png"
    target_scan = ROOT / "tools" / "testdata" / "ocr" / "tiny.png"
    (folder / "policar_research_2019.txt").write_text(
        "Research summary. Author: Policar. Year 2019. Local research notes.\n"
    )
    (folder / "titli_rabies_certificate.txt").write_text(
        "VETERINARY RABIES VACCINATION CERTIFICATE\n\n"
        "Patient: Titli\nSpecies: Feline\nOwner: Example Family\n"
        "Vaccine: Rabies, killed virus\nManufacturer: Example Veterinary\n"
        "Vaccination date: 2026-05-14\nExpires: 2027-05-14\n"
        "This certificate confirms Titli received the rabies vaccination listed above.\n"
        "Veterinarian signature: Dr. Example, DVM\n"
    )
    shutil.copyfile(target_scan, folder / "CamScanner_03-15-2024.png")
    # Unique trailing bytes avoid content-hash deduplication while keeping the
    # PNG payload itself intact for the reader. These are deliberately dull
    # image names/content and provide the scanned/image clutter mix.
    for prefix, count in (("CamScanner_junk", 25), ("IMG_junk", 15)):
        for i in range(count):
            target = folder / f"{prefix}_{i:03d}.png"
            shutil.copyfile(logo, target)
            with target.open("ab") as out:
                out.write(f"\nE-JI1 unique fixture {prefix} {i}\n".encode())
    for i in range(7):
        (folder / f"notes_{i:03d}.txt").write_text(
            f"Unrelated household note {i}: groceries, meetings, and travel plans.\n"
        )


def run_job(folder: Path, goal: str) -> tuple[dict, list[dict], float]:
    started = time.monotonic()
    data = json.dumps({"goal": goal, "folder": str(folder)}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/jobs/run", data=data,
                                 headers={"Content-Type": "application/json"})
    chunks, events = [], []
    with urllib.request.urlopen(req, timeout=7200) as response:
        for raw_line in response:
            line = raw_line.decode()
            chunks.append(line)
            if not line.startswith("data: ") or line == "data: [DONE]\n":
                continue
            try:
                event = json.loads(line[6:])
                event["_observed_seconds"] = time.monotonic() - started
                events.append(event)
            except json.JSONDecodeError:
                pass
    raw = "".join(chunks)
    elapsed = time.monotonic() - started
    job_id = next((e.get("job_id") for e in events if e.get("job_id")), None)
    return {"job_id": job_id, "raw": raw}, events, elapsed


def parse_events(raw: str) -> list[dict]:
    events = []
    for line in raw.splitlines():
        if line.startswith("data: ") and line != "data: [DONE]":
            try:
                events.append(json.loads(line[6:]))
            except json.JSONDecodeError:
                pass
    return events


def run_to_terminal(folder: Path, goal: str) -> tuple[dict, list[dict], float]:
    """Run through every mechanical checkpoint, including verify checkpoints."""
    job, events, elapsed = run_job(folder, goal)
    raw_parts = [job["raw"]]
    latest = events
    while any(e.get("type") == "await_continue" for e in latest) and not any(e.get("type") == "result" for e in events):
        started = time.monotonic()
        raw = request(f"http://127.0.0.1:{PORT}/v1/jobs/continue", {"job_id": job["job_id"]})
        elapsed += time.monotonic() - started
        raw_parts.append(raw)
        latest = parse_events(raw)
        for event in latest:
            event["_observed_seconds"] = elapsed
        events.extend(latest)
    job["raw"] = "\n".join(raw_parts)
    return job, events, elapsed


def summarize(events: list[dict], elapsed: float) -> dict:
    result = next((e for e in events if e.get("type") == "result"), {})
    last = lambda kind: next((e.get("_observed_seconds", elapsed) for e in reversed(events)
                              if e.get("type") == kind), 0.0)
    triage_end, skim_end, classify_end = last("triage_progress"), last("skim_progress"), last("classify_progress")
    return {
        "wall_seconds": round(elapsed, 3),
        "observed_phase_boundary_seconds": {
            "triage_through": round(triage_end, 3),
            "skim_increment": round(max(0.0, skim_end - triage_end), 3),
            "classify_increment": round(max(0.0, classify_end - skim_end), 3),
            "verify_increment": round(max(0.0, last("result") - classify_end), 3),
        },
        "event_counts": {kind: sum(e.get("type") == kind for e in events)
                         for kind in ("triage_progress", "skim_progress", "classify_progress", "tool_call")},
        "matches": result.get("matches", []),
        "unreadable": result.get("unreadable", []),
        "errors": [e for e in events if e.get("type") == "error"],
        "checkpoint": next((e for e in events if e.get("type") == "await_continue"), None),
    }


class ResourceSampler:
    """Best-effort, read-only local resource observations for the real gate."""
    def __init__(self, root_pid: int) -> None:
        self.root_pid = root_pid
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.peak_rss_kib = 0
        self.max_swap_used = "unavailable"
        self.thermal_note = "not sampled: macOS thermal telemetry requires privileged powermetrics"

    def start(self) -> None:
        self.thread.start()

    def finish(self) -> dict:
        self.stop_event.set()
        self.thread.join(timeout=3)
        return {"peak_gateway_plus_backend_rss_kib": self.peak_rss_kib,
                "max_observed_swap_used": self.max_swap_used,
                "thermal": self.thermal_note}

    def _loop(self) -> None:
        while not self.stop_event.is_set():
            pids = [self.root_pid]
            try:
                children = subprocess.run(["pgrep", "-P", str(self.root_pid)], text=True,
                                          capture_output=True, check=False).stdout.split()
                pids.extend(int(pid) for pid in children if pid.isdigit())
                rss = subprocess.run(["ps", "-o", "rss=", "-p", ",".join(map(str, pids))],
                                     text=True, capture_output=True, check=False).stdout.split()
                self.peak_rss_kib = max(self.peak_rss_kib, sum(int(value) for value in rss if value.isdigit()))
                swap = subprocess.run(["sysctl", "-n", "vm.swapusage"], text=True,
                                      capture_output=True, check=False).stdout.strip()
                if swap:
                    self.max_swap_used = swap
            except OSError:
                pass
            self.stop_event.wait(1)


def line_count(path: Path) -> int:
    return len(path.read_text().splitlines()) if path.exists() else 0


def probe_structured_batch() -> dict:
    """Verify the real server honors the exact Phase A/C JSON contract."""
    files = "\n".join(
        f"{i}. policar_research_{i:02d}.txt (text/plain, 10 bytes, 2019-01-01)"
        for i in range(1, 17)
    )
    raw = request(f"http://127.0.0.1:{BACKEND_PORT}/v1/chat/completions", {
        "model": "ornith-1.0-9b", "stream": False, "thinking": "off",
        "chat_template_kwargs": {"enable_thinking": False},
        "response_format": {"type": "json_object"},
        "messages": [
            {"role": "system", "content": "Return exactly {\"items\":[{\"i\":1,\"c\":\"h\"}]}. Include one item for every numbered file. c is h, m, or l. No prose, reasons, paths, markdown, or explanation."},
            {"role": "user", "content": f"Goal: find Policar research\\nFiles:\\n{files}"},
        ],
    })
    content = json.loads(raw)["choices"][0]["message"].get("content")
    if isinstance(content, str) and content.lstrip().startswith("```"):
        content = content.lstrip().split("\n", 1)[1].rsplit("```", 1)[0].strip()
    try:
        value = json.loads(content)
    except (TypeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"real Ornith returned non-JSON structured content: {content!r}; raw={raw!r}") from exc
    if not isinstance(value, dict) or not isinstance(value.get("items"), list) or len(value["items"]) != 16:
        raise RuntimeError(f"real Ornith did not honor structured batch contract: {content!r}")
    return {"content": content, "items": value["items"]}


def main() -> int:
    scratch = Path(tempfile.mkdtemp(prefix="samosa-eji1-"))
    report_dir = ROOT / "docs" / "regressions" / "jobs" / "e-ji1-2026-07-23"
    report_dir.mkdir(parents=True, exist_ok=True)
    folder = scratch / "fixtures"
    folder.mkdir()
    write_fixtures(folder)
    isolated_home = scratch / "home"
    ocr_count_log = scratch / "ocr-invocations.log"
    ocr_wrapper = scratch / "count-ocr.sh"
    ocr_wrapper.write_text(
        "#!/bin/sh\nprintf '1\\n' >> \"$SAMOSA_OCR_COUNT_LOG\"\nexec \"$SAMOSA_REAL_OCR\" \"$@\"\n"
    )
    ocr_wrapper.chmod(0o700)
    env = os.environ | {
        "SAMOSA_HOME": str(isolated_home), "SAMOSA_PORT": str(PORT),
        "SAMOSA_BACKEND_PORT": str(BACKEND_PORT), "SAMOSA_APP_HTML": str(ROOT / "assets" / "app.html"),
        "SAMOSA_APP_LOGO": str(ROOT / "assets" / "samosa-chat.png"),
        "SAMOSA_BONSAI_SERVER": str(HOME / "backends/prism-llama.cpp/build/bin/llama-server"),
        "SAMOSA_ORNITH_MODEL": str(HOME / "models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"),
        "SAMOSA_BONSAI_MODEL": str(HOME / "models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"),
        "SAMOSA_BONSAI_MMPROJ": str(HOME / "models/bonsai-27b-1bit/Bonsai-27B-mmproj-Q8_0.gguf"),
        "SAMOSA_FS": str(ROOT / "build/samosa-fs"), "SAMOSA_EXTRACT": str(ROOT / "build/samosa-extract"),
        "SAMOSA_OCR": str(ocr_wrapper), "SAMOSA_REAL_OCR": str(ROOT / "build/samosa-ocr"),
        "SAMOSA_OCR_COUNT_LOG": str(ocr_count_log), "SAMOSA_OCR_PACK": str(HOME / "models/ocr-pack-v1"),
        "SAMOSA_READ_CACHE_DIR": str(isolated_home / "cache/read"),
    }
    (isolated_home / "model-backend").parent.mkdir(parents=True, exist_ok=True)
    (isolated_home / "model-backend").write_text("ornith\n")
    log = (scratch / "gateway.log").open("w")
    gateway = subprocess.Popen([str(ROOT / "build/samosa-gateway")], env=env, stdout=log, stderr=subprocess.STDOUT)
    resources = ResourceSampler(gateway.pid)
    try:
        wait_ready()
        resources.start()
        probe = probe_structured_batch()
        if "--probe" in sys.argv:
            print(json.dumps(probe, indent=2))
            return 0
        goal1 = "Find all files related to the Poličar 2019 research."
        cold, cold_events, cold_wall = run_to_terminal(folder, goal1)
        cold_ocr = line_count(ocr_count_log)
        warm, warm_events, warm_wall = run_to_terminal(folder, "Find Titli's rabies certificate specifically.")
        warm_ocr = line_count(ocr_count_log) - cold_ocr
        cold_matches = {m.get("path") for m in summarize(cold_events, cold_wall)["matches"]}
        warm_matches = {m.get("path") for m in summarize(warm_events, warm_wall)["matches"]}
        required_cold = {"policar_research_2019.txt", "CamScanner_03-15-2024.png"}
        if warm_ocr != 0:
            failure = f"warm cache miss: {warm_ocr} OCR invocations"
        elif not required_cold <= cold_matches or "titli_rabies_certificate.txt" not in warm_matches:
            failure = f"E-JI gate did not find planted targets: cold={cold_matches}, warm={warm_matches}"
        else:
            failure = None
        payload = {"fixture_files": len(list(folder.iterdir())), "cold": summarize(cold_events, cold_wall),
                   "warm": summarize(warm_events, warm_wall), "cold_ocr_invocations": cold_ocr,
                   "warm_ocr_invocations": warm_ocr, "cold_job": cold["job_id"], "warm_job": warm["job_id"],
                   "resources": resources.finish()}
        (report_dir / "result.json").write_text(json.dumps(payload, indent=2) + "\n")
        (report_dir / "cold.sse").write_text(cold["raw"])
        (report_dir / "warm.sse").write_text(warm["raw"])
        if failure:
            raise RuntimeError(failure)
        print(json.dumps(payload, indent=2))
        return 0
    finally:
        if resources.thread.is_alive():
            resources.finish()
        gateway.send_signal(signal.SIGTERM)
        try:
            gateway.wait(timeout=15)
        except subprocess.TimeoutExpired:
            gateway.kill()
        log.close()
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
