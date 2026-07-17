#!/usr/bin/env python3
"""Samosa Jobs — batch, scheduled, local multimodal work.

One-shot runner for structured document/image extraction.  Processes a folder
of inputs against a user-defined schema, producing validated JSON results with
crash-durable state, idempotent re-runs, and machine-safety enforcement.

Usage:
    samosa jobs validate <job.json>
    samosa jobs arm <job.json>
    samosa jobs preview <job.json> [--file <path>]
    samosa jobs run <job.json>
    samosa jobs status <job.json>
    samosa jobs view <job.json>
    samosa jobs suggest-schema <job.json|--instruction "...">
    samosa jobs delete <job.json>
    samosa jobs archive <job.json>

Requires Python 3 standard library only.
"""

import base64
import copy
import csv
import fcntl
import hashlib
import html
import io
import json
import math
import os
import platform
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants — pinned from the engine (TASKS_JOBS.md §J1.2)
# ---------------------------------------------------------------------------
IMAGE_TOKENS = 576          # max vision tokens per rendered page
MAX_CONTEXT = 24576         # SAMOSA_MAX_CONTEXT_TOKENS (qwen36b.c:3564)
SYSTEM_RESERVE = 1024       # conservative reserve for system prompt overhead
LOW_TEXT_TOKENS = 20        # a PDF page under this is treated as needs_image (§J1.2)
MAX_JOB_INPUT_TOKENS = 8192 # hard Jobs prefill ceiling; leaves context headroom
MAX_FILE_BYTES_DEFAULT = 26214400  # 25 MiB
HTTP_MAX_BODY = 4 * 1024 * 1024    # 4 MiB (samosa_http.h:20)
RUNNER_VERSION = "j1-0.1"
SERVE_URL_DEFAULT = "http://127.0.0.1:8642"

# Magic bytes for file type detection
MAGIC_JPEG = b'\xff\xd8\xff'
MAGIC_PNG = b'\x89PNG'
MAGIC_PDF = b'%PDF'

# job_id regex
JOB_ID_RE = re.compile(r'^[a-z0-9][a-z0-9_-]{0,63}$')

# Supported schema keywords (§output_schema)
VALID_TOP_KEYWORDS = {'type', 'required', 'properties'}
VALID_FIELD_KEYWORDS = {'type', 'enum', 'minimum', 'maximum', 'maxLength'}
VALID_TYPES = {'string', 'number', 'integer', 'boolean', 'null'}

# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def rfc3339_now():
    """Return current UTC timestamp in RFC3339 format."""
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

