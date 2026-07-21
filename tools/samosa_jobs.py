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
import json
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jobs_fs as fs
import samosa_tools
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

_ORGANIZE_RE = re.compile(r'\b(organi[sz]e|sort|arrange|tidy|group)\b')
_BY_TYPE_RE = re.compile(r'\b(type|types|kind|extension|extensions|format|formats|file type)\b')
_REPORT_RE = re.compile(r'\b(report|count|how many|summar|inventory|breakdown|what.?s in)\b')
_FIND_RE = re.compile(r'\b(find|locate|search|look for|where is|which file)\b')
_RECEIPT_RE = re.compile(r'\b(receipt|receipts|invoice|invoices|merchant|total|purchase)\b')
_DATE_RE = re.compile(r'\b(date|day|month|year)\b')
_PHOTO_RE = re.compile(r'\b(photo|photos|picture|pictures|image|images|jpg|jpeg|png)\b')
_PEOPLE_RE = re.compile(r'\b(people|person|persons|human|humans|two people|2 people)\b')

JOB_TEMPLATES = {
    'folder-report': {
        'intent': 'report',
        'instruction': None,
        'output_schema': None,
        'inference': None,
    },
    'sort-by-type': {
        'intent': 'organize',
        'organize': {
            'rule': {'by': 'extension'},
            'dest_root': None,
            'on_collision': 'skip',
            'unmatched': 'leave',
        },
    },
    'receipts-by-date': {
        'intent': 'extract-and-organize',
        'instruction': "Extract the receipt date (YYYY-MM-DD), merchant name, and total amount.",
        'output_schema': {
            'type': 'object',
            'properties': {
                'date': {'type': 'string'},
                'merchant': {'type': 'string'},
                'total': {'type': 'number'},
            },
            'required': ['date', 'merchant', 'total'],
        },
        'organize': {
            'rule': {'by': 'field', 'field': 'date'},
            'dest_root': None,
            'on_collision': 'skip',
            'unmatched': 'leave',
        },
    },
    'photos-two-people': {
        'intent': 'vision-and-organize',
        'instruction': "Count the people visible in this image.",
        'output_schema': {
            'type': 'object',
            'properties': {
                'people': {'type': 'integer', 'minimum': 0, 'maximum': 20},
            },
            'required': ['people'],
        },
        'organize': {
            'rule': {'by': 'where', 'field': 'people', 'op': 'eq',
                     'value': 2, 'dest': 'Two people'},
            'dest_root': None,
            'on_collision': 'skip',
            'unmatched': 'leave',
        },
    },
}


def _suggest_template(goal):
    g = (goal or '').lower()
    if _REPORT_RE.search(g) and not _ORGANIZE_RE.search(g):
        return 'folder-report', 'deterministic'
    if _ORGANIZE_RE.search(g) and _BY_TYPE_RE.search(g):
        return 'sort-by-type', 'deterministic'
    if _RECEIPT_RE.search(g) and (_DATE_RE.search(g) or _ORGANIZE_RE.search(g)):
        return 'receipts-by-date', 'deterministic'
    if _PHOTO_RE.search(g) and _PEOPLE_RE.search(g):
        return 'photos-two-people', 'deterministic'
    return None, 'unsupported'


def _suggest_template_with_model(goal, model_call):
    try:
        reply = model_call([
            {'role': 'system',
             'content': "Select exactly one shipped Samosa job template for the "
                        "user request. Reply with JSON only: "
                        "{\"template\":\"folder-report|sort-by-type|receipts-by-date|"
                        "photos-two-people|unsupported\"}. Never invent a template."},
            {'role': 'user', 'content': goal},
        ])
    except Exception:
        return None
    try:
        data = json.loads((reply or '').strip())
    except ValueError:
        return None
    template = data.get('template') if isinstance(data, dict) else None
    return template if template in JOB_TEMPLATES else None


def _job_from_template(template_id, goal, folder):
    template = JOB_TEMPLATES[template_id]
    job = {
        'job_id': slugify(template_id + '-' + (goal or ''), maxlen=48),
        'schema_version': 1,
        'intent': template['intent'],
        'input': {'folder': folder},
    }
    for key in ('instruction', 'output_schema', 'inference', 'organize'):
        if key in template:
            job[key] = template[key]
    if 'instruction' not in job and template_id == 'sort-by-type':
        job['instruction'] = None
        job['output_schema'] = None
        job['inference'] = None
    return json.loads(json.dumps(job))


