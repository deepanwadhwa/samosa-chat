#!/usr/bin/env python3
"""Fake samosa serve — test harness for the jobs runner.

stdlib http.server answering POST /v1/chat/completions from a canned map
keyed by request-body hash.  Also stubs GET /internal/v1/status.

Usage:
    python3 tests/jobs/fake_serve.py --self-test     # exit 0 if OK
    python3 tests/jobs/fake_serve.py --port 0        # ephemeral port, print to stderr
"""

import hashlib
import json
import sys
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

# Configurable stub responses
_CANNED = {}          # sha256(body) -> response dict
_STATUS = {           # GET /internal/v1/status fields
    'interactive_active': False,
    'last_interactive_ts': None,
    'queue_depth': 0,
    'inference_busy': False,
    'threads': 2,
}
_BEHAVIOR = {         # special behaviors
    'hang_seconds': 0,
    'fail_count': 0,       # return 500 this many times then succeed
    'fail_counter': 0,     # current fail counter
    'return_429': False,
    'return_400_context_limit': False,
    'context_limit_count': 0,   # return 400 context_limit this many times then succeed
    'context_limit_counter': 0,
}
_REQUEST_COUNT = 0
_REQUEST_LOCK = threading.Lock()
_LAST_HEADERS = {}    # headers of the most recent POST /v1/chat/completions


def set_canned(body_hash, response):
    _CANNED[body_hash] = response

def set_status(**kwargs):
    _STATUS.update(kwargs)

def set_behavior(**kwargs):
    _BEHAVIOR.update(kwargs)


class FakeServeHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Silence logs

    def do_GET(self):
        global _REQUEST_COUNT
        with _REQUEST_LOCK:
            _REQUEST_COUNT += 1

        if self.path == '/internal/v1/status':
            body = json.dumps(_STATUS).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == '/healthz':
            body = b'{"status":"ok"}'
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_error(404)

    def do_POST(self):
        global _REQUEST_COUNT
        with _REQUEST_LOCK:
            _REQUEST_COUNT += 1

        if self.path != '/v1/chat/completions':
            self.send_error(404)
            return

        global _LAST_HEADERS
        _LAST_HEADERS = {k: v for k, v in self.headers.items()}

        length = int(self.headers.get('Content-Length', 0))
        raw = self.rfile.read(length)

        # Check for hang behavior
        if _BEHAVIOR.get('hang_seconds', 0) > 0:
            time.sleep(_BEHAVIOR['hang_seconds'])

        # Check for 429
        if _BEHAVIOR.get('return_429'):
            body = json.dumps({
                'error': {'message': 'Queue full', 'type': 'invalid_request_error', 'code': 'queue_full'}
            }).encode()
            self.send_response(429)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Retry-After', '1')
            self.end_headers()
            self.wfile.write(body)
            return

        # Check for 400 context_limit
        if _BEHAVIOR.get('return_400_context_limit'):
            body = json.dumps({
                'error': {'message': 'Context limit', 'type': 'invalid_request_error', 'code': 'context_limit'}
            }).encode()
            self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # Check for context_limit_count (400 context_limit N times then succeed)
        if _BEHAVIOR.get('context_limit_count', 0) > 0:
            _BEHAVIOR['context_limit_counter'] = _BEHAVIOR.get('context_limit_counter', 0) + 1
            if _BEHAVIOR['context_limit_counter'] <= _BEHAVIOR['context_limit_count']:
                body = json.dumps({
                    'error': {'message': 'Context limit', 'type': 'invalid_request_error', 'code': 'context_limit'}
                }).encode()
                self.send_response(400)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

        # Check for fail_count (500 N times then succeed)
        if _BEHAVIOR.get('fail_count', 0) > 0:
            _BEHAVIOR['fail_counter'] = _BEHAVIOR.get('fail_counter', 0) + 1
            if _BEHAVIOR['fail_counter'] <= _BEHAVIOR['fail_count']:
                self.send_error(500, 'Temporary failure')
                return

        # Look up canned response
        body_hash = hashlib.sha256(raw).hexdigest()
        if body_hash in _CANNED:
            resp = _CANNED[body_hash]
        else:
            # Default: return a valid JSON extraction response
            try:
                req = json.loads(raw)
            except json.JSONDecodeError:
                self.send_error(400)
                return
            resp = {
                'choices': [{
                    'message': {
                        'role': 'assistant',
                        'content': '{"merchant":"Test Store","date":"2026-07-16","total":42.50,"currency":"USD"}'
                    }
                }],
                'usage': {
                    'prompt_tokens': 100,
                    'completion_tokens': 50,
                }
            }

        body = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_server(port=0):
    """Start fake serve on given port (0 = ephemeral). Returns (server, port)."""
    server = HTTPServer(('127.0.0.1', port), FakeServeHandler)
    actual_port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, actual_port


def get_last_headers():
    return dict(_LAST_HEADERS)


def get_request_count():
    with _REQUEST_LOCK:
        return _REQUEST_COUNT

def reset_request_count():
    global _REQUEST_COUNT
    with _REQUEST_LOCK:
        _REQUEST_COUNT = 0


def self_test():
    """Run self-test."""
    import urllib.request

    server, port = start_server(0)
    base = f'http://127.0.0.1:{port}'

    # Test healthz
    resp = urllib.request.urlopen(f'{base}/healthz')
    data = json.loads(resp.read())
    assert data['status'] == 'ok', f"healthz failed: {data}"

    # Test /internal/v1/status
    resp = urllib.request.urlopen(f'{base}/internal/v1/status')
    data = json.loads(resp.read())
    assert 'interactive_active' in data, f"status missing fields: {data}"
    assert data['threads'] == 2

    # Test POST /v1/chat/completions
    body = json.dumps({'messages': [{'role': 'user', 'content': 'test'}]}).encode()
    req = urllib.request.Request(f'{base}/v1/chat/completions', data=body,
                                  headers={'Content-Type': 'application/json'})
    resp = urllib.request.urlopen(req)
    data = json.loads(resp.read())
    assert 'choices' in data, f"chat response missing choices: {data}"
    assert data['choices'][0]['message']['content']

    server.shutdown()
    print("fake_serve: self-test passed")
    return 0


if __name__ == '__main__':
    if '--self-test' in sys.argv:
        sys.exit(self_test())

    port = 8642
    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == '--port' and i < len(sys.argv) - 1:
            port = int(sys.argv[i + 1])

    server, actual_port = start_server(port)
    print(f'[fake_serve] ready http://127.0.0.1:{actual_port}', file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()
