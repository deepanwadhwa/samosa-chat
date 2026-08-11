#!/bin/sh
set -e

echo "=== 1. CHECKPOINT PROVENANCE ==="
python3 -c "
import json
import hashlib
import sys
import os

assets = json.load(open('assets/models.json'))
maple = next(m for m in assets['models'] if m['id'] == 'maple')

print(f'Maple model directory: models/maple')
print(f'Checkpoint revision: {maple[\"version\"]}')

total_bytes = 0
shards = []
for artifact in maple.get('artifacts', []):
    if artifact['role'] == 'weights':
        path = artifact['install_path']
        if os.path.exists(path):
            total_bytes += os.path.getsize(path)
            shards.append(artifact['name'])

print('Names of 3 loaded weight shards:')
for s in shards[:3]:
    print(f'  - {s}')
print(f'Total weight bytes loaded: {total_bytes}')
print('SHA-256 / catalogue verification status:')

for artifact in maple.get('artifacts', []):
    if artifact['role'] == 'weights':
        path = artifact['install_path']
        if not os.path.exists(path):
            print(f'FAIL: Missing {path}')
            sys.exit(1)
        h = hashlib.sha256()
        with open(path, 'rb') as fp:
            while chunk := fp.read(8192*1024):
                h.update(chunk)
        actual_sha = h.hexdigest()
        if actual_sha != artifact['sha256']:
            print(f'FAIL: SHA mismatch for {artifact[\"name\"]}: expected {artifact[\"sha256\"]}, got {actual_sha}')
            sys.exit(1)
        print(f'{actual_sha}  {path} (Verified against catalogue)')
"
