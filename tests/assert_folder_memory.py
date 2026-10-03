"""Exercise automatic folder memory through real gateway and Chutni routes."""
import json
import pathlib
import sys
import time
import urllib.request

base, token, home, phase = sys.argv[1:]
home = pathlib.Path(home)
receipt = home / 'automatic-memory-test.json'


def request(route, data=None):
    req = urllib.request.Request(base + route,
        data=json.dumps(data).encode() if data is not None else None,
        headers={'X-Samosa-Token': token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=60) as response:
        return response.read().decode()


def query(scope, term):
    return request('/v1/chutni/query', {
        'query': term, 'directory_context': {'scope_id': scope}})


def report(folder, expected='queued'):
    raw = request('/v1/jobs/run', {'recipe': 'folder_report', 'folder': str(folder)})
    events = [json.loads(line[6:]) for line in raw.splitlines()
              if line.startswith('data: {')]
    job = next(e['job_id'] for e in events if 'job_id' in e)
    for _ in range(600):
        memory = json.loads((home / 'jobs' / job / 'memory.json').read_text())
        if memory['state'] != 'busy':
            break
        time.sleep(.05)
    assert memory['state'] == expected, memory
    return job, memory['scope_id'], memory['build_job_id']


def ready(scope, build):
    for _ in range(600):
        status = json.loads(request('/v1/chutni/scopes/' + scope))
        job = json.loads((home / 'chutni' / 'scopes' / scope / 'job.json').read_text())
        if job.get('job_id') == build and job.get('state') in ('completed', 'completed_partial'):
            assert status['state'] in ('ready', 'ready_partial'), status
            return status
        assert job.get('state') != 'failed', job
        time.sleep(.05)
    raise AssertionError('automatic memory build did not finish')


if phase == 'before':
    folder = home / 'automatic-memory-fixture'
    folder.mkdir()
    source = folder / 'record.txt'
    source.write_text('AUTOMEM_OLD_731 appointment is May 17.\n')
    (folder / 'unchanged.txt').write_text('AUTOMEM_STABLE_482 reference.\n')
    job, scope, build = report(folder)
    status = ready(scope, build)
    assert status['content_readable_files'] == 2, status
    assert 'AUTOMEM_OLD_731' in query(scope, 'AUTOMEM_OLD_731')
    source.write_text('AUTOMEM_NEW_991 appointment is June 23.\n')
    assert 'AUTOMEM_OLD_731' not in query(scope, 'AUTOMEM_OLD_731')
    second_job, second_scope, build = report(folder)
    assert scope == second_scope, (scope, second_scope)
    ready(scope, build)
    assert 'AUTOMEM_NEW_991' in query(scope, 'AUTOMEM_NEW_991')
    assert 'AUTOMEM_STABLE_482' in query(scope, 'AUTOMEM_STABLE_482')
    protocol = json.loads((home / 'chutni' / 'scopes' / scope / 'protocol.json').read_text())
    assert protocol['scan']['unchanged'] >= 1, protocol
    # Two folder reports need eventual memory even when the single worker is
    # occupied. No optional decision model is needed to exercise this handoff.
    waiting = home / 'automatic-memory-waiting'
    waiting.mkdir()
    for number in range(8):
        (waiting / f'{number}.txt').write_text(f'AUTOMEM_WAIT_{number} saved evidence.\n')
    _, waiting_scope, waiting_build = report(waiting)
    _, _, next_build = report(folder)
    ready(waiting_scope, waiting_build)
    ready(scope, next_build)
    assert 'AUTOMEM_WAIT_3' in query(waiting_scope, 'AUTOMEM_WAIT_3')
    assert 'AUTOMEM_NEW_991' in query(scope, 'AUTOMEM_NEW_991')
    request('/v1/chutni/scopes/' + waiting_scope + '/forget', {'confirm': True})
    assert pathlib.Path(str(waiting) + '.chutni').exists()
    report(waiting, 'registration_required')
    unreadable = folder / 'unreadable.txt'
    unreadable.write_text('This unreadable source must not count as complete coverage.\n')
    unreadable.chmod(0)
    _, _, partial_build = report(folder)
    partial = ready(scope, partial_build)
    assert partial['state'] == 'ready_partial' and not partial['complete_for_policy'], partial
    assert partial['scan_errors'] > 0, partial
    receipt.write_text(json.dumps({'scope': scope, 'job': second_job}))
else:
    saved = json.loads(receipt.read_text())
    assert 'AUTOMEM_NEW_991' in query(saved['scope'], 'AUTOMEM_NEW_991')
    assert 'AUTOMEM_OLD_731' not in query(saved['scope'], 'AUTOMEM_OLD_731')
    events = request('/v1/jobs/history/events?job_id=' + saved['job'])
    assert saved['scope'] in events and 'folder_memory' in events, events
print('automatic folder memory ' + phase + ': PASS')
