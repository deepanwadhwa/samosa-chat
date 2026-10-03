#!/usr/bin/env python3
"""Verify a real memory turn's executed plan and validated counts, not just prose.

Opt-in: supply the isolated acceptance home's developer traces explicitly.
Only the selected turn's bounded decision facts are written to evidence.
"""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--home', type=Path, required=True)
parser.add_argument('--question', required=True)
parser.add_argument('--backend', required=True)
parser.add_argument('--subject', required=True)
parser.add_argument('--matches', type=int, required=True)
parser.add_argument('--uncertain', type=int, required=True)
parser.add_argument('--action', action='append', default=[])
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
events = []
for path in (args.home / 'logs/developer').glob('*.jsonl'):
    events.extend(json.loads(line) for line in path.read_text().splitlines())
events.sort(key=lambda row: row['wall_ms'])
candidates = []
for row in events:
    if row['event'] != 'chat_request_received':
        continue
    body = json.loads(row['fields']['payload'])
    users = [m for m in body.get('messages', []) if m.get('role') == 'user']
    if users and users[-1].get('content') == args.question:
        candidates.append(row)
assert candidates, 'No matching real chat turn was recorded'
turn = candidates[-1]['turn_id']
rows = [row for row in events if row.get('turn_id') == turn]
started = next(row for row in rows if row['event'] == 'chat_turn_started')
assert started['fields']['backend'] == args.backend, 'Wrong real backend'
facts = {row['event']: json.loads(row['fields']['payload']) for row in rows
         if row['event'] in ('memory_route', 'memory_decision', 'memory_membership_validated')}
report = {'turn_id': turn, 'backend': args.backend, 'question': args.question, 'facts': facts}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n')
plan = facts['memory_decision']
assert set(args.action) <= set(plan['actions']), 'Required operation was not executed'
membership = facts['memory_membership_validated']
assert membership['subject'].casefold() == args.subject.casefold(), 'Wrong membership subject'
assert len(membership['match']) == args.matches, 'Wrong gateway-validated count'
assert len(membership['uncertain']) == args.uncertain, 'Wrong uncertain remainder'
assert len(set(membership['match'])) == args.matches, 'Duplicate counted source'
assert not set(membership['match']) & set(membership['uncertain']), 'Overlapping associations'
print(json.dumps({'stage': 'passed', 'backend': args.backend,
                  'matches': args.matches, 'uncertain': args.uncertain}))
