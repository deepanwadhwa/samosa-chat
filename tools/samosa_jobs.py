#!/usr/bin/env python3
"""The Jobs layer: utility software built on models + tools.

A *job* is a goal in plain English plus a folder. `run_job` decodes the goal
into a deterministic plan, surveys the folder, compiles the steps, and — for
read-only work — runs them; for work that moves files it stops with the plan so
you can look before anything moves. Every step is yielded as an event and
persisted to an append-only log, so the UI can render exactly what the job is
doing, in order: *decoding intent · counting files · planning · moving 3/142*.

Layering (owner's model): this module drives a *model* (to decode intent) and
uses *tools* (`jobs_fs` primitives, via `samosa_tools`) to act. It owns no
transport and no model backend — the gateway injects a `model_call` and streams
the events. Standard library only.

The event stream (each event: {seq, ts, type, ...}):
  decode_intent  {goal}
  intent         {kind, rule, explain}
  counting       {total, skipped, by_type}
  plan           {moves:[{src,dst}], skips:[{src,reason}], dest_root}
  await_apply    {job_id, moves}         (confirm mode, when there are moves)
  action         {i, n, src, dst, ok, reason}
  applied        {applied, skipped}
  reverted       {reverted, skipped}
  report         {total, by_type}        (read-only jobs)
  done           {summary}
  error          {message}
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jobs_fs as fs
from samosa_tools import ToolContext


# --- Job directory / identity ---------------------------------------------

def get_jobs_root():
    root = os.environ.get('SAMOSA_JOBS_DIR')
    if not root:
        root = os.path.join(os.path.expanduser('~'), '.samosa', 'jobs')
    return root


def slugify(text, maxlen=32):
    slug = re.sub(r'[^a-z0-9]+', '-', (text or '').lower()).strip('-')[:maxlen].strip('-')
    return slug or 'job'


def new_job_id(goal):
    return f"{slugify(goal)}-{int(time.time())}"


def job_dir_for(job_id):
    return os.path.join(get_jobs_root(), job_id)


# --- Intent decode ---------------------------------------------------------

_ORGANIZE_RE = re.compile(r'\b(organi[sz]e|sort|arrange|tidy|group|file)\b')
_BY_TYPE_RE = re.compile(r'\b(type|types|kind|extension|extensions|format|formats|file type)\b')
_REPORT_RE = re.compile(r'\b(report|count|how many|summar|inventory|breakdown|what.?s in)\b')


def decode_intent(goal, folder, model_call=None):
    """Map a natural-language goal to a structured, deterministic intent.

    Returns {kind, rule?, explain}. `kind` is 'organize' (moves files) or
    'report' (read-only). Keyword rules resolve the common cases with no model;
    `model_call`, when given, is used only to refine an ambiguous goal and can
    never turn a report into a destructive move on its own.
    """
    g = (goal or '').lower()
    if _REPORT_RE.search(g) and not _ORGANIZE_RE.search(g):
        return {'kind': 'report',
                'explain': "Look through the folder and report what is there, by file type."}

    if _ORGANIZE_RE.search(g):
        # "by type"/"by extension" → the deterministic extension rule.
        return {'kind': 'organize', 'rule': {'by': 'extension'},
                'explain': "Sort the files into folders named for their type "
                           "(PDF, JPG, PNG, TXT …), by file extension and content."}

    # Ambiguous. Ask the model to classify, if we have one; else default to a
    # safe read-only report.
    if model_call is not None:
        try:
            reply = model_call([
                {'role': 'system',
                 'content': "Classify a file-management request. Reply with ONE word: "
                            "'organize' if the user wants files sorted/moved into folders "
                            "by type, otherwise 'report'. No other words."},
                {'role': 'user', 'content': goal},
            ])
            if reply and 'organize' in reply.strip().lower()[:20]:
                return {'kind': 'organize', 'rule': {'by': 'extension'},
                        'explain': "Sort the files into folders named for their type."}
        except Exception:
            pass

    return {'kind': 'report',
            'explain': "I read this as a request to look through the folder and "
                       "report what is there, by file type."}


# --- The runner ------------------------------------------------------------

class _Emitter:
    """Yields events to the caller and mirrors each to the job's EventLog."""

    def __init__(self, log):
        self.log = log

    def make(self, event_type, **fields):
        evt = self.log.append(event_type, **fields)
        return evt


