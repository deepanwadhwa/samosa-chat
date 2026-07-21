#!/usr/bin/env python3
"""Deterministic filesystem primitives for Samosa jobs.

The genuinely valuable, hard-to-get-right core lifted from the original jobs
runner: magic-byte type detection, local-folder discovery, the organize-rule
engine, collision-safe atomic moves, an undo-capable append-only event log, and
image downscaling. No model, no network, no CLI — those live one layer up
(`samosa_tools`, `samosa_jobs`). Standard library only, except an optional
Pillow import used to downscale oversized images.

This is the implementation the filesystem *tools* wrap. Keeping it pure keeps
the tools honest: given the same folder they produce byte-identical plans, and a
plan is a data structure you can inspect before anything on disk moves.
"""

import ctypes
import ctypes.util
import errno
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import stat as stat_mod
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

# --- Constants -------------------------------------------------------------

MAX_FILE_BYTES_DEFAULT = 26214400  # 25 MiB

# Magic bytes for file type detection.
MAGIC_JPEG = b'\xff\xd8\xff'
MAGIC_PNG = b'\x89PNG'
MAGIC_PDF = b'%PDF'

# Whitelist for organize destination folder names: a leading alnum, then a
# bounded run of safe characters. No leading dot/dash, no separators, no "..".
FOLDER_NAME_WHITELIST_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$')

# Default type filter for discovery (images, plain text, PDF).
DEFAULT_TYPES = ('image/jpeg', 'image/png', 'text/plain', 'application/pdf')

# libc for atomic no-clobber rename (renamex_np on macOS, renameat2 on Linux).
_libc = None
try:
    _libc_path = ctypes.util.find_library('c')
    if _libc_path:
        _libc = ctypes.CDLL(_libc_path, use_errno=True)
except Exception:
    _libc = None

RENAME_EXCL_MACOS = 4
AT_FDCWD_LINUX = -100
RENAME_NOREPLACE_LINUX = 1


# --- Small utilities -------------------------------------------------------

