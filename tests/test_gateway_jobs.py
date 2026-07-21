#!/usr/bin/env python3
"""tests/test_gateway_jobs.py — the gateway's /v1/jobs/* SSE routes.

Boots the real gateway HTTP handler (no model backend — organize-by-type is
deterministic) and drives the routes the Jobs tab depends on: run (confirm),
apply, undo, and execute. Asserts the live event contract and the on-disk
result. Standalone script (mirrors test_gateway_web.py) so env is set before the
gateway module imports.
"""

import http.client
import importlib.util
import json
import os
import tempfile
import threading
from pathlib import Path


def load_gateway(home):
    os.environ.update({
        "SAMOSA_HOME": home,
        "SAMOSA_APP_HTML": str(Path(home) / "app.html"),
        "SAMOSA_APP_LOGO": str(Path(home) / "logo.png"),
        "SAMOSA_QWEN_ENGINE": str(Path(home) / "qwen36b"),   # absent -> no backend
        "SAMOSA_QWEN_MODEL": str(Path(home) / "model"),
        "SAMOSA_TOKENIZER": str(Path(home) / "tokenizer.json"),
    })
    spec = importlib.util.spec_from_file_location(
        "samosa_gateway", Path(__file__).parents[1] / "tools/samosa_gateway.py")
    gateway = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gateway)
    return gateway


def make_inbox(base):
    inbox = os.path.join(base, "inbox")
    os.mkdir(inbox)
    specs = {"a.txt": "hello world", "b.pdf": b"%PDF-1.4 body",
             "c.jpg": b"\xff\xd8\xff\xe0 jpg", "d.png": b"\x89PNG\r\n\x1a\n png"}
    for name, data in specs.items():
        mode = "wb" if isinstance(data, bytes) else "w"
        with open(os.path.join(inbox, name), mode) as f:
            f.write(data)
    return inbox


