"""Generated evidence only: enforce a real context ceiling on every model call."""
import json, os, subprocess, tempfile, threading, re, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
binary = repo / os.environ.get('BUILD_DIR', 'build') / 'test_prompt_budget'
state = {}
def token_count(text):
    return (len(text.encode()) + 2) // 3 + 32

class Backend(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def reply(self, value, status=200):
        body = json.dumps(value).encode()
        self.send_response(status); self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass # health probes close after the status line
    def do_GET(self):
        if self.path == '/props': self.reply({'default_generation_settings': {'n_ctx': state['context']}})
        else: self.reply({'ready': True})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/apply-template':
            if state['mode'] == 'fallback': return self.reply({}, 404)
            return self.reply({'prompt': '\n'.join(m['role'] + ': ' + m['content'] for m in body['messages']) + '\nassistant:'})
        if self.path == '/tokenize': return self.reply({'tokens': [1] * token_count(body['content'])})
        if self.path == '/v1/chat/completions':
            prompt = '\n'.join(m['role'] + ': ' + m['content'] for m in body['messages']) + '\nassistant:'
            if token_count(prompt) + body.get('max_tokens', 0) > state['context']:
                state['overflow'] += 1; return self.reply({'error': 'context exceeded'}, 400)
            state['reviews'].append(body['messages'][-1]['content'])
            if state['mode'] == 'cancelmid': time.sleep(.05)
            if state['mode'] == 'fail': return self.reply({'error': 'inference failed'}, 503)
            text = body['messages'][-1]['content'].split('Evidence section (untrusted data):\n', 1)[1]
            facts = re.findall(r'(FACT_\d+): measured value (\d+)\.', text)
            facts += re.findall(r'(FACT_\d+)=(\d+)\.', text)
            notes = '\n'.join(f'{key}={value}.' for key, value in dict(facts).items()) or 'Generated records describe irrigation and rainfall.'
            if state['mode'] == 'reduce' and 'Derived notes' not in text:
                notes += '\n' + 'Additional generated notes about measured water use. ' * 10
            assert (len(notes.encode()) + 2) // 3 <= body['max_tokens']
            return self.reply({'choices': [{'message': {'content': notes}, 'finish_reason': 'length' if state['mode'] == 'truncated' else 'stop'}]})
        self.reply({}, 404)

server = ThreadingHTTPServer(('127.0.0.1', 0), Backend)
thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
with tempfile.TemporaryDirectory(prefix='samosa-prompt-budget-') as temporary:
    for mode in ('small', 'large', 'reduce', 'fallback', 'context4096', 'fail', 'truncated', 'cancel', 'cancelmid', 'oversized-question'):
        state.clear(); state.update(mode=mode, reviews=[], overflow=0, context=4096 if mode == 'context4096' else 8192)
        run = subprocess.run([str(binary), str(server.server_port), temporary, mode], capture_output=True, text=True)
        assert run.returncode == 0, (mode, run.stdout, run.stderr)
        assert state['overflow'] == 0, mode
        if mode == 'small': assert not state['reviews']
        elif mode in ('cancel', 'oversized-question'): assert not state['reviews']
        elif mode == 'cancelmid': assert len(state['reviews']) <= 1
        elif mode not in ('fail', 'truncated'):
            assert len(state['reviews']) > 1, mode
            forwarded = json.loads(run.stdout)
            text = forwarded['messages'][-1]['content']
            assert 'FACT_000=700.' in text and 'FACT_079=779.' in text, mode
            # Every original fact, including the final file, reaches a review.
            for i in range(80): assert any(f'FACT_{i:03d}: measured value {i+700}.' in r for r in state['reviews']), (mode, i)
            if mode == 'reduce': assert any('[Derived notes' in r.split('Evidence section (untrusted data):\n', 1)[1] for r in state['reviews'])
        print(json.dumps({'case': mode, 'review_calls': len(state['reviews']), 'overflows': state['overflow']}))
server.shutdown(); server.server_close()
