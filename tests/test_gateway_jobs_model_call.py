#!/usr/bin/env python3
"""Gateway Jobs model-call entrypoints.

Boots a fake backend and verifies the find/tool-loop entrypoint uses the same
admission discipline as the intent classifier, with a larger max_tokens budget.
"""

import importlib.util
import json
import os
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from unittest import mock


class FakeBackend(BaseHTTPRequestHandler):
    payloads = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/healthz":
            self._json(200, {"ok": True})
        else:
            self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        FakeBackend.payloads.append(body)
        if body.get('tools'):
            message = {
                'content': None,
                'tool_calls': [{
                    'id': 'call_native',
                    'type': 'function',
                    'function': {'name': 'fs_list', 'arguments': '{"path":"."}'},
                }],
            }
        else:
            message = {"content": "loop reply"}
        self._json(200, {"choices": [{"message": message}]})

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


def main():
    backend = HTTPServer(("127.0.0.1", 0), FakeBackend)
    backend_port = backend.server_address[1]
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as home:
            gateway = load_gateway(home, backend_port)
            handler = object.__new__(gateway.Handler)
            messages = [{"role": "user", "content": "find Titli"}]

            with mock.patch.object(gateway.Supervisor, "ready", return_value=True):
                gateway.supervisor.backend = 'ornith'
                reply = handler.jobs_tool_loop_model_call(messages)
                assert reply['tool_calls'][0]['id'] == 'call_native', reply
                assert FakeBackend.payloads[-1]["max_tokens"] == 512, FakeBackend.payloads[-1]
                assert FakeBackend.payloads[-1]["thinking"] == "off"
                assert FakeBackend.payloads[-1]["temperature"] == 0
                assert FakeBackend.payloads[-1]["tool_choice"] == 'auto'
                assert FakeBackend.payloads[-1]["parallel_tool_calls"] is False
                names = [item['function']['name'] for item in FakeBackend.payloads[-1]['tools']]
                assert 'fs_read_pages' in names, names

                gateway.supervisor.backend = 'qwen'
                reply = handler.jobs_suggest_model_call(messages)
                assert reply == "loop reply", reply
                assert FakeBackend.payloads[-1]["max_tokens"] == 128, FakeBackend.payloads[-1]
                assert FakeBackend.payloads[-1]["thinking"] == "off"
                assert FakeBackend.payloads[-1]["temperature"] == 0

                with gateway.supervisor.lock:
                    gateway.supervisor.generating = True
                try:
                    before = len(FakeBackend.payloads)
                    assert handler.jobs_tool_loop_model_call(messages) is None
                    assert len(FakeBackend.payloads) == before, "busy call should not hit backend"
                finally:
                    with gateway.supervisor.lock:
                        gateway.supervisor.generating = False
    finally:
        backend.shutdown()
        backend.server_close()
    print("test_gateway_jobs_model_call: OK")


if __name__ == "__main__":
    main()