def sse_post(port, path, payload):
    """POST JSON, read the whole SSE response, return the list of event dicts."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    body = json.dumps(payload).encode()
    conn.request("POST", path, body=body,
                 headers={"Content-Type": "application/json", "Content-Length": str(len(body))})
    resp = conn.getresponse()
    assert resp.status == 200, f"{path} -> HTTP {resp.status}"
    ctype = resp.getheader("Content-Type", "")
    assert "text/event-stream" in ctype, f"{path} content-type {ctype!r}"
    raw = resp.read().decode("utf-8")
    conn.close()
    events = []
    for block in raw.split("\n\n"):
        block = block.strip()
        if not block.startswith("data:"):
            continue
        data = block[len("data:"):].strip()
        if data == "[DONE]":
            continue
        events.append(json.loads(data))
    return events


def json_post(port, path, payload):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    body = json.dumps(payload).encode()
    conn.request("POST", path, body=body,
                 headers={"Content-Type": "application/json", "Content-Length": str(len(body))})
    resp = conn.getresponse()
    raw = resp.read().decode("utf-8")
    status = resp.status
    conn.close()
    return status, json.loads(raw)


def by_type(events):
    out = {}
    for e in events:
        out.setdefault(e["type"], []).append(e)
    return out


def main():
    with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as work, \
            tempfile.TemporaryDirectory() as jobsroot:
        os.environ["SAMOSA_JOBS_DIR"] = jobsroot
        gateway = load_gateway(home)
        inbox = make_inbox(work)

        server = gateway.GatewayServer(("127.0.0.1", 0), gateway.Handler)
        # A client that closes after reading an SSE stream trips the stdlib
        # server's default handle_error (a benign ConnectionReset traceback);
        # silence it so the test output is clean.
        server.handle_error = lambda request, client_address: None
        port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            # 1) confirm mode: decode -> count -> plan -> await_apply, nothing moved.
            events = sse_post(port, "/v1/jobs/run",
                              {"goal": "organize this folder by type", "folder": inbox,
                               "mode": "confirm"})
            bt = by_type(events)
            assert [e["type"] for e in events[:3]] == ["decode_intent", "intent", "counting"], \
                [e["type"] for e in events[:4]]
            assert bt["counting"][0]["total"] == 4
            assert len(bt["plan"][0]["moves"]) == 4
            assert "await_apply" in bt, "confirm mode must pause for apply"
            assert "action" not in bt, "confirm mode must not move files"
            assert os.path.exists(os.path.join(inbox, "a.txt"))
            job_id = bt["await_apply"][0]["job_id"]

            # 2) apply: files move into type folders.
            aevents = by_type(sse_post(port, "/v1/jobs/apply", {"job_id": job_id}))
            assert aevents["applied"][0]["applied"] == 4
            assert os.path.exists(os.path.join(inbox, "Organized", "TXT", "a.txt"))
            assert os.path.exists(os.path.join(inbox, "Organized", "PDF", "b.pdf"))
            assert not os.path.exists(os.path.join(inbox, "a.txt"))

            # 3) undo: files return.
            uevents = by_type(sse_post(port, "/v1/jobs/undo", {"job_id": job_id}))
            assert uevents["reverted"][0]["reverted"] == 4
            assert os.path.exists(os.path.join(inbox, "a.txt"))
            assert not os.path.exists(os.path.join(inbox, "Organized", "TXT", "a.txt"))

            # 4) report intent is read-only.
            revents = by_type(sse_post(port, "/v1/jobs/run",
                                       {"goal": "how many files are here?", "folder": inbox,
                                        "mode": "confirm"}))
            assert "report" in revents and "plan" not in revents

            # 5) suggest-job returns an editable job.json draft without running it.
            status, suggested = json_post(port, "/v1/jobs/suggest",
                                          {"goal": "sort these by file type", "folder": inbox})
            assert status == 200, suggested
            assert suggested["ok"] is True
            assert suggested["template"] == "sort-by-type"
            assert suggested["job"]["input"]["folder"] == inbox
            assert suggested["job"]["organize"]["rule"] == {"by": "extension"}
            assert suggested["estimate"]["unit_count"] == 4
            assert suggested["estimate"]["estimated_wall_seconds"] == 0

            status, estimate = json_post(port, "/v1/jobs/estimate",
                                         {"job": suggested["job"]})
            assert status == 200, estimate
            assert estimate["unit_count"] == 4
            assert estimate["model_units"] == 0

            # 6) review corrections persist through the gateway without rerun.
            review_job = "gateway-review"
            review_dir = Path(jobsroot) / review_job / "results"
            review_dir.mkdir(parents=True)
            receipt = Path(inbox) / "receipt.txt"
            receipt.write_text("Coffee Shop\nTotal 8.37\n")
            (review_dir / "output.jsonl").write_text(json.dumps({
                "unit_id": "u1",
                "status": "review_required",
                "input_path": str(receipt),
                "extracted": {"merchant": "Coffee", "total": 8.0},
            }) + "\n")

            status, review = json_post(port, "/v1/jobs/review",
                                       {"job_id": review_job})
            assert status == 200, review
            assert review["pending"] == 1
            assert review["items"][0]["fields"]["total"] == 8.0

            status, corrected = json_post(port, "/v1/jobs/review/correct", {
                "job_id": review_job,
                "item": {"unit_id": "u1"},
                "fields": {"merchant": "Coffee Shop", "total": 8.37},
            })
            assert status == 200, corrected
            assert corrected["pending"] == 0
            saved = [json.loads(line) for line in (review_dir / "output.jsonl").read_text().splitlines()]
            assert saved[0]["status"] == "passed"
            assert saved[0]["extracted"]["merchant"] == "Coffee Shop"

            # 7) scheduled public URL inputs route returns only new/changed pages.
            gateway.readable_page = lambda url: {
                "url": url,
                "title": "Public Posting",
                "text": "Role A",
                "truncated": False,
            }
            status, public = json_post(port, "/v1/jobs/public-inputs/update", {
                "job_id": "public-route",
                "urls": ["https://example.com/jobs"],
            })
            assert status == 200, public
            assert public["changed"] == 1
            status, public_again = json_post(port, "/v1/jobs/public-inputs/update", {
                "job_id": "public-route",
                "urls": ["https://example.com/jobs"],
            })
            assert status == 200, public_again
            assert public_again["changed"] == 0

            # 8) validation: missing fields -> 400 (not a stream).
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            b = json.dumps({"goal": "", "folder": ""}).encode()
            conn.request("POST", "/v1/jobs/run", body=b,
                         headers={"Content-Type": "application/json", "Content-Length": str(len(b))})
            assert conn.getresponse().status == 400
            conn.close()
        finally:
            server.shutdown()
            server.server_close()
        print("test_gateway_jobs: OK")


if __name__ == "__main__":
    main()
