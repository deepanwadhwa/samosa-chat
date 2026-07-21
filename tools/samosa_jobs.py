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
import plistlib
import platform
import re
import subprocess
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
DEFAULT_PREFILL_TOKENS_PER_SECOND = float(os.environ.get('SAMOSA_JOB_PREFILL_TPS', '20'))
DEFAULT_DECODE_TOKENS_PER_SECOND = float(os.environ.get('SAMOSA_JOB_DECODE_TPS', '6'))
DEFAULT_JOB_MAX_TOKENS = int(os.environ.get('SAMOSA_JOB_MAX_TOKENS', '512'))
PREVIEW_SAMPLE_TARGET = 3
DEFAULT_OVERNIGHT_WINDOW = ('22:00', '06:00')
DEFAULT_MISSED_POLICY = 'skip'


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
    result = {'ok': True, 'template': template_id, 'source': source, 'job': job,
              'estimate': estimate_job(job)}
    if out_path:
        fs.atomic_write(out_path, json.dumps(job, indent=2, sort_keys=True) + "\n")
        result['path'] = os.path.abspath(out_path)
    return result


def estimate_job(job, token_counter=None):
    """Estimate unit count and wall-clock for a job definition.

    `token_counter(text) -> int` may provide exact model-token counts. Without
    it the estimate still reports useful unit/decode cost, but marks token
    counts as conservative byte/word estimates.
    """
    folder = os.path.abspath((job.get('input') or {}).get('folder') or '')
    instruction = job.get('instruction') or ''
    schema = job.get('output_schema')
    has_model_work = bool(instruction or schema)
    items, skipped = fs.discover_files(job.get('input') or {},
                                       is_metadata_only=not has_model_work)
    unit_count = len(items)
    max_tokens = int((job.get('inference') or {}).get('max_tokens') or DEFAULT_JOB_MAX_TOKENS)
    if not has_model_work:
        return {
            'ok': True,
            'folder': folder,
            'unit_count': unit_count,
            'skipped_count': len(skipped),
            'model_units': 0,
            'input_tokens': 0,
            'output_tokens': 0,
            'token_counts_exact': True,
            'prefill_tokens_per_second': DEFAULT_PREFILL_TOKENS_PER_SECOND,
            'decode_tokens_per_second': DEFAULT_DECODE_TOKENS_PER_SECOND,
            'estimated_wall_seconds': 0,
            'battery_policy': 'run manually; daemon battery policy is not active yet',
        }
    prompt_prefix = instruction + "\n" + json.dumps(schema, sort_keys=True, separators=(',', ':'))
    base_tokens, exact = _count_text_tokens(prompt_prefix, token_counter)
    input_tokens = 0
    for item in items:
        path = item['input_path']
        if item.get('media_type') == 'text/plain':
            try:
                with open(path, 'r', encoding='utf-8') as f:
                    text = f.read()
            except (OSError, UnicodeDecodeError):
                text = ''
                exact = False
            count, item_exact = _count_text_tokens(text, token_counter)
            exact = exact and item_exact
            input_tokens += base_tokens + count
        else:
            exact = False
            input_tokens += base_tokens + max(1, min(8192, int((item.get('size') or 0) / 4)))
    output_tokens = unit_count * max_tokens
    prefill_tps = DEFAULT_PREFILL_TOKENS_PER_SECOND
    decode_tps = DEFAULT_DECODE_TOKENS_PER_SECOND
    estimated = (input_tokens / prefill_tps if prefill_tps > 0 else 0) + \
        (output_tokens / decode_tps if decode_tps > 0 else 0)
    return {
        'ok': True,
        'folder': folder,
        'unit_count': unit_count,
        'skipped_count': len(skipped),
        'model_units': unit_count,
        'input_tokens': input_tokens,
        'output_tokens': output_tokens,
        'token_counts_exact': exact,
        'prefill_tokens_per_second': prefill_tps,
        'decode_tokens_per_second': decode_tps,
        'estimated_wall_seconds': round(estimated, 1),
        'estimated_wall_human': _human_seconds(estimated),
        'battery_policy': 'run manually; daemon battery policy is not active yet',
    }


def select_preview_items(job, sample_count=1):
    """Return deterministic preview input items for a job definition.

    The default remains one unit.  When callers ask for more, choose up to
    `sample_count` files while spreading picks across media types where the
    folder contents allow it.  This is selection-only: actual preview runners
    should write their artifacts under preview/, never results/.
    """
    try:
        requested = int(sample_count)
    except (TypeError, ValueError):
        requested = 1
    requested = max(1, requested)
    has_model_work = bool(job.get('instruction') or job.get('output_schema'))
    items, skipped = fs.discover_files(job.get('input') or {},
                                       is_metadata_only=not has_model_work)
    path_ordered = sorted(items, key=lambda item: str(item.get('input_path') or ''))
    items = sorted(items, key=lambda item: (
        str(item.get('media_type') or ''),
        str(item.get('input_path') or ''),
        str(item.get('input_sha256') or ''),
    ))
    if requested == 1 or len(items) <= 1:
        return {'ok': True, 'sample_count': min(1, len(items)),
                'items': path_ordered[:1], 'skipped_count': len(skipped),
                'artifact_dir': 'preview'}

    by_type = {}
    for item in items:
        by_type.setdefault(item.get('media_type') or '', []).append(item)
    selected = []
    selected_keys = set()
    for media_type in sorted(by_type):
        item = by_type[media_type][0]
        key = (item.get('input_path'), item.get('input_sha256'))
        selected.append(item)
        selected_keys.add(key)
        if len(selected) >= requested:
            break
    if len(selected) < requested:
        for item in items:
            key = (item.get('input_path'), item.get('input_sha256'))
            if key in selected_keys:
                continue
            selected.append(item)
            selected_keys.add(key)
            if len(selected) >= requested:
                break
    selected.sort(key=lambda item: str(item.get('input_path') or ''))
    return {'ok': True, 'sample_count': len(selected), 'items': selected,
            'skipped_count': len(skipped), 'artifact_dir': 'preview'}