def run_job(goal, folder, mode='confirm', model_call=None):
    """Run a job, yielding event dicts as it goes.

    mode:
      'confirm' — read-only jobs complete; jobs that move files stop at
                  'await_apply' with the plan persisted for apply_job().
      'execute' — jobs that move files apply immediately.

    The generator persists every event to <job_dir>/events.jsonl and, for move
    jobs, writes the plan to <job_dir>/plan.jsonl so a later apply_job()/undo_job()
    can act on it.
    """
    folder = os.path.abspath(folder)
    if not os.path.isdir(folder):
        yield {'type': 'error', 'message': f"folder does not exist: {folder}"}
        return

    job_id = new_job_id(goal)
    jdir = job_dir_for(job_id)
    os.makedirs(jdir, exist_ok=True)
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    fs.atomic_write(os.path.join(jdir, 'job.json'),
                    _dumps({'job_id': job_id, 'goal': goal, 'folder': folder, 'mode': mode}))

    yield log.append('decode_intent', job_id=job_id, goal=goal, folder=folder)
    intent = decode_intent(goal, folder, model_call)
    yield log.append('intent', kind=intent['kind'], rule=intent.get('rule'),
                     explain=intent['explain'])

    # Survey / count — the same for every kind.
    items, skipped = fs.discover_files({'folder': folder, 'recursive': False},
                                       is_metadata_only=True)
    by_type = {k: v['count'] for k, v in fs.count_by_type(items).items()}
    yield log.append('counting', total=len(items), skipped=len(skipped), by_type=by_type)

    if intent['kind'] == 'report':
        yield log.append('report', total=len(items), by_type=by_type)
        yield log.append('done', summary=_report_summary(len(items), by_type))
        return

    # Organize: compile the deterministic plan.
    spec = {'input': {'folder': folder, 'recursive': False},
            'organize': {'rule': intent['rule']}}
    plan, err = fs.build_organize_plan(spec, jdir)
    if err:
        yield log.append('error', message=err)
        return
    moves = [m for m in plan if 'dst' in m]
    skips = [m for m in plan if 'skip' in m]
    dest_root = os.path.abspath(os.path.join(folder, 'Organized'))
    yield log.append('plan',
                     moves=[{'src': m['src'], 'dst': m['dst']} for m in moves],
                     skips=[{'src': s['src'], 'reason': s['skip']} for s in skips],
                     dest_root=dest_root)

    _write_plan(jdir, moves)

    if not moves:
        yield log.append('done', summary="Nothing to move — every file is already sorted.")
        return

    if mode == 'confirm':
        yield log.append('await_apply', job_id=job_id, moves=len(moves))
        return

    # execute mode: apply now.
    for evt in _apply(jdir, log, moves):
        yield evt


def apply_job(job_id, emit_only_new=True):
    """Apply the persisted plan of a previously planned job. Yields events."""
    jdir = job_dir_for(job_id)
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    log.load()
    moves = _read_plan(jdir)
    if not moves:
        yield log.append('error', message=f"no pending plan for job {job_id}")
        return
    for evt in _apply(jdir, log, moves):
        yield evt


def undo_job(job_id):
    """Revert the applied moves of a job (dst -> src). Yields events."""
    jdir = job_dir_for(job_id)
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    log.load()
    applied = [e for e in log.events if e['type'] == 'action' and e.get('ok')]
    if not applied:
        yield log.append('error', message=f"nothing to undo for job {job_id}")
        return
    reverted = 0
    skipped = 0
    n = len(applied)
    # Reverse order so nested destinations come apart cleanly.
    for i, e in enumerate(reversed(applied), 1):
        ok, reason = fs.revert_move({'src': e['src'], 'dst': e['dst']})
        reverted += 1 if ok else 0
        skipped += 0 if ok else 1
        yield log.append('action', op='revert', i=i, n=n, src=e['src'], dst=e['dst'],
                         ok=ok, reason=reason)
    yield log.append('reverted', reverted=reverted, skipped=skipped)
    yield log.append('done', summary=f"Undo complete: {reverted} restored, {skipped} skipped.")


# --- Internals -------------------------------------------------------------

def _apply(jdir, log, moves):
    applied = 0
    skipped = 0
    n = len(moves)
    folder = _job_folder(jdir)
    for i, m in enumerate(moves, 1):
        ok, reason = fs.apply_move(m, input_folder=folder)
        applied += 1 if ok else 0
        skipped += 0 if ok else 1
        yield log.append('action', op='move', i=i, n=n, src=m['src'], dst=m['dst'],
                         ok=ok, reason=reason)
    _clear_plan(jdir)
    yield log.append('applied', applied=applied, skipped=skipped)
    yield log.append('done',
                     summary=f"Moved {applied} file{'' if applied == 1 else 's'}"
                             + (f", skipped {skipped}" if skipped else "") + ".")


def _report_summary(total, by_type):
    if not total:
        return "The folder has no readable files."
    parts = ", ".join(f"{v} {k}" for k, v in sorted(by_type.items()))
    return f"{total} files: {parts}."


def _write_plan(jdir, moves):
    path = os.path.join(jdir, 'plan.jsonl')
    fs.atomic_write(path, "".join(_dumps(m) + "\n" for m in moves))


def _read_plan(jdir):
    path = os.path.join(jdir, 'plan.jsonl')
    if not os.path.exists(path):
        return []
    import json
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
    return out


def _clear_plan(jdir):
    path = os.path.join(jdir, 'plan.jsonl')
    try:
        os.unlink(path)
    except OSError:
        pass


def _job_folder(jdir):
    import json
    try:
        with open(os.path.join(jdir, 'job.json')) as f:
            return json.load(f).get('folder')
    except (OSError, ValueError):
        return None


def _dumps(obj):
    import json
    return json.dumps(obj, separators=(',', ':'))
