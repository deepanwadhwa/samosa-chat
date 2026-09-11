#!/usr/bin/env python3
"""Opt-in streaming acceptance for an existing attachment and selected model.

Creates an isolated conversation; checks the first lookup and reuse on a
follow-up. Prints actual activity events with elapsed times, never auth tokens.
"""
import argparse
import json
import os
import re
from pathlib import Path
import time
import urllib.request
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("--attachment", required=True)
parser.add_argument("--question", default="What is the name of the first chapter?")
parser.add_argument("--expect", required=True, action="append",
                    help="Required answer text; repeat to check every author, not just the first.")
parser.add_argument("--max-page", type=int, help="Fail if the model requests a page above this bound.")
parser.add_argument("--no-ocr", action="store_true", help="Require native text only.")
parser.add_argument("--prior-answer", help="Replay an earlier failed answer in an isolated conversation to test recovery.")
parser.add_argument("--turns", type=int, choices=(1, 2), default=2)
args = parser.parse_args()
home = Path(os.environ.get("SAMOSA_HOME", str(Path.home() / ".samosa")))
base = os.environ.get("SAMOSA_TEST_URL", "http://127.0.0.1:8642")
token = (home / "run/ui-token").read_text().strip()
headers = {"X-Samosa-Token": token, "Content-Type": "application/json"}
for attempt in range(30):
    with urllib.request.urlopen(base + "/healthz", timeout=5) as response:
        health = json.load(response)
    if health["ready"]:
        break
    time.sleep(1)
assert health["ready"], health
conversation = "harness-decisions-" + uuid.uuid4().hex[:16]
messages = ([{"role": "user", "content": args.question},
             {"role": "assistant", "content": args.prior_answer}] if args.prior_answer else [])
for turn in range(args.turns):
    messages.append({"role": "user", "content": args.question})
    body = {"model_id": health["backend"], "model_version": health["model_version"],
            "conversation_id": conversation, "analysis_depth": "fast", "stream": True,
            "messages": messages}
    if turn == 0:
        body["attachment_ids"] = [args.attachment]
    started = time.monotonic()
    answer = ""
    activities = []
    with urllib.request.urlopen(urllib.request.Request(
            base + "/v1/chat/completions", data=json.dumps(body).encode(), headers=headers), timeout=180) as response:
        for line in response:
            if not line.startswith(b"data: ") or line.strip() == b"data: [DONE]":
                continue
            event = json.loads(line[6:])
            assert "error" not in event, event
            delta = event.get("choices", [{}])[0].get("delta", {})
            if "file_activity" in delta:
                activity = delta["file_activity"]
                activities.append(activity)
                print(json.dumps({"turn": turn, "seconds": round(time.monotonic() - started, 2),
                                  "stage": activity["stage"], "message": activity["message"]}), flush=True)
            answer += delta.get("content") or ""
    elapsed = round(time.monotonic() - started, 2)
    print(json.dumps({"turn": turn, "seconds": elapsed, "answer": answer, "conversation": conversation}), flush=True)
    if args.no_ocr:
        assert not any(a["stage"] == "ocr" for a in activities), activities
    if args.max_page:
        for activity in activities:
            if activity["stage"] == "reading":
                selected = re.search(r"PDF pages (\d+)[–-](\d+)", activity["message"])
                assert selected and int(selected[2]) <= args.max_page, activity
    for expected in args.expect:
        assert expected.lower() in answer.lower(), ("missing expected text", expected, answer)
    assert activities and activities[0]["stage"] in ("routing", "reused"), activities
    if turn == 0:
        reads = [a for a in activities if a["stage"] == "reading"]
        assert len(reads) <= 2, "simple lookup required too many separate reading decisions"
        assert reads and "pages" in reads[0]["message"] and " to " in reads[0]["message"]
        assert any(a["stage"] == "checking" for a in activities)
    else:
        assert any(a["stage"] == "reused" for a in activities), activities
        assert not any(a["stage"] in ("reading", "searching") for a in activities), activities
    messages.append({"role": "assistant", "content": answer})
print("test_document_decisions_live.py: PASS", flush=True)