def preview_job(job, file_path=None, sample_count=1, model_call=None):
    """Prepare preview artifacts for one or more selected units.

    Preview has its own namespace and deliberately avoids the real run's
    events/results files.  This function proves input selection and readable
    source preparation; model-backed extraction can consume the same per-unit
    source files later without changing where artifacts live.
    """
    selected = _select_preview_for_file(job, file_path) if file_path else \
        select_preview_items(job, sample_count=sample_count)
    if not selected.get('ok'):
        return selected
    preview_dir = _preview_dir(job)
    os.makedirs(preview_dir, exist_ok=True)
    items_dir = os.path.join(preview_dir, 'items')
    os.makedirs(items_dir, exist_ok=True)

    records = []
    for index, item in enumerate(selected.get('items') or [], 1):
        unit_id = _preview_unit_id(index, item)
        source_text, source_error = _preview_source_text(item)
        source_path = os.path.join(items_dir, f'{unit_id}.source.txt')
        fs.atomic_write(source_path, source_text)
        if model_call is not None and source_error is None:
            record = _model_extract_record(job, item, source_text, model_call)
            record.update({'unit_id': unit_id, 'source_path': source_path,
                           'source_chars': len(source_text)})
        else:
            record = {
                'unit_id': unit_id,
                'status': 'preview_ready' if source_error is None else 'review_required',
                'input_path': item.get('input_path'),
                'input_sha256': item.get('input_sha256'),
                'media_type': item.get('media_type'),
                'source_path': source_path,
                'source_chars': len(source_text),
            }
        if source_error is not None:
            record['reasons'] = [source_error]
            record['status'] = 'review_required'
        record.setdefault('input_path', item.get('input_path'))
        record.setdefault('input_sha256', item.get('input_sha256'))
        record.setdefault('media_type', item.get('media_type'))
        record_path = os.path.join(items_dir, f'{unit_id}.record.json')
        fs.atomic_write(record_path, json.dumps(record, indent=2, sort_keys=True) + '\n')
        records.append(record)

    manifest = {
        'ok': True,
        'artifact_dir': 'preview',
        'preview_dir': preview_dir,
        'sample_count': len(records),
        'expanded': len(records) > 1,
        'records': records,
    }
    fs.atomic_write(os.path.join(preview_dir, 'manifest.json'),
                    json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    fs.atomic_write(os.path.join(preview_dir, 'records.jsonl'),
                    ''.join(json.dumps(r, separators=(',', ':'), sort_keys=True) + '\n'
                            for r in records))
    return manifest


# --- Daemon / scheduler primitives ----------------------------------------

def arm_scheduled_job(job_path, window_start=None, window_end=None,
                      missed_policy=DEFAULT_MISSED_POLICY, keep_awake=True):
    """Freeze a job definition and arm it for daemon pickup.

    The frozen copy lives under the jobs root so later edits to the source
    job.json do not silently alter an armed overnight run.
    """
    try:
        with open(job_path, 'r', encoding='utf-8') as f:
            job = json.load(f)
    except (OSError, ValueError) as error:
        return {'ok': False, 'reason': f'could not read job.json: {error}'}
    if not isinstance(job, dict):
        return {'ok': False, 'reason': 'job must be a JSON object'}
    job_id = job.get('job_id') or slugify(os.path.splitext(os.path.basename(job_path))[0])
    frozen = json.dumps(job, sort_keys=True, separators=(',', ':')).encode('utf-8')
    job_sha = fs.sha256_bytes(frozen)
    jdir = job_dir_for(job_id)
    os.makedirs(jdir, exist_ok=True)
    frozen_path = os.path.join(jdir, 'job.json')
    existing = _read_json_file(frozen_path)
    if existing is not None:
        existing_sha = fs.sha256_bytes(json.dumps(existing, sort_keys=True,
                                                  separators=(',', ':')).encode('utf-8'))
        if existing_sha != job_sha:
            return {'ok': False, 'reason': f'job {job_id} already armed with a different definition'}
    fs.atomic_write(frozen_path, json.dumps(job, indent=2, sort_keys=True) + '\n')

    start = window_start or DEFAULT_OVERNIGHT_WINDOW[0]
    end = window_end or DEFAULT_OVERNIGHT_WINDOW[1]
    if _parse_hhmm(start) is None or _parse_hhmm(end) is None:
        return {'ok': False, 'reason': 'window times must be HH:MM'}
    if missed_policy not in ('skip', 'run_next_start'):
        return {'ok': False, 'reason': 'missed_policy must be skip or run_next_start'}
    resources = job.get('resources') if isinstance(job.get('resources'), dict) else {}
    schedule = {
        'schema_version': 1,
        'job_id': job_id,
        'job_path': frozen_path,
        'job_sha256': job_sha,
        'source_job_path': os.path.abspath(job_path),
        'enabled': True,
        'window_start': start,
        'window_end': end,
        'missed_policy': missed_policy,
        'keep_awake': bool(keep_awake),
        'run_on_battery': bool(resources.get('run_on_battery', False)),
        'review_required_policy': 'queue',
        'armed_at': fs.rfc3339_now(),
    }
    schedule_path = os.path.join(jdir, 'schedule.json')
    fs.atomic_write(schedule_path, json.dumps(schedule, indent=2, sort_keys=True) + '\n')
    estimate = estimate_job(job)
    return {'ok': True, 'job_id': job_id, 'job_dir': jdir,
            'schedule_path': schedule_path, 'schedule': schedule,
            'estimate': estimate}


def scheduler_decision(schedule, now_minutes=None, power=None):
    """Return whether an armed schedule should run, defer, or record a miss."""
    if not schedule.get('enabled', True):
        return {'action': 'defer', 'reason': 'disabled'}
    if schedule.get('last_status') == 'complete':
        return {'action': 'defer', 'reason': 'complete'}
    now = _now_minutes() if now_minutes is None else int(now_minutes)
    power = power or {'on_battery': False, 'ac_power': True}
    if power.get('on_battery') and not schedule.get('run_on_battery', False):
        return {'action': 'defer', 'reason': 'on_battery'}
    start = _parse_hhmm(schedule.get('window_start') or DEFAULT_OVERNIGHT_WINDOW[0])
    end = _parse_hhmm(schedule.get('window_end') or DEFAULT_OVERNIGHT_WINDOW[1])
    if start is None or end is None:
        return {'action': 'defer', 'reason': 'invalid_window'}
    if _minutes_in_window(now, start, end):
        return {'action': 'run', 'reason': 'inside_window'}
    if schedule.get('missed') and schedule.get('missed_policy') == 'run_next_start':
        return {'action': 'run', 'reason': 'missed_window'}
    return {'action': 'defer', 'reason': 'outside_window'}


def record_missed_window(schedule, now_minutes=None):
    """Mark a schedule missed after its window has elapsed."""
    now = _now_minutes() if now_minutes is None else int(now_minutes)
    start = _parse_hhmm(schedule.get('window_start') or DEFAULT_OVERNIGHT_WINDOW[0])
    end = _parse_hhmm(schedule.get('window_end') or DEFAULT_OVERNIGHT_WINDOW[1])
    if start is None or end is None or _minutes_in_window(now, start, end):
        return dict(schedule)
    updated = dict(schedule)
    updated['missed'] = True
    updated['missed_recorded_at_minutes'] = now
    return updated


def caffeinate_command(command, keep_awake=True, system_name=None):
    """Wrap a command with macOS caffeinate when requested."""
    if not keep_awake:
        return list(command)
    system_name = system_name or platform.system()
    if system_name == 'Darwin':
        return ['caffeinate', '-dimsu', *command]
    return list(command)


def host_power_status(system_name=None, pmset_output=None):
    """Return {'ac_power': bool, 'on_battery': bool} for scheduler policy."""
    system_name = system_name or platform.system()
    if system_name != 'Darwin':
        return {'ac_power': True, 'on_battery': False, 'source': 'default_ac'}
    if pmset_output is None:
        try:
            pmset_output = subprocess.check_output(
                ['pmset', '-g', 'batt'], text=True, stderr=subprocess.DEVNULL, timeout=3)
        except (OSError, subprocess.SubprocessError):
            return {'ac_power': False, 'on_battery': True, 'source': 'unknown'}
    lowered = pmset_output.lower()
    on_battery = 'battery power' in lowered
    ac_power = 'ac power' in lowered or 'charged' in lowered
    return {'ac_power': ac_power and not on_battery,
            'on_battery': on_battery,
            'source': 'pmset'}


def launchd_plist(program_args, label='com.samosa.jobsd', interval_seconds=300,
                  log_dir=None):
    """Generate a launchd plist for polling the jobs daemon on macOS."""
    log_dir = log_dir or os.path.join(os.path.expanduser('~'), '.samosa', 'logs')
    payload = {
        'Label': label,
        'ProgramArguments': list(program_args),
        'RunAtLoad': True,
        'StartInterval': int(interval_seconds),
        'StandardOutPath': os.path.join(log_dir, 'jobsd.out.log'),
        'StandardErrorPath': os.path.join(log_dir, 'jobsd.err.log'),
    }
    return plistlib.dumps(payload, sort_keys=True).decode('utf-8')


def install_launchd_plist(dest_path=None, program_args=None):
    """Write the macOS launchd plist; loading it is an explicit user action."""
    dest = dest_path or os.path.join(os.path.expanduser('~'), 'Library',
                                     'LaunchAgents', 'com.samosa.jobsd.plist')
    args = program_args or [sys.executable, os.path.abspath(__file__), 'jobsd-once']
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    fs.atomic_write(dest, launchd_plist(args), mode=0o644)
    return {'ok': True, 'path': dest, 'label': 'com.samosa.jobsd',
            'load_command': ['launchctl', 'load', dest],
            'unload_command': ['launchctl', 'unload', dest]}


def arm_overnight_job(job_path, missed_policy=DEFAULT_MISSED_POLICY):
    """Manual overnight flow: freeze the job and return the daemon commands."""
    result = arm_scheduled_job(job_path, window_start=DEFAULT_OVERNIGHT_WINDOW[0],
                               window_end=DEFAULT_OVERNIGHT_WINDOW[1],
                               missed_policy=missed_policy, keep_awake=True)
    if not result.get('ok'):
        return result
    command = caffeinate_command([sys.executable, os.path.abspath(__file__), 'jobsd-once'],
                                 keep_awake=result['schedule'].get('keep_awake', True))
    return {**result, 'overnight': True, 'manual_run_command': command}


def list_armed_schedules():
    root = get_jobs_root()
    schedules = []
    if not os.path.isdir(root):
        return schedules
    for entry in sorted(os.listdir(root)):
        path = os.path.join(root, entry, 'schedule.json')
        schedule = _read_json_file(path)
        if schedule is not None:
            schedule = dict(schedule)
            schedule['schedule_path'] = path
            schedules.append(schedule)
    return schedules


def jobsd_once(now_minutes=None, power=None):
    """Evaluate armed schedules once.

    This is the launchd-friendly polling unit.  It intentionally queues review
    work instead of prompting: review-required items are represented by job
    artifacts/events, never by a daemon-blocking question.
    """
    decisions = []
    power = power or host_power_status()
    for schedule in list_armed_schedules():
        decision = scheduler_decision(schedule, now_minutes=now_minutes, power=power)
        if decision.get('action') == 'run':
            run_result = run_scheduled_job(schedule)
            decision = {**decision, 'run': run_result}
        elif decision.get('reason') == 'outside_window':
            updated = record_missed_window(schedule, now_minutes=now_minutes)
            if updated != schedule:
                _write_json_file(schedule.get('schedule_path'), updated)
        decisions.append({'job_id': schedule.get('job_id'),
                          'schedule_path': schedule.get('schedule_path'),
                          **decision})
    return {'ok': True, 'decisions': decisions}


def run_scheduled_job(schedule):
    """Execute one due frozen job definition."""
    job_path = schedule.get('job_path')
    job = _read_json_file(job_path)
    if job is None:
        return _finish_schedule(schedule, 'failed', reason='job_unavailable')
    job_id = schedule.get('job_id') or job.get('job_id') or slugify('scheduled-job')
    jdir = os.path.dirname(schedule.get('schedule_path') or job_dir_for(job_id))
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    log.load()
    log.append('scheduled_job_start', job_id=job_id, job_path=job_path)
    try:
        result = _run_job_definition(job, jdir, log)
    except Exception as error:
        log.append('error', message=str(error))
        return _finish_schedule(schedule, 'failed', reason=str(error))
    log.append('scheduled_job_complete', job_id=job_id, **result)
    return _finish_schedule(schedule, 'complete', result=result)


def run_job_definition(job, model_call=None, job_id=None):
    """Run a frozen-style job definition and yield persisted events."""
    job_id = job_id or job.get('job_id') or slugify(job.get('name') or 'job-definition')
    jdir = job_dir_for(job_id)
    os.makedirs(jdir, exist_ok=True)
    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    fs.atomic_write(os.path.join(jdir, 'job.json'),
                    json.dumps(job, indent=2, sort_keys=True) + '\n')
    yield log.append('scheduled_job_start', job_id=job_id,
                     job_path=os.path.join(jdir, 'job.json'))
    try:
        before = len(log.events)
        result = _run_job_definition(job, jdir, log, model_call=model_call)
    except Exception as error:
        yield log.append('error', message=str(error))
        return
    for event in log.events[before:]:
        yield event
    yield log.append('scheduled_job_complete', job_id=job_id, **result)


def review_items(job_id):
    """List reviewable records from <job_dir>/results/output.jsonl."""
    jdir = job_dir_for(job_id)
    records = _read_output_records(jdir)
    items = []
    for index, rec in enumerate(records):
        if not _is_review_pending(rec):
            continue
        items.append(_review_item_payload(index, rec))
    return {'ok': True, 'job_id': job_id, 'pending': len(items), 'items': items}


def correct_review_item(job_id, item, fields=None, mark_done=True):
    """Persist human corrections for one output record without rerunning a job."""
    jdir = job_dir_for(job_id)
    records = _read_output_records(jdir)
    index = _find_output_record(records, item)
    if index is None:
        return {'ok': False, 'reason': 'review item not found'}
    if fields is None:
        fields = {}
    if not isinstance(fields, dict):
        return {'ok': False, 'reason': 'fields must be an object'}
    fields = dict(fields)

    record = dict(records[index])
    extracted = record.get('extracted') if isinstance(record.get('extracted'), dict) else {}
    if not extracted:
        extracted = {k: v for k, v in record.items()
                     if k not in _OUTPUT_METADATA_KEYS and not k.startswith('review_')}
    extracted.update(fields)
    record['extracted'] = extracted
    for key, value in fields.items():
        record[key] = value
    record['review_state'] = 'done' if mark_done else 'pending'
    record['reviewed'] = bool(mark_done)
    record['corrected'] = True
    record['status'] = 'passed' if mark_done else record.get('status', 'review_required')
    record['review_corrected_at'] = fs.rfc3339_now()
    records[index] = record
    _write_output_records(jdir, records)

    log = fs.EventLog(os.path.join(jdir, 'events.jsonl'))
    log.load()
    event = log.append('review_item_done' if mark_done else 'review_item_corrected',
                       job_id=job_id, item_index=index,
                       unit_id=record.get('unit_id'),
                       input_path=record.get('input_path'),
                       fields=sorted(fields.keys()))
    return {'ok': True, 'job_id': job_id, 'item': _review_item_payload(index, record),
            'event': event, 'pending': len([r for r in records if _is_review_pending(r)])}


def _count_text_tokens(text, token_counter=None):
    if token_counter is not None:
        try:
            return int(token_counter(text or '')), True
        except Exception:
            pass
    return max(1, len(re.findall(r'\S+', text or ''))), False


def _human_seconds(seconds):
    seconds = int(round(seconds))
    if seconds < 60:
        return f"{seconds}s"
    minutes, sec = divmod(seconds, 60)
    if minutes < 60:
        return f"{minutes}m {sec}s"
    hours, minutes = divmod(minutes, 60)
    return f"{hours}h {minutes}m"


def decode_intent(goal, folder, model_call=None):
    """Map a natural-language goal to a structured, deterministic intent.

    Returns {kind, rule?, explain}. `kind` is 'organize' (moves files),
    'report' (read-only), or 'find' (read-only tool loop). Keyword rules
    resolve the common cases with no model; `model_call`, when given, is used
    only to refine an ambiguous goal and can never turn an explicit read-only
    request into a destructive move on its own.
    """
    g = (goal or '').lower()
    if _FIND_RE.search(g) and not _ORGANIZE_RE.search(g):
        return {'kind': 'find',
                'explain': "Search through the folder using read-only tools and report the matching path."}

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
              'fs_read_text', 'fs_read_pages',
              'notes_append', 'notes_read', 'ask_user', 'fs_move']
