#!/usr/bin/env python3
"""Opt-in real app find -> select -> ask -> reopen acceptance (synthetic data).

Uses public search/selection/conversation APIs without seeding job state.
--fixture-manifest qualifies the shared larger fixture; --organize exercises
reviewed copy/move, reopened follow-up and undo. Browser acceptance is separate.
"""
import argparse
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


def request(path, body=None, method=None, events=False):
    headers = {"X-Samosa-Token": token}
    if body is not None:
        body = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    try:
        with urllib.request.urlopen(urllib.request.Request(base + path, data=body,
                                    headers=headers, method=method), timeout=300) as response:
            if events:
                return [json.loads(line[6:]) for line in response.read().decode().splitlines()
                        if line.startswith("data: {")]
            return json.load(response)
    except urllib.error.HTTPError as error:
        print(json.dumps({"stage": "http_error", "path": path, "status": error.code,
                          "body": error.read().decode()}), flush=True)
        raise


parser = argparse.ArgumentParser()
parser.add_argument("--backend", choices=("qwen", "ornith", "bonsai"))
parser.add_argument("--clone-from", help="Reuse an existing real discovery task in an independent model fork")
parser.add_argument("--fixture-manifest", help="Use a generated shared acceptance folder")
parser.add_argument("--organize", action="store_true", help="Qualify copy, move, reopen, cited follow-up and undo")
args = parser.parse_args()
if args.backend:
    request("/v1/backends/select", {"backend": args.backend})
for _ in range(120):
    health = request("/healthz")
    if health["ready"]:
        break
    time.sleep(1)
assert health["ready"], health

run = uuid.uuid4().hex[:12]
conversation = "workflow-live-" + run
fixture_manifest = json.loads(Path(args.fixture_manifest).read_text()) if args.fixture_manifest else None
expected = set(fixture_manifest["relevant"]) if fixture_manifest else {"Person X records.pdf", "Person X appointment.txt"}
started = time.monotonic()
if args.clone_from:
    source = request("/v1/jobs/decision/result?job_id=" + args.clone_from)
    fixture = Path(source["result"]["folder"])
    assert set(source["selected_paths"]) == expected, source
    cloned = request(f"/v1/conversations/{conversation}/work",
                     {"job_id": args.clone_from, "clone": True}, "PUT")
    search = {"job_id": cloned["job_id"], "result": source["result"]}
    retry = request(f"/v1/conversations/{conversation}/work",
                    {"job_id": args.clone_from, "clone": True}, "PUT")
    assert retry["job_id"] == search["job_id"], retry
    print(json.dumps({"stage": "model_clone", "model": health["backend"],
                      "source_job": args.clone_from, "job_id": search["job_id"],
                      "conversation": conversation, "seconds": round(time.monotonic() - started, 2)}), flush=True)
else:
    fixture = ROOT / "build" / "file-workflow-live" / run
    if args.fixture_manifest:
        fixture_manifest = json.loads(Path(args.fixture_manifest).read_text())
        fixture = Path(fixture_manifest["root"])
    else:
        fixture.mkdir(parents=True)
    # Equal-length replacement keeps the PDF's xref offsets valid.
    pdf = build(40).replace(b"Page 37 of 40", b"Code: QZ-731 ", 1)
    assert len(b"Page 37 of 40") == len(b"Code: QZ-731 ")
    if not args.fixture_manifest:
        (fixture / "Person X records.pdf").write_bytes(pdf)
        (fixture / "Person X appointment.txt").write_text("Person X appointment date is 17 May 2031. Reference: WX-482.\n")
        (fixture / "Person Y appointment.txt").write_text("Person Y appointment date is 19 June 2032. Reference: YD-991.\n")
    health = request("/healthz")
    assert health["ready"], health
    started = time.monotonic()
    search = request("/v1/jobs/decision/start", {
        "goal": "Find all documents about Person X", "folder": str(fixture)})
    assert search["ok"], search
    paths = {item["path"] for item in search["result"]["items"]}
    expected = set(fixture_manifest["relevant"]) if args.fixture_manifest else {"Person X records.pdf", "Person X appointment.txt"}
    print(json.dumps({"stage": "discovery_coverage", "expected": len(expected), "found": len(expected & paths),
                      "missing": sorted(expected - paths), "folder_decisions": search["result"].get("folder_decisions")}), flush=True)
    assert expected <= paths, search
    if args.fixture_manifest:
        expected_inventory = expected | set(fixture_manifest["distractors"]) | {"unreadable.txt", "unsupported.bin"}
        assert paths == expected_inventory, sorted(paths ^ expected_inventory)
        coverage = {item["path"]: item for item in search["result"]["items"]}
        assert coverage["unreadable.txt"]["source"] == "unreadable" and not coverage["unreadable.txt"]["excerpt"]
        assert coverage["unsupported.bin"]["needs_check"] and not coverage["unsupported.bin"]["excerpt"]
    request("/v1/jobs/selection", {"job_id": search["job_id"], "selected_paths": sorted(expected)})
    conversation = "workflow-live-" + run
    request(f"/v1/conversations/{conversation}/work", {"job_id": search["job_id"]}, "PUT")
    print(json.dumps({"stage": "find_select", "model": health["backend"], "job_id": search["job_id"],
                      "conversation": conversation, "seconds": round(time.monotonic() - started, 2),
                      "checked": search["result"].get("checked_files"), "selected": sorted(expected)}), flush=True)