def sha256_file(path):
    """Compute SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

def sha256_bytes(data):
    """Compute SHA-256 hex digest of bytes."""
    return hashlib.sha256(data).hexdigest()

def fsync_file(path):
    """fsync a file by path."""
    fd = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def fsync_dir(path):
    """fsync a directory to ensure rename durability."""
    fd = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def atomic_write(dest, data, mode=0o600):
    """Write data atomically: write .partial, fsync, rename, fsync dir."""
    dest = Path(dest)
    partial = dest.with_suffix(dest.suffix + '.partial')
    with open(partial, 'w') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(str(partial), mode)
    os.rename(str(partial), str(dest))
    fsync_dir(str(dest.parent))

def html_escape(s):
    """HTML-escape a string for safe interpolation."""
    return html.escape(str(s), quote=True)


# ---------------------------------------------------------------------------
# J1.0 — Job definition loader / validator
# ---------------------------------------------------------------------------

def validate_output_schema(schema):
    """Validate the output_schema subset. Returns list of error strings."""
    errors = []
    if not isinstance(schema, dict):
        return ["output_schema must be a JSON object"]
    # Check top-level keywords
    for k in schema:
        if k not in VALID_TOP_KEYWORDS:
            errors.append(f"output_schema: unknown top-level keyword '{k}'")
    if schema.get('type') != 'object':
        errors.append("output_schema.type must be 'object'")
    req = schema.get('required')
    if req is not None and not isinstance(req, list):
        errors.append("output_schema.required must be an array")
    props = schema.get('properties')
    if props is not None:
        if not isinstance(props, dict):
            errors.append("output_schema.properties must be an object")
        else:
            for fname, frule in props.items():
                if not isinstance(frule, dict):
                    errors.append(f"output_schema.properties.{fname} must be an object")
                    continue
                for fk in frule:
                    if fk not in VALID_FIELD_KEYWORDS:
                        errors.append(f"output_schema.properties.{fname}: unknown keyword '{fk}'")
                # Validate type
                ftype = frule.get('type')
                if ftype is not None:
                    types = [ftype] if isinstance(ftype, str) else ftype
                    if not isinstance(types, list):
                        errors.append(f"output_schema.properties.{fname}.type must be a string or array")
                    else:
                        for t in types:
                            if t not in VALID_TYPES:
                                errors.append(f"output_schema.properties.{fname}: unsupported type '{t}'")
                # Validate enum
                fenum = frule.get('enum')
                if fenum is not None and not isinstance(fenum, list):
                    errors.append(f"output_schema.properties.{fname}.enum must be an array")
                # Validate numeric constraints
                for nk in ('minimum', 'maximum', 'maxLength'):
                    nv = frule.get(nk)
                    if nv is not None and not isinstance(nv, (int, float)):
                        errors.append(f"output_schema.properties.{fname}.{nk} must be a number")
                # Reject nested objects/arrays in type
                if ftype is not None:
                    types = [ftype] if isinstance(ftype, str) else ftype
                    if isinstance(types, list):
                        for t in types:
                            if t in ('object', 'array'):
                                errors.append(f"output_schema.properties.{fname}: nested type '{t}' not supported")
    return errors


def validate_job(job):
    """Validate a parsed job.json. Returns (normalized_job, errors)."""
    errors = []

    # Required top-level fields
    for key in ('job_id', 'input', 'instruction', 'output_schema'):
        if key not in job:
            errors.append(f"missing required field: {key}")
    if errors:
        return None, errors

    # job_id
    jid = job.get('job_id', '')
    if not JOB_ID_RE.match(jid):
        errors.append(f"job_id: must match {JOB_ID_RE.pattern}")

    # schema_version
    sv = job.get('schema_version', 1)
    if sv != 1:
        errors.append(f"schema_version: must be 1, got {sv}")

    # input
    inp = job.get('input', {})
    if not isinstance(inp, dict):
        errors.append("input must be an object")
    else:
        folder = inp.get('folder')
        if not folder:
            errors.append("input.folder is required")
        elif not os.path.isabs(folder):
            errors.append("input.folder must be an absolute path")
        mfb = inp.get('max_file_bytes', MAX_FILE_BYTES_DEFAULT)
        if not isinstance(mfb, (int, float)) or mfb <= 0:
            errors.append("input.max_file_bytes must be a positive number")

    # unit
    unit = job.get('unit', 'auto')
    if unit not in ('auto', 'file', 'page'):
        errors.append(f"unit: must be auto, file, or page; got '{unit}'")

    # instruction
    instr = job.get('instruction', '')
    if not isinstance(instr, str) or not instr.strip():
        errors.append("instruction must be a non-empty string")

    # output_schema
    schema_errors = validate_output_schema(job.get('output_schema', {}))
    errors.extend(schema_errors)

    # inference
    inf = job.get('inference', {})
    if not isinstance(inf, dict):
        errors.append("inference must be an object")
    else:
        mt = inf.get('max_tokens', 512)
        if not isinstance(mt, int) or mt < 1 or mt > 8192:
            errors.append("inference.max_tokens must be an integer in 1..8192")

    # output
    out = job.get('output', {})
    if isinstance(out, dict):
        fmt = out.get('format', 'jsonl')
        if fmt not in ('jsonl', 'csv'):
            errors.append(f"output.format must be jsonl or csv; got '{fmt}'")
        outdir = out.get('dir')
        if outdir and not os.path.isabs(outdir):
            errors.append("output.dir must be an absolute path")

    # resources
    res = job.get('resources', {})
    if isinstance(res, dict):
        ma = res.get('max_attempts', 3)
        if not isinstance(ma, int) or ma < 1:
            errors.append("resources.max_attempts must be a positive integer")
        mit = res.get('max_input_tokens', MAX_JOB_INPUT_TOKENS)
        if (not isinstance(mit, int) or isinstance(mit, bool)
                or mit < 256 or mit > MAX_JOB_INPUT_TOKENS):
            errors.append(f"resources.max_input_tokens must be an integer in 256..{MAX_JOB_INPUT_TOKENS}")

    # Normalize
    normalized = copy.deepcopy(job)
    normalized.setdefault('schema_version', 1)
    normalized.setdefault('unit', 'auto')
    normalized.setdefault('name', jid)
    normalized.setdefault('created_at', rfc3339_now())
    normalized.setdefault('reduce', {'mode': 'deterministic', 'model_fields': []})
    normalized.setdefault('inference', {})
    normalized['inference'].setdefault('thinking', 'off')
    normalized['inference'].setdefault('seed', 11)
    normalized['inference'].setdefault('temperature', 0)
    normalized['inference'].setdefault('max_tokens', 512)
    normalized['inference'].setdefault('timeout_s', None)
    normalized.setdefault('output', {})
    normalized['output'].setdefault('format', 'jsonl')
    normalized.setdefault('resources', {})
    normalized['resources'].setdefault('max_attempts', 3)
    normalized['resources'].setdefault('run_on_battery', False)
    normalized['resources'].setdefault('pause_when_user_active', True)
    normalized['resources'].setdefault('min_free_gb', 5)
    normalized['resources'].setdefault('max_input_tokens', MAX_JOB_INPUT_TOKENS)
    normalized.setdefault('validation', {})

    return normalized, errors


def load_and_validate_job(path):
    """Load a job.json file and validate it. Returns (job, errors)."""
    try:
        with open(path) as f:
            job = json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        return None, [f"cannot load job file: {e}"]
    if not isinstance(job, dict):
        return None, ["job.json must be a JSON object"]
    return validate_job(job)


# ---------------------------------------------------------------------------
# J1.1 — Input discovery (TOCTOU-safe, magic-byte typing)
# ---------------------------------------------------------------------------

def detect_media_type(header_bytes):
    """Detect media type from the first bytes of a file."""
    if header_bytes[:3] == MAGIC_JPEG:
        return 'image/jpeg'
    if header_bytes[:4] == MAGIC_PNG:
        return 'image/png'
    if header_bytes[:4] == MAGIC_PDF:
        return 'application/pdf'
    return None


def is_valid_utf8_text(data):
    """Check if data is valid UTF-8 and contains no problematic control characters."""
    try:
        text = data.decode('utf-8', errors='strict')
    except UnicodeDecodeError:
        return False
    # Reject if it contains control chars other than \t, \n, \r
    for ch in text:
        cp = ord(ch)
        if cp < 32 and cp not in (9, 10, 13):
            return False
    return True


def discover_inputs(input_config, allowed_types=None):
    """Discover input files. Returns list of {input_path, input_sha256, media_type, size}
    and a list of skip reasons."""
    folder = input_config['folder']
    recursive = input_config.get('recursive', False)
    max_bytes = input_config.get('max_file_bytes', MAX_FILE_BYTES_DEFAULT)
    type_filter = set(input_config.get('types', [
        'image/jpeg', 'image/png', 'text/plain', 'application/pdf'
    ]))

    items = []
    skipped = []
    seen_hashes = set()

    if recursive:
        walker = sorted(Path(folder).rglob('*'))
    else:
        walker = sorted(Path(folder).iterdir())

    for entry in walker:
        path = str(entry)
        # Open with O_NOFOLLOW to reject symlinks
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        except OSError as e:
            skipped.append((path, f"cannot open (O_NOFOLLOW): {e}"))
            continue

        try:
            st = os.fstat(fd)
            # Must be a regular file
            if not stat_is_regular(st):
                skipped.append((path, "not a regular file"))
                continue
            # Size check
            if st.st_size > max_bytes:
                skipped.append((path, f"exceeds max_file_bytes ({st.st_size} > {max_bytes})"))
                continue
            if st.st_size == 0:
                skipped.append((path, "empty file"))
                continue

            # Read and hash
            data = b''
            while True:
                chunk = os.read(fd, 1 << 20)
                if not chunk:
                    break
                data += chunk

            # Re-fstat to detect changes during read
            st2 = os.fstat(fd)
            if st2.st_size != st.st_size or st2.st_mtime != st.st_mtime:
                skipped.append((path, "file changed during read"))
                continue
        finally:
            os.close(fd)

        file_hash = sha256_bytes(data)
        if file_hash in seen_hashes:
            skipped.append((path, f"duplicate content (same SHA-256 as earlier file)"))
            continue
        seen_hashes.add(file_hash)

        # Detect type by magic bytes
        media_type = detect_media_type(data[:8])
        if media_type is None:
            if is_valid_utf8_text(data):
                media_type = 'text/plain'
            else:
                skipped.append((path, "unsupported: not a recognized image/PDF and not valid UTF-8 text"))
                continue

        if media_type not in type_filter:
            skipped.append((path, f"type {media_type} not in allowed types"))
            continue

        items.append({
            'input_path': path,
            'input_sha256': file_hash,
            'media_type': media_type,
            'size': len(data),
        })

    return items, skipped


def stat_is_regular(st):
    """Check if a stat result represents a regular file."""
    import stat as stat_mod
    return stat_mod.S_ISREG(st.st_mode)


# ---------------------------------------------------------------------------
# J1.5 — Output validation
# ---------------------------------------------------------------------------

def find_json_object(text):
    """Find the first complete JSON object in text using a string-aware scanner.
    Returns (parsed_obj, has_trailing) or (None, False) if no object found."""
    # Find first '{'
    start = text.find('{')
    if start < 0:
        return None, False

    depth = 0
    in_string = False
    escape_next = False
    end = -1

    for i in range(start, len(text)):
        ch = text[i]
        if escape_next:
            escape_next = False
            continue
        if in_string:
            if ch == '\\':
                escape_next = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                end = i
                break

    if end < 0:
        return None, False

    obj_str = text[start:end + 1]
    try:
        obj = json.loads(obj_str)
    except json.JSONDecodeError:
        return None, False

    trailing = text[end + 1:].strip()
    return obj, len(trailing) > 0


def validate_output(content_str, schema, domain_rules=None):
    """Validate model output against the schema.
    Returns {status, errors, warnings, record}."""
    errors = []
    warnings = []
    record = None

    # Parse
    try:
        record = json.loads(content_str)
    except (json.JSONDecodeError, TypeError):
        # Try to recover a JSON object
        if isinstance(content_str, str):
            obj, has_trailing = find_json_object(content_str)
            if obj is not None:
                record = obj
                if has_trailing:
                    warnings.append("trailing_prose")
            else:
                errors.append("unparseable")
        else:
            errors.append("unparseable")

    if record is None:
        return {'status': 'review_required', 'errors': errors, 'warnings': warnings, 'record': None}

    if not isinstance(record, dict):
        errors.append("unparseable")
        return {'status': 'review_required', 'errors': errors, 'warnings': warnings, 'record': None}

    # Schema validation
    required = schema.get('required', [])
    properties = schema.get('properties', {})

    for rk in required:
        if rk not in record:
            errors.append(f"missing_required_field:{rk}")

    for fname, frule in properties.items():
        if fname not in record:
            continue
        val = record[fname]
        # Type check
        ftype = frule.get('type')
        if ftype is not None:
            types = [ftype] if isinstance(ftype, str) else ftype
            if not _check_json_type(val, types):
                errors.append(f"type_mismatch:{fname}")
                continue
        # Enum check
        fenum = frule.get('enum')
        if fenum is not None:
            if not _check_enum(val, fenum):
                errors.append(f"constraint:{fname}")
        # Bounds
        if isinstance(val, (int, float)) and not isinstance(val, bool):
            fmin = frule.get('minimum')
            if fmin is not None and val < fmin:
                errors.append(f"constraint:{fname}")
            fmax = frule.get('maximum')
            if fmax is not None and val > fmax:
                errors.append(f"constraint:{fname}")
        # maxLength
        if isinstance(val, str):
            ml = frule.get('maxLength')
            if ml is not None and len(val) > ml:
                errors.append(f"constraint:{fname}")

    # Domain rules
    if domain_rules:
        for rule in domain_rules:
            dr_err = _check_domain_rule(rule, record)
            if dr_err:
                errors.append(f"domain:{rule}")

    status = 'review_required' if errors else 'passed'
    return {'status': status, 'errors': errors, 'warnings': warnings, 'record': record}


def _check_json_type(val, types):
    """Check if val matches one of the JSON types. bool is NOT number/integer."""
    for t in types:
        if t == 'null' and val is None:
            return True
        if t == 'string' and isinstance(val, str):
            return True
        if t == 'boolean' and isinstance(val, bool):
            return True
        if t == 'integer' and isinstance(val, int) and not isinstance(val, bool):
            return True
        if t == 'number' and isinstance(val, (int, float)) and not isinstance(val, bool):
            return True
    return False


def _check_enum(val, enum_values):
    """Check if val equals one of the enum values by JSON type.
    True != 1, False != 0 in JSON-typed comparison."""
    for ev in enum_values:
        if type(val) == type(ev) and val == ev:
            return True
        # Allow int/float cross-comparison for numbers (but not bool)
        if (isinstance(val, (int, float)) and not isinstance(val, bool) and
            isinstance(ev, (int, float)) and not isinstance(ev, bool) and
            val == ev):
            return True
    return False


def _check_domain_rule(rule, record):
    """Check a domain rule like 'subtotal + tax ~= total'.
    Returns True if the rule fails."""
    # Parse "a + b ~= c"
    m = re.match(r'(\w+)\s*\+\s*(\w+)\s*~=\s*(\w+)', rule)
    if not m:
        return False  # Unknown rule format, skip
    a_name, b_name, c_name = m.group(1), m.group(2), m.group(3)
    a_val = record.get(a_name)
    b_val = record.get(b_name)
    c_val = record.get(c_name)
    # All three must be numbers
    if not all(isinstance(v, (int, float)) and not isinstance(v, bool)
               for v in (a_val, b_val, c_val) if v is not None):
        return False  # Can't check, not an error
    if a_val is None or b_val is None or c_val is None:
        return False  # Missing values, not a domain error
    tolerance = 0.01 * max(1, abs(c_val))
    if abs(a_val + b_val - c_val) > tolerance:
        return True
    return False


# ---------------------------------------------------------------------------
# J1.2 — Granularity planner
# ---------------------------------------------------------------------------

def _file_unit(sha, reason, warning=None):
    u = {
        'unit_id': sha,
        'input_sha256': sha,
        'granularity': 'file',
        'plan_reason': reason,
        'reduce_group': None,
    }
    if warning:
        u['warning'] = warning
    return u


def _page_units(sha, pages, reason, context_budget=None):
    """One unit per PDF page; recombined by the reducer (reduce_group=sha)."""
    units = []
    for p in pages:
        idx = p['index']
        page_reason = reason
        if context_budget is not None:
            page_tokens = p.get('text_tokens', 0)
            if _page_needs_image(p):
                page_tokens += IMAGE_TOKENS
            if page_tokens > context_budget:
                page_reason = 'page_over_safe_prefill_budget'
        units.append({
            'unit_id': f"{sha}#p{idx}",
            'input_sha256': sha,
            'granularity': 'page',
            'plan_reason': page_reason,
            'page_index': idx,
            'reduce_group': sha,
        })
    return units


def _page_needs_image(page):
    """F-J4/J1.2: a page needs a rendered image if it has little/no text or a figure."""
    return page.get('text_tokens', 0) < LOW_TEXT_TOKENS or page.get('has_raster_figure', False)


def plan_units(input_meta, unit_mode, context_budget, tokenizer_cmd=None):
    """Plan processing units for an input file (§J1.2).

    Deterministic; the PDF path consumes page metadata
    (list of {index, text_tokens, has_raster_figure}) from the #5 extract_meta
    contract. Without that metadata a real PDF cannot be planned and is flagged
    `extractor_unavailable` for controlled capability degradation.

    Args:
        input_meta: dict with input_sha256, media_type, input_path, size,
            text_tokens (optional), pages (optional, PDF only)
        unit_mode: 'auto' (default), 'file', or 'page'
        context_budget: input-token ceiling = MAX_CONTEXT - max_tokens - SYSTEM_RESERVE
        tokenizer_cmd: command to run `tokenize --count` (list)

    Returns: list of unit dicts.
    """
    sha = input_meta['input_sha256']
    media_type = input_meta['media_type']
    path = input_meta['input_path']

    # --- single image: always one unit (one image, F-J4 is satisfied) ---
    if media_type.startswith('image/'):
        return [_file_unit(sha, 'single_image')]

    # --- text / markdown: no pages; auto chunks when over budget ---
    if media_type == 'text/plain':
        text_tokens = input_meta.get('text_tokens')
        if text_tokens is None and tokenizer_cmd:
            text_tokens = count_tokens_file(path, tokenizer_cmd)
        if text_tokens is None:
            text_tokens = math.ceil(input_meta.get('size', 0) / 4)
        if unit_mode == 'file' and text_tokens <= context_budget:
            return [_file_unit(sha, 'forced_file')]
        if text_tokens <= context_budget:
            return [_file_unit(sha, 'fits_budget')]
        return _plan_text_chunks(sha, path, text_tokens, context_budget, tokenizer_cmd)

    # --- PDF: needs #5 sidecar page metadata to decide granularity ---
    if media_type == 'application/pdf':
        pages = input_meta.get('pages')
        if not pages:
            # No sidecar metadata → cannot plan; J1.3 extraction flags it too.
            return [_file_unit(sha, 'extractor_unavailable')]

        image_pages = sum(1 for p in pages if _page_needs_image(p))
        # Tokenization is not additive across page boundaries.  The sidecar's
        # whole-document count is therefore authoritative for a whole-file
        # decision; retain the page sum only for synthetic metadata/tests that
        # do not provide it.
        text_tokens = input_meta.get('text_tokens')
        if not isinstance(text_tokens, int) or isinstance(text_tokens, bool) or text_tokens < 0:
            text_tokens = sum(p.get('text_tokens', 0) for p in pages)
        total_tokens = text_tokens + image_pages * IMAGE_TOKENS

        if unit_mode == 'file' and total_tokens <= context_budget:
            # Honor the explicit choice, but warn: one inference sees only one
            # image (F-J4), so a multi-image doc loses the rest.
            warning = 'forced_file_multi_image' if image_pages >= 2 else None
            return [_file_unit(sha, 'forced_file', warning)]
        if unit_mode == 'page':
            return _page_units(sha, pages, 'forced_page', context_budget)

        # auto
        if image_pages >= 2:
            return _page_units(sha, pages, 'multi_image_pages', context_budget)  # forced by F-J4
        if total_tokens > context_budget:
            return _page_units(sha, pages, 'over_context', context_budget)
        return [_file_unit(sha, 'fits_budget')]

    # --- unknown type ---
    return [_file_unit(sha, 'unknown_type')]


def _plan_text_chunks(sha, path, total_tokens, budget, tokenizer_cmd):
    """Split a text file into chunk units on paragraph/line boundaries."""
    with open(path, 'r', errors='replace') as f:
        lines = f.readlines()

    if not lines:
        return [{
            'unit_id': sha,
            'input_sha256': sha,
            'granularity': 'file',
            'plan_reason': 'fits_budget',
            'reduce_group': None,
        }]

    # Simple split: divide into roughly equal chunks by line count
    overlap_tokens = 64
    chars_per_token = max(1, sum(len(l) for l in lines) / max(1, total_tokens))
    chunk_chars = int(budget * chars_per_token * 0.9)  # 90% to leave margin
    overlap_chars = int(overlap_tokens * chars_per_token)

    units = []
    chunk_idx = 0
    pos = 0
    total_chars = sum(len(l) for l in lines)

    line_offsets = []
    offset = 0
    for l in lines:
        line_offsets.append(offset)
        offset += len(l)

    while pos < total_chars:
        chunk_start = pos
        end = min(pos + chunk_chars, total_chars)
        # Find line boundary
        best_line = 0
        for i, lo in enumerate(line_offsets):
            if lo <= end:
                best_line = i
            else:
                break

        # The chunk covers [chunk_start, chunk_end) aligned to a line boundary.
        chunk_end = line_offsets[best_line] + len(lines[best_line])
        if chunk_end <= chunk_start:
            chunk_end = end  # Prevent a zero-width chunk
        chunk_end = min(chunk_end, total_chars)

        unit_id = f"{sha}#c{chunk_idx}"
        units.append({
            'unit_id': unit_id,
            'input_sha256': sha,
            'granularity': 'chunk',
            'plan_reason': 'over_context',
            'chunk_index': chunk_idx,
            'char_start': chunk_start,
            'char_end': chunk_end,
            'reduce_group': sha,
        })
        chunk_idx += 1

        # Once a chunk reaches the end, stop — otherwise the overlap pull-back
        # would spawn a run of 1-char-advancing chunks over the tail.
        if chunk_end >= total_chars:
            break

        # Advance to the next chunk, keeping a small overlap for continuity.
        pos = max(chunk_end - overlap_chars, pos + 1)

    return units


def count_tokens_file(path, tokenizer_cmd):
    """Count tokens in a file using the tokenizer command."""
    try:
        result = subprocess.run(
            tokenizer_cmd + [path],
            capture_output=True, text=True, timeout=30
        )
        if result.returncode == 0:
            return int(result.stdout.strip())
    except (subprocess.TimeoutExpired, ValueError, OSError):
        pass
    return None


# ---------------------------------------------------------------------------
# J1.3 — Extraction dispatch
# ---------------------------------------------------------------------------

def get_pdf_extractor():
    """Return the installed sidecar path, if this release has one.

    The explicit environment variable is useful for a staged release and tests;
    otherwise a packaged sibling is preferred before consulting PATH.  We never
    substitute a host PDF utility when the sidecar is absent.
    """
    configured = os.environ.get('SAMOSA_EXTRACTOR')
    if configured:
        return configured
    sibling = Path(__file__).resolve().with_name('samosa-extract')
    if sibling.is_file() and os.access(str(sibling), os.X_OK):
        return str(sibling)
    return shutil.which('samosa-extract')


def get_pdf_tokenizer():
    """Return the trusted installed tokenizer used for exact PDF page counts."""
    return (os.environ.get('SAMOSA_EXTRACT_TOKENIZER')
            or os.environ.get('TOKENIZER'))


def _run_pdf_sidecar(command):
    """Run a short-lived sidecar with a parent watchdog and parse its JSON.

    The extractor already applies its own CPU/memory limits.  The runner adds a
    wall-clock watchdog and starts a process group so a timeout cannot strand a
    child process in an unattended job.
    """
    try:
        proc = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True,
        )
        try:
            stdout, stderr = proc.communicate(timeout=25)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            return None, 'extract_timeout'
    except OSError:
        return None, 'extractor_unavailable:application/pdf'

    try:
        payload = json.loads(stdout)
    except (TypeError, json.JSONDecodeError):
        return None, 'extract_invalid_response'
    if proc.returncode != 0 or not isinstance(payload, dict):
        return None, 'extract_failed'
    if not payload.get('ok'):
        error = payload.get('error')
        return None, f"extract_failed:{error}" if isinstance(error, str) else 'extract_failed'
    return payload, None


def extract_pdf_metadata(input_meta):
    """Hydrate PDF metadata from `samosa-extract` with exact page token counts."""
    extractor = get_pdf_extractor()
    tokenizer = get_pdf_tokenizer()
    if not extractor:
        return None, 'extractor_unavailable:application/pdf'
    if not tokenizer or not os.path.isfile(tokenizer):
        return None, 'extractor_unavailable:tokenizer'

    payload, error = _run_pdf_sidecar(
        [extractor, '--json', input_meta['input_path'], '--tokenizer', tokenizer]
    )
    if error:
        return None, error

    document_tokens = payload.get('tokens')
    if (not isinstance(document_tokens, int) or isinstance(document_tokens, bool)
            or document_tokens < 0):
        return None, 'extract_invalid_response'
    raw_pages = payload.get('pages')
    if not isinstance(raw_pages, list) or not raw_pages:
        return None, 'extract_invalid_response'
    pages = []
    for page in raw_pages:
        if not isinstance(page, dict):
            return None, 'extract_invalid_response'
        index = page.get('index')
        tokens = page.get('tokens')
        text = page.get('text')
        if (not isinstance(index, int) or isinstance(index, bool) or index < 1
                or not isinstance(tokens, int) or isinstance(tokens, bool) or tokens < 0
                or not isinstance(text, str)):
            return None, 'extract_invalid_response'
        pages.append({
            'index': index,
            'text_tokens': tokens,
            'has_raster_figure': bool(page.get('has_raster_figure', False)),
            'text': text,
        })
    if len({page['index'] for page in pages}) != len(pages):
        return None, 'extract_invalid_response'
    document_text = payload.get('text')
    if not isinstance(document_text, str):
        return None, 'extract_invalid_response'
    return {'pages': pages, 'text': document_text, 'text_tokens': document_tokens}, None


def hydrate_pdf_input(input_meta):
    """Attach sidecar metadata to a discovered PDF, retaining a stable error."""
    if input_meta.get('media_type') != 'application/pdf' or input_meta.get('pages'):
        return None
    metadata, error = extract_pdf_metadata(input_meta)
    if error:
        input_meta['_pdf_extract_error'] = error
        return error
    input_meta['pages'] = metadata['pages']
    input_meta['_pdf_text'] = metadata['text']
    input_meta['text_tokens'] = metadata['text_tokens']
    return None


def _render_pdf_page(input_meta, page_index):
    """Render one PDF page and return it as an in-memory PPM data URI.

    PPM is the sidecar's bounded, local interchange format.  The file exists
    only under the job's intermediates directory and is unlinked before this
    function returns, so terminal events never leave document imagery behind.
    """
    extractor = get_pdf_extractor()
    if not extractor:
        return None, 'extractor_unavailable:application/pdf'
    parent = input_meta.get('_intermediates_dir')
    if parent:
        Path(parent).mkdir(parents=True, exist_ok=True)
        os.chmod(str(parent), 0o700)
    else:
        parent = tempfile.gettempdir()
    fd, output_path = tempfile.mkstemp(prefix='pdf-page-', suffix='.ppm', dir=str(parent))
    os.close(fd)
    os.unlink(output_path)  # the sidecar deliberately refuses pre-existing output
    try:
        _payload, error = _run_pdf_sidecar(
            [extractor, '--render-ppm', input_meta['input_path'], str(page_index), output_path]
        )
        if error:
            return None, error
        try:
            with open(output_path, 'rb') as f:
                image = f.read()
        except OSError:
            return None, 'extract_render_missing'
        if not image:
            return None, 'extract_render_empty'
        encoded = base64.b64encode(image).decode('ascii')
        return f'data:image/x-portable-pixmap;base64,{encoded}', None
    finally:
        try:
            os.unlink(output_path)
        except FileNotFoundError:
            pass


def extract_unit(unit, input_meta):
    """Extract content for a processing unit.
    Returns {text, image_data_uri, error}."""
    media_type = input_meta['media_type']
    path = input_meta['input_path']

    if unit.get('plan_reason') == 'extractor_unavailable':
        return {'error': input_meta.get('_pdf_extract_error',
                                        'extractor_unavailable:application/pdf')}
    if unit.get('plan_reason') == 'page_over_safe_prefill_budget':
        return {'error': 'unit_over_safe_prefill_budget'}

    if media_type.startswith('image/'):
        # Read image, encode as base64 data URI
        try:
            with open(path, 'rb') as f:
                data = f.read()
            mime = media_type
            b64 = base64.b64encode(data).decode('ascii')
            return {'image_data_uri': f'data:{mime};base64,{b64}'}
        except OSError as e:
            return {'error': f'read_failed:{e}'}

    if media_type == 'text/plain':
        try:
            with open(path, 'r', errors='replace') as f:
                text = f.read()
            # A chunk unit carries the char range the planner assigned (J1.2);
            # send only that slice so the split actually bounds the context.
            if unit.get('chunk_index') is not None:
                start = unit.get('char_start')
                end = unit.get('char_end')
                if start is not None and end is not None:
                    text = text[start:end]
            return {'text': text}
        except OSError as e:
            return {'error': f'read_failed:{e}'}

    if media_type == 'application/pdf':
        pages = input_meta.get('pages')
        if not pages:
            return {'error': input_meta.get('_pdf_extract_error',
                                            'extractor_unavailable:application/pdf')}
        if unit.get('granularity') == 'page':
            page_index = unit.get('page_index')
            page = next((p for p in pages if p['index'] == page_index), None)
            if page is None:
                return {'error': 'extract_page_not_found'}
            result = {'text': page['text']}
            if _page_needs_image(page):
                image, error = _render_pdf_page(input_meta, page_index)
                if error:
                    return {'error': error}
                result['image_data_uri'] = image
            return result

        result = {'text': input_meta.get('_pdf_text', '')}
        image_pages = [p for p in pages if _page_needs_image(p)]
        # A forced whole-file unit with multiple image pages intentionally keeps
        # only the first image (and carries the planner warning); F-J4 forbids
        # putting more than one image in one inference request.
        if image_pages:
            image, error = _render_pdf_page(input_meta, image_pages[0]['index'])
            if error:
                return {'error': error}
            result['image_data_uri'] = image
        return result

    return {'error': f'unsupported_type:{media_type}'}


# Minimum char span still worth splitting; below this a context_limit is
# treated as irreducible (J1.4).
MIN_SPLIT_CHARS = 200


def split_text_unit(unit, item):
    """Halve a text unit's char range on a line boundary, for a `400
    context_limit` retry (J1.4). Returns two chunk units, or None if the unit is
    not splittable text or is already minimal (→ context_limit_irreducible)."""
    if item.get('media_type') != 'text/plain':
        return None
    sha = item['input_sha256']
    start = unit.get('char_start', 0)
    end = unit.get('char_end')
    try:
        with open(item['input_path'], 'r', errors='replace') as f:
            text = f.read()
    except OSError:
        return None
    if end is None:
        end = len(text)
    if end - start < MIN_SPLIT_CHARS:
        return None

    mid = (start + end) // 2
    nl = text.find('\n', mid, end)          # snap to the next line boundary
    if nl == -1 or nl <= start or nl >= end - 1:
        nl = mid
    else:
        nl += 1

    def _piece(cs, ce):
        return {
            'unit_id': f"{sha}#c{cs}_{ce}",   # stable across re-runs (idempotent)
            'input_sha256': sha,
            'granularity': 'chunk',
            'plan_reason': 'context_split',
            'chunk_index': cs,                # ordered by char position
            'char_start': cs,
            'char_end': ce,
            'reduce_group': sha,
        }

    return [_piece(start, nl), _piece(nl, end)]


# ---------------------------------------------------------------------------
# J1.4 — Model call
# ---------------------------------------------------------------------------

def build_request_body(job, extraction, unit):
    """Build the serve request body for a unit."""
    schema_str = json.dumps(job['output_schema'], indent=None)
    system_prompt = (
        f"{job['instruction']}\n\n"
        f"Output JSON Schema:\n{schema_str}\n\n"
        f"Return ONLY a JSON object matching the schema above. "
        f"No prose, no markdown fences, no explanation."
    )

    user_content = []
    if extraction.get('text'):
        user_content.append({'type': 'text', 'text': extraction['text']})
    if extraction.get('image_data_uri'):
        user_content.append({
            'type': 'image_url',
            'image_url': {'url': extraction['image_data_uri']}
        })

    if not user_content:
        return None

    inf = job.get('inference', {})
    body = {
        'model': 'qwen3.6-35b-a3b',
        'messages': [
            {'role': 'system', 'content': system_prompt},
            {'role': 'user', 'content': user_content if len(user_content) > 1
             else user_content[0].get('text') or user_content},
        ],
        'thinking': inf.get('thinking', 'off'),
        'seed': inf.get('seed', 11),
        'temperature': inf.get('temperature', 0),
        'max_tokens': inf.get('max_tokens', 512),
        'stream': False,
    }
    return body


def call_serve(body, serve_url, timeout=None, is_background=True):
    """POST to samosa serve. Returns (response_dict, error_str)."""
    data = json.dumps(body).encode('utf-8')

    # Check body size (F-J5)
    if len(data) > HTTP_MAX_BODY:
        return None, 'image_too_large'

    headers = {
        'Content-Type': 'application/json',
    }
    if is_background:
        headers['X-Samosa-Priority'] = 'background'

    url = f"{serve_url}/v1/chat/completions"
    req = urllib.request.Request(url, data=data, headers=headers, method='POST')

    try:
        if timeout:
            resp = urllib.request.urlopen(req, timeout=timeout)
        else:
            resp = urllib.request.urlopen(req)
        resp_data = resp.read()
        return json.loads(resp_data), None
    except urllib.error.HTTPError as e:
        body_text = e.read().decode('utf-8', errors='replace')
        try:
            err_json = json.loads(body_text)
            code = err_json.get('error', {}).get('code', str(e.code))
        except json.JSONDecodeError:
            code = str(e.code)
        return None, code
    except (TimeoutError, socket.timeout):
        return None, 'timeout'
    except urllib.error.URLError as e:
        if isinstance(e.reason, (TimeoutError, socket.timeout)):
            return None, 'timeout'
        return None, f'connection_error:{e.reason}'
    except Exception as e:
        return None, f'request_error:{e}'


def request_cancel(serve_url):
    """Ask the local server to cooperatively stop its active inference.

    Closing the timed-out client socket alone cannot interrupt a non-streaming
    prefill: the server has nothing to write until inference finishes.  The
    loopback-only cancel endpoint flips the model's checked cancellation flag.
    """
    req = urllib.request.Request(f"{serve_url}/v1/cancel", data=b'', method='POST')
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            payload = json.loads(response.read())
        return bool(payload.get('cancelled'))
    except (OSError, ValueError, json.JSONDecodeError, urllib.error.URLError):
        return False


def derive_timing(resp, wall_seconds):
    """B2 — provenance timing: serve stats when present, else runner wall-clock.

    Serve carries no explicit prefill/decode seconds in its response; the only
    timing it exposes is ``samosa.tokens_per_second`` (the decode rate). Combined
    with the completion-token count that yields ``decode_seconds``; the remainder
    of the measured wall-clock is attributed to prefill (loopback HTTP overhead is
    negligible against the minutes a real prefill costs). When the decode rate is
    absent, the split cannot be recovered honestly, so only the measured total
    (``wall_seconds``) is recorded and prefill/decode stay ``null`` rather than
    being fabricated. ``wall_seconds`` is always the runner's own measurement.
    """
    timing = {
        'wall_seconds': round(wall_seconds, 3),
        'prefill_seconds': None,
        'decode_seconds': None,
    }
    samosa = (resp or {}).get('samosa') or {}
    usage = (resp or {}).get('usage') or {}
    tps = samosa.get('tokens_per_second')
    completion = usage.get('completion_tokens')
    if (isinstance(tps, (int, float)) and not isinstance(tps, bool) and tps > 0
            and isinstance(completion, int) and not isinstance(completion, bool)
            and completion > 1):
        decode_s = (completion - 1) / tps
        timing['decode_seconds'] = round(decode_s, 3)
        timing['prefill_seconds'] = round(max(0.0, wall_seconds - decode_s), 3)
    return timing


def get_serve_status(serve_url):
    """GET /internal/v1/status. Returns dict or None."""
    url = f"{serve_url}/internal/v1/status"
    try:
        resp = urllib.request.urlopen(url, timeout=5)
        return json.loads(resp.read())
    except Exception:
        return None


# ---------------------------------------------------------------------------
# J1.6 — Atomic artifact + provenance write
# ---------------------------------------------------------------------------

def write_result_and_provenance(job_dir, unit_id, result, provenance):
    """Atomically write result and provenance files."""
    items_dir = Path(job_dir) / 'results' / 'items'
    items_dir.mkdir(parents=True, exist_ok=True)

    safe_uid = unit_id.replace('#', '_').replace('/', '_')
    result_path = items_dir / f"{safe_uid}.json"
    prov_path = items_dir / f"{safe_uid}.provenance.json"

    atomic_write(result_path, json.dumps(result, indent=2))
    atomic_write(prov_path, json.dumps(provenance, indent=2))

    return str(result_path), str(prov_path)


# ---------------------------------------------------------------------------
# J1.7 — Event log + recovery + process lock
# ---------------------------------------------------------------------------

class EventLog:
    """Append-only JSONL event log with monotonic sequence numbers."""

    def __init__(self, path):
        self.path = Path(path)
        self.seq = 0
        self.events = []

    def load(self):
        """Load existing events, ignoring a torn final line."""
        self.events = []
        self.seq = 0
        if not self.path.exists():
            return
        with open(self.path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    evt = json.loads(line)
                    self.events.append(evt)
                    s = evt.get('seq', 0)
                    if s > self.seq:
                        self.seq = s
                except json.JSONDecodeError:
                    # Torn write — ignore the last non-JSON line
                    pass

    def append(self, event_type, **fields):
        """Append an event to the log."""
        self.seq += 1
        event = {
            'seq': self.seq,
            'ts': rfc3339_now(),
            'type': event_type,
            **fields,
        }
        self.events.append(event)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.path, 'a') as f:
            f.write(json.dumps(event, separators=(',', ':')) + '\n')
            f.flush()
            os.fsync(f.fileno())
        return event

    def get_unit_state(self, unit_id):
        """Get the last event type for a unit_id."""
        last = None
        for evt in self.events:
            if evt.get('unit_id') == unit_id:
                last = evt
        return last

    def get_terminal_units(self):
        """Get set of unit_ids that have reached a terminal state."""
        terminal = set()
        # item_split resolves the original unit: it is replaced by its pieces,
        # which carry their own planned + terminal events (J1.4 split).
        terminal_types = {'item_complete', 'item_review_required', 'item_failed',
                          'item_split'}
        for evt in self.events:
            uid = evt.get('unit_id')
            if uid and evt['type'] in terminal_types:
                terminal.add(uid)
        return terminal

    def get_processed_inputs(self, max_attempts):
        """Get set of input_sha256 whose planned units are all terminal.

        A split file (>1 planned unit) also requires its `doc_reduced` event
        (recovery step 5): without it the reduce would be skipped forever on a
        resumed run. Single-unit inputs need no reduce."""
        # Build mapping: input_sha256 -> set of planned unit_ids
        planned = {}
        for evt in self.events:
            if evt['type'] == 'item_planned':
                sha = evt.get('input_sha256')
                uid = evt.get('unit_id')
                if sha and uid:
                    planned.setdefault(sha, set()).add(uid)

        reduced = {evt.get('input_sha256') for evt in self.events
                   if evt['type'] == 'doc_reduced'}

        terminal = self.get_terminal_units()
        processed = set()
        for sha, units in planned.items():
            if not units.issubset(terminal):
                continue
            if len(units) > 1 and sha not in reduced:
                continue  # split file awaiting reduction
            processed.add(sha)
        return processed


class JobLock:
    """Advisory flock on job.lock."""

    def __init__(self, lock_path):
        self.lock_path = Path(lock_path)
        self.fd = None

    def acquire(self):
        """Acquire the lock. Returns True on success, False if held."""
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        self.fd = open(self.lock_path, 'w')
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.fd.write(str(os.getpid()))
            self.fd.flush()
            return True
        except (IOError, OSError):
            self.fd.close()
            self.fd = None
            return False

    def release(self):
        if self.fd:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_UN)
                self.fd.close()
            except (IOError, OSError):
                pass
            self.fd = None


# ---------------------------------------------------------------------------
# J1.9 — Page reduction
# ---------------------------------------------------------------------------

# Conservative char budget for a single model-reduce call; the server's
# `400 context_limit` (F-J9) remains the ultimate authority. When the serialized
# reducer payload exceeds this, the set is reduced hierarchically (in batches).
REDUCE_PAYLOAD_CHAR_BUDGET = 40000


def _det_merge_field(fname, units_results):
    """Deterministic scalar merge for one field: (value, conflict?)."""
    values = []
    for ur in units_results:
        rec = ur.get('record')
        if rec and fname in rec and rec[fname] is not None:
            values.append(rec[fname])
    if not values:
        return None, False
    if len(set(json.dumps(v, sort_keys=True) for v in values)) == 1:
        return values[0], False
    return None, True  # conflict


def _missing_pages(units_results):
    """Units with no usable record — must never be papered over (J1.9)."""
    missing = []
    for ur in units_results:
        if ur.get('status') == 'review_required' and 'unparseable' in ur.get('errors', []):
            missing.append(ur.get('unit_id', '?'))
        elif ur.get('status') == 'failed':
            missing.append(ur.get('unit_id', '?'))
    return missing


def _reduce_payload(units_results):
    """Reducer input always carries page status + provenance so a model reduce
    cannot silently drop pages (J1.9)."""
    payload = []
    for i, ur in enumerate(units_results):
        entry = {
            'page': ur.get('page_index', ur.get('unit_id', i)),
            'status': ur.get('status', 'passed'),
            'record': ur.get('record') or {},
        }
        if ur.get('errors'):
            entry['reasons'] = ur['errors']
        payload.append(entry)
    return payload


def _model_reduce(payload, fields, model_call, _depth=0):
    """Call the model over the page-status payload for the narrative `fields`.
    Reduces hierarchically when the payload would exceed the context ceiling.
    Returns a record dict, or None if the model call failed."""
    if (len(json.dumps(payload)) > REDUCE_PAYLOAD_CHAR_BUDGET
            and len(payload) > 1 and _depth < 8):
        mid = len(payload) // 2
        partials = []
        for half in (payload[:mid], payload[mid:]):
            rec = _model_reduce(half, fields, model_call, _depth + 1)
            partials.append({
                'page': 'batch',
                'status': 'passed' if rec is not None else 'review_required',
                'record': rec or {},
            })
        return _model_reduce(partials, fields, model_call, _depth + 1)

    content = model_call(payload, fields)
    if content is None:
        return None
    # Reuse the J1.5 string-aware scanner to recover the JSON object.
    val = validate_output(content, {'type': 'object',
                                     'properties': {f: {} for f in fields}})
    return val.get('record')


def reduce_units(units_results, schema, reduce_config, model_call=None):
    """Merge split-file page/chunk units into a single document record.

    `model_call(payload, fields) -> content_str | None` is injected so the model
    path is testable offline; when a model reduce is required but no `model_call`
    is provided the document is flagged for review rather than fabricated.
    Returns (merged_record, validation, method)."""
    mode = reduce_config.get('mode', 'deterministic')
    model_fields = reduce_config.get('model_fields', [])
    properties = schema.get('properties', {})

    # Which fields does the model own? mode:"model" sends the whole set; otherwise
    # only the named narrative `model_fields` use the model, scalars stay deterministic.
    model_field_set = set(properties) if mode == 'model' else set(model_fields)

    merged = {}
    errors = []

    # Deterministic scalar merge for every field the model does NOT own.
    for fname in properties:
        if fname in model_field_set:
            continue
        value, conflict = _det_merge_field(fname, units_results)
        merged[fname] = value
        if conflict:
            errors.append(f"reduce_conflict:{fname}")

    # Missing/failed pages are surfaced, never hidden.
    missing = _missing_pages(units_results)
    if missing:
        errors.append(f"missing_pages:{','.join(missing)}")

    # Carry the union of page-level review reasons.
    all_reasons = []
    for ur in units_results:
        all_reasons.extend(ur.get('errors', []))

    method = 'deterministic'
    if model_field_set:
        method = 'model'
        if model_call is None:
            for fname in model_field_set:
                merged[fname] = None
            errors.append('model_reduce_unavailable')
        else:
            model_record = _model_reduce(
                _reduce_payload(units_results), sorted(model_field_set), model_call)
            if model_record is None:
                for fname in model_field_set:
                    merged[fname] = None
                errors.append('model_reduce_failed')
            else:
                for fname in model_field_set:
                    merged[fname] = model_record.get(fname)

    status = 'review_required' if errors or all_reasons else 'passed'
    return merged, {'status': status, 'errors': errors}, method


def _gather_group_results(group_units, job_dir, event_log):
    """Rebuild units_results for a reduce group from stored records + event status.

    Record content comes from results/items/<unit>.json (written for every unit
    that produced output); status/errors come from the unit's last terminal event.
    Ordered deterministically by page/chunk index then unit_id (J1.11)."""
    status_by_uid = {}
    split_uids = set()
    for e in event_log.events:
        uid = e.get('unit_id')
        if not uid:
            continue
        t = e['type']
        if t == 'item_complete':
            status_by_uid[uid] = ('passed', [])
        elif t == 'item_review_required':
            status_by_uid[uid] = ('review_required', list(e.get('reasons', [])))
        elif t == 'item_failed':
            status_by_uid[uid] = ('failed', [e.get('error', 'failed')])
        elif t == 'item_split':
            split_uids.add(uid)  # replaced by its pieces; not a page of its own

    items_dir = Path(job_dir) / 'results' / 'items'
    results = []
    seen = set()
    for u in group_units:
        uid = u['unit_id']
        if uid in split_uids or uid in seen:
            continue
        seen.add(uid)
        status, errors = status_by_uid.get(uid, ('failed', ['no_terminal_event']))
        safe_uid = uid.replace('#', '_').replace('/', '_')
        rec_path = items_dir / f"{safe_uid}.json"
        record = None
        if rec_path.exists():
            try:
                record = json.loads(rec_path.read_text())
            except (json.JSONDecodeError, OSError):
                record = None
        results.append({
            'unit_id': uid,
            'page_index': u.get('page_index'),
            'chunk_index': u.get('chunk_index'),
            'char_start': u.get('char_start'),
            'record': record,
            'status': status,
            'errors': errors,
        })

    def _order(r):
        # char_start orders text chunks (incl. runtime splits); page_index orders
        # PDF pages. A group is homogeneous, so one key applies.
        for k in ('char_start', 'page_index', 'chunk_index'):
            if r.get(k) is not None:
                return (0, r[k], r['unit_id'])
        return (1, 0, r['unit_id'])

    results.sort(key=_order)
    return results


def _make_reduce_model_call(job, serve_url):
    """Return a `model_call(payload, fields) -> content|None` that asks serve to
    merge the narrative fields of a split document (J1.9 model path)."""
    inf = job.get('inference', {})

    def model_call(payload, fields):
        system = (
            "You merge structured records extracted from the pages of ONE document "
            "into a single record. Each entry has a status and its page record. "
            "Use ONLY information present in those records; never invent a value. "
            f"Produce ONLY these fields: {', '.join(fields)}. "
            "Return ONLY a JSON object — no prose, no code fences."
        )
        user = json.dumps(payload, ensure_ascii=False)
        body = {
            'messages': [
                {'role': 'system', 'content': system},
                {'role': 'user', 'content': user},
            ],
            'thinking': 'off',
            'temperature': 0,
            'seed': inf.get('seed', 11),
            'max_tokens': inf.get('max_tokens', 512),
            'stream': False,
        }
        call_started = time.perf_counter()
        resp, err = call_serve(body, serve_url)
        model_call.wall_seconds += time.perf_counter() - call_started
        if err:
            return None
        return resp.get('choices', [{}])[0].get('message', {}).get('content', '')

    model_call.wall_seconds = 0.0
    return model_call


def _recover_orphans(job, job_dir, event_log, all_units):
    """Recovery step 4 — an artifact + provenance present with NO terminal event
    (a crash in the window between rename and the event append) is reconciled by
    re-validating and appending the missing event, not silently reprocessed.
    Absence of an event does not imply absence of output."""
    items_dir = Path(job_dir) / 'results' / 'items'
    if not items_dir.exists():
        return
    terminal = event_log.get_terminal_units()
    schema = job['output_schema']
    domain_rules = job.get('validation', {}).get('domain_rules')
    recovered = 0
    for unit, item in all_units:
        uid = unit['unit_id']
        if uid in terminal:
            continue
        safe_uid = uid.replace('#', '_').replace('/', '_')
        rec_path = items_dir / f"{safe_uid}.json"
        prov_path = items_dir / f"{safe_uid}.provenance.json"
        if not (rec_path.exists() and prov_path.exists()):
            continue
        try:
            record = json.loads(rec_path.read_text())
            provenance = json.loads(prov_path.read_text())
            if (not isinstance(provenance, dict)
                    or provenance.get('unit_id') != uid):
                raise ValueError('invalid provenance')
        except (json.JSONDecodeError, OSError, ValueError):
            # Not usable — drop both and let the unit re-run as READY.
            rec_path.unlink(missing_ok=True)
            prov_path.unlink(missing_ok=True)
            continue
        model_call_seconds = provenance.get('wall_seconds')
        validation = validate_output(json.dumps(record), schema, domain_rules)
        if validation['status'] == 'passed':
            event_log.append('item_complete', unit_id=uid,
                             input_sha256=item['input_sha256'],
                             input_path=item['input_path'],
                             artifact=uid, validation='passed',
                             model_call_seconds=model_call_seconds)
        else:
            review_dir = Path(job_dir) / 'results' / 'review'
            review_dir.mkdir(parents=True, exist_ok=True)
            atomic_write(review_dir / rec_path.name,
                         json.dumps(record, indent=2))
            event_log.append('item_review_required', unit_id=uid,
                             input_sha256=item['input_sha256'],
                             input_path=item['input_path'],
                             reasons=validation['errors'],
                             model_call_seconds=model_call_seconds)
        recovered += 1
    if recovered:
        print(f"[jobs] recovered {recovered} orphaned artifact(s) with no terminal event")


def _reduce_completed_groups(job, job_dir, event_log, unit_by_group, serve_url):
    """J1.9 — for each split-file reduce group whose units are all terminal and
    which has not yet been reduced, produce one document record and emit
    doc_reduced. Deterministic groups need no model; model groups gate first."""
    reduce_config = job.get('reduce', {'mode': 'deterministic', 'model_fields': []})
    schema = job['output_schema']
    needs_model = (reduce_config.get('mode') == 'model'
                   or bool(reduce_config.get('model_fields')))

    already_reduced = {e.get('input_sha256') for e in event_log.events
                       if e['type'] == 'doc_reduced'}

    for group_sha in sorted(unit_by_group):
        if group_sha in already_reduced:
            continue
        group_units = unit_by_group[group_sha]
        terminal = event_log.get_terminal_units()
        if not all(u['unit_id'] in terminal for u in group_units):
            continue  # group not finished; leave for a later run

        units_results = _gather_group_results(group_units, job_dir, event_log)

        mc = None
        model_call_seconds = 0.0
        if needs_model:
            ok, reason = gate_check(job, serve_url)
            while not ok:
                event_log.append('job_paused', reason=reason)
                print(f"[jobs] paused (reduce): {reason}. Waiting...")
                time.sleep(30)
                ok, reason = gate_check(job, serve_url)
                if ok:
                    event_log.append('job_resumed', reason=f"cleared:{reason}")
            mc = _make_reduce_model_call(job, serve_url)

        merged, validation, method = reduce_units(
            units_results, schema, reduce_config, model_call=mc)
        if mc is not None:
            model_call_seconds = mc.wall_seconds

        docs_dir = Path(job_dir) / 'results' / 'documents'
        docs_dir.mkdir(parents=True, exist_ok=True)
        doc_path = docs_dir / f"{group_sha}.json"
        atomic_write(doc_path, json.dumps(merged, indent=2))

        event_log.append('doc_reduced',
                         input_sha256=group_sha,
                         artifact=doc_path.name,
                         validation=validation['status'],
                         method=method,
                         model_call_seconds=model_call_seconds)
        print(f"[jobs] reduced {group_sha[:12]}: {validation['status']} ({method})")


# ---------------------------------------------------------------------------
# J1.12 — Static Jobs view
# ---------------------------------------------------------------------------

def render_view_html(job, events, job_dir):
    """Render results/view.html — fully escaped, self-contained."""
    items_dir = Path(job_dir) / 'results' / 'items'

    # Rebuild per-unit rows without losing discovery/planning metadata when a
    # later state event becomes authoritative.
    input_paths = {
        evt.get('input_sha256'): evt.get('input_path', '')
        for evt in events
        if evt['type'] == 'item_discovered' and evt.get('input_sha256')
    }
    units = {}
    for evt in events:
        uid = evt.get('unit_id')
        if not uid:
            continue
        row = units.setdefault(uid, {'unit_id': uid})
        for field in ('input_sha256', 'input_path', 'granularity',
                      'page_index', 'chunk_index'):
            if evt.get(field) is not None:
                row[field] = evt[field]
        if evt['type'].startswith('item_'):
            row['state_event'] = evt
    for row in units.values():
        if not row.get('input_path'):
            row['input_path'] = input_paths.get(row.get('input_sha256'), '')

    # Count stats
    total = len(units)
    passed = sum(1 for u in units.values()
                 if u.get('state_event', {}).get('type') == 'item_complete')
    review = sum(1 for u in units.values()
                 if u.get('state_event', {}).get('type') == 'item_review_required')
    failed = sum(1 for u in units.values()
                 if u.get('state_event', {}).get('type') == 'item_failed')

    # Wall time: first → last event.
    timestamps = [evt.get('ts', '') for evt in events if evt.get('ts')]
    wall_range = ''
    wall_seconds = None
    if len(timestamps) >= 2:
        wall_range = f"{timestamps[0]} → {timestamps[-1]}"
        try:
            t0 = datetime.fromisoformat(timestamps[0].replace('Z', '+00:00'))
            t1 = datetime.fromisoformat(timestamps[-1].replace('Z', '+00:00'))
            wall_seconds = (t1 - t0).total_seconds()
        except (ValueError, TypeError):
            pass

    # Active inference time: sum every recorded model call, including retries,
    # context-limit calls, and model reduction. For older logs that predate the
    # event field, fall back to the final per-unit provenance.
    active_seconds = 0.0
    units_with_event_timing = set()
    for evt in events:
        w = evt.get('model_call_seconds')
        if isinstance(w, (int, float)) and not isinstance(w, bool):
            active_seconds += w
            if evt.get('unit_id'):
                units_with_event_timing.add(evt['unit_id'])
    for uid in units:
        if uid in units_with_event_timing:
            continue
        safe_uid = uid.replace('#', '_').replace('/', '_')
        prov_path = items_dir / f"{safe_uid}.provenance.json"
        if prov_path.exists():
            try:
                p = json.loads(prov_path.read_text())
                w = p.get('wall_seconds')
                if isinstance(w, (int, float)) and not isinstance(w, bool):
                    active_seconds += w
            except (json.JSONDecodeError, OSError):
                pass

    def _fmt(secs):
        return f"{secs:.1f}s" if isinstance(secs, (int, float)) else "—"

    job_name = html_escape(job.get('name', job.get('job_id', 'unknown')))

    # REVIEW_REQUIRED queue first (each reason shown).
    review_rows = []
    for uid in sorted(units.keys()):
        row = units[uid]
        evt = row.get('state_event', {})
        if evt.get('type') != 'item_review_required':
            continue
        reasons = html_escape(', '.join(evt.get('reasons', [])))
        review_rows.append(
            f'<tr><td>{html_escape(uid)}</td>'
            f'<td>{html_escape(row.get("input_path", row.get("input_sha256", "")))}</td>'
            f'<td>{reasons}</td></tr>'
        )
    review_section = ''
    if review_rows:
        review_section = (
            '<h2 class="review">Needs review (' + str(review) + ')</h2>'
            '<table><tr><th>Unit ID</th><th>Input</th><th>Reasons</th></tr>'
            + ''.join(review_rows) + '</table>'
        )

    # Full per-item table with links to result / provenance.
    rows_html = []
    for uid in sorted(units.keys()):
        row = units[uid]
        evt = row.get('state_event', {})
        state = evt.get('type', 'unknown').replace('item_', '')
        input_path = html_escape(row.get('input_path', row.get('input_sha256', '')))
        granularity = html_escape(row.get('granularity', ''))
        safe_uid = uid.replace('#', '_').replace('/', '_')
        artifact_links = []
        if (items_dir / f"{safe_uid}.json").exists():
            artifact_links.append(
                f'<a href="{html_escape(f"items/{safe_uid}.json")}">result</a>')
        if (items_dir / f"{safe_uid}.provenance.json").exists():
            artifact_links.append(
                f'<a href="{html_escape(f"items/{safe_uid}.provenance.json")}">prov</a>')
        reasons = ''
        if 'reasons' in evt:
            reasons = html_escape(', '.join(evt['reasons']))
        elif 'error' in evt:
            reasons = html_escape(str(evt['error']))
        rows_html.append(
            f'<tr><td>{html_escape(uid)}</td><td>{input_path}</td>'
            f'<td>{granularity}</td><td class="{html_escape(state)}">{html_escape(state)}</td>'
            f'<td>{reasons}</td>'
            f'<td>{" · ".join(artifact_links)}</td></tr>'
        )

    view_html = f"""<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>Samosa Jobs — {job_name}</title>
<style>
body {{ font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 2em; background: #f5f5f5; color: #333; }}
h1 {{ font-size: 1.4em; }}
h2 {{ font-size: 1.1em; margin-top: 1.5em; }}
.summary {{ background: #fff; padding: 1em; border-radius: 8px; margin-bottom: 1em; box-shadow: 0 1px 3px rgba(0,0,0,.1); }}
table {{ border-collapse: collapse; width: 100%; background: #fff; border-radius: 8px; overflow: hidden; box-shadow: 0 1px 3px rgba(0,0,0,.1); margin-bottom: 1em; }}
th, td {{ text-align: left; padding: 0.5em 1em; border-bottom: 1px solid #eee; font-size: 0.9em; }}
th {{ background: #fafafa; font-weight: 600; }}
.review {{ color: #d97706; font-weight: 600; }}
.failed {{ color: #dc2626; }}
.passed {{ color: #16a34a; }}
</style></head>
<body>
<h1>Samosa Jobs — {job_name}</h1>
<div class="summary">
<p><strong>Total:</strong> {total} &nbsp; <span class="passed">Passed: {passed}</span> &nbsp;
<span class="review">Review: {review}</span> &nbsp; <span class="failed">Failed: {failed}</span></p>
<p><strong>Wall time:</strong> {_fmt(wall_seconds)} <span style="color:#999">({html_escape(wall_range)})</span></p>
<p><strong>Active inference time:</strong> {_fmt(active_seconds)}</p>
</div>
{review_section}
<h2>All items</h2>
<table>
<tr><th>Unit ID</th><th>Input</th><th>Granularity</th><th>State</th><th>Details</th><th>Artifacts</th></tr>
{''.join(rows_html)}
</table>
</body></html>"""

    view_path = Path(job_dir) / 'results' / 'view.html'
    view_path.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(view_path, view_html)
    return str(view_path)


# ---------------------------------------------------------------------------
# J1.13 — Resource gate + chat interlock
# ---------------------------------------------------------------------------

def gate_check(job, serve_url):
    """Check if it is safe to proceed. Returns (ok, reason) where ok=True means proceed."""
    resources = job.get('resources', {})

    # 1. Chat interlock
    status = get_serve_status(serve_url)
    if status:
        if status.get('interactive_active'):
            return False, 'interactive_chat'
        # Check cool-down (60s)
        last_ts = status.get('last_interactive_ts')
        if last_ts and last_ts != 'null' and isinstance(last_ts, str):
            try:
                last_dt = datetime.fromisoformat(last_ts.replace('Z', '+00:00'))
                elapsed = (datetime.now(timezone.utc) - last_dt).total_seconds()
                if elapsed < 60:
                    return False, 'interactive_chat'
            except (ValueError, TypeError):
                pass

    # 2. Free storage
    min_free_gb = resources.get('min_free_gb', 5)
    try:
        st = os.statvfs('/')
        free_gb = (st.f_bavail * st.f_frsize) / (1024 ** 3)
        if free_gb < min_free_gb:
            return False, 'low_disk'
    except OSError:
        pass

    # 3. Power policy
    if not resources.get('run_on_battery', False):
        if _on_battery():
            return False, 'on_battery'

    # 4. Memory pressure (macOS)
    if platform.system() == 'Darwin':
        pressure = _macos_memory_pressure()
        if pressure and pressure >= 2:  # WARN or CRITICAL
            return False, 'memory_pressure'

    return True, None


def _on_battery():
    """Check if running on battery (macOS)."""
    if platform.system() == 'Darwin':
        try:
            result = subprocess.run(
                ['pmset', '-g', 'batt'], capture_output=True, text=True, timeout=5
            )
            if 'Battery Power' in result.stdout:
                return True
        except (subprocess.TimeoutExpired, OSError):
            pass
    elif platform.system() == 'Linux':
        try:
            for ps in Path('/sys/class/power_supply').iterdir():
                type_file = ps / 'type'
                if type_file.exists() and type_file.read_text().strip() == 'Mains':
                    online_file = ps / 'online'
                    if online_file.exists() and online_file.read_text().strip() == '1':
                        return False
                    return True
        except OSError:
            pass
    return False


def _macos_memory_pressure():
    """Read macOS memory pressure level. Returns 0=normal, 1=warn, 2+=critical."""
    try:
        result = subprocess.run(
            ['memory_pressure'], capture_output=True, text=True, timeout=5
        )
        if 'CRITICAL' in result.stdout:
            return 3
        if 'WARN' in result.stdout:
            return 2
    except (subprocess.TimeoutExpired, OSError, FileNotFoundError):
        pass
    return 0


# ---------------------------------------------------------------------------
# J1.11 — Merged output
# ---------------------------------------------------------------------------

def write_merged_output(job, job_dir, event_log):
    """Write results/output.jsonl or output.csv."""
    fmt = job.get('output', {}).get('format', 'jsonl')
    items_dir = Path(job_dir) / 'results' / 'items'
    docs_dir = Path(job_dir) / 'results' / 'documents'
    results_dir = Path(job_dir) / 'results'

    # A split file that was reduced becomes ONE document row; its per-chunk/page
    # units are folded in, never emitted individually (J1.11). Whole-file inputs
    # (no doc_reduced) emit their unit record.
    reduced = {}        # input_sha256 -> doc_reduced event
    input_path_by_sha = {}
    for evt in event_log.events:
        if evt['type'] == 'doc_reduced':
            reduced[evt.get('input_sha256')] = evt
        sha = evt.get('input_sha256')
        if sha and evt.get('input_path'):
            input_path_by_sha.setdefault(sha, evt['input_path'])

    completed = []

    # One record per passed reduced document.
    for sha, dev in reduced.items():
        if dev.get('validation') != 'passed':
            continue
        doc_file = docs_dir / f"{sha}.json"
        if doc_file.exists():
            try:
                record = json.loads(doc_file.read_text())
                completed.append({
                    'input_sha256': sha,
                    'input_path': input_path_by_sha.get(sha, ''),
                    **record,
                })
            except (json.JSONDecodeError, OSError):
                pass

    # One record per passed whole-file unit not belonging to a reduced document.
    for evt in event_log.events:
        if evt['type'] != 'item_complete':
            continue
        sha = evt.get('input_sha256', '')
        if sha in reduced:
            continue  # covered by the document row above
        uid = evt.get('unit_id', '')
        safe_uid = uid.replace('#', '_').replace('/', '_')
        result_file = items_dir / f"{safe_uid}.json"
        if result_file.exists():
            try:
                record = json.loads(result_file.read_text())
                completed.append({
                    'input_sha256': sha,
                    'input_path': evt.get('input_path', ''),
                    **record,
                })
            except (json.JSONDecodeError, OSError):
                pass

    # Deterministic order by input_path (one row per document/whole-file input).
    completed.sort(key=lambda r: r.get('input_path', ''))

    # Honor the job's configured output.dir; fall back to the job's results/ dir.
    out_dir = Path(job.get('output', {}).get('dir') or results_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if fmt == 'jsonl':
        out_path = out_dir / 'output.jsonl'
        lines = [json.dumps(r, separators=(',', ':')) for r in completed]
        atomic_write(out_path, '\n'.join(lines) + '\n' if lines else '')
    else:
        out_path = out_dir / 'output.csv'
        if completed:
            props = list(job.get('output_schema', {}).get('properties', {}).keys())
            fieldnames = ['input_sha256', 'input_path'] + props
            buf = io.StringIO()
            writer = csv.DictWriter(buf, fieldnames=fieldnames, extrasaction='ignore')
            writer.writeheader()
            for r in completed:
                writer.writerow(r)
            atomic_write(out_path, buf.getvalue())
        else:
            atomic_write(out_path, '')


# ---------------------------------------------------------------------------
# Main runner — orchestrates J1.0 through J1.13
# ---------------------------------------------------------------------------

def get_jobs_root():
    """Get the jobs root directory."""
    env = os.environ.get('SAMOSA_JOBS_DIR')
    if env:
        return Path(env)
    home = os.environ.get('HOME', '.')
    return Path(home) / '.samosa' / 'jobs'


def _planned_event_fields(unit):
    """Persist enough planner state to reconstruct dynamically split units."""
    fields = {
        'unit_id': unit['unit_id'],
        'input_sha256': unit['input_sha256'],
        'granularity': unit['granularity'],
        'plan_reason': unit['plan_reason'],
    }
    for name in ('page_index', 'chunk_index', 'char_start', 'char_end',
                 'reduce_group'):
        if unit.get(name) is not None:
            fields[name] = unit[name]
    return fields


def _restore_logged_units(event_log, items_by_sha, existing_ids):
    """Rebuild units that were created at runtime by context-limit splitting.

    Older logs did not persist char ranges, so recover them from the stable
    ``<sha>#c<start>_<end>`` unit id when possible.
    """
    restored = []
    for evt in event_log.events:
        if evt['type'] != 'item_planned':
            continue
        uid = evt.get('unit_id')
        sha = evt.get('input_sha256')
        if not uid or uid in existing_ids or sha not in items_by_sha:
            continue
        unit = {
            name: evt[name]
            for name in ('unit_id', 'input_sha256', 'granularity', 'plan_reason',
                         'page_index', 'chunk_index', 'char_start', 'char_end',
                         'reduce_group')
            if evt.get(name) is not None
        }
        if unit.get('plan_reason') == 'context_split':
            match = re.search(r'#c(\d+)_(\d+)$', uid)
            if match:
                unit.setdefault('char_start', int(match.group(1)))
                unit.setdefault('char_end', int(match.group(2)))
                unit.setdefault('chunk_index', unit['char_start'])
            unit.setdefault('reduce_group', sha)
        if (unit.get('granularity') == 'chunk'
                and (unit.get('char_start') is None
                     or unit.get('char_end') is None)):
            continue
        existing_ids.add(uid)
        restored.append((unit, items_by_sha[sha]))
    return restored


def get_serve_url():
    return os.environ.get('SAMOSA_SERVE_URL', SERVE_URL_DEFAULT)


def get_tokenizer_cmd():
    """Get the tokenizer command for token counting."""
    engine = os.environ.get('SAMOSA_ENGINE', 'qwen36b')
    tokenizer = os.environ.get('TOKENIZER')
    cmd = [engine, 'tokenize', '--count']
    if tokenizer:
        cmd.extend(['--tokenizer', tokenizer])
    return cmd


def get_prefill_budget(job):
    """Return the Jobs input ceiling, deliberately below engine context.

    The context window is a correctness limit, not a sensible unattended
    prefill target on a laptop.  The job may lower this ceiling but can never
    raise it beyond the conservative product maximum.
    """
    inference = job.get('inference', {})
    context_budget = MAX_CONTEXT - inference.get('max_tokens', 512) - SYSTEM_RESERVE
    configured = job.get('resources', {}).get('max_input_tokens', MAX_JOB_INPUT_TOKENS)
    return min(context_budget, configured, MAX_JOB_INPUT_TOKENS)


def cmd_validate(args):
    """samosa jobs validate <job.json>"""
    if len(args) < 1:
        print("Usage: samosa jobs validate <job.json>", file=sys.stderr)
        return 2
    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2
    print(json.dumps(job, indent=2))
    return 0


def cmd_arm(args):
    """samosa jobs arm <job.json> — freeze the definition into the job dir."""
    if len(args) < 1:
        print("Usage: samosa jobs arm <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_id = job['job_id']
    jobs_root = get_jobs_root()
    job_dir = jobs_root / job_id
    frozen_path = job_dir / 'job.json'

    if frozen_path.exists():
        existing = json.loads(frozen_path.read_text())
        existing_hash = sha256_bytes(json.dumps(existing, sort_keys=True).encode())
        new_hash = sha256_bytes(json.dumps(job, sort_keys=True).encode())
        if existing_hash != new_hash:
            print(f"error: job {job_id} already armed with different content; "
                  f"use a new job_id or 'clone'", file=sys.stderr)
            return 4

    job_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(str(job_dir), 0o700)
    atomic_write(frozen_path, json.dumps(job, indent=2))
    print(f"armed: {job_dir}")
    return 0


def cmd_status(args):
    """samosa jobs status <job.json> — print job progress."""
    if len(args) < 1:
        print("Usage: samosa jobs status <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_id = job['job_id']
    job_dir = get_jobs_root() / job_id
    log_path = job_dir / 'events.jsonl'

    if not log_path.exists():
        print(f"job {job_id}: not started")
        return 0

    event_log = EventLog(log_path)
    event_log.load()

    # Count states
    states = {}
    for evt in event_log.events:
        uid = evt.get('unit_id')
        if uid:
            states[uid] = evt['type']

    counts = {}
    for state in states.values():
        counts[state] = counts.get(state, 0) + 1

    total = len(states)
    print(f"job {job_id}: {total} units")
    for state, count in sorted(counts.items()):
        print(f"  {state}: {count}")

    return 0


def cmd_preview(args):
    """samosa jobs preview <job.json> [--file <path>]"""
    if len(args) < 1:
        print("Usage: samosa jobs preview <job.json> [--file <path>]", file=sys.stderr)
        return 2

    preview_file = None
    job_path = args[0]
    i = 1
    while i < len(args):
        if args[i] == '--file' and i + 1 < len(args):
            preview_file = args[i + 1]
            i += 2
        else:
            print(f"Unknown argument: {args[i]}", file=sys.stderr)
            return 2

    job, errors = load_and_validate_job(job_path)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_id = job['job_id']
    job_dir = get_jobs_root() / job_id
    preview_dir = job_dir / 'preview'
    preview_dir.mkdir(parents=True, exist_ok=True)

    # Discover or use specified file
    if preview_file:
        with open(preview_file, 'rb') as f:
            data = f.read()
        file_hash = sha256_bytes(data)
        media_type = detect_media_type(data[:8])
        if media_type is None:
            if is_valid_utf8_text(data):
                media_type = 'text/plain'
            else:
                print("error: unsupported file type", file=sys.stderr)
                return 2
        input_meta = {
            'input_path': preview_file,
            'input_sha256': file_hash,
            'media_type': media_type,
            'size': len(data),
        }
    else:
        items, skipped = discover_inputs(job['input'])
        if not items:
            print("error: no input files found", file=sys.stderr)
            return 2
        input_meta = items[0]

    # Plan
    budget = get_prefill_budget(job)
    hydrate_pdf_input(input_meta)
    units = plan_units(input_meta, job.get('unit', 'auto'), budget,
                       get_tokenizer_cmd())
    unit = units[0]  # Preview: first unit only

    # Extract
    input_meta['_intermediates_dir'] = str(preview_dir / 'intermediates')
    extraction = extract_unit(unit, input_meta)
    if extraction.get('error'):
        result = {'error': extraction['error']}
        atomic_write(preview_dir / 'result.json', json.dumps(result, indent=2))
        print(json.dumps(result, indent=2))
        return 0

    # Model call
    serve_url = get_serve_url()
    body = build_request_body(job, extraction, unit)
    if body is None:
        print("error: could not build request body", file=sys.stderr)
        return 2

    _call_t0 = time.perf_counter()
    resp, err = call_serve(body, serve_url, timeout=300)
    wall_seconds = time.perf_counter() - _call_t0
    if err:
        print(f"error: serve call failed: {err}", file=sys.stderr)
        return 2

    content = resp.get('choices', [{}])[0].get('message', {}).get('content', '')
    usage = resp.get('usage', {})
    validation = validate_output(content, job['output_schema'],
                                  job.get('validation', {}).get('domain_rules'))

    result = validation.get('record', {})
    prov = {
        'unit_id': unit['unit_id'],
        'input_sha256': input_meta['input_sha256'],
        'input_path': input_meta['input_path'],
        'media_type': input_meta['media_type'],
        'input_tokens': usage.get('prompt_tokens'),
        'output_tokens': usage.get('completion_tokens'),
        **derive_timing(resp, wall_seconds),
        'validation': validation['status'],
        'runner_version': RUNNER_VERSION,
    }

    atomic_write(preview_dir / 'result.json', json.dumps(result, indent=2))
    atomic_write(preview_dir / 'provenance.json', json.dumps(prov, indent=2))

    print(json.dumps({'result': result, 'validation': validation}, indent=2))
    return 0


def cmd_run(args):
    """samosa jobs run <job.json> — main execution flow."""
    if len(args) < 1:
        print("Usage: samosa jobs run <job.json>", file=sys.stderr)
        return 2

    # Validate and arm
    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_id = job['job_id']
    jobs_root = get_jobs_root()
    job_dir = jobs_root / job_id
    job_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(str(job_dir), 0o700)

    # Arm (freeze job definition)
    frozen_path = job_dir / 'job.json'
    if frozen_path.exists():
        existing = json.loads(frozen_path.read_text())
        existing_hash = sha256_bytes(json.dumps(existing, sort_keys=True).encode())
        new_hash = sha256_bytes(json.dumps(job, sort_keys=True).encode())
        if existing_hash != new_hash:
            print(f"error: job {job_id} already armed with different content; "
                  f"use a new job_id or 'clone'", file=sys.stderr)
            return 4
    else:
        atomic_write(frozen_path, json.dumps(job, indent=2))

    # Acquire lock
    lock = JobLock(job_dir / 'job.lock')
    if not lock.acquire():
        print(f"error: job {job_id} is already being run by another process", file=sys.stderr)
        return 3

    try:
        return _run_job(job, job_dir)
    finally:
        lock.release()


def _run_job(job, job_dir):
    """Execute the job pipeline."""
    serve_url = get_serve_url()
    event_log = EventLog(job_dir / 'events.jsonl')
    event_log.load()

    job_id = job['job_id']
    max_attempts = job.get('resources', {}).get('max_attempts', 3)

    # Emit job_created if first run
    if not any(e['type'] == 'job_created' for e in event_log.events):
        job_hash = sha256_bytes(json.dumps(job, sort_keys=True).encode())
        event_log.append('job_created', job_id=job_id, job_sha256=job_hash)

    # Discover inputs
    print(f"[jobs] discovering inputs in {job['input']['folder']}...")
    items, skipped = discover_inputs(job['input'])
    for path, reason in skipped:
        print(f"  skip: {path}: {reason}", file=sys.stderr)

    # Get processed set for idempotency
    processed = event_log.get_processed_inputs(max_attempts)
    new_items = [it for it in items if it['input_sha256'] not in processed]
    print(f"[jobs] {len(items)} inputs found, {len(new_items)} new, {len(processed)} already processed")

    if not new_items:
        print("[jobs] nothing to do")
        return 0

    # Emit item_discovered for new items
    for item in new_items:
        already_discovered = any(
            e['type'] == 'item_discovered' and e.get('input_sha256') == item['input_sha256']
            for e in event_log.events
        )
        if not already_discovered:
            event_log.append('item_discovered',
                             input_sha256=item['input_sha256'],
                             input_path=item['input_path'],
                             media_type=item['media_type'])

    # Plan units
    inf = job.get('inference', {})
    budget = get_prefill_budget(job)
    tokenizer_cmd = get_tokenizer_cmd()

    all_units = []
    for item in new_items:
        hydrate_pdf_input(item)
        units = plan_units(item, job.get('unit', 'auto'), budget, tokenizer_cmd)
        for u in units:
            already_planned = any(
                e['type'] == 'item_planned' and e.get('unit_id') == u['unit_id']
                for e in event_log.events
            )
            if not already_planned:
                event_log.append('item_planned', **_planned_event_fields(u))
            all_units.append((u, item))

    # Units created by a previous run's server-driven context split are not
    # produced by the original planner. Restore them from the durable log so an
    # interrupted child resumes instead of being stranded.
    items_by_sha = {item['input_sha256']: item for item in new_items}
    existing_ids = {unit['unit_id'] for unit, _item in all_units}
    all_units.extend(_restore_logged_units(event_log, items_by_sha, existing_ids))

    # Recovery step 4: reconcile any artifact written just before a crash lost
    # its terminal event, so we neither reprocess it nor lose the output.
    _recover_orphans(job, job_dir, event_log, all_units)

    # Group split-file units for J1.9 reduction (reduce_group == input_sha256)
    unit_by_group = {}
    for u, _item in all_units:
        grp = u.get('reduce_group')
        if grp:
            unit_by_group.setdefault(grp, []).append(u)

    # Filter out already-terminal units
    terminal = event_log.get_terminal_units()
    pending = [(u, item) for u, item in all_units if u['unit_id'] not in terminal]

    print(f"[jobs] {len(all_units)} units planned, {len(pending)} pending")

    # Process units. A worklist (not a fixed loop) so a `400 context_limit` split
    # can re-enqueue the smaller pieces in the same run (J1.4).
    processed_count = 0
    review_count = 0
    failed_count = 0

    worklist = list(pending)
    wi = 0
    while wi < len(worklist):
        unit, item = worklist[wi]
        wi += 1
        uid = unit['unit_id']

        # Gate check before each unit
        ok, reason = gate_check(job, serve_url)
        while not ok:
            event_log.append('job_paused', reason=reason)
            print(f"[jobs] paused: {reason}. Waiting...")
            time.sleep(30)
            ok, reason = gate_check(job, serve_url)
            if ok:
                event_log.append('job_resumed', reason=f"cleared:{reason}")

        # Extract
        item['_intermediates_dir'] = str(Path(job_dir) / 'results' / 'intermediates')
        extraction = extract_unit(unit, item)
        if extraction.get('error'):
            event_log.append('item_review_required',
                             unit_id=uid,
                             input_sha256=item['input_sha256'],
                             input_path=item['input_path'],
                             reasons=[extraction['error']])
            review_count += 1
            continue

        event_log.append('item_ingested', unit_id=uid)

        # Model call with retry
        attempt = 0
        success = False
        while attempt < max_attempts:
            attempt += 1
            event_log.append('item_running', unit_id=uid, attempt=attempt)

            body = build_request_body(job, extraction, unit)
            if body is None:
                event_log.append('item_failed', unit_id=uid, attempt=attempt,
                                 error='could_not_build_request')
                failed_count += 1
                break

            # Check body size
            encoded = json.dumps(body).encode()
            if len(encoded) > HTTP_MAX_BODY:
                event_log.append('item_review_required',
                                 unit_id=uid,
                                 input_sha256=item['input_sha256'],
                                 input_path=item['input_path'],
                                 reasons=['image_too_large'])
                review_count += 1
                success = True  # Terminal, don't retry
                break

            # Derive timeout
            timeout = inf.get('timeout_s')
            if timeout is None:
                est_tokens = item.get('size', 0) // 4 + inf.get('max_tokens', 512)
                timeout = max(120, 30 + est_tokens * 0.5)  # Conservative

            _call_t0 = time.perf_counter()
            resp, err = call_serve(body, serve_url, timeout=timeout)
            wall_seconds = time.perf_counter() - _call_t0

            if err == 'context_limit':
                # Split on a line boundary and re-enqueue the pieces (J1.4); if
                # the unit is already minimal / not splittable text, it is
                # irreducible and goes to review.
                halves = split_text_unit(unit, item)
                if halves:
                    for h in halves:
                        event_log.append('item_planned',
                                         **_planned_event_fields(h))
                        unit_by_group.setdefault(item['input_sha256'], []).append(h)
                        worklist.append((h, item))
                    event_log.append('item_split', unit_id=uid,
                                     input_sha256=item['input_sha256'],
                                     into=[h['unit_id'] for h in halves],
                                     model_call_seconds=wall_seconds)
                    print(f"[jobs] split {uid} on context_limit -> {len(halves)} pieces")
                else:
                    event_log.append('item_review_required',
                                     unit_id=uid,
                                     input_sha256=item['input_sha256'],
                                     input_path=item['input_path'],
                                     reasons=['context_limit_irreducible'],
                                     model_call_seconds=wall_seconds)
                    review_count += 1
                success = True
                break
            elif err == 'queue_full':
                # Back-off, don't count against attempts
                time.sleep(2 ** min(attempt, 5))
                attempt -= 1  # Don't count this attempt
                continue
            elif err:
                if err == 'timeout':
                    # A non-streaming response does not discover a closed
                    # client during prefill.  Explicitly flip the server's
                    # cooperative cancel flag before waiting for the slot.
                    request_cancel(serve_url)
                if attempt < max_attempts:
                    event_log.append('item_retry_wait', unit_id=uid,
                                     attempt=attempt, error=err,
                                     model_call_seconds=wall_seconds)
                    # Poll status until inference_busy is false
                    for _ in range(30):
                        st = get_serve_status(serve_url)
                        if st and not st.get('inference_busy'):
                            break
                        time.sleep(2)
                    time.sleep(1)  # Small additional delay
                    continue
                else:
                    event_log.append('item_failed', unit_id=uid,
                                     attempt=attempt, error=err,
                                     model_call_seconds=wall_seconds)
                    failed_count += 1
                    break

            # Success — validate output
            content = resp.get('choices', [{}])[0].get('message', {}).get('content', '')
            usage = resp.get('usage', {})
            validation = validate_output(content, job['output_schema'],
                                          job.get('validation', {}).get('domain_rules'))

            record = validation.get('record', {})
            prov = {
                'unit_id': uid,
                'input_sha256': item['input_sha256'],
                'input_path': item['input_path'],
                'granularity': unit['granularity'],
                'media_type': item['media_type'],
                'schema_version': 1,
                'seed': inf.get('seed', 11),
                'attempt': attempt,
                'input_tokens': usage.get('prompt_tokens'),
                'output_tokens': usage.get('completion_tokens'),
                **derive_timing(resp, wall_seconds),
                'validation': validation['status'],
                'runner_version': RUNNER_VERSION,
            }

            # Atomic write
            write_result_and_provenance(str(job_dir), uid, record, prov)

            if validation['status'] == 'passed':
                event_log.append('item_complete', unit_id=uid,
                                 input_sha256=item['input_sha256'],
                                 input_path=item['input_path'],
                                 artifact=uid,
                                 validation=validation['status'],
                                 model_call_seconds=wall_seconds)
                processed_count += 1
            else:
                # Copy to review/
                review_dir = Path(job_dir) / 'results' / 'review'
                review_dir.mkdir(parents=True, exist_ok=True)
                safe_uid = uid.replace('#', '_').replace('/', '_')
                shutil.copy2(
                    str(Path(job_dir) / 'results' / 'items' / f"{safe_uid}.json"),
                    str(review_dir / f"{safe_uid}.json")
                )
                event_log.append('item_review_required', unit_id=uid,
                                 input_sha256=item['input_sha256'],
                                 input_path=item['input_path'],
                                 reasons=validation['errors'],
                                 model_call_seconds=wall_seconds)
                review_count += 1

            success = True
            break

    # J1.9 — reduce completed split-file groups into document records
    _reduce_completed_groups(job, job_dir, event_log, unit_by_group, serve_url)

    # Job complete
    event_log.append('job_complete',
                     processed=processed_count,
                     review=review_count,
                     failed=failed_count)

    # Write merged output
    write_merged_output(job, str(job_dir), event_log)

    # Render view
    render_view_html(job, event_log.events, str(job_dir))

    print(f"[jobs] complete: {processed_count} passed, {review_count} review, {failed_count} failed")
    return 0


def cmd_view(args):
    """samosa jobs view <job.json>"""
    if len(args) < 1:
        print("Usage: samosa jobs view <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_dir = get_jobs_root() / job['job_id']
    log_path = job_dir / 'events.jsonl'
    if not log_path.exists():
        print(f"error: no events found for job {job['job_id']}", file=sys.stderr)
        return 2

    event_log = EventLog(log_path)
    event_log.load()

    view_path = render_view_html(job, event_log.events, str(job_dir))
    print(f"view: {view_path}")
    return 0


def cmd_delete(args):
    """samosa jobs delete <job.json>"""
    if len(args) < 1:
        print("Usage: samosa jobs delete <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_dir = get_jobs_root() / job['job_id']
    if job_dir.exists():
        shutil.rmtree(str(job_dir))
        print(f"deleted: {job_dir}")
    else:
        print(f"job directory not found: {job_dir}")
    return 0


def cmd_archive(args):
    """samosa jobs archive <job.json>"""
    if len(args) < 1:
        print("Usage: samosa jobs archive <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_dir = get_jobs_root() / job['job_id']
    if not job_dir.exists():
        print(f"job directory not found: {job_dir}", file=sys.stderr)
        return 2

    archive_dir = get_jobs_root() / '.archive'
    archive_dir.mkdir(parents=True, exist_ok=True)
    dest = archive_dir / job['job_id']
    if dest.exists():
        shutil.rmtree(str(dest))
    shutil.move(str(job_dir), str(dest))
    print(f"archived: {dest}")
    return 0


def cmd_suggest_schema(args):
    """samosa jobs suggest-schema <job.json|--instruction "...">"""
    instruction = None
    job = None

    if len(args) >= 2 and args[0] == '--instruction':
        instruction = args[1]
    elif len(args) >= 1:
        job, errors = load_and_validate_job(args[0])
        if errors:
            for e in errors:
                print(f"error: {e}", file=sys.stderr)
            return 2
        instruction = job.get('instruction', '')
    else:
        print("Usage: samosa jobs suggest-schema <job.json|--instruction '...'>", file=sys.stderr)
        return 2

    serve_url = get_serve_url()
    body = {
        'model': 'qwen3.6-35b-a3b',
        'messages': [
            {'role': 'system', 'content':
                'You are a JSON schema designer. Given a data extraction instruction, '
                'propose a JSON Schema (type: object) with appropriate required fields '
                'and property types. Use only: string, number, integer, boolean, null. '
                'Return ONLY the JSON schema object, no prose.'},
            {'role': 'user', 'content': instruction},
        ],
        'thinking': 'off',
        'temperature': 0,
        'max_tokens': 1024,
        'stream': False,
    }

    resp, err = call_serve(body, serve_url, timeout=120)
    if err:
        print(f"error: serve call failed: {err}", file=sys.stderr)
        return 2

    content = resp.get('choices', [{}])[0].get('message', {}).get('content', '')
    # Try to parse the schema
    try:
        schema = json.loads(content)
    except json.JSONDecodeError:
        obj, _ = find_json_object(content)
        schema = obj

    if schema:
        if job:
            out_path = get_jobs_root() / job['job_id'] / 'suggested_schema.json'
            out_path.parent.mkdir(parents=True, exist_ok=True)
            atomic_write(out_path, json.dumps(schema, indent=2))
            print(f"schema written to: {out_path}")
        print(json.dumps(schema, indent=2))
    else:
        print("error: could not parse schema from model output", file=sys.stderr)
        print(content)
        return 2

    return 0


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

COMMANDS = {
    'validate': cmd_validate,
    'arm': cmd_arm,
    'preview': cmd_preview,
    'run': cmd_run,
    'status': cmd_status,
    'view': cmd_view,
    'delete': cmd_delete,
    'archive': cmd_archive,
    'suggest-schema': cmd_suggest_schema,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        cmds = ', '.join(COMMANDS.keys())
        print(f"Usage: samosa jobs <command> [args...]", file=sys.stderr)
        print(f"Commands: {cmds}", file=sys.stderr)
        return 1
    cmd = COMMANDS[sys.argv[1]]
    return cmd(sys.argv[2:])


if __name__ == '__main__':
    sys.exit(main())