TOOL_RESULT_EVENT_PREVIEW_CHARS = 2000
FIND_CANDIDATE_LIMIT = 40

_FIND_STOP_WORDS = {
    'a', 'all', 'an', 'and', 'can', 'could', 'file', 'files', 'find', 'folder',
    'for', 'from', 'i', 'in', 'is', 'it', 'locate', 'look', 'matching', 'me',
    'my', 'of', 'on', 'please', 'record', 'records', 'search', 'show', 'tell',
    'the', 'this', 'to', 'where', 'you',
}
_FIND_DOMAIN_TERMS = {
    'cat': ('cat', 'kitten', 'pet', 'vet', 'veterinary', 'medical', 'vaccination',
            'vaccine', 'rabies', 'prescription', 'lab', 'bill', 'invoice'),
    'medical': ('medical', 'health', 'doctor', 'hospital', 'clinic', 'visit', 'lab',
                'test', 'prescription', 'vaccination', 'vaccine', 'bill', 'invoice'),
    'pet': ('pet', 'cat', 'dog', 'vet', 'veterinary', 'vaccination', 'vaccine',
            'rabies', 'prescription', 'lab', 'bill', 'invoice'),
}


def _find_candidate_names(goal, items, limit=FIND_CANDIDATE_LIMIT):
    """Rank names from the complete folder index before model tool use."""
    words = [w for w in re.findall(r'[a-z0-9]+', (goal or '').lower())
             if len(w) > 1 and w not in _FIND_STOP_WORDS]
    terms = set(words)
    for word in words:
        terms.update(_FIND_DOMAIN_TERMS.get(word, ()))
    ranked = []
    for item in items or []:
        name = str(item.get('name') or os.path.basename(item.get('input_path') or ''))
        haystack = re.sub(r'[^a-z0-9]+', ' ', name.lower())
        score = sum(4 if term in words else 1 for term in terms if term in haystack)
        if score:
            ranked.append((-score, name.lower(), name))
    ranked.sort()
    return [name for _score, _key, name in ranked[:limit]]