questions = [
    ("What verification code appears on page 37 of Person X records.pdf? Cite the file and page.", "QZ-731", "Person X records.pdf"),
    ("What is Person X's appointment date? Cite the source. Use only the selected files.", "17 May 2031", "Person X appointment.txt"),
]
if args.fixture_manifest:
    questions.append(("What scan reference appears on page 1 of Person X scan.pdf? Cite the source.", "SC-913", "Person X scan.pdf"))
for question, fact, citation in questions:
    turn_start = time.monotonic()
    saved = request("/v1/jobs/decision/result?job_id=" + search["job_id"])
    assert set(saved["selected_paths"]) == expected, saved
    assert saved["conversation_id"] == conversation, saved
    reply = request("/v1/chat/completions", {
        "conversation_id": conversation, "model_id": health["backend"],
        "model_version": health["model_version"], "stream": False, "max_tokens": 1024,
        "analysis_depth": "fast",
        "messages": [{"role": "user", "content": question}],
    })
    answer = reply["choices"][0]["message"]["content"]
    evidence = (home / "chats" / conversation / "document-context.txt").read_text()
    print(json.dumps({"stage": "answer", "model": health["backend"], "question": question,
                      "answer": answer, "seconds": round(time.monotonic() - turn_start, 2),
                      "evidence_chars": len(evidence)}), flush=True)
    assert fact in answer and citation.lower() in answer.lower(), answer
    assert fact in evidence and "YD-991" not in evidence and "19 June 2032" not in answer, evidence
    assert "attachment_id=" not in evidence and "attachment_id" not in answer.lower(), answer
print("test_file_workflow_live.py questions: PASS", flush=True)

if args.organize:
    original = {path: (fixture / path).read_bytes() for path in expected}
    for operation in ("copy", "move"):
        destination = "reviewed-" + operation + "-" + run
        plan = request("/v1/jobs/selection/plan", {"job_id": search["job_id"],
                        "destination": destination, "operation": operation})
        assert len(plan["moves"]) == len(expected), plan
        assert not (fixture / destination).exists(), "preview mutated source folder"
        try:
            for _ in range(2):
                events = request("/v1/jobs/apply", {"job_id": plan["job_id"],
                                     "plan_id": plan["plan_id"]}, events=True)
                assert not any(event.get("type") == "error" for event in events), events
            for path, content in original.items():
                assert (fixture / destination / path).read_bytes() == content
                assert (fixture / path).exists() == (operation == "copy")
            reopened = request("/v1/jobs/decision/result?job_id=" + search["job_id"])
            selected = expected if operation == "copy" else {destination + "/" + path for path in expected}
            assert set(reopened["selected_paths"]) == selected, reopened
            assert any(action["job_id"] == plan["job_id"] for action in reopened["actions"]), reopened
            if operation == "move":
                turn_start = time.monotonic()
                reply = request("/v1/chat/completions", {
                    "conversation_id": conversation, "model_id": health["backend"],
                    "model_version": health["model_version"], "stream": False, "max_tokens": 1024,
                    "analysis_depth": "fast", "messages": [{"role": "user", "content":
                    "What is Person X's appointment date? Cite its current selected source file."}]})
                answer = reply["choices"][0]["message"]["content"]
                evidence = (home / "chats" / conversation / "document-context.txt").read_text()
                print(json.dumps({"stage": "moved_followup", "answer": answer,
                                  "seconds": round(time.monotonic() - turn_start, 2)}), flush=True)
                assert "17 May 2031" in answer and "Person X appointment.txt" in answer, answer
                assert destination in evidence and "YD-991" not in evidence, evidence
                assert "attachment_id=" not in evidence and "attachment_id" not in answer.lower(), answer
        finally:
            events = request("/v1/jobs/undo", {"job_id": plan["job_id"]}, events=True)
        assert any(event.get("undone") == len(expected) for event in events), events
        for path, content in original.items():
            assert (fixture / path).read_bytes() == content
            assert not (fixture / destination / path).exists()
        assert set(request("/v1/jobs/decision/result?job_id=" + search["job_id"])["selected_paths"]) == expected
        print(json.dumps({"stage": operation + "_reopen_undo", "files": len(expected), "job_id": plan["job_id"]}), flush=True)
    if args.clone_from:
        assert set(request("/v1/jobs/decision/result?job_id=" + args.clone_from)["selected_paths"]) == expected
print("test_file_workflow_live.py: PASS", flush=True)
