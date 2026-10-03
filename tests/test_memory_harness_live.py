#!/usr/bin/env python3
"""Opt-in general folder/file acceptance through a real installed backend.

Generated, retained fixtures only. Uses public APIs and the shared reader.
Run with SAMOSA_HOME and SAMOSA_TEST_URL pointing at an isolated application.
No existing user documents or conversations are opened.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen_multipage_pdf import build

parser = argparse.ArgumentParser()
parser.add_argument('--fixture-root', type=Path, required=True)
parser.add_argument('--backend', choices=('ornith', 'qwen'))
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--reuse-scope', help='Reuse this test fixture and its indexed scope without rewriting files')
parser.add_argument('--empty-scope', help='Existing empty-fixture scope; required with --reuse-scope')
parser.add_argument('--case', action='append', help='Run only named cases (repeatable); otherwise run the full suite')
args = parser.parse_args()
home = Path(os.environ.get('SAMOSA_HOME', str(Path.home() / '.samosa')))
base = os.environ.get('SAMOSA_TEST_URL', 'http://127.0.0.1:8642')
token = (home / 'run/ui-token').read_text().strip()
evidence = []

def record(row):
    evidence.append(row)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2))
    print(json.dumps(row), flush=True)

def request(path, body=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    with urllib.request.urlopen(urllib.request.Request(base + path, data=data, method=method,
        headers={'X-Samosa-Token': token, 'Content-Type': 'application/json'}), timeout=600) as response:
        return json.load(response)

for _ in range(180):
    try:
        request('/healthz')
        break
    except urllib.error.URLError:
        time.sleep(1)
else:
    raise RuntimeError('The application control plane did not become available')
if args.backend:
    request('/v1/backends/select', {'backend': args.backend})
for _ in range(180):
    health = request('/healthz')
    if health['ready']:
        break
    time.sleep(1)
else:
    raise RuntimeError('The selected model did not become ready')
record({'stage': 'backend_ready', 'backend': health['backend']})
root = args.fixture_root.resolve()
empty = root.parent / (root.name + '-empty')
fixture = {
    'alpha/notes.txt': b'Cedar project: Mira Lee meeting is 11 June 2032, reference CE-405.\n',
    'beta/notes.txt': b'Cedar project: Mira Lee owns the launch checklist. Release code BETA-619.\n',
    'Mira Lee brief.pdf': build(7),
    'Mira Leena diary.txt': b'Mira Leena gardening diary. This is a different person.\n',
    'groceries.txt': b'Shopping: apples, oats, coffee.\n',
}
if args.reuse_scope:
    if not args.empty_scope:
        raise RuntimeError('--empty-scope is required when reusing a fixture')
    for name, content in fixture.items():
        if (root / name).read_bytes() != content:
            raise RuntimeError('Existing generated fixture differs: ' + name)
    if any(empty.iterdir()):
        raise RuntimeError('Existing empty fixture is no longer empty')
elif root.exists() and any(root.iterdir()):
    raise RuntimeError('Use a new empty generated-fixture destination')
else:
    for name, content in fixture.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    empty.mkdir(exist_ok=False)

def remember(folder):
    preflight = request('/v1/chutni/preflight', {'kind': 'folder', 'roots': [{'path': str(folder)}]})
    created = request('/v1/chutni/scopes', {'preflight_id': preflight['preflight_id'],
        'policy_fingerprint': preflight['policy_fingerprint'], 'display_name': folder.name})
    scope = created['scope_id']
    for _ in range(180):
        state = request('/v1/chutni/scopes/' + scope)
        if state.get('state') in ('ready', 'ready_partial'):
            record({'stage': 'memory_ready', 'scope': scope, 'state': state.get('state'),
                'files': state.get('files_indexed')})
            return scope
        if state.get('state') == 'failed':
            raise RuntimeError(state)
        time.sleep(1)
    raise RuntimeError('Memory did not finish')

scope = args.reuse_scope or remember(root)
empty_scope = args.empty_scope or remember(empty)
if args.reuse_scope:
    for scope_id, folder in ((scope, root), (empty_scope, empty)):
        state = request('/v1/chutni/scopes/' + scope_id)
        if Path(state['canonical_root']).resolve() != folder or state.get('state') != 'ready':
            raise RuntimeError('Reused scope does not match the ready test fixture')
        record({'stage': 'memory_reused', 'scope': scope_id, 'files': state.get('files_indexed')})
messages = [{'role': 'system', 'content': 'Answer the current question accurately from supplied evidence.'}]

def ask(question, expected=(), forbidden=(), use_scope=None, followup=False):
    started = time.monotonic()
    context = messages if followup else messages[:1]
    response = request('/v1/chat/completions', {'model': health['model'],
        'messages': context + [{'role': 'user', 'content': question}],
        'directory_context': {'scope_id': use_scope or scope}, 'stream': False,
        'thinking': 'off', 'max_tokens': 1024})
    answer = response['choices'][0]['message']['content']
    # API acceptance covers raw backend output, not just browser rendering.
    record({'stage': 'answer', 'question': question, 'answer': answer,
        'seconds': round(time.monotonic() - started, 3)})
    assert not re.search(r'</?think>', answer), 'Raw thinking leaked into the API answer'
    for pattern in expected:
        assert re.search(pattern, answer, re.I), f'Missing fact: {pattern}'
    for pattern in forbidden:
        assert not re.search(pattern, answer, re.I), f'Unsupported claim: {pattern}'
    messages.append({'role': 'user', 'content': question})
    messages.append({'role': 'assistant', 'content': answer})
    return answer

def reference_case():
    ask('What meeting date is recorded in alpha/notes.txt?', expected=('11 June 2032',))
    ask('And what is its reference?', expected=('CE-405',), forbidden=('BETA-619',), followup=True)

# A two-sentence overview need not repeat names. Require grounded categories
# spanning distinct files, and reject the original no-data refusal.
cases = [
    ('overview', lambda: ask('Give me the gist of this directory in two sentences.',
        expected=(r'project|notes', r'shopping|grocer', r'garden|diary'),
        forbidden=(r'no indexed passages', r"cannot summarize"))),
    ('membership', lambda: ask('Which documents concern Mira Lee, and how many are there?',
        expected=(r'\b3\b|three', r'alpha', r'beta'), forbidden=(r'four (?:files|matches)',))),
    ('pages', lambda: ask('How long is Mira Lee brief.pdf in pages?',
        expected=(r'\b7\b|seven',), forbidden=(r'\b40\b', r'no page count'))),
    ('qualified_content', lambda: ask('What is the release code in beta/notes.txt?', expected=('BETA-619',))),
    ('content', lambda: ask('What is the meeting date and its reference?',
        expected=('11 June 2032', 'CE-405'), followup=True)),
    ('total', lambda: ask('How many files are indexed here?', expected=(r'\b5\b|five',))),
    ('empty', lambda: ask('Summarize this empty folder.', use_scope=empty_scope,
        expected=(r'empty|no files|0 (?:files|indexed)',), forbidden=(r'not attached', r'no folder'))),
    ('ambiguous_basename', lambda: ask('What is in notes.txt?', expected=('alpha', 'beta'),
        forbidden=(r'repeat(?:ed|s)? twice', r'no confirmed match', r'uncertain associations', r'not confirmed matches'))),
    ('missing_subject', lambda: ask('Are there any documents about Mira Wen?',
        expected=(r'no\b|zero|\b0\b|none|not found', r'confirm|uncertain|checked|preview|partial|incomplete|unverified'),
        forbidden=(r'yes[, ]', r'there are no documents about Mira Wen',))),
    ('reference', reference_case),
    ('semantic_topic', lambda: ask('Which documents are about horticulture?',
        expected=('Mira Leena', r'garden|horticultur'), forbidden=(r'four (?:files|matches)',))),
    ('missing_file', lambda: ask('What is in absent.txt?',
        expected=(r'not (?:found|present|listed|available)|no (?:confirmed )?(?:file|document|match)|cannot|couldn.t|does not (?:exist|appear)|don.t',),
        forbidden=(r'absent.txt[^\n]{0,60}(?:contains|records|states) (?:Cedar|Mira|Shopping)',))),
]
if args.case and set(args.case) - {name for name, _ in cases}:
    raise RuntimeError('Unknown live test case')
try:
    completed = []
    for name, run in cases:
        if args.case and name not in args.case:
            continue
        run()
        completed.append(name)
    record({'stage': 'passed', 'backend': health['backend'], 'cases': completed,
        'questions': sum(row.get('stage') == 'answer' for row in evidence)})
except Exception as error:
    record({'stage': 'failed', 'error': str(error)})
    raise