def suggest_job(goal, folder, model_call=None, out_path=None):
    """Compile plain English into one of the shipped job templates.

    The model may help select a template, but it cannot invent a workflow. The
    returned payload always includes either a complete editable job definition
    or an explicit unsupported reason.
    """
    folder = os.path.abspath(folder)
    template_id, source = _suggest_template(goal)
    if template_id is None and model_call is not None:
        template_id = _suggest_template_with_model(goal, model_call)
        source = 'model' if template_id else 'unsupported'
    if template_id is None:
        return {
            'ok': False,
            'reason': 'no shipped job shape matches this request yet',
            'supported_templates': sorted(JOB_TEMPLATES),
        }
    job = _job_from_template(template_id, goal, folder)
    result = {'ok': True, 'template': template_id, 'source': source, 'job': job}
    if out_path:
        fs.atomic_write(out_path, json.dumps(job, indent=2, sort_keys=True) + "\n")
        result['path'] = os.path.abspath(out_path)
    return result


def decode_intent(goal, folder, model_call=None):
    """Map a natural-language goal to a structured, deterministic intent.

    Returns {kind, rule?, explain}. `kind` is 'organize' (moves files),
    'report' (read-only), or 'find' (read-only tool loop). Keyword rules
    resolve the common cases with no model; `model_call`, when given, is used
    only to refine an ambiguous goal and can never turn an explicit read-only
    request into a destructive move on its own.
    """
    g = (goal or '').lower()
    if _REPORT_RE.search(g) and not _ORGANIZE_RE.search(g):
        return {'kind': 'report',
                'explain': "Look through the folder and report what is there, by file type."}

    if _FIND_RE.search(g) and not _ORGANIZE_RE.search(g):
        return {'kind': 'find',
                'explain': "Search through the folder using read-only tools and report the matching path."}

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
                            "by type; 'find' if the user wants to locate a specific file "
                            "or record; otherwise 'report'. No other words."},
                {'role': 'user', 'content': goal},
            ])
            label = (reply or '').strip().lower()[:20]
            if 'organize' in label:
                return {'kind': 'organize', 'rule': {'by': 'extension'},
                        'explain': "Sort the files into folders named for their type."}
            if 'find' in label:
                return {'kind': 'find',
                        'explain': "Search through the folder using read-only tools and report the matching path."}
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


FIND_TOOLS = ['fs_survey', 'fs_list', 'fs_metadata', 'fs_detect_type',
              'fs_read_text', 'fs_read_document', 'fs_read_page',
              'notes_append', 'notes_read', 'ask_user', 'fs_move']


def run_job(goal, folder, mode='confirm', model_call=None, loop_model_call=None):
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

    # Survey / count — the same for every kind. This uses the compiled
    # metadata sidecar so large-file scans are contained outside the gateway.
    survey, survey_error = fs.fs_sidecar_survey(folder, recursive=False)
    if survey_error:
        yield log.append('error', message=survey_error)
        return
    by_type = {k: v.get('count', 0) for k, v in survey.get('by_type', {}).items()
               if isinstance(v, dict)}
    total = int(survey.get('total', 0))
    skipped_count = int(survey.get('skipped_count', 0))
    yield log.append('counting', total=total, skipped=skipped_count, by_type=by_type)

    if intent['kind'] == 'report':
        yield log.append('report', total=total, by_type=by_type)
        yield log.append('done', summary=_report_summary(total, by_type))
        return

    if intent['kind'] == 'find':
        call_model = loop_model_call or model_call
        if call_model is None:
            yield log.append('error', message="the model is not available for find jobs")
            return
        pending = []

        def emit_tool_event(event_type, **fields):
            pending.append(log.append(event_type, **fields))

        ctx_mode = 'execute' if mode == 'execute' else 'preview'
        ctx = ToolContext(folder, mode=ctx_mode, emit=emit_tool_event, job_dir=jdir,
                          stage_mutations=(mode == 'confirm'))
        tools = samosa_tools.REGISTRY.subset(FIND_TOOLS)
        messages = [
            {'role': 'system',
             'content': "You are running a local find job. Use metadata first, "
                        "read only likely candidates, save notes when useful, and answer "
                        "with plain sentences that include the path and why it matches. "
                        "Always pass relative paths to filesystem tools: '.', 'sub/file.pdf', "
                        "or a filename from fs_list. Never pass '/' or an absolute path. "
                        "Start with fs_list on '.' using a limit large enough to see likely "
                        "candidates before reading files. If you need clarification, call "
                        "ask_user with the question; do not end with a question as your final "
                        "answer. If the user's goal asks to move the matching file, confirm "
                        "the match first, then call fs_move with relative src and dst. In review "
                        "mode that move will pause for the user to apply. You must not delete, "
                        "rename, email, or upload files."},
            {'role': 'user',
             'content': f"Goal: {goal}\nThe working folder is already selected; use relative paths only."},
        ]
        for loop_event in samosa_tools.iter_tool_loop(call_model, messages, tools, ctx):
            while pending:
                yield pending.pop(0)
            if loop_event.get('type') == 'await_user':
                _write_convo(jdir, loop_event['convo'], loop_event['call'],
                             loop_event.get('round_i', 0))
                yield log.append('await_user', job_id=job_id,
                                 question=loop_event.get('question', ''))
                return
            if loop_event.get('type') == 'await_apply':
                moves, err = _stage_tool_move(jdir, folder, loop_event.get('call') or {})
                if err:
                    yield log.append('error', message=err)
                    return
                yield log.append('plan',
                                 moves=[{'src': m['src'], 'dst': m['dst']} for m in moves],
                                 skips=[], dest_root=os.path.dirname(moves[0]['dst']))
                yield log.append('await_apply', job_id=job_id, moves=len(moves))
                return
            if loop_event.get('type') == 'final':
                final_text = (loop_event.get('text') or '').strip()
                if _looks_like_question(final_text):
                    call = {'samosa_tool': 'ask_user', 'question': final_text}
                    _write_convo(jdir, loop_event.get('convo') or messages, call, 0)
                    yield log.append('await_user', job_id=job_id, question=final_text)
                    return
                yield log.append('done', summary=final_text or "Find job completed without a summary.")
                return
        yield log.append('done', summary="Find job completed without a summary.")
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


