#!/usr/bin/env python3
"""Opt-in real-model acceptance using ONLY a newly generated 40-page PDF."""
import base64
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_mixed_document_fixture import generate

home = Path(os.environ.get("SAMOSA_HOME", str(Path.home() / ".samosa")))
base = os.environ.get("SAMOSA_TEST_URL", "http://127.0.0.1:8642")
token = (home / "run/ui-token").read_text().strip()


def request(path, data=None, headers=None, stream=False):
    headers = {"X-Samosa-Token": token, **(headers or {})}
    if isinstance(data, dict):
        headers["Content-Type"] = "application/json"
        data = json.dumps(data).encode()
    response = urllib.request.urlopen(urllib.request.Request(base + path, data=data, headers=headers), timeout=180)
    if not stream:
        with response:
            return json.load(response)
    answer = ""
    with response:
        for line in response:
            if not line.startswith(b"data: ") or line.strip() == b"data: [DONE]":
                continue
            event = json.loads(line[6:])
            assert "error" not in event, event
            for choice in event.get("choices", []):
                delta = choice.get("delta", {})
                activity = delta.get("file_activity")
                if activity:
                    print(json.dumps({"seconds": round(time.monotonic() - started, 2),
                                      "activity": activity.get("message")}), flush=True)
                answer += delta.get("content") or ""
    return answer


signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError("Synthetic live read exceeded seven minutes")))
signal.alarm(420)
for _ in range(60):
    health = request("/healthz")
    if health.get("ready"):
        break
    time.sleep(1)
assert health.get("ready"), "Local model is not ready"
conversation = "synthetic-mixed-40-" + uuid.uuid4().hex[:12]
with tempfile.TemporaryDirectory(prefix="samosa-own-live-fixture-") as temporary:
    pdf = Path(temporary) / "harbor.pdf"
    generate(pdf)
    attachment = request("/v1/attachments", pdf.read_bytes(), {
        "X-Samosa-Filename-B64": base64.b64encode(b"Synthetic harbor inspection.pdf").decode()})
    started = time.monotonic()
    answer = request("/v1/chat/completions", {
        "model_id": health["backend"], "model_version": health["model_version"],
        "conversation_id": conversation, "attachment_ids": [attachment["id"]],
        "analysis_depth": "fast", "stream": True, "max_tokens": 512,
        "messages": [{"role": "user", "content":
            "Read this document carefully. OCR it. Understand the text. Give a brief summary, "
            "then state how many blue lanterns were approved at stations 01, 20, 37 and 40. "
            "Cite the document page for each count. State any reading gaps."}]}, stream=True)
    # Read only the conversation created above, never any existing user chat.
    evidence = (home / "chats" / conversation / "document-context.txt").read_text()
    print(json.dumps({"model": health["backend"], "seconds": round(time.monotonic() - started, 2),
                      "conversation": conversation, "answer": answer,
                      "reviewed_sections": evidence.count("[Source summary;") or evidence.count("[Reviewed section"),
                      "coverage_recorded": "40 pages processed" in evidence}), flush=True)
    assert "40 pages processed" in evidence, "Complete coverage metadata missing"
    for count in (7101, 7120, 7137, 7140):
        assert str(count) in answer.replace(",", ""), f"Missing or incorrect known fixture fact: {count}"
signal.alarm(0)
print("test_document_mixed_live.py: PASS", flush=True)