def rfc3339_now():
    """Return the current UTC timestamp in RFC3339 format."""
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def sha256_file(path):
    """Compute the SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def sha256_bytes(data):
    """Compute the SHA-256 hex digest of bytes."""
    return hashlib.sha256(data).hexdigest()


def read_up_to(fd, limit):
    """Read at most `limit` bytes from an open fd. Returns (data, truncated).

    Chunks are joined once at the end rather than accumulated with repeated
    `bytes +=` (which reallocates and copies the whole buffer on every append
    — O(n^2) time and up to 2x peak memory for a large file).
    """
    chunks = []
    total = 0
    while total < limit:
        chunk = os.read(fd, min(1 << 20, limit - total))
        if not chunk:
            return b''.join(chunks), False
        chunks.append(chunk)
        total += len(chunk)
    truncated = bool(os.read(fd, 1))
    return b''.join(chunks), truncated


def fsync_dir(path):
    """fsync a directory so a rename inside it is durable."""
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


def stat_is_regular(st):
    """Return True if a stat result is a regular file."""
    return stat_mod.S_ISREG(st.st_mode)


# --- Type detection --------------------------------------------------------

def detect_media_type(header_bytes):
    """Detect a media type from the first bytes of a file, or None."""
    if header_bytes[:3] == MAGIC_JPEG:
        return 'image/jpeg'
    if header_bytes[:4] == MAGIC_PNG:
        return 'image/png'
    if header_bytes[:4] == MAGIC_PDF:
        return 'application/pdf'
    return None


def is_valid_utf8_text(data):
    """Return True if data is valid UTF-8 with no problematic control chars."""
    try:
        text = data.decode('utf-8', errors='strict')
    except UnicodeDecodeError:
        return False
    for ch in text:
        cp = ord(ch)
        if cp < 32 and cp not in (9, 10, 13):
            return False
    return True


def is_valid_folder_name(name):
    """Whitelist an organize destination folder name (no separators/.. /leading dot)."""
    if not isinstance(name, str):
        return False
    if name in ('.', '..') or '/' in name or '\\' in name or '\x00' in name:
        return False
    return bool(FOLDER_NAME_WHITELIST_RE.match(name))


# --- Document extraction (PDF and beyond, via the samosa-extract sidecar) --
#
# "Reading a file" and "knowing its magic bytes" are different problems.
# detect_media_type() answers the second; this answers the first, for the
# formats the project has decided to support text extraction for (2026-07-15,
# docs/TASKS_DOCUMENTS.md — libpdfium via a sandboxed sidecar binary, plain
# text natively). The sidecar already recognizes .docx/.html/.rtf by content
# and reports them as not-yet-supported rather than mishandling them, so this
# wrapper's job is just: find the binary, run it, and turn its JSON into a
# clear result or a clear reason. No new parser logic belongs here — a new
# format is a sidecar capability, not a Python one.

_EXTRACTOR_ERROR_MESSAGES = {
    'docx_extractor_unavailable': "reading .docx files is not supported yet",
    'html_extractor_unavailable': "reading HTML files is not supported yet",
    'rtf_unsupported': "reading .rtf files is not supported yet",
    'pdf_encrypted': "this PDF is password-protected",
    'pdf_unsupported_security': "this PDF uses an unsupported security scheme",
    'pdf_malformed': "this PDF file is malformed",
    'pdf_file_error': "this PDF could not be opened",
    'pdf_load_failed': "this PDF could not be loaded",
    'pdf_page_error': "a page in this PDF could not be read",
    'page_out_of_range': "the requested page is outside this PDF",
    'page_count_limit': "this PDF has too many pages to read",
    'text_invalid_utf8': "this file is not a recognized document or text file",
    'file_unavailable': "the file could not be opened",
    'output_too_large': "the extracted text was too large",
    'wall_timeout': "reading this file took too long and was stopped",
    'extractor_unavailable': "the document reader is not installed in this release",
    'extract_timeout': "reading this file took too long and was stopped",
    'extract_invalid_response': "the document reader returned an unexpected response",
}

_EXTRACTOR_TIMEOUT_S = 25
_FS_SIDECAR_TIMEOUT_S = 15


def find_extractor():
    """Locate the samosa-extract sidecar binary, or None if unavailable.

    Checked in order: SAMOSA_EXTRACTOR env override; a sibling of this file
    (the installed layout, where jobs_fs.py/samosa_tools.py/samosa_jobs.py and
    samosa-extract are staged together in bin/); the source-tree dev
    convention of a freshly built <repo_root>/samosa-extract, then the
    <repo_root>/dist/samosa-extract fallback, then PATH. We never
    substitute a host PDF utility when the sidecar is absent — a missing
    extractor is a clear capability gap, not a silent fallback.
    """
    configured = os.environ.get('SAMOSA_EXTRACTOR')
    if configured and os.path.isfile(configured) and os.access(configured, os.X_OK):
        return configured

    here = Path(__file__).resolve().parent
    sibling = here / 'samosa-extract'
    if sibling.is_file() and os.access(str(sibling), os.X_OK):
        return str(sibling)

    for dev_tree in (here.parent / 'samosa-extract',
                     here.parent / 'dist' / 'samosa-extract'):
        if dev_tree.is_file() and os.access(str(dev_tree), os.X_OK):
            return str(dev_tree)

    return shutil.which('samosa-extract')


def find_fs_sidecar():
    """Locate the samosa-fs sidecar binary, or None if unavailable."""
    configured = os.environ.get('SAMOSA_FS')
    if configured and os.path.isfile(configured) and os.access(configured, os.X_OK):
        return configured

    here = Path(__file__).resolve().parent
    sibling = here / 'samosa-fs'
    if sibling.is_file() and os.access(str(sibling), os.X_OK):
        return str(sibling)

    dev_tree = here.parent / 'samosa-fs'
    if dev_tree.is_file() and os.access(str(dev_tree), os.X_OK):
        return str(dev_tree)

    dist_tree = here.parent / 'dist' / 'samosa-fs'
    if dist_tree.is_file() and os.access(str(dist_tree), os.X_OK):
        return str(dist_tree)

    return shutil.which('samosa-fs')


def run_fs_sidecar(args, timeout=_FS_SIDECAR_TIMEOUT_S):
    """Run samosa-fs and return its JSON payload or (None, reason)."""
    binary = find_fs_sidecar()
    if not binary:
        return None, "the filesystem sidecar is not installed in this release"
    try:
        proc = subprocess.Popen(
            [binary, *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True,
        )
        try:
            stdout, _stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                pass
            proc.communicate()
            return None, "filesystem metadata scan took too long and was stopped"
    except OSError as e:
        return None, f"the filesystem sidecar could not be run: {e}"

    try:
        payload = json.loads(stdout)
    except (TypeError, ValueError):
        return None, "the filesystem sidecar returned an unexpected response"
    if not isinstance(payload, dict):
        return None, "the filesystem sidecar returned an unexpected response"
    if not payload.get('ok'):
        code = payload.get('error') if isinstance(payload.get('error'), str) else 'unknown error'
        return None, code
    return payload, None


def fs_sidecar_list(folder, recursive=False, max_file_bytes=MAX_FILE_BYTES_DEFAULT):
    args = ['list']
    if recursive:
        args.append('--recursive')
    args.extend(['--max-file-bytes', str(max_file_bytes), folder])
    payload, error = run_fs_sidecar(args)
    if error:
        return None, None, error
    items = [{
        'input_path': item.get('path'),
        'input_sha256': item.get('input_sha256'),
        'media_type': item.get('media_type'),
        'size': item.get('size'),
        'mtime': item.get('mtime'),
        'name': item.get('name'),
    } for item in payload.get('items', []) if isinstance(item, dict)]
    skipped = [(s.get('path'), s.get('reason')) for s in payload.get('skipped', [])
               if isinstance(s, dict)]
    return items, skipped, None


def fs_sidecar_survey(folder, recursive=False, max_file_bytes=MAX_FILE_BYTES_DEFAULT):
    args = ['survey']
    if recursive:
        args.append('--recursive')
    args.extend(['--max-file-bytes', str(max_file_bytes), folder])
    payload, error = run_fs_sidecar(args)
    if error:
        return None, error
    return payload, None


def fs_sidecar_metadata(path, max_file_bytes=MAX_FILE_BYTES_DEFAULT):
    payload, error = run_fs_sidecar(['metadata', '--max-file-bytes', str(max_file_bytes), path])
    if error:
        return None, error
    return payload, None


def fs_sidecar_move(src, dst, input_folder=None, size=None, mtime=None, sha256=None):
    args = ['move']
    if input_folder:
        args.extend(['--root', input_folder])
    if size is not None:
        args.extend(['--size', str(size)])
    if mtime is not None:
        args.extend(['--mtime', str(mtime)])
    if sha256 is not None:
        args.extend(['--sha256', sha256])
    args.extend([src, dst])
    payload, error = run_fs_sidecar(args)
    if error:
        return False, error
    if payload.get('moved'):
        return True, None
    return False, payload.get('reason') or 'unknown'


def fs_sidecar_undo(src, dst, input_folder=None):
    args = ['undo']
    if input_folder:
        args.extend(['--root', input_folder])
    args.extend([src, dst])
    payload, error = run_fs_sidecar(args)
    if error:
        return False, error
    if payload.get('moved'):
        return True, None
    return False, payload.get('reason') or 'unknown'


def extract_document(path, extractor=None, timeout=_EXTRACTOR_TIMEOUT_S):
    """Extract text from a document (PDF today; plain text natively too).

    Returns (result, error). On success, result is
    {'input_type', 'text', 'pages': [{'index','text','has_raster_figure'}, …],
    'text_layer', 'tokens_estimate'}. On failure, result is None and error is a
    short, human-readable reason (never a raw sidecar error code). The sidecar
    applies its own CPU/memory sandboxing; this adds a wall-clock watchdog so a
    stuck child can never strand a job.
    """
    binary = extractor or find_extractor()
    if not binary:
        return None, _EXTRACTOR_ERROR_MESSAGES['extractor_unavailable']
    if not os.path.isfile(path):
        return None, _EXTRACTOR_ERROR_MESSAGES['file_unavailable']

    try:
        proc = subprocess.Popen(
            [binary, '--json', path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True,
        )
        try:
            stdout, _stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                pass
            proc.communicate()
            return None, _EXTRACTOR_ERROR_MESSAGES['extract_timeout']
    except OSError as e:
        return None, f"the document reader could not be run: {e}"

    try:
        payload = json.loads(stdout)
    except (TypeError, ValueError):
        return None, _EXTRACTOR_ERROR_MESSAGES['extract_invalid_response']
    if not isinstance(payload, dict):
        return None, _EXTRACTOR_ERROR_MESSAGES['extract_invalid_response']
    if not payload.get('ok'):
        code = payload.get('error') if isinstance(payload.get('error'), str) else ''
        return None, _EXTRACTOR_ERROR_MESSAGES.get(code, f"could not read this file ({code or 'unknown error'})")

    text = payload.get('text')
    pages = payload.get('pages')
    if not isinstance(text, str) or not isinstance(pages, list):
        return None, _EXTRACTOR_ERROR_MESSAGES['extract_invalid_response']
    result = {
        'input_type': payload.get('input_type', 'application/pdf'),
        'text': text,
        'pages': [{'index': p.get('index'), 'text': p.get('text', ''),
                   'has_raster_figure': bool(p.get('has_raster_figure', False))}
                  for p in pages if isinstance(p, dict)],
        'text_layer': bool(payload.get('text_layer', True)),
        'tokens_estimate': payload.get('tokens_estimate'),
    }
    return result, None


def extract_document_pages(path, start, count, extractor=None,
                           timeout=_EXTRACTOR_TIMEOUT_S):
    """Run the native extractor for one bounded PDF page range (maximum 5)."""
    try:
        start = int(start)
        count = int(count)
    except (TypeError, ValueError):
        return None, "page start and count must be integers"
    if start < 1 or count < 1 or count > 5:
        return None, "page start must be 1 or greater and count must be between 1 and 5"
    binary = extractor or find_extractor()
    if not binary:
        return None, _EXTRACTOR_ERROR_MESSAGES['extractor_unavailable']
    if not os.path.isfile(path):
        return None, _EXTRACTOR_ERROR_MESSAGES['file_unavailable']
    try:
        proc = subprocess.Popen(
            [binary, '--json-pages', path, str(start), str(count)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True,
        )
        try:
            stdout, _stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                pass
            proc.communicate()
            return None, _EXTRACTOR_ERROR_MESSAGES['extract_timeout']
    except OSError as error:
        return None, f"the document reader could not be run: {error}"
    try:
        payload = json.loads(stdout)
    except (TypeError, ValueError):
        return None, _EXTRACTOR_ERROR_MESSAGES['extract_invalid_response']
    if not isinstance(payload, dict) or not payload.get('ok'):
        code = payload.get('error') if isinstance(payload, dict) else ''
        return None, _EXTRACTOR_ERROR_MESSAGES.get(
            code, f"could not read these pages ({code or 'unknown error'})")
    pages = payload.get('pages')
    if not isinstance(pages, list) or not isinstance(payload.get('text'), str):
        return None, _EXTRACTOR_ERROR_MESSAGES['extract_invalid_response']
    return {
        'text': payload['text'],
        'pages': pages,
        'page_count': int(payload.get('page_count', len(pages))),
        'page_start': int(payload.get('page_start', start)),
        'page_end': int(payload.get('page_end', start + len(pages) - 1)),
        'text_layer': bool(payload.get('text_layer', True)),
    }, None


# --- Discovery (local folder only) -----------------------------------------

def discover_files(input_config, allowed_types=None, is_metadata_only=False):
    """Discover local input files under a folder.

    Returns (items, skipped) where each item is
    {input_path, input_sha256, media_type, size} and skipped is a list of
    (path, reason). Symlinks are rejected (O_NOFOLLOW), non-regular files are
    skipped, content is hashed for dedup, and the type is detected by magic
    bytes (falling back to UTF-8 text, then octet-stream for metadata-only
    jobs). URL inputs are intentionally not handled here — that is a web-tool
    concern, kept out of the filesystem layer.
    """
    folder = input_config.get('folder')
    recursive = input_config.get('recursive', False)
    max_bytes = input_config.get('max_file_bytes', MAX_FILE_BYTES_DEFAULT)

    if 'types' in input_config:
        type_filter = set(input_config['types'])
    else:
        type_filter = set(DEFAULT_TYPES)
        if is_metadata_only:
            type_filter.add('application/octet-stream')

    items = []
    skipped = []
    seen_hashes = set()

    walker = []
    if folder and os.path.isdir(folder):
        if recursive:
            walker = sorted(Path(folder).rglob('*'))
        else:
            walker = sorted(Path(folder).iterdir())

    for entry in walker:
        path = str(entry)
        # Reject symlinks by opening with O_NOFOLLOW.
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        except OSError as e:
            skipped.append((path, f"cannot open (O_NOFOLLOW): {e}"))
            continue

        try:
            st = os.fstat(fd)
            if not stat_is_regular(st):
                skipped.append((path, "not a regular file"))
                continue
            if not is_metadata_only and st.st_size > max_bytes:
                skipped.append((path, f"exceeds max_file_bytes ({st.st_size} > {max_bytes})"))
                continue
            if st.st_size == 0:
                skipped.append((path, "empty file"))
                continue

            # Metadata-only scans (a read-only "report" job's folder survey)
            # used to read arbitrarily large files in full just to sniff their
            # type — one big file in the scanned folder (a disk image, a
            # video, a VM export: normal contents for a Downloads folder)
            # pulled its whole size into memory for what's supposed to be a
            # lightweight count. Cap the read at max_bytes always; a
            # truncated file still gets a magic-byte type and its real size
            # from stat, it just skips full-content UTF-8 sniffing and is
            # hashed (for dedup) over the truncated prefix instead.
            data, truncated = read_up_to(fd, max_bytes)

            # Re-fstat to detect a change during the read.
            st2 = os.fstat(fd)
            if st2.st_size != st.st_size or st2.st_mtime != st.st_mtime:
                skipped.append((path, "file changed during read"))
                continue
        finally:
            os.close(fd)

        digest_input = data if not truncated else data + b'\0truncated\0' + str(st.st_size).encode()
        file_hash = sha256_bytes(digest_input)
        if file_hash in seen_hashes:
            skipped.append((path, "duplicate content (same SHA-256 as earlier file)"))
            continue
        seen_hashes.add(file_hash)

        media_type = detect_media_type(data[:8])
        if media_type is None:
            if not truncated and is_valid_utf8_text(data):
                media_type = 'text/plain'
            elif is_metadata_only:
                media_type = 'application/octet-stream'
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
            'size': st.st_size,
        })

    return items, skipped


def count_by_type(items):
    """Summarize discovered items as {media_type: {count, bytes}}."""
    summary = {}
    for item in items:
        bucket = summary.setdefault(item['media_type'], {'count': 0, 'bytes': 0})
        bucket['count'] += 1
        bucket['bytes'] += item['size']
    return summary


# --- Organize plan (deterministic, no model) -------------------------------

def eval_op(op, val1, val2):
    """Evaluate a comparison op for a `where` rule, with JSON-typed semantics."""
    if op == 'eq':
        if type(val1) is not type(val2) and not (
            isinstance(val1, (int, float)) and isinstance(val2, (int, float))
            and not isinstance(val1, bool) and not isinstance(val2, bool)
        ):
            return False
        return val1 == val2
    if op == 'ne':
        return not eval_op('eq', val1, val2)
    try:
        if op == 'lt':
            return val1 < val2
        if op == 'le':
            return val1 <= val2
        if op == 'gt':
            return val1 > val2
        if op == 'ge':
            return val1 >= val2
    except TypeError:
        return False
    return False


_MAGIC_FOLDER_MAP = {
    'image/jpeg': 'JPEG',
    'image/png': 'PNG',
    'application/pdf': 'PDF',
    'text/plain': 'TEXT',
}


def build_organize_plan(job, job_dir, results=None):
    """Compile an organize plan for a job spec. Returns (records, error).

    `job` supplies `input` (a discovery config) and `organize` (the rule).
    For `field`/`where` rules the per-file extraction records are read from
    `results` (a list of dicts) when given, else from
    `<job_dir>/results/output.jsonl`. `extension`/`media_type` rules need no
    model and no results file. Records with a `dst` are moves; records with a
    `skip` explain why a file stayed put. The plan is deterministic: the same
    folder yields byte-identical output across runs.
    """
    org = job.get('organize')
    if not org:
        return None, "job has no organize block"

    rule = org.get('rule', {})
    by = rule.get('by')
    is_metadata_only = by in ('extension', 'media_type')

    items, _skipped = discover_files(job['input'], is_metadata_only=is_metadata_only)
    items.sort(key=lambda x: x['input_path'])

    results_by_path = {}
    results_by_hash = {}
    if not is_metadata_only:
        records = results
        if records is None:
            records = []
            out_file = Path(job_dir) / 'results' / 'output.jsonl'
            if out_file.exists():
                with open(out_file, 'r', encoding='utf-8') as f:
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            records.append(json.loads(line))
                        except ValueError:
                            pass
        for rec in records:
            if rec.get('input_path'):
                results_by_path[rec['input_path']] = rec
            if rec.get('input_sha256'):
                results_by_hash[rec['input_sha256']] = rec

    dest_root = org.get('dest_root')
    if not dest_root:
        dest_root = os.path.join(job['input']['folder'], 'Organized')
    dest_root = os.path.abspath(dest_root)

    moves_or_skips = []
    taken_dsts = set()

    for item in items:
        input_path = item['input_path']
        input_sha256 = item['input_sha256']
        size = item['size']
        try:
            mtime = os.path.getmtime(input_path)
        except OSError:
            mtime = 0.0

        folder_name = None

        if by == 'extension':
            p = Path(input_path)
            ext = p.suffix[1:].lower() if p.suffix.startswith('.') else ''
            mapping = rule.get('map', {})
            if ext and ext in mapping:
                folder_name = mapping[ext]
            elif ext:
                folder_name = ext.upper()
            else:
                folder_name = _MAGIC_FOLDER_MAP.get(item['media_type'], 'OTHER')

        elif by == 'media_type':
            folder_name = _MAGIC_FOLDER_MAP.get(item['media_type'], 'OTHER')

        elif by in ('field', 'where'):
            rec = results_by_path.get(input_path) or results_by_hash.get(input_sha256)
            if not rec:
                moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "not_validated"})
                continue
            status = rec.get('status', 'passed')
            if status != 'passed':
                moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "not_validated"})
                continue
            extracted = rec.get('extracted') if isinstance(rec.get('extracted'), dict) else rec
            field_name = rule.get('field')
            field_val = extracted.get(field_name)

            if by == 'field':
                if field_val is None:
                    moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "not_validated"})
                    continue
                folder_name = str(field_val).strip()
            elif by == 'where':
                op = rule.get('op')
                target_val = rule.get('value')
                if eval_op(op, field_val, target_val):
                    folder_name = rule.get('dest')
                else:
                    unmatched = org.get('unmatched', 'leave')
                    if unmatched == 'leave':
                        moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "unmatched"})
                        continue
                    folder_name = unmatched

        if not folder_name or not is_valid_folder_name(folder_name):
            moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "unsafe_dest"})
            continue

        dst_dir = os.path.join(dest_root, folder_name)
        base_name = os.path.basename(input_path)
        dst_path = os.path.join(dst_dir, base_name)

        if os.path.realpath(input_path) == os.path.realpath(dst_path):
            moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "already_sorted"})
            continue

        on_coll = org.get('on_collision', 'skip')
        has_collision = os.path.exists(dst_path) or dst_path in taken_dsts
        if has_collision:
            if on_coll == 'skip':
                moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "dest_exists"})
                continue
            elif on_coll == 'suffix_sha8':
                suffix = input_sha256[:8]
                p = Path(input_path)
                new_base = f"{p.stem}.{suffix}{p.suffix}"
                dst_path = os.path.join(dst_dir, new_base)
                if os.path.exists(dst_path) or dst_path in taken_dsts:
                    moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "dest_exists"})
                    continue

        taken_dsts.add(dst_path)
        moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "dst": dst_path, "size": size, "mtime": mtime})

    moves_or_skips.sort(key=lambda x: x['src'])
    return moves_or_skips, None


# --- Move / undo primitives ------------------------------------------------

def atomic_no_clobber_rename(src, dst):
    """Atomic rename that never overwrites an existing dst.

    Uses renamex_np(RENAME_EXCL) on macOS, renameat2(RENAME_NOREPLACE) on
    Linux, and an os.link+inode-assert+unlink fallback elsewhere. Returns
    (success, skip_reason) where skip_reason is one of 'dest_exists',
    'cross_device', 'inode_mismatch', or a 'link_failed: …' string.
    """
    src_bytes = os.fsencode(src)
    dst_bytes = os.fsencode(dst)

    if _libc is not None:
        if sys.platform == 'darwin' and hasattr(_libc, 'renamex_np'):
            res = _libc.renamex_np(ctypes.c_char_p(src_bytes), ctypes.c_char_p(dst_bytes), ctypes.c_uint(RENAME_EXCL_MACOS))
            if res == 0:
                return True, None
            err = ctypes.get_errno()
            if err in (errno.EEXIST, errno.EACCES):
                return False, 'dest_exists'
            if err == errno.EXDEV:
                return False, 'cross_device'

        elif sys.platform.startswith('linux') and hasattr(_libc, 'renameat2'):
            res = _libc.renameat2(
                ctypes.c_int(AT_FDCWD_LINUX),
                ctypes.c_char_p(src_bytes),
                ctypes.c_int(AT_FDCWD_LINUX),
                ctypes.c_char_p(dst_bytes),
                ctypes.c_uint(RENAME_NOREPLACE_LINUX),
            )
            if res == 0:
                return True, None
            err = ctypes.get_errno()
            if err in (errno.EEXIST, errno.EACCES):
                return False, 'dest_exists'
            if err == errno.EXDEV:
                return False, 'cross_device'

    # Fallback: hard-link + inode assertion + unlink source.
    if os.path.exists(dst):
        return False, 'dest_exists'
    try:
        os.link(src, dst)
    except FileExistsError:
        return False, 'dest_exists'
    except OSError as e:
        if e.errno == errno.EXDEV:
            return False, 'cross_device'
        return False, f"link_failed: {e}"

    st_src = os.stat(src)
    st_dst = os.stat(dst)
    if st_src.st_ino != st_dst.st_ino or st_src.st_dev != st_dst.st_dev:
        return False, 'inode_mismatch'

    os.unlink(src)
    return True, None


def apply_move(plan_line, input_folder=None, verify_hash=False):
    """Apply a single planned move. Returns (success, skip_reason).

    Re-validates the source against the plan (regular file, unchanged size/mtime,
    optionally hash), enforces a scope jail when `input_folder` is given, creates
    the destination directory, then performs the atomic no-clobber rename.
    """
    src = plan_line['src']
    dst = plan_line['dst']
    sidecar = find_fs_sidecar()
    if sidecar:
        return fs_sidecar_move(
            src, dst, input_folder=input_folder,
            size=plan_line.get('size'), mtime=plan_line.get('mtime'),
            sha256=plan_line.get('input_sha256') if verify_hash else None,
        )

    try:
        fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as e:
        return False, f"cannot_open_src: {e}"

    try:
        st = os.fstat(fd)
        if not stat_is_regular(st):
            return False, 'not_regular_file'

        expected_size = plan_line.get('size')
        expected_mtime = plan_line.get('mtime')
        if expected_size is not None and st.st_size != expected_size:
            return False, 'changed_since_scan'
        if expected_mtime is not None and abs(st.st_mtime - expected_mtime) > 1e-4:
            return False, 'changed_since_scan'

        if verify_hash and 'input_sha256' in plan_line:
            data = b''
            os.lseek(fd, 0, os.SEEK_SET)
            while True:
                chunk = os.read(fd, 1 << 20)
                if not chunk:
                    break
                data += chunk
            if sha256_bytes(data) != plan_line['input_sha256']:
                return False, 'changed_since_scan'
    finally:
        os.close(fd)

    if input_folder:
        folder_real = os.path.realpath(input_folder)
        src_real = os.path.realpath(src)
        dst_real = os.path.realpath(dst)
        try:
            rel_src = os.path.relpath(src_real, folder_real)
            rel_dst = os.path.relpath(dst_real, folder_real)
            if rel_src.startswith('..') or rel_dst.startswith('..'):
                return False, 'outside_jail'
        except ValueError:
            return False, 'outside_jail'

    dst_dir = os.path.dirname(dst)
    try:
        os.makedirs(dst_dir, exist_ok=True)
    except OSError as e:
        return False, f"mkdir_failed: {e}"

    return atomic_no_clobber_rename(src, dst)


def revert_move(applied_line):
    """Reverse a previously applied move (dst -> src). Returns (success, reason).

    Used by undo. The original source location must be free (no-clobber) and the
    moved file must still be at the destination.
    """
    src = applied_line['src']
    dst = applied_line['dst']
    sidecar = find_fs_sidecar()
    if sidecar:
        folder = os.path.commonpath([os.path.abspath(src), os.path.abspath(dst)])
        return fs_sidecar_undo(src, dst, input_folder=folder)
    if not os.path.exists(dst):
        return False, 'dest_missing'
    src_dir = os.path.dirname(src)
    try:
        os.makedirs(src_dir, exist_ok=True)
    except OSError as e:
        return False, f"mkdir_failed: {e}"
    return atomic_no_clobber_rename(dst, src)


# --- Append-only event log -------------------------------------------------

class EventLog:
    """Append-only JSONL event log with monotonic sequence numbers.

    Every job records its actions here — the same stream the live UI renders and
    the record undo replays. Writes are flushed and fsynced so a crash leaves a
    truncated-but-valid tail, and a torn final line is ignored on load.
    """

    def __init__(self, path):
        self.path = Path(path)
        self.seq = 0
        self.events = []

    def load(self):
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
                    pass

    def append(self, event_type, **fields):
        self.seq += 1
        event = {'seq': self.seq, 'ts': rfc3339_now(), 'type': event_type, **fields}
        self.events.append(event)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.path, 'a') as f:
            f.write(json.dumps(event, separators=(',', ':')) + '\n')
            f.flush()
            os.fsync(f.fileno())
        return event


# --- Image downscale (optional Pillow, macOS sips fallback) ----------------

def auto_downscale_image_bytes(image_bytes, mime_type, target_max_bytes=3 * 1024 * 1024):
    """Downscale image bytes so the payload stays under target_max_bytes.

    Returns (bytes, mime_type, changed). Prefers Pillow; on macOS falls back to
    `sips`; otherwise returns the input unchanged.
    """
    if len(image_bytes) <= target_max_bytes:
        return image_bytes, mime_type, False

    try:
        import io
        from PIL import Image
        img = Image.open(io.BytesIO(image_bytes))
        w, h = img.size
        scale = (target_max_bytes / len(image_bytes)) ** 0.5
        new_w = max(1, int(w * scale * 0.9))
        new_h = max(1, int(h * scale * 0.9))
        img = img.resize((new_w, new_h), getattr(Image, 'Resampling', Image).LANCZOS)
        buf = io.BytesIO()
        fmt = 'JPEG' if 'jpeg' in mime_type or 'jpg' in mime_type else 'PNG'
        img.save(buf, format=fmt, quality=85)
        res_bytes = buf.getvalue()
        out_mime = 'image/jpeg' if fmt == 'JPEG' else 'image/png'
        return res_bytes, out_mime, True
    except Exception:
        pass

    if platform.system() == 'Darwin':
        ext = '.jpg' if 'jpeg' in mime_type or 'jpg' in mime_type else '.png'
        with tempfile.NamedTemporaryFile('wb', suffix=ext, delete=False) as in_f:
            in_f.write(image_bytes)
            in_path = in_f.name
        out_path = in_path + '.downscaled' + ext
        try:
            res = subprocess.run(['sips', '-Z', '1024', in_path, '--out', out_path],
                                 capture_output=True, text=True, timeout=5)
            if res.returncode == 0 and os.path.exists(out_path):
                with open(out_path, 'rb') as out_f:
                    downscaled_bytes = out_f.read()
                if downscaled_bytes and len(downscaled_bytes) < len(image_bytes):
                    return downscaled_bytes, mime_type, True
        except Exception:
            pass
        finally:
            for p in (in_path, out_path):
                if os.path.exists(p):
                    try:
                        os.unlink(p)
                    except OSError:
                        pass

    return image_bytes, mime_type, False
