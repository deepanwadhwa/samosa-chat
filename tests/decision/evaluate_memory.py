#!/usr/bin/env python3
"""Opt-in pinned, real-model routing evaluation; no application dependency.

OpenDecision uses the installed local adapter. Laya may be supplied in an
isolated --laya-path. Both use exactly the production vocabulary and gate.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import samosa_decision as decision

parser = argparse.ArgumentParser()
parser.add_argument('--engine', choices=('opendecision', 'laya'), required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--laya-path', type=Path)
args = parser.parse_args()
if args.engine == 'opendecision':
    model = decision.engine()
    revision = decision.MODEL_REVISION
else:
    if args.laya_path:
        sys.path.insert(0, str(args.laya_path))
    import laya
    import torch
    torch.set_num_threads(4)
    revision = 'f9ab0b228f0fc0f14d873dbc99038f135c2da1b2'
    snapshot = Path.home() / '.cache/huggingface/hub/models--convaiinnovations--laya-typed-decisions/snapshots' / revision
    agent = laya.load(str(snapshot), device='cpu')
    class LayaAdapter:
        def decide(self, kind, **kwargs):
            question = {'type': kind, 'instructions': kwargs['instructions']}
            if 'criteria' in kwargs:
                question['criteria'] = kwargs['criteria']
            return agent.predict(kwargs['state'], {'decision': question})['answers']['decision']
        def choice(self, **kwargs):
            return self.decide('choice', **kwargs)
        choice_fast = choice
        def noul(self, **kwargs):
            return self.decide('noul', **kwargs)
    model = LayaAdapter()
rows = []
for line in (Path(__file__).with_name('memory_cases.jsonl')).read_text().splitlines():
    case = json.loads(line)
    started = time.monotonic()
    result = decision.memory_plan(model, {'question': case['question'], 'sources': []})
    if args.engine == 'laya':
        result['model'] = 'convaiinnovations/laya-typed-decisions'
        result['revision'] = revision
    passed = result['scope'] == case['scope'] and all(a in result['actions'] for a in case['required_actions'])
    row = {**case, 'engine': args.engine, 'revision': revision, 'prediction': result,
           'seconds': round(time.monotonic() - started, 3), 'passed': passed}
    rows.append(row)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(rows, indent=2))
    print(json.dumps(row), flush=True)
print(json.dumps({'cases': len(rows), 'passed': sum(r['passed'] for r in rows)}))
raise SystemExit(0 if all(r['passed'] for r in rows) else 1)