def _find_incomplete_question(goal):
    g = (goal or '').lower()
    if re.search(r'\b(cat|dog|pet)\b', g):
        return "I couldn't identify the right record yet. What is your pet's name?"
    return ("I couldn't identify a reliable match yet. What filename, name, date, "
            "or phrase should I search for?")


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
        yield log.append('indexing', total=total)
        indexed_items, _indexed_skips, index_error = fs.fs_sidecar_list(folder, recursive=False)
        candidate_names = [] if index_error else _find_candidate_names(goal, indexed_items)
        yield log.append('index_complete', total=len(indexed_items or []),
                         candidates=len(candidate_names))
        candidate_note = ("\nLikely candidates from all filenames and metadata:\n- " +
                          "\n- ".join(candidate_names)) if candidate_names else \
                         "\nNo filename was a clear match; ask for a distinguishing name or date."
        messages = [
            {'role': 'system',
             'content': "You are running a local find job. Use metadata first, "
                        "read only likely candidates, save notes when useful, and answer "
                        "with plain sentences that include the path and why it matches. "
                        "Always pass relative paths to filesystem tools: '.', 'sub/file.pdf', "
                        "or a filename from fs_list. Never pass '/' or an absolute path. "
                        "The goal includes candidates selected from every filename. Inspect the "
                        "best candidate first. Read PDFs in page chunks of at most 5 pages and "
                        "request the next chunk only when needed. Do not read a whole long document "
                        "or inspect every file. If the candidates are weak, call "
                        "ask_user with the question; do not end with a question as your final "
                        "answer. If the user's goal asks to move the matching file, confirm "
                        "the match first, then call fs_move with relative src and dst. In review "
                        "mode that move will pause for the user to apply. You must not delete, "
                        "rename, email, or upload files."},
            {'role': 'user',
             'content': (f"Goal: {goal}\nThe working folder is already selected; use relative "
                         f"paths only.{candidate_note}")},
        ]
        for loop_event in samosa_tools.iter_tool_loop(call_model, messages, tools, ctx):
            while pending:
                yield pending.pop(0)
            if loop_event.get('type') == 'tool_result':
                yield log.append('tool_result', **_tool_result_event(loop_event))
                continue
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
                if not final_text or '"samosa_tool"' in final_text:
                    question = _find_incomplete_question(goal)
                    call = {'samosa_tool': 'ask_user', 'question': question}
                    _write_convo(jdir, loop_event.get('convo') or messages, call, 0)
                    yield log.append('await_user', job_id=job_id, question=question)
                    return
                if _looks_like_question(final_text):
                    call = {'samosa_tool': 'ask_user', 'question': final_text}
                    _write_convo(jdir, loop_event.get('convo') or messages, call, 0)
                    yield log.append('await_user', job_id=job_id, question=final_text)
                    return
                yield log.append('done', summary=final_text)
                return
        question = _find_incomplete_question(goal)
        _write_convo(jdir, messages, {'samosa_tool': 'ask_user', 'question': question}, 0)
        yield log.append('await_user', job_id=job_id, question=question)
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
        if loop_event.get('type') == 'tool_result':
            yield log.append('tool_result', **_tool_result_event(loop_event))
            continue
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
            if not final_text or '"samosa_tool"' in final_text:
                question = _find_incomplete_question(_job_goal(jdir))
                call = {'samosa_tool': 'ask_user', 'question': question}
                _write_convo(jdir, loop_event.get('convo') or convo, call, 0)
                yield log.append('await_user', job_id=job_id, question=question)
                return
            if _looks_like_question(final_text):
                call = {'samosa_tool': 'ask_user', 'question': final_text}
                _write_convo(jdir, loop_event.get('convo') or convo, call, 0)
                yield log.append('await_user', job_id=job_id, question=final_text)
                return
            _clear_convo(jdir)
            yield log.append('done', summary=final_text)
            return
    question = _find_incomplete_question(_job_goal(jdir))
    call = {'samosa_tool': 'ask_user', 'question': question}
    _write_convo(jdir, convo, call, 0)
    yield log.append('await_user', job_id=job_id, question=question)


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


