#!/usr/bin/env python3
"""Opt-in acceptance against an already running local Samosa model.

Creates two synthetic document conversations, then checks their saved exact
evidence. No model switch or existing conversation is modified.
"""
import base64
import json
import os
from pathlib import Path
import sys
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_multipage_pdf import build

home = Path(os.environ.get("SAMOSA_HOME", str(Path.home() / ".samosa")))
base = os.environ.get("SAMOSA_TEST_URL", "http://127.0.0.1:8642")
token = (home / "run/ui-token").read_text().strip()


def request(path, data=None, headers=None):
    headers = {"X-Samosa-Token": token, **(headers or {})}
    if isinstance(data, dict):
        data = json.dumps(data).encode()
        headers["Content-Type"] = "application/json"
    with urllib.request.urlopen(urllib.request.Request(base + path, data=data, headers=headers), timeout=180) as response:
        return json.load(response)


health = request("/healthz")
assert health["ready"], health
for filename, metadata_only in [("The Quiet Orchard - Jane Doe.pdf", True), ("book.pdf", False)]:
    # Equal-length stream replacement preserves the generated PDF's offsets.
    pdf = build(100).replace(b"Page 1 of 100", b"By Jane Doe  ", 1)
    if not metadata_only:
        pdf = pdf.replace(b"Page 2 of 100", b"Title page   ", 1)
    source = request("/v1/attachments", pdf, {
        "X-Samosa-Filename-B64": base64.b64encode(filename.encode()).decode()})
    conversation = "harness-live-" + uuid.uuid4().hex[:16]
    started = time.monotonic()
    result = request("/v1/chat/completions", {
        "model_id": health["backend"], "model_version": health["model_version"],
        "conversation_id": conversation, "attachment_ids": [source["id"]],
        "analysis_depth": "fast", "stream": False,
        "messages": [{"role": "user", "content": "Who is the author of this book?"}]})
    answer = result["choices"][0]["message"]["content"]
    evidence = (home / "chats" / conversation / "document-context.txt").read_text()
    print(json.dumps({"model": health["backend"], "filename": filename, "answer": answer,
                      "seconds": round(time.monotonic() - started, 2), "conversation": conversation,
                      "metadata_only": "No document content read" in evidence}), flush=True)
    assert "Jane Doe" in answer, answer
    if metadata_only:
        assert "No document content read" in evidence and "[PDF page " not in evidence, evidence
    else:
        assert "[PDF page 1]" in evidence and "By Jane Doe" in evidence, evidence
        assert "[PDF page 3]" not in evidence and "mode=full" not in evidence, evidence
        assert "filename" not in answer.lower(), "Page evidence was misattributed to the filename"
print("test_document_harness_live.py: PASS", flush=True)
