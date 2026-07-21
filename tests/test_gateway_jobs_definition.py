#!/usr/bin/env python3
"""Gateway job-definition preview/run through an Ornith-style backend."""

import http.client
import importlib.util
import json
import os
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from unittest import mock


class FakeOrnithBackend(BaseHTTPRequestHandler):
    payloads = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/health":
            self._json(200, {"ok": True})
        else:
            self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        FakeOrnithBackend.payloads.append(body)
        self._json(200, {"choices": [{"message": {"content": '{"merchant":"Cafe","total":4.5}'}}]})

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
    Path(home, "model-backend").write_text("ornith\n")
    spec = importlib.util.spec_from_file_location(
        "samosa_gateway", Path(__file__).parents[1] / "tools/samosa_gateway.py")
    gateway = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gateway)
    return gateway


def json_post(port, path, payload):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    body = json.dumps(payload).encode()
    conn.request("POST", path, body=body,
                 headers={"Content-Type": "application/json", "Content-Length": str(len(body))})
    resp = conn.getresponse()
    raw = resp.read().decode("utf-8")
    conn.close()
    assert resp.status == 200, f"{path} -> HTTP {resp.status}: {raw}"
    return json.loads(raw)


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


def main():
    FakeOrnithBackend.payloads = []
    backend = HTTPServer(("127.0.0.1", 0), FakeOrnithBackend)
    backend_port = backend.server_address[1]
    threading.Thread(target=backend.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as work, \
                tempfile.TemporaryDirectory() as jobsroot:
            os.environ["SAMOSA_JOBS_DIR"] = jobsroot
            inbox = Path(work) / "inbox"
            out = Path(work) / "out"
            inbox.mkdir()
            (inbox / "r.txt").write_text("Cafe total 4.50")
            gateway = load_gateway(home, backend_port)
            gateway.BONSAI_SERVER = Path(__file__)
            gateway.ORNITH_MODEL = Path(__file__)
            gateway.GGUF_MODELS["ornith"] = Path(__file__)
            gateway.supervisor.backend = "ornith"

            server = gateway.GatewayServer(("127.0.0.1", 0), gateway.Handler)
            server.handle_error = lambda request, client_address: None
            port = server.server_address[1]
            threading.Thread(target=server.serve_forever, daemon=True).start()
            try:
                job = {
                    "job_id": "definition-route",
                    "input": {"folder": str(inbox), "recursive": False, "types": ["text/plain"]},
                    "instruction": "Extract merchant and total.",
                    "output_schema": {
                        "type": "object",
                        "required": ["merchant", "total"],
                        "properties": {
                            "merchant": {"type": "string"},
                            "total": {"type": "number"},
                        },
                    },
                    "output": {"dir": str(out)},
                }
                with mock.patch.object(gateway.Supervisor, "ready", return_value=True):
                    preview = json_post(port, "/v1/jobs/definition/preview", {"job": job})
                    assert preview["records"][0]["status"] == "passed", preview
                    events = sse_post(port, "/v1/jobs/definition/run", {"job": job})
            finally:
                server.shutdown()
                server.server_close()
            types = [e["type"] for e in events]
            assert "item_complete" in types, types
            assert (out / "output.jsonl").is_file()
            record = json.loads((out / "output.jsonl").read_text().splitlines()[0])
            assert record["merchant"] == "Cafe", record
            assert FakeOrnithBackend.payloads[-1]["thinking"] == "off"
            assert FakeOrnithBackend.payloads[-1]["model"] == "ornith-1.0-9b"
            assert FakeOrnithBackend.payloads[-1]["max_tokens"] == 512
    finally:
        backend.shutdown()
        backend.server_close()
    print("test_gateway_jobs_definition: OK")


if __name__ == "__main__":
    main()
