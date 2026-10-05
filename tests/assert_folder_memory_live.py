"""Opt-in check of later Jobs preview reuse on an existing synthetic live task."""
import json
import os
from pathlib import Path
import sys
import urllib.request

home = Path(os.environ.get('SAMOSA_HOME', str(Path.home() / '.samosa')))
base = os.environ.get('SAMOSA_TEST_URL', 'http://127.0.0.1:8642')
token = (home / 'run/ui-token').read_text().strip()
job = sys.argv[1]
original = json.loads((home / 'jobs' / job / 'job.json').read_text())
memory = json.loads((home / 'jobs' / job / 'memory.json').read_text())
body = {'goal': original['goal'], 'folder': original['folder']}
req = urllib.request.Request(base + '/v1/jobs/decision/start',
    data=json.dumps(body).encode(),
    headers={'X-Samosa-Token': token, 'Content-Type': 'application/json'})
with urllib.request.urlopen(req, timeout=180) as response:
    later = json.load(response)
assert later['ok'], later
items = later['result']['items']
pdf = next(item for item in items if item['path'] == 'Person X records.pdf')
assert pdf['source'] == 'shared_pdf_cache', pdf
assert later['folder_memory']['handoff']['scope_id'] == memory['scope_id'], later['folder_memory']
assert 'Page 1 of 40' in pdf['excerpt'] and 'QZ-731' not in pdf['excerpt'], pdf
assert not later['selected_paths'], later
original_selection = json.loads((home / 'jobs' / job / 'selection.json').read_text())
assert set(original_selection) == {'Person X records.pdf', 'Person X appointment.txt'}, original_selection
print(json.dumps({'original_job': job, 'later_job': later['job_id'],
    'scope_id': memory['scope_id'], 'pdf_preview_source': pdf['source'],
    'original_selection_preserved': True}))
print('later Jobs shared extraction: PASS')