def _tool_result_event(loop_event):
    call = loop_event.get('call') or {}
    result = str(loop_event.get('result') or '')
    return {
        'tool': call.get('samosa_tool', ''),
        'chars': len(result),
        'bytes': len(result.encode('utf-8')),
        'preview': result[:TOOL_RESULT_EVENT_PREVIEW_CHARS],
        'truncated': len(result) > TOOL_RESULT_EVENT_PREVIEW_CHARS,
    }


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


def _job_goal(jdir):
    import json
    try:
        with open(os.path.join(jdir, 'job.json')) as f:
            return json.load(f).get('goal') or ''
    except (OSError, ValueError):
        return ''


def _dumps(obj):
    import json
    return json.dumps(obj, separators=(',', ':'))


def _looks_like_question(text):
    return bool(text and text.rstrip().endswith('?'))


_OUTPUT_METADATA_KEYS = {
    'status', 'unit_id', 'input_path', 'input_sha256', 'media_type', 'source',
    'reason', 'reasons', 'errors', 'warnings', 'provenance', 'attempt',
    'review_state', 'reviewed', 'corrected', 'review_corrected_at',
}


def _output_path(jdir):
    return os.path.join(jdir, 'results', 'output.jsonl')


def _read_output_records(jdir):
    path = _output_path(jdir)
    records = []
    try:
        with open(path, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    value = json.loads(line)
                    if isinstance(value, dict):
                        records.append(value)
                except ValueError:
                    pass
    except OSError:
        pass
    return records


def _write_output_records(jdir, records):
    path = _output_path(jdir)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = ''.join(json.dumps(r, separators=(',', ':'), sort_keys=True) + '\n'
                   for r in records)
    fs.atomic_write(path, data)


def _is_review_pending(record):
    if record.get('review_state') == 'done' or record.get('reviewed') is True:
        return False
    return record.get('status') == 'review_required' or bool(record.get('review_required'))


def _review_item_payload(index, record):
    extracted = record.get('extracted') if isinstance(record.get('extracted'), dict) else None
    if extracted is None:
        extracted = {k: v for k, v in record.items()
                     if k not in _OUTPUT_METADATA_KEYS and not k.startswith('review_')}
    return {
        'index': index,
        'unit_id': record.get('unit_id'),
        'input_path': record.get('input_path'),
        'input_sha256': record.get('input_sha256'),
        'status': record.get('status'),
        'reasons': record.get('reasons') or record.get('errors') or record.get('reason') or [],
        'fields': extracted,
        'source': _source_preview(record.get('input_path')),
        'done': not _is_review_pending(record),
    }


def _source_preview(path):
    if not path or not os.path.isfile(path):
        return ''
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return f.read(4000)
    except (OSError, UnicodeDecodeError):
        return os.path.basename(path)


def _preview_dir(job):
    output_dir = (job.get('output') or {}).get('dir') if isinstance(job.get('output'), dict) else None
    if output_dir:
        return os.path.join(os.path.abspath(output_dir), 'preview')
    job_id = job.get('job_id') or slugify(job.get('name') or 'preview')
    return os.path.join(job_dir_for(job_id), 'preview')


def _select_preview_for_file(job, file_path):
    wanted = os.path.abspath(file_path)
    has_model_work = bool(job.get('instruction') or job.get('output_schema'))
    items, skipped = fs.discover_files(job.get('input') or {},
                                       is_metadata_only=not has_model_work)
    for item in items:
        if os.path.abspath(item.get('input_path') or '') == wanted:
            return {'ok': True, 'sample_count': 1, 'items': [item],
                    'skipped_count': len(skipped), 'artifact_dir': 'preview'}
    return {'ok': False, 'reason': f'preview file is not a discoverable job input: {file_path}'}


def _preview_unit_id(index, item):
    digest = str(item.get('input_sha256') or '')[:12] or str(index)
    stem = slugify(os.path.basename(str(item.get('input_path') or 'unit')), maxlen=24)
    return f'{index:03d}-{stem}-{digest}'


def _preview_source_text(item):
    path = item.get('input_path')
    media_type = item.get('media_type')
    if media_type == 'text/plain':
        try:
            with open(path, 'r', encoding='utf-8') as f:
                return f.read(), None
        except (OSError, UnicodeDecodeError) as error:
            return '', f'could_not_read_text:{error}'
    if media_type == 'application/pdf':
        extracted, error = fs.extract_document(path)
        if error:
            return '', error
        return extracted.get('text', ''), None
    return '', f'preview_source_unavailable:{media_type}'


def _read_json_file(path):
    try:
        with open(path, 'r', encoding='utf-8') as f:
            value = json.load(f)
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def _write_json_file(path, value):
    if not path:
        return
    fs.atomic_write(path, json.dumps(value, indent=2, sort_keys=True) + '\n')


def _finish_schedule(schedule, status, result=None, reason=None):
    updated = dict(schedule)
    updated['last_status'] = status
    updated['last_finished_at'] = fs.rfc3339_now()
    updated['enabled'] = False if status == 'complete' else schedule.get('enabled', True)
    if reason:
        updated['last_reason'] = reason
    if result is not None:
        updated['last_result'] = result
    _write_json_file(updated.get('schedule_path'), updated)
    out = {'status': status}
    if reason:
        out['reason'] = reason
    if result is not None:
        out['result'] = result
    return out


def _run_job_definition(job, jdir, log, model_call=None):
    input_cfg = job.get('input') if isinstance(job.get('input'), dict) else {}
    folder = os.path.abspath(input_cfg.get('folder') or '')
    if not os.path.isdir(folder):
        raise ValueError(f'input folder does not exist: {folder}')
    org = job.get('organize') if isinstance(job.get('organize'), dict) else None
    has_model_work = bool(job.get('instruction') or job.get('output_schema'))
    extraction_records = None
    if has_model_work:
        extraction_records = _run_model_extractions(job, jdir, log, model_call)
        if not org:
            passed = [r for r in extraction_records if r.get('status') == 'passed']
            review = [r for r in extraction_records if r.get('status') == 'review_required']
            log.append('done', summary=f"Extracted {len(passed)} item{'' if len(passed) == 1 else 's'}"
                       + (f", {len(review)} need review." if review else "."))
            return {'kind': 'extract', 'passed': len(passed), 'review': len(review)}
    if not org:
        items, skipped = fs.discover_files(input_cfg, is_metadata_only=True)
        by_type = {k: v['count'] for k, v in fs.count_by_type(items).items()}
        log.append('counting', total=len(items), skipped=len(skipped), by_type=by_type)
        log.append('report', total=len(items), by_type=by_type)
        summary = _report_summary(len(items), by_type)
        log.append('done', summary=summary)
        return {'kind': 'report', 'total': len(items), 'skipped': len(skipped)}

    plan, err = fs.build_organize_plan(job, jdir, results=extraction_records)
    if err:
        raise ValueError(err)
    moves = [m for m in plan if 'dst' in m]
    skips = [m for m in plan if 'skip' in m]
    log.append('plan',
               moves=[{'src': m['src'], 'dst': m['dst']} for m in moves],
               skips=[{'src': s['src'], 'reason': s['skip']} for s in skips],
               dest_root=os.path.abspath(org.get('dest_root') or os.path.join(folder, 'Organized')))
    applied = 0
    skipped = len(skips)
    for i, move in enumerate(moves, 1):
        ok, reason = fs.apply_move(move, input_folder=folder)
        applied += 1 if ok else 0
        skipped += 0 if ok else 1
        log.append('action', op='move', i=i, n=len(moves), src=move['src'],
                   dst=move['dst'], ok=ok, reason=reason)
    log.append('applied', applied=applied, skipped=skipped)
    log.append('done', summary=f"Scheduled job moved {applied} file{'' if applied == 1 else 's'}.")
    return {'kind': 'organize', 'planned': len(moves), 'applied': applied, 'skipped': skipped}


def _run_model_extractions(job, jdir, log, model_call):
    if model_call is None:
        raise ValueError('model is required for extraction jobs')
    input_cfg = job.get('input') if isinstance(job.get('input'), dict) else {}
    items, skipped = fs.discover_files(input_cfg, is_metadata_only=False)
    records = []
    for i, item in enumerate(sorted(items, key=lambda it: it.get('input_path') or ''), 1):
        unit_id = _preview_unit_id(i, item)
        source_text, source_error = _preview_source_text(item)
        if source_error is None:
            record = _model_extract_record(job, item, source_text, model_call)
        else:
            record = {
                'status': 'review_required',
                'reasons': [source_error],
                'input_path': item.get('input_path'),
                'input_sha256': item.get('input_sha256'),
                'media_type': item.get('media_type'),
                'extracted': {},
            }
        record['unit_id'] = unit_id
        records.append(record)
        if record.get('status') == 'passed':
            log.append('item_complete', unit_id=unit_id,
                       input_path=record.get('input_path'), artifact='output.jsonl',
                       validation={'status': 'passed'})
        else:
            log.append('item_review_required', unit_id=unit_id,
                       input_path=record.get('input_path'),
                       reasons=record.get('reasons') or [])
    out_dir = _job_output_dir(job, jdir)
    os.makedirs(out_dir, exist_ok=True)
    fs.atomic_write(os.path.join(out_dir, 'output.jsonl'),
                    ''.join(json.dumps(r, separators=(',', ':'), sort_keys=True) + '\n'
                            for r in records))
    if skipped:
        log.append('input_skipped', skipped=len(skipped),
                   reasons=[{'path': p, 'reason': r} for p, r in skipped[:20]])
    return records


def _model_extract_record(job, item, source_text, model_call):
    schema = job.get('output_schema') if isinstance(job.get('output_schema'), dict) else {}
    instruction = job.get('instruction') or 'Extract the requested fields.'
    messages = [
        {'role': 'system',
         'content': 'Extract structured data. Reply with JSON only, no markdown.'},
        {'role': 'user',
         'content': (f"Instruction:\n{instruction}\n\nSchema:\n"
                     f"{json.dumps(schema, sort_keys=True)}\n\nSource:\n{source_text}")},
    ]
    try:
        reply = model_call(messages)
    except Exception as error:
        reply = ''
        parse_error = f'model_error:{error}'
    else:
        parse_error = None
    extracted, error = _parse_model_json(reply)
    if error is not None and parse_error is None:
        parse_error = error
    errors = []
    if parse_error:
        errors.append(parse_error)
    errors.extend(_validate_extracted(extracted, schema))
    status = 'review_required' if errors else 'passed'
    record = {
        'status': status,
        'input_path': item.get('input_path'),
        'input_sha256': item.get('input_sha256'),
        'media_type': item.get('media_type'),
        'extracted': extracted,
    }
    for key, value in extracted.items():
        record[key] = value
    if errors:
        record['reasons'] = errors
    return record


def _parse_model_json(reply):
    text = (reply or '').strip()
    if text.startswith('```'):
        text = re.sub(r'^```(?:json)?\s*', '', text)
        text = re.sub(r'\s*```$', '', text).strip()
    try:
        value = json.loads(text)
    except (TypeError, ValueError):
        return {}, 'malformed_json'
    if not isinstance(value, dict):
        return {}, 'json_not_object'
    return value, None


def _validate_extracted(record, schema):
    errors = []
    required = schema.get('required') if isinstance(schema.get('required'), list) else []
    props = schema.get('properties') if isinstance(schema.get('properties'), dict) else {}
    for field in required:
        if field not in record or record.get(field) is None:
            errors.append(f'missing_required_field:{field}')
    for field, spec in props.items():
        if field not in record or record[field] is None or not isinstance(spec, dict):
            continue
        allowed = spec.get('type')
        allowed_types = allowed if isinstance(allowed, list) else [allowed]
        if allowed_types and not _json_type_matches(record[field], allowed_types):
            errors.append(f'type:{field}')
        if isinstance(record[field], str) and spec.get('maxLength') is not None:
            try:
                if len(record[field]) > int(spec['maxLength']):
                    errors.append(f'constraint:{field}:maxLength')
            except (TypeError, ValueError):
                pass
        if 'enum' in spec and isinstance(spec.get('enum'), list) and record[field] not in spec['enum']:
            errors.append(f'constraint:{field}:enum')
    return errors


def _json_type_matches(value, allowed_types):
    for allowed in allowed_types:
        if allowed == 'null' and value is None:
            return True
        if allowed == 'string' and isinstance(value, str):
            return True
        if allowed == 'number' and isinstance(value, (int, float)) and not isinstance(value, bool):
            return True
        if allowed == 'integer' and isinstance(value, int) and not isinstance(value, bool):
            return True
        if allowed == 'boolean' and isinstance(value, bool):
            return True
        if allowed == 'object' and isinstance(value, dict):
            return True
        if allowed == 'array' and isinstance(value, list):
            return True
    return False


def _job_output_dir(job, jdir):
    output = job.get('output') if isinstance(job.get('output'), dict) else {}
    if output.get('dir'):
        return os.path.abspath(output['dir'])
    return os.path.join(jdir, 'results')


def _parse_hhmm(value):
    if not isinstance(value, str):
        return None
    match = re.match(r'^([01]?\d|2[0-3]):([0-5]\d)$', value)
    if not match:
        return None
    return int(match.group(1)) * 60 + int(match.group(2))


def _now_minutes():
    local = time.localtime()
    return local.tm_hour * 60 + local.tm_min


def _minutes_in_window(now, start, end):
    now = int(now) % (24 * 60)
    if start == end:
        return True
    if start < end:
        return start <= now < end
    return now >= start or now < end


def _find_output_record(records, item):
    if isinstance(item, int):
        return item if 0 <= item < len(records) else None
    if not isinstance(item, dict):
        return None
    if 'index' in item:
        try:
            index = int(item['index'])
            if 0 <= index < len(records):
                return index
        except (TypeError, ValueError):
            pass
    for key in ('unit_id', 'input_sha256', 'input_path'):
        value = item.get(key)
        if not value:
            continue
        for index, record in enumerate(records):
            if record.get(key) == value:
                return index
    return None


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
              "  samosa jobs estimate <job.json>\n"
              "  samosa jobs preview <job.json> [--file <path>] [--expanded|--samples N]\n"
              "  samosa jobs arm <job.json> [--overnight] [--window HH:MM-HH:MM]\n"
              "  samosa jobs overnight <job.json>\n"
              "  samosa jobs launchd-plist --print\n"
              "  samosa jobs launchd-install [--path <plist>]\n"
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
    if cmd == 'estimate':
        if len(argv) < 2:
            print("Usage: samosa jobs estimate <job.json>", file=sys.stderr)
            return 2
        try:
            with open(argv[1]) as f:
                job = json.load(f)
        except (OSError, ValueError) as e:
            print(f"error: could not read job.json: {e}", file=sys.stderr)
            return 1
        print(json.dumps(estimate_job(job), indent=2, sort_keys=True))
        return 0
    if cmd == 'preview':
        rest = argv[1:]
        file_path = None
        sample_count = 1
        i = 0
        positional = []
        while i < len(rest):
            arg = rest[i]
            if arg == '--file':
                if i + 1 >= len(rest):
                    print("Usage: samosa jobs preview <job.json> [--file <path>] [--expanded|--samples N]",
                          file=sys.stderr)
                    return 2
                file_path = rest[i + 1]
                i += 2
            elif arg == '--expanded':
                sample_count = PREVIEW_SAMPLE_TARGET
                i += 1
            elif arg == '--samples':
                if i + 1 >= len(rest):
                    print("--samples requires a positive integer", file=sys.stderr)
                    return 2
                try:
                    sample_count = max(1, int(rest[i + 1]))
                except ValueError:
                    print("--samples requires a positive integer", file=sys.stderr)
                    return 2
                i += 2
            else:
                positional.append(arg)
                i += 1
        if len(positional) != 1:
            print("Usage: samosa jobs preview <job.json> [--file <path>] [--expanded|--samples N]",
                  file=sys.stderr)
            return 2
        try:
            with open(positional[0]) as f:
                job = json.load(f)
        except (OSError, ValueError) as e:
            print(f"error: could not read job.json: {e}", file=sys.stderr)
            return 1
        result = preview_job(job, file_path=file_path, sample_count=sample_count)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result.get('ok') else 1
    if cmd == 'arm':
        rest = argv[1:]
        window_start = None
        window_end = None
        missed_policy = DEFAULT_MISSED_POLICY
        keep_awake = True
        positional = []
        i = 0
        while i < len(rest):
            arg = rest[i]
            if arg == '--overnight':
                window_start, window_end = DEFAULT_OVERNIGHT_WINDOW
                i += 1
            elif arg == '--window':
                if i + 1 >= len(rest) or '-' not in rest[i + 1]:
                    print("--window requires HH:MM-HH:MM", file=sys.stderr)
                    return 2
                window_start, window_end = rest[i + 1].split('-', 1)
                i += 2
            elif arg == '--missed-policy':
                if i + 1 >= len(rest):
                    print("--missed-policy requires skip or run_next_start", file=sys.stderr)
                    return 2
                missed_policy = rest[i + 1]
                i += 2
            elif arg == '--no-keep-awake':
                keep_awake = False
                i += 1
            else:
                positional.append(arg)
                i += 1
        if len(positional) != 1:
            print("Usage: samosa jobs arm <job.json> [--overnight] [--window HH:MM-HH:MM]",
                  file=sys.stderr)
            return 2
        result = arm_scheduled_job(positional[0], window_start=window_start,
                                   window_end=window_end,
                                   missed_policy=missed_policy,
                                   keep_awake=keep_awake)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result.get('ok') else 1
    if cmd == 'launchd-plist':
        if argv[1:] != ['--print']:
            print("Usage: samosa jobs launchd-plist --print", file=sys.stderr)
            return 2
        program = [sys.executable, os.path.abspath(__file__), 'jobsd-once']
        print(launchd_plist(program), end='')
        return 0
    if cmd == 'launchd-install':
        rest = argv[1:]
        dest = None
        if rest:
            if len(rest) == 2 and rest[0] == '--path':
                dest = rest[1]
            else:
                print("Usage: samosa jobs launchd-install [--path <plist>]", file=sys.stderr)
                return 2
        print(json.dumps(install_launchd_plist(dest_path=dest), indent=2, sort_keys=True))
        return 0
    if cmd == 'overnight':
        if len(argv) != 2:
            print("Usage: samosa jobs overnight <job.json>", file=sys.stderr)
            return 2
        result = arm_overnight_job(argv[1])
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result.get('ok') else 1
    if cmd == 'jobsd-once':
        print(json.dumps(jobsd_once(), indent=2, sort_keys=True))
        return 0
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