def answer_job(job_id, answer, loop_model_call=None):
    """Resume a paused find job after an ask_user answer."""
    jdir = job_dir_for(job_id)
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    log.load()
    state = _read_convo(jdir)
    if not state:
        yield log.append('error', message=f"no paused conversation for job {job_id}")
        return
    if loop_model_call is None:
        yield log.append('error', message="the model is not available to resume this job")
        return
    folder = _job_folder(jdir)
    if not folder or not os.path.isdir(folder):
        yield log.append('error', message=f"folder for job {job_id} is unavailable")
        return
    call = state.get('call') or {'samosa_tool': 'ask_user'}
    convo = list(state.get('convo') or [])
    convo.append({'role': 'assistant', 'content': _dumps(call)})
    convo.append({'role': 'user',
                  'content': f"SAMOSA_TOOL_RESULT ask_user\n{answer}\n\n(Continue the job now.)"})
    pending = []

    def emit_tool_event(event_type, **fields):
        pending.append(log.append(event_type, **fields))

    ctx = ToolContext(folder, mode='preview', emit=emit_tool_event, job_dir=jdir,
                      stage_mutations=True)
    tools = samosa_tools.REGISTRY.subset(FIND_TOOLS)
    for loop_event in samosa_tools.iter_tool_loop(
            loop_model_call, convo, tools, ctx, add_ability_prompt=False):
        while pending:
            yield pending.pop(0)
        if loop_event.get('type') == 'await_user':
            _write_convo(jdir, loop_event['convo'], loop_event['call'],
                         loop_event.get('round_i', 0))
            yield log.append('await_user', job_id=job_id,
                             question=loop_event.get('question', ''))
            return
        if loop_event.get('type') == 'await_apply':
            moves, err = _stage_tool_move(jdir, folder, loop_event.get('call') or {})
            if err:
                yield log.append('error', message=err)
                return
            yield log.append('plan',
                             moves=[{'src': m['src'], 'dst': m['dst']} for m in moves],
                             skips=[], dest_root=os.path.dirname(moves[0]['dst']))
            yield log.append('await_apply', job_id=job_id, moves=len(moves))
            return
        if loop_event.get('type') == 'final':
            final_text = (loop_event.get('text') or '').strip()
            if _looks_like_question(final_text):
                call = {'samosa_tool': 'ask_user', 'question': final_text}
                _write_convo(jdir, loop_event.get('convo') or convo, call, 0)
                yield log.append('await_user', job_id=job_id, question=final_text)
                return
            _clear_convo(jdir)
            yield log.append('done', summary=final_text or "Find job completed without a summary.")
            return
    _clear_convo(jdir)
    yield log.append('done', summary="Find job completed without a summary.")


def undo_job(job_id):
    """Revert the applied moves of a job (dst -> src). Yields events.

    Safe to call twice: a prior undo's own 'revert' actions, and any move
    already reverted, are excluded from the re-scan rather than replayed —
    otherwise a second call would re-report the first undo's work as fresh
    (spurious) skips instead of cleanly saying there is nothing left to do.
    """
    jdir = job_dir_for(job_id)
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    log.load()
    already_reverted = {(e['src'], e['dst']) for e in log.events
                        if e['type'] == 'action' and e.get('op') == 'revert' and e.get('ok')}
    applied = [e for e in log.events
              if e['type'] == 'action' and e.get('op') == 'move' and e.get('ok')
              and (e['src'], e['dst']) not in already_reverted]
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

