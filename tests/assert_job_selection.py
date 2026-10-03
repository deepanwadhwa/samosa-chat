#!/usr/bin/env python3
"""Exercise durable selections through an already-running compiled gateway.

Called before and after a gateway restart by test_compiled_gateway.sh. Seeded
shortlists deliberately avoid requiring a classifier for a checkbox operation.
"""
import json
from pathlib import Path
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError
from urllib.request import Request, urlopen

url, token, home, phase = sys.argv[1:]
jobs = Path(home) / "jobs"


def request(path, body=None, method=None):
    headers = {"X-Samosa-Token": token, "Content-Type": "application/json"}
    req = Request(url + path, headers=headers,
                  data=json.dumps(body).encode() if body is not None else None, method=method)
    try:
        with urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except HTTPError as error:
        return error.code, json.load(error)


def result(job):
    status, data = request("/v1/jobs/decision/result?job_id=" + job)
    assert status == 200, data
    return data


def select(job, paths, expected=200):
    status, data = request("/v1/jobs/selection",
                           {"job_id": job, "selected_paths": paths})
    assert status == expected, (status, data)


def action(path, body):
    req = Request(url + path, data=json.dumps(body).encode(), headers={
        "X-Samosa-Token": token, "Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=20) as response:
            raw = response.read().decode()
            events = [json.loads(line[6:]) for line in raw.splitlines()
                      if line.startswith("data: {")]
            return response.status, events
    except HTTPError as error:
        return error.code, json.load(error)


def stop_paused_batch(path, body):
    pause = Path(home).parent / 'dogfood-move-pause'
    reached, release = Path(str(pause) + '.reached'), Path(str(pause) + '.release')
    release.unlink(missing_ok=True)
    reached.unlink(missing_ok=True)
    pause.touch()
    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(action, path, body)
            try:
                deadline = time.monotonic() + 5
                while not reached.exists() and time.monotonic() < deadline:
                    time.sleep(.01)
                assert reached.exists(), 'first file never reached its durable checkpoint'
                run_id = (jobs / body['job_id'] / 'action-run').read_text()
                status, refusal = request(path, body)
                assert status == 409 and refusal['error']['code'] == 'action_busy', refusal
                assert request('/v1/jobs/action/stop', {'job_id': body['job_id'], 'run_id': 'expired-run'})[0] == 409
                assert request('/v1/jobs/action/stop', {'job_id': body['job_id'], 'run_id': run_id})[0] == 202
            finally:
                release.touch()
            status, events = pending.result(timeout=10)
        assert status == 200 and any(e.get('stopped') and e.get('pending') == 1 for e in events), events
        assert not (jobs / body['job_id'] / 'action-run').exists()
        assert request('/v1/jobs/action/stop', {'job_id': body['job_id'], 'run_id': run_id})[0] == 409
        return events
    finally:
        pause.unlink(missing_ok=True)
        reached.unlink(missing_ok=True)
        release.unlink(missing_ok=True)


def organize(job, destination, operation="copy"):
    status, plan = request("/v1/jobs/selection/plan", {
        "job_id": job, "destination": destination, "operation": operation})
    assert status == 200, (status, plan)
    return plan


def work(conversation, job=None):
    return request(f"/v1/conversations/{conversation}/work",
                   {"job_id": job} if job else None, "PUT" if job else "GET")


def ask(conversation, **extra):
    return request("/v1/chat/completions", {
        "conversation_id": conversation, "model": "local", "stream": False,
        "model_id": request("/healthz")[1]["backend"],
        "model_version": request("/healthz")[1]["model_version"],
        "messages": [{"role": "user", "content": "selected workflow probe: what is Person X's code?"}],
        "max_tokens": 2048, **extra,
    })


if phase == "before":
    helper = Path(home) / "current" / "bin" / "samosa-decision"
    helper.parent.mkdir(parents=True, exist_ok=True)
    # A bounded helper fixture observes what the native gateway sends. It
    # intentionally does no classification or document answering.
    helper.write_text(f"#!{sys.executable}\n" + '''import json, sys
from pathlib import Path
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
assert args["--mode"] == "next"
request = json.loads(Path(args["--request"]).read_text())
result = json.loads(Path(args["--previous"]).read_text())
result["focus_paths"] = request["selected_paths"]
result["asked"] = request["followup"]
print(json.dumps(result))
''')
    helper.chmod(0o700)
    for job in ("selection-one", "selection-two"):
        directory = jobs / job
        directory.mkdir()
        (directory / "job.json").write_text(json.dumps({
            "goal": "Find Person X immigration documents", "folder": "/fixture",
        }))
        (directory / "decision.json").write_text(json.dumps({
            "ok": True, "schema_version": 5, "goal": "Find Person X immigration documents",
            "folder": "/fixture", "items": [{"path": f"document-{i}.txt"} for i in range(51)],
        }))
    root = Path(home) / "workflow-sources"
    root.mkdir()
    for name, text in (("x.txt", "Person X code: WORKFLOW_X_QUARTZ."),
                       ("y.txt", "Person Y code: WORKFLOW_Y_DISTRACTOR.")):
        (root / name).write_text(text)
    for folder, text in (("a", "first same-basename source"), ("b", "second same-basename source")):
        (root / folder).mkdir()
        (root / folder / "report.txt").write_text(text)
    long_relative = "/".join(["long-source-directory-" + "x" * 55] * 4) + "/record.txt"
    long_source = root / long_relative
    long_source.parent.mkdir(parents=True)
    long_source.write_text("Person X long relative source path")
    directory = jobs / "workflow-answer"
    directory.mkdir()
    items = []
    for source in root.rglob("*.txt"):
        st = source.stat()
        items.append({"path": source.relative_to(root).as_posix(), "dev": st.st_dev, "ino": st.st_ino,
                      "size": st.st_size, "mtime_ns": str(st.st_mtime_ns)})
    (directory / "job.json").write_text(json.dumps({"goal": "Find Person X", "folder": str(root)}))
    (directory / "decision.json").write_text(json.dumps({"ok": True, "schema_version": 5,
        "goal": "Find Person X", "folder": str(root), "items": items}))
    select("workflow-answer", ["x.txt"])
    assert work("workflow-chat")[1] == {"job_id": None}
    assert work("workflow-chat", "workflow-answer")[0] == 200
    assert work("workflow-chat", "workflow-answer")[0] == 200  # retry is idempotent
    assert work("other-workflow-chat", "workflow-answer")[0] == 409
    assert work("workflow-chat", "selection-one")[0] == 409
    assert work("unrelated-chat")[1] == {"job_id": None}
    select("workflow-answer", [long_relative])
    status, limitation = ask("workflow-chat")
    assert status == 409 and "source-label limit" in limitation["error"]["message"], limitation
    assert result("workflow-answer")["selected_paths"] == [long_relative]
    select("workflow-answer", ["x.txt"])
    status, cloned = request("/v1/conversations/workflow-fork/work", {"job_id": "workflow-answer", "clone": True}, "PUT")
    assert status == 200 and cloned["job_id"] != "workflow-answer", cloned
    cloned_job = cloned["job_id"]
    assert result(cloned_job)["selected_paths"] == ["x.txt"]
    assert result(cloned_job)["result"] == result("workflow-answer")["result"]
    assert request("/v1/conversations/workflow-fork/work", {"job_id": "workflow-answer", "clone": True}, "PUT")[1] == cloned
    select(cloned_job, [])
    assert result("workflow-answer")["selected_paths"] == ["x.txt"]
    assert ask("workflow-fork")[0] == 409

    status, refused = ask("workflow-chat", directory_context={"scope_id": "another-folder"})
    assert status == 409 and refused["error"]["code"] == "mixed_document_scope", refused
    status, answer = ask("workflow-chat")
    assert status == 200, (status, answer)
    assert answer["choices"][0]["message"]["content"] == "selected workflow evidence and citation received", answer
    assert request("/v1/conversations/workflow-chat/documents")[1]["documents"] == []
    evidence = (Path(home) / "chats/workflow-chat/document-context.txt").read_text()
    assert "WORKFLOW_X_QUARTZ" in evidence and "WORKFLOW_Y_DISTRACTOR" not in evidence
    assert "attachment_id=" not in evidence and "[Source: x.txt; mode=selected]" in evidence
    assert result("workflow-answer")["conversation_id"] == "workflow-chat"
    select("workflow-answer", ["x.txt", "a/report.txt", "b/report.txt"])
    status, answer = ask("workflow-chat")
    assert status == 200 and answer["choices"][0]["message"]["content"] == "selected workflow evidence and citation received", answer
    evidence = (Path(home) / "chats/workflow-chat/document-context.txt").read_text()
    assert "WORKFLOW_X_QUARTZ" in evidence and "first same-basename source" not in evidence
    assert "Selected source not inspected" in evidence and "a/report.txt" in evidence
    assert result("workflow-answer")["selected_paths"] == ["x.txt", "a/report.txt", "b/report.txt"]
    status, answer = ask("workflow-chat", messages=[{"role": "user", "content": "selected workflow probe: find every mention of WORKFLOW_X_QUARTZ in x.txt and disclose coverage."}])
    assert status == 200, answer
    evidence = (Path(home) / "chats/workflow-chat/document-context.txt").read_text()
    assert "WORKFLOW_X_QUARTZ" in evidence and "first same-basename source" not in evidence
    assert "second same-basename source" not in evidence and "Selected source not inspected" in evidence
    status, answer = ask("workflow-chat", messages=[{"role": "user", "content": "selected workflow probe: find every mention across all selected files, including x.txt."}])
    assert status == 200, answer
    evidence = (Path(home) / "chats/workflow-chat/document-context.txt").read_text()
    assert "first same-basename source" in evidence and "second same-basename source" in evidence
    status, answer = ask("workflow-chat", messages=[{"role": "user", "content": "all source selection probe: selected workflow probe"}])
    assert status == 200, answer
    evidence = (Path(home) / "chats/workflow-chat/document-context.txt").read_text()
    assert "first same-basename source" in evidence and "second same-basename source" in evidence
    assert "attachment_id=" not in evidence
    status, refused = ask("workflow-chat", messages=[{"role": "user", "content": "invalid source selection probe"}])
    assert status == 422 and refused["error"]["code"] == "selected_read_planning_unavailable", refused
    assert result("workflow-answer")["selected_paths"] == ["x.txt", "a/report.txt", "b/report.txt"]
    select("workflow-answer", ["x.txt"])
    plan = organize("workflow-answer", "organized")
    assert not (root / "organized").exists(), "preview mutated source files"
    assert [Path(move["src"]).name for move in plan["moves"]] == ["x.txt"]
    assert action("/v1/jobs/apply", {"job_id": plan["job_id"]})[0] == 409
    select("workflow-answer", [])
    assert action("/v1/jobs/apply", {"job_id": plan["job_id"], "plan_id": plan["plan_id"]})[0] == 409
    select("workflow-answer", ["x.txt"])
    for _ in range(2):
        status, events = action("/v1/jobs/apply", {"job_id": plan["job_id"], "plan_id": plan["plan_id"]})
        assert status == 200 and any(e.get("applied") == 1 for e in events), events
    assert (root / "organized/x.txt").read_bytes() == (root / "x.txt").read_bytes()
    assert not (root / "organized/y.txt").exists()
    journal = (jobs / plan["job_id"] / "applied.jsonl").read_text().splitlines()
    assert len(journal) == 1, journal
    status, events = action("/v1/jobs/undo", {"job_id": plan["job_id"]})
    assert status == 200 and any(e.get("undone") == 1 for e in events), events
    assert not (root / "organized/x.txt").exists() and (root / "x.txt").exists()
    moved = organize("workflow-answer", "moved", "move")
    for _ in range(2):
        status, events = action("/v1/jobs/apply", {"job_id": moved["job_id"], "plan_id": moved["plan_id"]})
        assert status == 200 and any(e.get("applied") == 1 for e in events), events
    assert not (root / "x.txt").exists()
    assert result("workflow-answer")["selected_paths"] == ["moved/x.txt"]
    assert ask("workflow-chat")[0] == 200, "moved source could not be asked about"
    status, events = action("/v1/jobs/undo", {"job_id": moved["job_id"]})
    assert status == 200 and any(e.get("undone") == 1 for e in events), events
    assert result("workflow-answer")["selected_paths"] == ["x.txt"]
    assert (root / "x.txt").exists() and not (root / "moved/x.txt").exists()
    assert len(result("workflow-answer")["actions"]) == 2
    select("workflow-answer", ["a/report.txt", "b/report.txt"])
    same_names = organize("workflow-answer", "same-names")
    status, events = action("/v1/jobs/apply", {"job_id": same_names["job_id"], "plan_id": same_names["plan_id"]})
    assert status == 200 and any(e.get("applied") == 2 for e in events), events
    for folder in ("a", "b"):
        assert (root / "same-names" / folder / "report.txt").read_bytes() == (root / folder / "report.txt").read_bytes()
    assert {e["n"] for e in events if e.get("type") == "action"} == {2}
    assert {e["i"] for e in events if e.get("type") == "action"} == {1, 2}
    action("/v1/jobs/undo", {"job_id": same_names["job_id"]})
    assert not (root / "same-names/a/report.txt").exists() and not (root / "same-names/b/report.txt").exists()
    # A second-file collision leaves the first copy resumable and preserves the
    # conflicting destination. Retry must reuse that first inode and journal.
    partial = organize("workflow-answer", "partial-copy")
    conflict = root / "partial-copy/b/report.txt"
    conflict.parent.mkdir(parents=True)
    conflict.write_bytes(b"existing user destination")
    status, events = action("/v1/jobs/apply", {"job_id": partial["job_id"], "plan_id": partial["plan_id"]})
    assert status == 200 and any(e.get("applied") == 1 for e in events), events
    assert conflict.read_bytes() == b"existing user destination"
    completed = root / "partial-copy/a/report.txt"
    first_inode = completed.stat().st_ino
    conflict.unlink()
    status, events = action("/v1/jobs/apply", {"job_id": partial["job_id"], "plan_id": partial["plan_id"]})
    assert status == 200 and any(e.get("applied") == 2 for e in events), events
    assert completed.stat().st_ino == first_inode
    assert len((jobs / partial["job_id"] / "applied.jsonl").read_text().splitlines()) == 2
    assert conflict.read_bytes() == (root / "b/report.txt").read_bytes()
    status, events = action("/v1/jobs/undo", {"job_id": partial["job_id"]})
    assert any(e.get("undone") == 2 for e in events), events
    assert not completed.exists() and not conflict.exists()
    # Stop a reviewed batch at the existing deterministic first-file pause,
    # leave it durable, and resume through a new gateway in the after phase.
    status, cloned = request('/v1/conversations/workflow-stop-chat/work',
                             {'job_id': 'workflow-answer', 'clone': True}, 'PUT')
    assert status == 200, cloned
    stopped = organize(cloned['job_id'], 'stop-restart')
    events = stop_paused_batch('/v1/jobs/apply', {'job_id': stopped['job_id'], 'plan_id': stopped['plan_id']})
    assert (root / 'stop-restart/a/report.txt').read_bytes() == (root / 'a/report.txt').read_bytes()
    assert not (root / 'stop-restart/b/report.txt').exists()
    assert len((jobs / stopped['job_id'] / 'applied.jsonl').read_text().splitlines()) == 1
    select("workflow-answer", ["x.txt"])
    # Leave a copied file/action for recovery and undo after gateway restart.
    persistent = organize("workflow-answer", "persistent")
    status, events = action("/v1/jobs/apply", {"job_id": persistent["job_id"], "plan_id": persistent["plan_id"]})
    assert status == 200 and any(e.get("applied") == 1 for e in events), events


    select("workflow-answer", [])
    assert ask("workflow-chat")[0] == 409
    select("workflow-answer", ["x.txt"])
    original = (jobs / "selection-one" / "decision.json").read_bytes()
    assert result("selection-one")["selected_paths"] == []
    select("selection-one", ["document-0.txt", "document-1.txt"])
    select("selection-one", ["document-1.txt"])
    assert result("selection-one")["selected_paths"] == ["document-1.txt"]
    assert result("selection-two")["selected_paths"] == []
    for invalid in (["../outside.txt"], ["/outside.txt"], ["unknown.txt"],
                    ["document-1.txt", "document-1.txt"], [42], "document-1.txt",
                    [f"document-{i}.txt" for i in range(51)]):
        select("selection-one", invalid, 400)
        assert result("selection-one")["selected_paths"] == ["document-1.txt"]
    select("missing-job", [], 400)
    assert (jobs / "selection-one" / "decision.json").read_bytes() == original
    status, data = request("/v1/jobs/decision/next", {
        "job_id": "selection-one", "followup": "Refine the selected files",
    })
    assert status == 200, data
    assert data["result"]["focus_paths"] == ["document-1.txt"]
    assert data["selected_paths"] == ["document-1.txt"]
    # Legacy clients can still explicitly set a selection in a follow-up.
    status, data = request("/v1/jobs/decision/next", {
        "job_id": "selection-two", "followup": "Refine these",
        "selected_paths": ["document-2.txt"],
    })
    assert status == 200, data
    assert data["result"]["focus_paths"] == ["document-2.txt"]
    select("selection-two", [])
elif phase == "after":
    stopped_job = work('workflow-stop-chat')[1]['job_id']
    stopped = result(stopped_job)['actions'][-1]
    stop_root = Path(home) / 'workflow-sources'
    first = stop_root / 'stop-restart/a/report.txt'
    inode = first.stat().st_ino
    status, events = action('/v1/jobs/apply', {'job_id': stopped['job_id'], 'plan_id': stopped['plan_id']})
    assert status == 200 and any(e.get('applied') == 2 for e in events), events
    assert first.stat().st_ino == inode
    assert len((jobs / stopped['job_id'] / 'applied.jsonl').read_text().splitlines()) == 2
    assert (stop_root / 'stop-restart/b/report.txt').read_bytes() == (stop_root / 'b/report.txt').read_bytes()
    stop_paused_batch('/v1/jobs/undo', {'job_id': stopped['job_id']})
    assert not first.exists() and (stop_root / 'stop-restart/b/report.txt').exists()
    assert (jobs / stopped['job_id'] / 'applied.jsonl').exists(), 'partial undo lost its recovery journal'
    status, events = action('/v1/jobs/undo', {'job_id': stopped['job_id']})
    assert status == 200 and any(e.get('undone') == 2 for e in events), events
    assert not first.exists() and not (stop_root / 'stop-restart/b/report.txt').exists()
    assert work("workflow-chat")[1] == {"job_id": "workflow-answer"}
    status, refused = ask("workflow-chat", directory_context={"scope_id": "another-folder"})
    assert status == 409 and refused["error"]["code"] == "mixed_document_scope", refused
    status, answer = ask("workflow-chat")
    assert status == 200, (status, answer)
    assert answer["choices"][0]["message"]["content"] == "selected workflow evidence and citation received", answer
    root = Path(home) / "workflow-sources"
    saved_action = result("workflow-answer")["actions"][-1]
    saved_plan = request("/v1/jobs/selection/plan?job_id=" + saved_action["job_id"])[1]
    assert saved_plan["plan_id"] == saved_action["plan_id"]
    assert (root / "persistent/x.txt").read_bytes() == (root / "x.txt").read_bytes()
    status, events = action("/v1/jobs/undo", {"job_id": saved_action["job_id"]})
    assert status == 200 and any(e.get("undone") == 1 for e in events), events
    assert not (root / "persistent/x.txt").exists()
    original = (root / "x.txt").read_bytes()
    (root / "x.txt").write_bytes(original.replace(b"QUARTZ", b"EDITED"))
    status, answer = ask("workflow-chat")
    assert status == 409 and "changed" in answer["error"]["message"], answer
    data = result("selection-one")
    assert data["selected_paths"] == ["document-1.txt"]
    assert data["result"]["goal"] == "Find Person X immigration documents"
    assert result("selection-two")["selected_paths"] == []
    select("selection-one", [])
    assert result("selection-one")["selected_paths"] == []
    # Corrupt saved state must fail visibly instead of becoming an empty scope.
    (jobs / "selection-one" / "selection.json").write_text("broken")
    assert request("/v1/jobs/decision/result?job_id=selection-one")[0] == 500
    select("selection-one", ["document-2.txt"])
    assert result("selection-one")["selected_paths"] == ["document-2.txt"]
else:
    raise AssertionError(phase)
print(f"compiled gateway durable selection ({phase} restart): PASS")
