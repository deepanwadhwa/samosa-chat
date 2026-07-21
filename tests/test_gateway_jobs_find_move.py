#!/usr/bin/env python3
"""Gateway /v1/jobs/run find->move staging integration with a fake model."""

import http.client
import importlib.util
import json
import os
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from unittest import mock


class FakeBackend(BaseHTTPRequestHandler):
    calls = 0

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/healthz":
            self._json(200, {"ok": True})
        else:
            self.send_error(404)

    def do_POST(self):
        FakeBackend.calls += 1
        if FakeBackend.calls == 1:
            content = '{"samosa_tool":"fs_list","path":"."}'
        elif FakeBackend.calls == 2:
            content = '{"samosa_tool":"fs_read_text","path":"a.txt"}'
        else:
            content = '{"samosa_tool":"fs_move","src":"a.txt","dst":"Found/a.txt"}'
        self._json(200, {"choices": [{"message": {"content": content}}]})

    def _json(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def load_gateway(home, backend_port):
    os.environ.update({
        "SAMOSA_HOME": home,
        "SAMOSA_BACKEND_PORT": str(backend_port),
        "SAMOSA_APP_HTML": str(Path(home) / "app.html"),
        "SAMOSA_APP_LOGO": str(Path(home) / "logo.png"),
        "SAMOSA_QWEN_ENGINE": str(Path(home) / "qwen36b"),
        "SAMOSA_QWEN_MODEL": str(Path(home) / "model"),
        "SAMOSA_TOKENIZER": str(Path(home) / "tokenizer.json"),
    })
    spec = importlib.util.spec_from_file_location(
        "samosa_gateway", Path(__file__).parents[1] / "tools/samosa_gateway.py")
    gateway = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gateway)
    return gateway


def sse_post(port, path, payload):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    body = json.dumps(payload).encode()
    conn.request("POST", path, body=body,
                 headers={"Content-Type": "application/json", "Content-Length": str(len(body))})
    resp = conn.getresponse()
    raw = resp.read().decode("utf-8")
    conn.close()
    assert resp.status == 200, f"{path} -> HTTP {resp.status}: {raw}"
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


def by_type(events):
    out = {}
    for event in events:
        out.setdefault(event["type"], []).append(event)
    return out


def main():
    FakeBackend.calls = 0
    backend = HTTPServer(("127.0.0.1", 0), FakeBackend)
    backend_port = backend.server_address[1]
    threading.Thread(target=backend.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as work, \
                tempfile.TemporaryDirectory() as jobsroot:
            os.environ["SAMOSA_JOBS_DIR"] = jobsroot
            inbox = Path(work) / "inbox"
            inbox.mkdir()
            (inbox / "a.txt").write_text("hello world")
            gateway = load_gateway(home, backend_port)

            with mock.patch.object(gateway.Supervisor, "ready", return_value=True):
                server = gateway.GatewayServer(("127.0.0.1", 0), gateway.Handler)
                server.handle_error = lambda request, client_address: None
                port = server.server_address[1]
                threading.Thread(target=server.serve_forever, daemon=True).start()
                try:
                    events = sse_post(port, "/v1/jobs/run",
                                      {"goal": "find the hello note and move it to Found",
                                       "folder": str(inbox), "mode": "confirm"})
                    bt = by_type(events)
                    assert [e["tool"] for e in bt["tool_call"]] == ["fs_list", "fs_read_text"]
                    assert "plan" in bt and "await_apply" in bt
                    assert "action" not in bt
                    assert (inbox / "a.txt").exists()
                    job_id = bt["await_apply"][0]["job_id"]

                    applied = by_type(sse_post(port, "/v1/jobs/apply", {"job_id": job_id}))
                    assert applied["applied"][0]["applied"] == 1
                    assert (inbox / "Found" / "a.txt").exists()
                    assert not (inbox / "a.txt").exists()

                    undone = by_type(sse_post(port, "/v1/jobs/undo", {"job_id": job_id}))
                    assert undone["reverted"][0]["reverted"] == 1
                    assert (inbox / "a.txt").exists()
                    assert not (inbox / "Found" / "a.txt").exists()
                finally:
                    server.shutdown()
                    server.server_close()
    finally:
        backend.shutdown()
        backend.server_close()
    print("test_gateway_jobs_find_move: OK")


if __name__ == "__main__":
    main()