def _stage_tool_move(jdir, folder, call):
    if call.get('samosa_tool') != 'fs_move':
        return None, f"cannot stage unsupported mutating tool {call.get('samosa_tool')!r}"
    try:
        ctx = ToolContext(folder, mode='preview')
        src = ctx.resolve(call.get('src'), must_exist=True)
        dst = ctx.resolve(call.get('dst'))
    except samosa_tools.ToolError as e:
        return None, f"move refused: {e}"
    try:
        st = os.stat(src)
    except OSError as e:
        return None, f"move refused: cannot stat source: {e}"
    if not fs.stat_is_regular(st):
        return None, "move refused: source is not a regular file"
    move = {'src': src, 'dst': dst, 'size': st.st_size, 'mtime': st.st_mtime}
    _write_plan(jdir, [move])
    return [move], None


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


def _convo_path(jdir):
    return os.path.join(jdir, 'convo.json')


def _write_convo(jdir, convo, call, round_i):
    fs.atomic_write(_convo_path(jdir), _dumps({
        'convo': convo,
        'call': call,
        'round_i': round_i,
    }))


def _read_convo(jdir):
    import json
    try:
        with open(_convo_path(jdir)) as f:
            state = json.load(f)
    except (OSError, ValueError):
        return None
    return state if isinstance(state, dict) else None


def _clear_convo(jdir):
    try:
        os.unlink(_convo_path(jdir))
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


def _looks_like_question(text):
    return bool(text and text.rstrip().endswith('?'))


# --- Minimal terminal CLI --------------------------------------------------
# The app (gateway Jobs tab) is the primary way to run jobs; this CLI is a thin
# convenience for the terminal and for end-to-end testing. It prints the same
# event stream the UI renders, one readable line per action.

def _print_event(evt):
    t = evt.get('type')
    if t == 'decode_intent':
        print(f"• decoding intent: {evt.get('goal')!r}")
    elif t == 'intent':
        print(f"• intent: {evt.get('kind')} — {evt.get('explain')}")
    elif t == 'counting':
        parts = ", ".join(f"{v} {k}" for k, v in sorted(evt.get('by_type', {}).items()))
        print(f"• counting files: {evt.get('total')} found" + (f" ({parts})" if parts else ""))
    elif t == 'plan':
        print(f"• planned {len(evt.get('moves', []))} move(s), {len(evt.get('skips', []))} skip(s)")
    elif t == 'await_apply':
        print(f"• plan ready — apply with:  samosa jobs apply {evt.get('job_id')}")
    elif t == 'action':
        verb = 'restore' if evt.get('op') == 'revert' else 'move'
        state = 'ok' if evt.get('ok') else f"skip ({evt.get('reason')})"
        print(f"  [{evt.get('i')}/{evt.get('n')}] {verb} {os.path.basename(evt.get('src',''))} … {state}")
    elif t in ('applied', 'reverted', 'report'):
        pass
    elif t == 'done':
        print(f"✓ {evt.get('summary')}")
    elif t == 'error':
        print(f"✗ error: {evt.get('message')}")


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print("Usage:\n"
              "  samosa jobs suggest-job \"<goal>\" <folder> [--out <job.json>]\n"
              "  samosa jobs run \"<goal>\" <folder> [--execute]\n"
              "  samosa jobs apply <job_id>\n"
              "  samosa jobs undo <job_id>", file=sys.stderr)
        return 2
    cmd = argv[0]
    if cmd == 'suggest-job':
        rest = argv[1:]
        out_path = None
        if '--out' in rest:
            idx = rest.index('--out')
            if idx + 1 >= len(rest):
                print("Usage: samosa jobs suggest-job \"<goal>\" <folder> [--out <job.json>]",
                      file=sys.stderr)
                return 2
            out_path = rest[idx + 1]
            rest = rest[:idx] + rest[idx + 2:]
        if len(rest) < 2:
            print("Usage: samosa jobs suggest-job \"<goal>\" <folder> [--out <job.json>]",
                  file=sys.stderr)
            return 2
        result = suggest_job(rest[0], rest[1], out_path=out_path)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result.get('ok') else 1
    if cmd == 'run':
        rest = [a for a in argv[1:] if a != '--execute']
        mode = 'execute' if '--execute' in argv[1:] else 'confirm'
        if len(rest) < 2:
            print("Usage: samosa jobs run \"<goal>\" <folder> [--execute]", file=sys.stderr)
            return 2
        goal, folder = rest[0], rest[1]
        gen = run_job(goal, folder, mode=mode)
    elif cmd == 'apply' and len(argv) >= 2:
        gen = apply_job(argv[1])
    elif cmd == 'undo' and len(argv) >= 2:
        gen = undo_job(argv[1])
    else:
        print(f"unknown jobs command: {cmd}", file=sys.stderr)
        return 2

    had_error = False
    for evt in gen:
        _print_event(evt)
        if evt.get('type') == 'error':
            had_error = True
    return 1 if had_error else 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
