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
import http.client
import http.server
import io
import ipaddress
import json
import math
import os
import platform
import re
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import urllib.error
import urllib.parse
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


# Folder destination whitelist (JO.1)
FOLDER_NAME_WHITELIST_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$')

def is_valid_folder_name(name):
    """Check if a folder name is whitelisted (no leading dot/dash, no path separators, no ..)."""
    if not isinstance(name, str):
        return False
    if name in ('.', '..') or '/' in name or '\\' in name or '\x00' in name:
        return False
    return bool(FOLDER_NAME_WHITELIST_RE.match(name))


def validate_job(job):
    """Validate a parsed job.json. Returns (normalized_job, errors)."""
    errors = []

    # Check if job is metadata-only
    org = job.get('organize')
    is_metadata_only = False
    if job.get('instruction') is None and job.get('output_schema') is None:
        is_metadata_only = True
    elif isinstance(org, dict):
        rule = org.get('rule')
        if isinstance(rule, dict):
            by = rule.get('by')
            if by in ('extension', 'media_type'):
                is_metadata_only = True
            elif 'instruction' not in job or 'output_schema' not in job or job.get('instruction') is None or job.get('output_schema') is None:
                is_metadata_only = True

    # Required top-level fields
    required_keys = ('job_id', 'input') if is_metadata_only else ('job_id', 'input', 'instruction', 'output_schema')
    for key in required_keys:
        if key not in job:
            errors.append(f"missing required field: {key}")

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
        urls = inp.get('urls')
        
        if urls is not None:
            if not isinstance(urls, list) or not all(isinstance(u, str) for u in urls):
                errors.append("input.urls must be a list of strings")
            if not folder:
                errors.append("input.folder is required as a spool directory when input.urls is used")
        else:
            if not folder:
                errors.append("input.folder is required")

        if folder and not os.path.isabs(folder):
            errors.append("input.folder must be an absolute path")
        mfb = inp.get('max_file_bytes', MAX_FILE_BYTES_DEFAULT)
        if not isinstance(mfb, (int, float)) or mfb <= 0:
            errors.append("input.max_file_bytes must be a positive number")

    # unit
    unit = job.get('unit', 'auto')
    if unit not in ('auto', 'file', 'page'):
        errors.append(f"unit: must be auto, file, or page; got '{unit}'")

    # instruction
    if is_metadata_only:
        if 'instruction' in job and job['instruction'] is not None:
            instr = job['instruction']
            if not isinstance(instr, str):
                errors.append("instruction must be a string or null")
    else:
        instr = job.get('instruction', '')
        if not isinstance(instr, str) or not instr.strip():
            errors.append("instruction must be a non-empty string")

    # output_schema
    if is_metadata_only:
        if 'output_schema' in job and job['output_schema'] is not None:
            schema_errors = validate_output_schema(job['output_schema'])
            errors.extend(schema_errors)
    else:
        schema_errors = validate_output_schema(job.get('output_schema', {}))
        errors.extend(schema_errors)

    # inference
    inf = job.get('inference')
    if is_metadata_only and inf is None:
        pass
    elif inf is not None and not isinstance(inf, dict):
        errors.append("inference must be an object or null")
    else:
        inf_dict = inf if inf is not None else {}
        mt = inf_dict.get('max_tokens', 512)
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

    # organize (JO.0)
    if org is not None:
        if not isinstance(org, dict):
            errors.append("organize must be an object")
        else:
            valid_org_keys = {'rule', 'dest_root', 'on_collision', 'unmatched'}
            for k in org:
                if k not in valid_org_keys:
                    errors.append(f"organize: unknown key '{k}'")

            rule = org.get('rule')
            if not isinstance(rule, dict):
                errors.append("organize.rule must be an object")
            else:
                by = rule.get('by')
                if by not in ('extension', 'media_type', 'field', 'where'):
                    errors.append(f"organize.rule.by: unknown rule type '{by}'")

                if by == 'extension':
                    mapping = rule.get('map')
                    if mapping is not None:
                        if not isinstance(mapping, dict):
                            errors.append("organize.rule.map must be an object")
                        else:
                            for k, v in mapping.items():
                                if not is_valid_folder_name(v):
                                    errors.append(f"organize.rule.map value '{v}' is not a valid folder name")
                elif by in ('field', 'where'):
                    field_name = rule.get('field')
                    if not field_name or not isinstance(field_name, str):
                        errors.append("organize.rule.field is required and must be a string")
                    else:
                        schema_props = (job.get('output_schema') or {}).get('properties', {})
                        if field_name not in schema_props:
                            errors.append(f"organize.rule.field '{field_name}' not in output_schema properties")
                    
                    if by == 'where':
                        op = rule.get('op')
                        if op not in ('eq', 'ne', 'lt', 'le', 'gt', 'ge'):
                            errors.append(f"organize.rule.op: unknown op '{op}'")
                        if 'value' not in rule:
                            errors.append("organize.rule.value is required for 'where' rule")
                        dest = rule.get('dest')
                        if not dest or not is_valid_folder_name(dest):
                            errors.append(f"organize.rule.dest '{dest}' is not a valid folder name")

            dest_root = org.get('dest_root')
            if dest_root is not None:
                if not isinstance(dest_root, str) or not os.path.isabs(dest_root):
                    errors.append("organize.dest_root must be an absolute path")
                else:
                    folder_path = (job.get('input') or {}).get('folder', '')
                    if folder_path and os.path.isabs(folder_path):
                        folder_real = os.path.realpath(folder_path)
                        dest_real = os.path.realpath(dest_root)
                        try:
                            rel = os.path.relpath(dest_real, folder_real)
                            if rel.startswith('..') or rel == '..':
                                errors.append("organize.dest_root must be an absolute path inside input.folder")
                        except ValueError:
                            errors.append("organize.dest_root must be an absolute path inside input.folder")

                    folder_real = os.path.realpath(folder_path) if folder_path else ''
                    cur = Path(dest_root)
                    while cur != cur.parent:
                        if folder_real and os.path.realpath(cur) == folder_real:
                            break
                        if cur.is_symlink():
                            errors.append("organize.dest_root path component cannot be a symlink")
                            break
                        cur = cur.parent

            on_coll = org.get('on_collision', 'skip')
            if on_coll not in ('skip', 'suffix_sha8'):
                errors.append(f"organize.on_collision: must be 'skip' or 'suffix_sha8', got '{on_coll}'")

            unmatched = org.get('unmatched', 'leave')
            if unmatched != 'leave' and not is_valid_folder_name(unmatched):
                errors.append(f"organize.unmatched '{unmatched}' is not a valid folder name")

    # Normalize
    normalized = copy.deepcopy(job)
    normalized.setdefault('schema_version', 1)
    normalized.setdefault('unit', 'auto')
    normalized.setdefault('name', jid)
    normalized.setdefault('created_at', rfc3339_now())
    normalized.setdefault('reduce', {'mode': 'deterministic', 'model_fields': []})
    normalized.setdefault('inference', {})
    if normalized['inference'] is not None:
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

def is_ssrf_safe_ip(ip_str):
    try:
        ip = ipaddress.ip_address(ip_str)
    except ValueError:
        return False
    if ip.is_loopback or ip.is_multicast or ip.is_unspecified:
        return False
    blocked_networks = [
        "127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
        "169.254.0.0/16", "0.0.0.0/8", "100.64.0.0/10", "192.0.0.0/24",
        "198.18.0.0/15", "224.0.0.0/4", "240.0.0.0/4", "255.255.255.255/32",
        "::/128", "fe80::/10", "::ffff:0:0/96", "64:ff9b::/96", "2002::/16",
        "fc00::/7"
    ]
    for net in blocked_networks:
        if ip in ipaddress.ip_network(net, strict=False):
            return False
    return True

class PinningHTTPConnection(http.client.HTTPConnection):
    def __init__(self, host, ip, **kwargs):
        self._ip = ip
        super().__init__(host, **kwargs)
    def connect(self):
        self.sock = socket.create_connection((self._ip, self.port), self.timeout)

class PinningHTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, host, ip, **kwargs):
        self._ip = ip
        super().__init__(host, **kwargs)
    def connect(self):
        self.sock = socket.create_connection((self._ip, self.port), self.timeout)
        if self._tunnel_host:
            self._tunnel()
        self.sock = self._context.wrap_socket(self.sock, server_hostname=self.host)

def fetch_public_url(url, max_hops=5):
    if os.environ.get("SAMOSA_OFFLINE") == "1":
        return None, "SAMOSA_OFFLINE is set"
    
    current_url = url
    hops = 0
    while hops < max_hops:
        parsed = urllib.parse.urlparse(current_url)
        if parsed.scheme not in ("http", "https"):
            return None, f"unsupported scheme: {parsed.scheme}"
        
        hostname = parsed.hostname
        if not hostname:
            return None, "missing hostname"
            
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        try:
            addrinfo = socket.getaddrinfo(hostname, port, proto=socket.IPPROTO_TCP)
        except socket.gaierror as e:
            return None, f"DNS resolution failed: {e}"
            
        safe_ip = None
        for info in addrinfo:
            ip_str = info[4][0]
            if is_ssrf_safe_ip(ip_str):
                safe_ip = ip_str
                break
        
        if not safe_ip:
            return None, f"no safe IP address found for {hostname} (SSRF blocked)"
            
        if parsed.scheme == "https":
            context = ssl.create_default_context()
            conn = PinningHTTPSConnection(hostname, safe_ip, context=context, timeout=20)
        else:
            conn = PinningHTTPConnection(hostname, safe_ip, timeout=20)
            
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query
            
        try:
            conn.request("GET", path, headers={"User-Agent": "samosa-jobs/1.0", "Host": hostname})
            resp = conn.getresponse()
            if resp.status in (301, 302, 303, 307, 308):
                loc = resp.getheader("Location")
                if not loc:
                    return None, "redirect missing Location header"
                current_url = urllib.parse.urljoin(current_url, loc)
                hops += 1
                conn.close()
                continue
            if resp.status != 200:
                return None, f"HTTP {resp.status} {resp.reason}"
            body = resp.read(HTTP_MAX_BODY)
            return body, None
        except Exception as e:
            return None, f"fetch failed: {e}"
        finally:
            conn.close()
    return None, "too many redirects"

def extract_html_text(html_bytes):
    try:
        html_str = html_bytes.decode('utf-8', errors='replace')
    except Exception:
        html_str = html_bytes.decode('latin1', errors='ignore')
    
    # Strip script and style blocks entirely
    html_str = re.sub(r'<script\b[^<]*(?:(?!</script>)<[^<]*)*</script>', ' ', html_str, flags=re.IGNORECASE | re.DOTALL)
    html_str = re.sub(r'<style\b[^<]*(?:(?!</style>)<[^<]*)*</style>', ' ', html_str, flags=re.IGNORECASE | re.DOTALL)
    
    # Extract remaining text
    text = re.sub(r'<[^>]+>', ' ', html_str)
    text = html.unescape(text)
    text = re.sub(r'\s+', ' ', text).strip()
    
    # JS-heavy SPA detection heuristic
    if len(html_str) > 2000 and len(text) < 100:
        return None, "JS page detected, content unavailable"
        
    return text.encode('utf-8'), None


def discover_inputs(input_config, allowed_types=None, is_metadata_only=False):
    """Discover input files. Returns list of {input_path, input_sha256, media_type, size}
    and a list of skip reasons."""
    folder = input_config.get('folder')
    urls = input_config.get('urls', [])
    recursive = input_config.get('recursive', False)
    max_bytes = input_config.get('max_file_bytes', MAX_FILE_BYTES_DEFAULT)

    if 'types' in input_config:
        type_filter = set(input_config['types'])
    else:
        type_filter = set(input_config.get('types', [
            'image/jpeg', 'image/png', 'text/plain', 'application/pdf'
        ]))
        if is_metadata_only:
            type_filter.add('application/octet-stream')

    items = []
    skipped = []
    seen_hashes = set()

    if urls and folder:
        Path(folder).mkdir(parents=True, exist_ok=True)
        for i, url in enumerate(urls):
            body, err = fetch_public_url(url)
            if err:
                skipped.append((url, f"fetch failed: {err}"))
                continue
            
            text_bytes, extract_err = extract_html_text(body)
            if extract_err:
                skipped.append((url, extract_err))
                continue
                
            file_hash = sha256_bytes(text_bytes)
            if file_hash in seen_hashes:
                skipped.append((url, f"duplicate content (same SHA-256 as earlier item)"))
                continue
            seen_hashes.add(file_hash)
            
            safe_slug = re.sub(r'[^a-zA-Z0-9_-]', '_', url)[:100]
            spool_name = f"fetched_{file_hash[:8]}_{safe_slug}.txt"
            spool_path = os.path.join(folder, spool_name)
            if not os.path.exists(spool_path):
                atomic_write(spool_path, text_bytes.decode('utf-8'))
            
            items.append({
                'input_path': spool_path,
                'input_sha256': file_hash,
                'media_type': 'text/plain',
                'size': len(text_bytes),
                'source_url': url
            })

    walker = []
    if folder and os.path.isdir(folder):
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
            # Size check (bypassed for metadata-only jobs)
            if not is_metadata_only and st.st_size > max_bytes:
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


def auto_downscale_image_bytes(image_bytes, mime_type, target_max_bytes=3 * 1024 * 1024):
    """Downscale image bytes (JPEG, PNG, PPM) so base64 representation stays under target_max_bytes."""
    if len(image_bytes) <= target_max_bytes:
        return image_bytes, mime_type, False

    try:
        from PIL import Image
        import io
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


def downscale_body_images_if_needed(body):
    """Scan request body for image data URIs and auto-downscale if payload exceeds HTTP_MAX_BODY."""
    data = json.dumps(body).encode('utf-8')
    if len(data) <= HTTP_MAX_BODY:
        return body, False

    modified = False
    messages = body.get('messages', [])
    for msg in messages:
        content = msg.get('content')
        if isinstance(content, list):
            for part in content:
                if part.get('type') == 'image_url':
                    img_url = part.get('image_url', {}).get('url', '')
                    if img_url.startswith('data:image/'):
                        try:
                            header, b64_data = img_url.split(';base64,', 1)
                            mime_type = header.replace('data:', '')
                            raw_bytes = base64.b64decode(b64_data)
                            downscaled_bytes, out_mime, shrank = auto_downscale_image_bytes(
                                raw_bytes, mime_type, target_max_bytes=2 * 1024 * 1024
                            )
                            if shrank:
                                new_b64 = base64.b64encode(downscaled_bytes).decode('ascii')
                                part['image_url']['url'] = f"data:{out_mime};base64,{new_b64}"
                                modified = True
                        except Exception:
                            pass

    return body, modified


def call_serve(body, serve_url, timeout=None, is_background=True):
    """POST to samosa serve. Returns (response_dict, error_str)."""
    body, shrank = downscale_body_images_if_needed(body)
    data = json.dumps(body).encode('utf-8')

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
        return None, f'unexpected_error:{e}'


def call_serve_prefill(body, serve_url, timeout=None):
    """POST to /v1/chat/prefill to pre-warm KV cache and get exact prefill stats. Returns (response_dict, error_str)."""
    data = json.dumps(body).encode('utf-8')
    headers = {'Content-Type': 'application/json'}
    url = f"{serve_url}/v1/chat/prefill"
    req = urllib.request.Request(url, data=data, headers=headers, method='POST')
    try:
        if timeout:
            resp = urllib.request.urlopen(req, timeout=timeout)
        else:
            resp = urllib.request.urlopen(req)
        resp_data = resp.read()
        return json.loads(resp_data), None
    except Exception as e:
        return None, str(e)


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


def wait_for_slot_clear(serve_url, polls=30, interval_s=2):
    """Wait for a cancelled serve request to release the single inference slot."""
    for _ in range(polls):
        status = get_serve_status(serve_url)
        if status and not status.get('inference_busy'):
            return True
        time.sleep(interval_s)
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

    props = job.get('output_schema', {}).get('properties', {})
    review_cards = []
    for uid in sorted(units.keys()):
        row = units[uid]
        evt = row.get('state_event', {})
        if evt.get('type') != 'item_review_required':
            continue
        
        safe_uid = uid.replace('#', '_').replace('/', '_')
        input_path = row.get("input_path", row.get("input_sha256", ""))
        
        # Load current data
        current_data = {}
        review_file = items_dir.parent / 'review' / f"{safe_uid}.json"
        if review_file.exists():
            try:
                current_data = json.loads(review_file.read_text())
            except Exception: pass
        else:
            item_file = items_dir / f"{safe_uid}.json"
            if item_file.exists():
                try:
                    current_data = json.loads(item_file.read_text())
                except Exception: pass
        
        # Generate left pane (preview)
        media_type = ''
        for e in events:
            if e.get('type') == 'item_discovered' and e.get('input_sha256') == row.get('input_sha256'):
                media_type = e.get('media_type', '')
                break
                
        preview_html = '<div style="flex:1; padding:1em; border-right: 1px solid #eee; overflow:auto; max-height: 600px;">'
        if media_type.startswith('image/'):
            preview_html += f'<img src="file://{html_escape(input_path)}" style="max-width:100%; display:block;" />'
        elif media_type == 'text/plain':
            try:
                with open(input_path, 'r', encoding='utf-8') as f:
                    content = f.read(4096)
                    if len(content) == 4096: content += '...'
                preview_html += f'<pre style="white-space: pre-wrap; font-size: 12px; margin:0;">{html_escape(content)}</pre>'
            except Exception:
                preview_html += '<p>Could not read text preview.</p>'
        else:
            preview_html += f'<p>Preview not available for {html_escape(media_type)}</p><a href="file://{html_escape(input_path)}" target="_blank">Open file</a>'
        preview_html += '</div>'
        
        # Generate right pane (fields)
        fields_html = f'<div style="flex:1; padding:1em;"><h3>Review: {html_escape(uid)}</h3>'
        reasons = html_escape(', '.join(evt.get('reasons', [])))
        fields_html += f'<p class="review" style="margin-top:0;">Reasons: {reasons}</p><table style="width:100%; border:none; box-shadow:none; margin:0;">'
        
        for fname, fschema in props.items():
            val = current_data.get(fname)
            val_str = str(val) if val is not None else ""
            cmd = f'samosa jobs review-patch job.json --unit "{uid}" --field "{fname}" --val "{val_str}"'
            field_id = f"cmd_{safe_uid}_{html_escape(fname)}"
            
            fields_html += f'''
            <tr style="background:none;">
                <td style="padding:0.5em 0; border:none; border-bottom: 1px solid #eee; width:30%;"><strong>{html_escape(fname)}</strong></td>
                <td style="padding:0.5em 0; border:none; border-bottom: 1px solid #eee;">
                    <input type="text" value="{html_escape(val_str)}" style="width:100%; padding:0.4em; box-sizing:border-box;"
                           oninput="document.getElementById('{field_id}').innerText = 'samosa jobs review-patch job.json --unit \\'{uid}\\' --field \\'{html_escape(fname)}\\' --val \\'' + this.value.replace(/'/g, '\\\\'') + '\\''" />
                </td>
            </tr>
            <tr style="background:none;">
                <td colspan="2" style="padding:0 0 1em 0; border:none;">
                    <code id="{field_id}" style="font-size:11px; color:#555; display:block; background:#f4f4f4; padding:0.5em; border-radius:4px; margin-top:0.2em; word-break: break-all;">
                        {html_escape(cmd)}
                    </code>
                </td>
            </tr>
            '''
        fields_html += '</table></div>'
        review_cards.append(f'<div style="display:flex; background:#fff; border-radius:8px; margin-bottom:1.5em; box-shadow:0 1px 3px rgba(0,0,0,.1);">{preview_html}{fields_html}</div>')

    review_section = ''
    if review_cards:
        review_section = f'<h2 class="review">Needs review ({review})</h2>' + ''.join(review_cards)

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

    # Moves section (JO.6 - Bakery Test compliant)
    moves_applied_events = [ev for ev in events if (ev.get('type') or ev.get('event')) == 'move_applied']
    moves_skipped_events = [ev for ev in events if (ev.get('type') or ev.get('event')) == 'move_skipped']
    moves_reverted_events = [ev for ev in events if (ev.get('type') or ev.get('event')) == 'move_reverted']

    SKIP_PLAIN_SENTENCES = {
        'unsafe_dest': "The folder name contained special characters and wasn't safe to use",
        'dest_exists': 'A file with that name already exists in the destination folder',
        'not_validated': 'The file was not validated or needs manual review',
        'changed_since_scan': 'The file was modified after the organize plan was created',
        'already_sorted': 'The file is already in its destination folder',
        'cross_device': 'The destination is on a different drive or volume',
        'unmatched': 'The file did not match any organize rule condition',
        'changed_since_apply': 'The file was modified after the move was applied',
        'unresolved_crash': 'Skipped due to an unresolved process interruption',
    }

    moves_section = ''
    if moves_applied_events or moves_skipped_events or moves_reverted_events:
        # Group events by destination folder
        grouped_moves = {}
        all_move_evs = moves_applied_events + moves_skipped_events + moves_reverted_events
        for ev in all_move_evs:
            dst = ev.get('dst', '')
            dst_dir = os.path.dirname(dst) if dst else 'Unmapped'
            grouped_moves.setdefault(dst_dir, []).append(ev)

        # Build plain-sentence summary items
        summary_sentences = []
        n_moved = len(moves_applied_events)
        summary_sentences.append(f"Where your files are: <strong>{n_moved} file(s) are sorted</strong>.")
        if moves_reverted_events:
            summary_sentences.append(f"Reverted <strong>{len(moves_reverted_events)}</strong> move(s).")
        if moves_skipped_events:
            reasons_count = {}
            for ev in moves_skipped_events:
                r = ev.get('skip', 'skipped')
                reasons_count[r] = reasons_count.get(r, 0) + 1
            reason_parts = [f"{SKIP_PLAIN_SENTENCES.get(r, f'Skipped ({r})')} ({cnt})" for r, cnt in reasons_count.items()]
            summary_sentences.append(f"Needs attention: {'; '.join(reason_parts)}.")

        moves_rows = []
        for dst_dir in sorted(grouped_moves.keys()):
            evs = grouped_moves[dst_dir]
            dir_escaped = html_escape(dst_dir)
            moves_rows.append(f'<tr style="background:#f0f4f8"><td colspan="4"><strong>Folder:</strong> <code>{dir_escaped}</code></td></tr>')
            for ev in evs:
                ev_type = ev.get('type') or ev.get('event')
                src = ev.get('src', '')
                dst = ev.get('dst', '')
                src_dir = os.path.dirname(src)
                src_base = os.path.basename(src)
                dst_base = os.path.basename(dst)

                src_formatted = f'<span style="color:#999">{html_escape(src_dir)}/</span>{html_escape(src_base)}'
                dst_formatted = f'<span style="color:#999">{html_escape(dst_dir)}/</span>{html_escape(dst_base)}'

                if ev_type == 'move_applied':
                    moves_rows.append(f'<tr><td><code>{src_formatted}</code></td><td style="color:#999">→</td><td><code>{dst_formatted}</code></td><td class="passed">applied</td></tr>')
                elif ev_type == 'move_skipped':
                    raw_reason = ev.get('skip', 'skipped')
                    plain_reason = html_escape(SKIP_PLAIN_SENTENCES.get(raw_reason, f'Skipped: {raw_reason}'))
                    moves_rows.append(f'<tr><td><code>{src_formatted}</code></td><td style="color:#999">→</td><td><code>{dst_formatted}</code></td><td class="review" title="{plain_reason}">{plain_reason}</td></tr>')
                elif ev_type == 'move_reverted':
                    moves_rows.append(f'<tr><td><code>{src_formatted}</code></td><td style="color:#999">←</td><td><code>{dst_formatted}</code></td><td class="failed">reverted</td></tr>')

        undo_cmd = f"samosa jobs undo {job_name} --yes"
        plain_summary_html = ' '.join(summary_sentences)
        moves_section = f"""
<div class="summary">
<h2>Moves Summary</h2>
<p>{plain_summary_html}</p>
<p><small>Nothing was deleted. User files are never deleted by Samosa Jobs. Revert moves at any time with: <code>{undo_cmd}</code></small></p>
</div>
<details>
<summary style="font-weight:600; cursor:pointer; margin: 1em 0;">Details for the record (Filesystem Moves Manifest)</summary>
<table>
<tr><th>Source</th><th></th><th>Destination</th><th>Status</th></tr>
{''.join(moves_rows)}
</table>
</details>
"""

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
{moves_section}
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


def get_host_profile():
    """Load host capability profile from config files or environment (TASKS_HARDWARE.md H5).
    Returns dict with host capability profile and assigned resource tier.
    """
    env_path = os.environ.get('SAMOSA_HOST_PROFILE')
    paths_to_check = []
    if env_path:
        paths_to_check.append(Path(env_path))
    paths_to_check.extend([
        Path.home() / '.samosa' / 'host_profile.json',
        Path('/etc/samosa/host_profile.json')
    ])

    for path in paths_to_check:
        if path.exists() and path.is_file():
            try:
                data = json.loads(path.read_text())
                if isinstance(data, dict):
                    data.setdefault('source', str(path))
                    return derive_host_resource_budget(data)
            except (json.JSONDecodeError, OSError):
                pass

    detected = _detect_host_capabilities()
    return derive_host_resource_budget(detected)


def _detect_host_capabilities():
    """Detect host system capabilities for dynamic host-tuning tier."""
    cpu_count = os.cpu_count() or 2
    is_container = os.path.exists('/.dockerenv') or os.path.exists('/run/.containerenv')
    on_ac = not _on_battery()
    
    ram_gb = 16
    if platform.system() == 'Darwin':
        try:
            res = subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True, timeout=2)
            if res.returncode == 0:
                ram_gb = int(res.stdout.strip()) // (1024 ** 3)
        except Exception:
            pass
    elif platform.system() == 'Linux':
        try:
            with open('/proc/meminfo', 'r') as f:
                for line in f:
                    if line.startswith('MemTotal:'):
                        ram_gb = int(line.split()[1]) // (1024 * 1024)
                        break
        except Exception:
            pass

    if is_container:
        tier = 'container-blind'
    elif ram_gb >= 32 and cpu_count >= 8 and on_ac:
        tier = 'desktop-cooled'
    elif ram_gb < 12:
        tier = 'constrained'
    else:
        tier = 'reference-fanless'

    return {
        'tier': tier,
        'ram_gb': ram_gb,
        'phys_perf_cores': max(2, cpu_count // 2 if platform.system() == 'Darwin' else cpu_count),
        'smt': False,
        'on_ac': on_ac,
        'container': is_container,
        'source': 'detected'
    }


def derive_host_resource_budget(profile_data):
    """Derive resource budget based on host capability tier."""
    tier = profile_data.get('tier', 'reference-fanless')
    ram_gb = profile_data.get('ram_gb', 16)
    cores = profile_data.get('phys_perf_cores', 2)
    
    if tier == 'desktop-cooled':
        thread_budget = min(cores, 8)
        prefill_budget = min(ram_gb * 1024 // 2, 16384)
        max_mem_mb = ram_gb * 1024 // 2
    elif tier == 'constrained':
        thread_budget = 2
        prefill_budget = 2048
        max_mem_mb = 4096
    elif tier == 'container-blind':
        thread_budget = 2
        prefill_budget = 4096
        max_mem_mb = 8192
    else:
        thread_budget = 2
        prefill_budget = 4096
        max_mem_mb = 8192

    profile_data.update({
        'thread_budget': profile_data.get('thread_budget', thread_budget),
        'prefill_budget': profile_data.get('prefill_budget', prefill_budget),
        'max_mem_mb': profile_data.get('max_mem_mb', max_mem_mb)
    })
    return profile_data



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


def estimate_unit_input_tokens(unit, input_meta, extraction):
    """Estimate one request's input from its planned unit, never file bytes.

    PDF page units already carry exact sidecar counts in ``input_meta.pages``;
    using the enclosing PDF's byte size here would turn a small page into an
    hours-long timeout. Text chunks retain a conservative character fallback
    because they are split after the whole-file exact count is obtained.
    """
    text_tokens = None
    if unit.get('granularity') == 'page':
        page_index = unit.get('page_index')
        for page in input_meta.get('pages', []):
            if page.get('index') == page_index:
                text_tokens = page.get('text_tokens')
                break
    elif unit.get('granularity') == 'file':
        text_tokens = input_meta.get('text_tokens')

    if not isinstance(text_tokens, int) or isinstance(text_tokens, bool) or text_tokens < 0:
        text_tokens = math.ceil(len(extraction.get('text', '')) / 4)
    if extraction.get('image_data_uri'):
        text_tokens += IMAGE_TOKENS
    return text_tokens


def estimate_job_cost(job):
    """Calculate pre-arm time, token, and resource estimates for a job."""
    inp = job.get('input', {})
    is_meta = job.get('instruction') is None and job.get('organize') is not None
    items, _ = discover_inputs(inp, is_metadata_only=is_meta)
    if not items:
        return {
            'total_files': 0,
            'total_units': 0,
            'total_input_tokens': 0,
            'estimated_wall_clock_s': 0.0,
            'formatted_time': '0.0 seconds',
            'run_on_battery': job.get('resources', {}).get('run_on_battery', False),
            'min_free_gb': job.get('resources', {}).get('min_free_gb', 5)
        }

    budget = get_prefill_budget(job)
    tokenizer_cmd = get_tokenizer_cmd()
    unit_mode = job.get('unit', 'auto')

    total_tokens = 0
    all_units_count = 0

    for item in items:
        hydrate_pdf_input(item)
        units = plan_units(item, unit_mode, budget, tokenizer_cmd)
        all_units_count += len(units)
        for u in units:
            if item.get('media_type', '').startswith('image/'):
                total_tokens += 576
            elif item.get('media_type') == 'application/pdf' and u.get('plan_reason') == 'multi_image_pages':
                total_tokens += 576
            else:
                sz = item.get('size', 0)
                total_tokens += math.ceil(sz / 4) if sz > 0 else 100

    max_tokens_per_unit = job.get('inference', {}).get('max_tokens', 512)
    est_output_tokens = max_tokens_per_unit * all_units_count

    # Reference speeds: prefill ~25 tok/s, decode ~6 tok/s
    est_prefill_s = total_tokens / 25.0 if total_tokens > 0 else 0.0
    est_decode_s = est_output_tokens / 6.0 if est_output_tokens > 0 else 0.0
    total_sec = est_prefill_s + est_decode_s

    if total_sec < 60:
        formatted = f"~{total_sec:.1f} seconds"
    elif total_sec < 3600:
        formatted = f"~{total_sec / 60.0:.1f} minutes"
    else:
        hours = total_sec / 3600.0
        formatted = f"~{hours:.1f} hours" + (" (run overnight recommended)" if hours >= 2.0 else "")

    return {
        'total_files': len(items),
        'total_units': all_units_count,
        'total_input_tokens': total_tokens,
        'estimated_wall_clock_s': round(total_sec, 1),
        'formatted_time': formatted,
        'run_on_battery': job.get('resources', {}).get('run_on_battery', False),
        'min_free_gb': job.get('resources', {}).get('min_free_gb', 5)
    }


def print_job_cost_estimate(job):
    """Print pre-arm time/cost estimation summary."""
    est = estimate_job_cost(job)
    print(f"Pre-arm Estimate for job {job['job_id']}:")
    urls = job.get('input', {}).get('urls', [])
    if urls:
        print(f"  [!] INTERNET USE: This job will make active network requests to {len(urls)} public URL(s).")
    print(f"  Input Files: {est['total_files']} ({est['total_units']} units)")
    print(f"  Estimated Input Tokens: {est['total_input_tokens']}")
    print(f"  Projected Wall-Clock: {est['formatted_time']}")
    batt = "Allowed" if est['run_on_battery'] else "AC Power Preferred (paused on battery)"
    print(f"  Power Policy: {batt}")


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
    jobs_root = Path(get_jobs_root())
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

    print_job_cost_estimate(job)
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
    """samosa jobs preview <job.json> [--file <path>] [--samples N]"""
    if len(args) < 1:
        print("Usage: samosa jobs preview <job.json> [--file <path>] [--samples N]", file=sys.stderr)
        return 2

    preview_file = None
    samples_count = 1
    job_path = args[0]
    i = 1
    while i < len(args):
        if args[i] == '--file' and i + 1 < len(args):
            preview_file = args[i + 1]
            i += 2
        elif args[i] == '--samples' and i + 1 < len(args):
            try:
                samples_count = max(1, int(args[i + 1]))
            except ValueError:
                samples_count = 1
            i += 2
        else:
            print(f"Unknown argument: {args[i]}", file=sys.stderr)
            return 2

    job, errors = load_and_validate_job(job_path)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    print_job_cost_estimate(job)

    job_id = job['job_id']
    job_dir = Path(get_jobs_root()) / job_id
    preview_dir = job_dir / 'preview'
    preview_dir.mkdir(parents=True, exist_ok=True)

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
        sample_items = [{
            'input_path': preview_file,
            'input_sha256': file_hash,
            'media_type': media_type,
            'size': len(data),
        }]
    else:
        items, _ = discover_inputs(job['input'])
        if not items:
            print("error: no input files found", file=sys.stderr)
            return 2
        if samples_count == 1 or len(items) <= samples_count:
            sample_items = items[:samples_count]
        else:
            step = len(items) / float(samples_count)
            sample_items = [items[int(idx * step)] for idx in range(samples_count)]

    budget = get_prefill_budget(job)
    serve_url = get_serve_url()
    preview_results = []

    for s_idx, input_meta in enumerate(sample_items):
        hydrate_pdf_input(input_meta)
        units = plan_units(input_meta, job.get('unit', 'auto'), budget, get_tokenizer_cmd())
        unit = units[0]

        input_meta['_intermediates_dir'] = str(preview_dir / 'intermediates')
        extraction = extract_unit(unit, input_meta)
        if extraction.get('error'):
            res = {'error': extraction['error'], 'unit_id': unit['unit_id']}
            preview_results.append(res)
            continue

        body = build_request_body(job, extraction, unit)
        if body is None:
            res = {'error': 'could not build request body', 'unit_id': unit['unit_id']}
            preview_results.append(res)
            continue

        _call_t0 = time.perf_counter()
        resp, err = call_serve(body, serve_url, timeout=300)
        wall_seconds = time.perf_counter() - _call_t0
        if err:
            if err == 'timeout':
                request_cancel(serve_url)
                wait_for_slot_clear(serve_url)
            print(f"error: serve call failed for {input_meta['input_path']}: {err}", file=sys.stderr)
            return 2

        content = resp.get('choices', [{}])[0].get('message', {}).get('content', '')
        usage = resp.get('usage', {})
        validation = validate_output(content, job['output_schema'], job.get('validation', {}).get('domain_rules'))

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

        s_suffix = f"_{s_idx+1}" if len(sample_items) > 1 else ""
        atomic_write(preview_dir / f'result{s_suffix}.json', json.dumps(result, indent=2))
        atomic_write(preview_dir / f'provenance{s_suffix}.json', json.dumps(prov, indent=2))

        preview_results.append({'sample': s_idx + 1, 'unit_id': unit['unit_id'], 'result': result, 'validation': validation})

    if len(preview_results) == 1:
        print(json.dumps({'result': preview_results[0]['result'], 'validation': preview_results[0]['validation']}, indent=2))
    else:
        print(json.dumps({'preview_samples': preview_results}, indent=2))
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
    urls = job['input'].get('urls', [])
    if urls:
        print(f"[jobs] [!] INTERNET USE: Fetching {len(urls)} public URL(s) from the internet...")
    
    if job['input'].get('folder'):
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
                est_tokens = (estimate_unit_input_tokens(unit, item, extraction)
                              + inf.get('max_tokens', 512))
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
                    # Poll status until inference_busy is false.
                    wait_for_slot_clear(serve_url)
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

    send_local_notification("Samosa Jobs", f"Job complete: {processed_count} passed, {review_count} review, {failed_count} failed")

    # Write merged output
    write_merged_output(job, str(job_dir), event_log)

    # Render view
    render_view_html(job, event_log.events, str(job_dir))

    print(f"[jobs] complete: {processed_count} passed, {review_count} review, {failed_count} failed")
    return 0


class JobsUIHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == '/api/units':
            job_dir_str = getattr(self.server, 'job_dir', '')
            job_dir = Path(job_dir_str) if job_dir_str else Path('.')
            log_path = job_dir / 'events.jsonl'
            events = []
            if log_path.exists():
                el = EventLog(log_path)
                el.load()
                events = el.events
            job = getattr(self.server, 'job', {})
            data = json.dumps({'job': job, 'events': events})
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(data.encode('utf-8'))
            return

        job_dir_str = getattr(self.server, 'job_dir', '')
        job = getattr(self.server, 'job', {})
        job_dir = Path(job_dir_str) if job_dir_str else Path('.')
        log_path = job_dir / 'events.jsonl'
        events = []
        if log_path.exists():
            el = EventLog(log_path)
            el.load()
            events = el.events
        
        view_path = render_view_html(job, events, str(job_dir))
        content = Path(view_path).read_text(encoding='utf-8')
        
        interactive_script = """
        <script>
        document.addEventListener('DOMContentLoaded', () => {
            const patchModal = document.createElement('div');
            patchModal.id = 'patch-modal';
            patchModal.style = 'display:none; position:fixed; top:20px; right:20px; background:#1e1e2e; color:#cdd6f4; border:1px solid #45475a; padding:15px; border-radius:8px; z-index:9999; width:350px; box-shadow: 0 4px 12px rgba(0,0,0,0.5);';
            patchModal.innerHTML = `
                <h3 style="margin-top:0; color:#89b4fa;">Side-by-Side Unit Patch</h3>
                <label style="display:block; margin-bottom:4px;">Unit ID:</label>
                <input id="patch-unit-id" style="width:100%; margin-bottom:8px; background:#181825; color:#cdd6f4; border:1px solid #45475a; padding:4px;" readonly />
                <label style="display:block; margin-bottom:4px;">Field Name:</label>
                <input id="patch-field" style="width:100%; margin-bottom:8px; background:#181825; color:#cdd6f4; border:1px solid #45475a; padding:4px;" placeholder="e.g. total" />
                <label style="display:block; margin-bottom:4px;">New Value:</label>
                <input id="patch-val" style="width:100%; margin-bottom:12px; background:#181825; color:#cdd6f4; border:1px solid #45475a; padding:4px;" placeholder="e.g. 42.5" />
                <button id="btn-save-patch" style="background:#89b4fa; color:#11111b; border:none; padding:6px 12px; font-weight:bold; cursor:pointer; border-radius:4px;">Save & Patch JSONL</button>
                <button onclick="document.getElementById('patch-modal').style.display='none'" style="background:#45475a; color:#cdd6f4; border:none; padding:6px 12px; margin-left:8px; cursor:pointer; border-radius:4px;">Cancel</button>
            `;
            document.body.appendChild(patchModal);

            document.addEventListener('click', (e) => {
                const tr = e.target.closest('tr');
                if (tr && tr.cells && tr.cells.length > 0) {
                    const uid = tr.cells[0].innerText ? tr.cells[0].innerText.trim() : '';
                    if (uid && uid.length >= 8) {
                        document.getElementById('patch-unit-id').value = uid;
                        document.getElementById('patch-modal').style.display = 'block';
                    }
                }
            });

            document.getElementById('btn-save-patch').onclick = async () => {
                const unit_id = document.getElementById('patch-unit-id').value;
                const field = document.getElementById('patch-field').value;
                const val = document.getElementById('patch-val').value;
                if (!unit_id || !field) return alert('Unit ID and Field are required');
                const resp = await fetch('/api/review-patch', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({unit_id, field, val})
                });
                const res = await resp.json();
                if (res.status === 'ok') {
                    alert('Patched successfully!');
                    location.reload();
                } else {
                    alert('Patch error: ' + (res.error || 'failed'));
                }
            };
        });
        </script>
        </body>
        """
        content = content.replace('</body>', interactive_script)

        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(content.encode('utf-8'))

    def do_POST(self):
        if self.path == '/api/review-patch':
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length)
            try:
                data = json.loads(body)
                unit_id = data.get('unit_id')
                field = data.get('field')
                val = data.get('val')
                job_file = str(Path(self.server.job_dir) / 'job.json')
                
                code = cmd_review_patch([job_file, '--unit', unit_id, '--field', field, '--val', str(val)])
                if code == 0:
                    res_body = json.dumps({'status': 'ok', 'unit_id': unit_id})
                    self.send_response(200)
                else:
                    res_body = json.dumps({'status': 'error', 'error': 'review patch failed'})
                    self.send_response(400)
            except Exception as e:
                res_body = json.dumps({'status': 'error', 'error': str(e)})
                self.send_response(500)

            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(res_body.encode('utf-8'))
            return

        self.send_error(404)


JobsAppHandler = JobsUIHandler


def cmd_view(args):
    """samosa jobs view <job.json> [--serve] [--port PORT]"""
    if len(args) < 1:
        print("Usage: samosa jobs view <job.json> [--serve] [--port PORT]", file=sys.stderr)
        return 2

    job_file = args[0]
    serve_mode = '--serve' in args
    port = 8085
    if '--port' in args:
        idx = args.index('--port')
        if idx + 1 < len(args):
            try:
                port = int(args[idx + 1])
            except ValueError:
                pass

    job, errors = load_and_validate_job(job_file)
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

    if serve_mode:
        server = http.server.HTTPServer(('127.0.0.1', port), JobsUIHandler)
        server.job = job
        server.job_dir = str(job_dir)
        print(f"Interactive Jobs UI running at http://127.0.0.1:{port}")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            server.server_close()

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
# JO.3 — Move engine (audited atomic no-clobber renames)
# ---------------------------------------------------------------------------

import ctypes
import ctypes.util
import errno

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


def atomic_no_clobber_rename(src, dst):
    """Perform an atomic no-clobber rename using renamex_np (macOS) or renameat2 (Linux), or os.link fallback.
    Returns (success: bool, skip_reason: str or None)."""
    src_bytes = os.fsencode(src)
    dst_bytes = os.fsencode(dst)

    if _libc is not None:
        sys_platform = sys.platform
        if sys_platform == 'darwin' and hasattr(_libc, 'renamex_np'):
            res = _libc.renamex_np(ctypes.c_char_p(src_bytes), ctypes.c_char_p(dst_bytes), ctypes.c_uint(RENAME_EXCL_MACOS))
            if res == 0:
                return True, None
            err = ctypes.get_errno()
            if err in (errno.EEXIST, errno.EACCES):
                return False, 'dest_exists'
            if err == errno.EXDEV:
                return False, 'cross_device'

        elif sys_platform.startswith('linux') and hasattr(_libc, 'renameat2'):
            res = _libc.renameat2(
                ctypes.c_int(AT_FDCWD_LINUX),
                ctypes.c_char_p(src_bytes),
                ctypes.c_int(AT_FDCWD_LINUX),
                ctypes.c_char_p(dst_bytes),
                ctypes.c_uint(RENAME_NOREPLACE_LINUX)
            )
            if res == 0:
                return True, None
            err = ctypes.get_errno()
            if err in (errno.EEXIST, errno.EACCES):
                return False, 'dest_exists'
            if err == errno.EXDEV:
                return False, 'cross_device'

    # Fallback path (os.link)
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

    # Inode assertion check
    st_src = os.stat(src)
    st_dst = os.stat(dst)
    if st_src.st_ino != st_dst.st_ino or st_src.st_dev != st_dst.st_dev:
        return False, 'inode_mismatch'

    # Safe to remove source link
    os.unlink(src)
    return True, None


def apply_move(plan_line, input_folder=None, verify_hash=False):
    """Apply a single planned move. Returns (success: bool, skip_reason: str or None)."""
    src = plan_line['src']
    dst = plan_line['dst']

    # 1. Open src with O_NOFOLLOW
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

    # 2. Scope jail check
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

    # 3. Create destination directory safely
    dst_dir = os.path.dirname(dst)
    try:
        os.makedirs(dst_dir, exist_ok=True)
    except OSError as e:
        return False, f"mkdir_failed: {e}"

    # 4. Atomic no-clobber rename
    return atomic_no_clobber_rename(src, dst)


# ---------------------------------------------------------------------------
# JO.1 / JO.2 — Plan compiler & Report
# ---------------------------------------------------------------------------

def eval_op(op, val1, val2):
    """Evaluate comparison op for where rule. Ensures JSON-typed comparisons."""
    if op == 'eq':
        if type(val1) is not type(val2) and not (isinstance(val1, (int, float)) and isinstance(val2, (int, float)) and not isinstance(val1, bool) and not isinstance(val2, bool)):
            return False
        return val1 == val2
    if op == 'ne':
        return not eval_op('eq', val1, val2)
    try:
        if op == 'lt': return val1 < val2
        if op == 'le': return val1 <= val2
        if op == 'gt': return val1 > val2
        if op == 'ge': return val1 >= val2
    except TypeError:
        return False
    return False


def build_organize_plan(job, job_dir):
    """Compile organize plan for a job. Returns (records, error)."""
    org = job.get('organize')
    if not org:
        return None, "job.json has no organize block"

    rule = org.get('rule', {})
    by = rule.get('by')
    is_metadata_only = by in ('extension', 'media_type')

    items, skipped_discovery = discover_inputs(job['input'], is_metadata_only=is_metadata_only)
    items.sort(key=lambda x: x['input_path'])

    results_by_path = {}
    results_by_hash = {}
    if not is_metadata_only:
        out_file = Path(job_dir) / 'results' / 'output.jsonl'
        if out_file.exists():
            with open(out_file, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rec = json.loads(line)
                        if rec.get('input_path'):
                            results_by_path[rec['input_path']] = rec
                        if rec.get('input_sha256'):
                            results_by_hash[rec['input_sha256']] = rec
                    except json.JSONDecodeError:
                        pass

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
                magic_map = {
                    'image/jpeg': 'JPEG',
                    'image/png': 'PNG',
                    'application/pdf': 'PDF',
                    'text/plain': 'TEXT'
                }
                folder_name = magic_map.get(item['media_type'], 'OTHER')

        elif by == 'media_type':
            magic_map = {
                'image/jpeg': 'JPEG',
                'image/png': 'PNG',
                'application/pdf': 'PDF',
                'text/plain': 'TEXT'
            }
            folder_name = magic_map.get(item['media_type'], 'OTHER')

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
                stem = p.stem
                ext_with_dot = p.suffix
                new_base = f"{stem}.{suffix}{ext_with_dot}"
                dst_path = os.path.join(dst_dir, new_base)
                if os.path.exists(dst_path) or dst_path in taken_dsts:
                    moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "skip": "dest_exists"})
                    continue

        taken_dsts.add(dst_path)
        moves_or_skips.append({"input_sha256": input_sha256, "src": input_path, "dst": dst_path, "size": size, "mtime": mtime})

    moves_or_skips.sort(key=lambda x: x['src'])
    return moves_or_skips, None


def cmd_organize(args):
    """samosa jobs organize <job.json>"""
    if len(args) < 1:
        print("Usage: samosa jobs organize <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_dir = get_jobs_root() / job['job_id']
    job_dir.mkdir(parents=True, exist_ok=True)
    results_dir = job_dir / 'results'
    results_dir.mkdir(parents=True, exist_ok=True)
    log_path = job_dir / 'events.jsonl'

    event_log = EventLog(log_path)
    event_log.load()
    built_at_seq = event_log.seq

    records, err = build_organize_plan(job, job_dir)
    if err:
        print(f"error: {err}", file=sys.stderr)
        return 2

    lines = []
    moves_count = 0
    skips_count = 0
    for rec in records:
        if 'dst' in rec:
            moves_count += 1
        else:
            skips_count += 1
        lines.append(json.dumps(rec))

    plan_sha = sha256_bytes('\n'.join(lines).encode('utf-8'))
    summary_line = {
        "plan_sha256": plan_sha,
        "built_at_seq": built_at_seq,
        "moves": moves_count,
        "skips": skips_count
    }
    lines.append(json.dumps(summary_line))

    plan_file = results_dir / 'organize_plan.jsonl'
    atomic_write(plan_file, '\n'.join(lines) + '\n')

    event_log.append('plan_created',
        plan_sha256=plan_sha,
        moves=moves_count,
        skips=skips_count
    )

    print(f"plan created: {plan_file} (moves: {moves_count}, skips: {skips_count})")
    return 0


def cmd_report(args):
    """samosa jobs report <job.json>"""
    if len(args) < 1:
        print("Usage: samosa jobs report <job.json>", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(args[0])
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    items, skipped = discover_inputs(job['input'], is_metadata_only=True)

    by_type = {}
    total_bytes = 0
    for item in items:
        mt = item['media_type']
        size = item['size']
        total_bytes += size
        if mt not in by_type:
            by_type[mt] = {'count': 0, 'bytes': 0, 'largest': (0, '')}
        by_type[mt]['count'] += 1
        by_type[mt]['bytes'] += size
        if size > by_type[mt]['largest'][0]:
            by_type[mt]['largest'] = (size, item['input_path'])

    print(f"Folder Report: {job['input']['folder']}")
    print(f"Total files: {len(items)}, Total size: {total_bytes} bytes")
    print("-" * 50)
    for mt, info in sorted(by_type.items()):
        print(f"Type: {mt:<24} Count: {info['count']:<6} Bytes: {info['bytes']:<10}")
        if info['largest'][1]:
            print(f"  Largest: {info['largest'][1]} ({info['largest'][0]} bytes)")
    return 0


def send_local_notification(title, message):
    """Post a local desktop notification on macOS (best-effort, counts only, no filenames)."""
    if sys.platform == 'darwin':
        try:
            script = f'display notification "{message}" with title "{title}"'
            subprocess.run(['osascript', '-e', script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        except Exception:
            pass


def cmd_apply(args):
    """samosa jobs apply <job.json> [--yes] [--verify-hash]"""
    if len(args) < 1:
        print("Usage: samosa jobs apply <job.json> [--yes] [--verify-hash]", file=sys.stderr)
        return 2

    job_file = args[0]
    auto_yes = '--yes' in args
    verify_hash = '--verify-hash' in args

    job, errors = load_and_validate_job(job_file)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_dir = get_jobs_root() / job['job_id']
    plan_file = job_dir / 'results' / 'organize_plan.jsonl'
    if not plan_file.exists():
        print("error: no organize plan found. Run 'samosa jobs organize <job.json>' first.", file=sys.stderr)
        return 2

    lines = []
    with open(plan_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line:
                lines.append(line)

    if not lines:
        print("error: organize plan is empty", file=sys.stderr)
        return 2

    summary = json.loads(lines[-1])
    plan_sha = summary.get('plan_sha256')
    built_at_seq = summary.get('built_at_seq', 0)

    log_path = job_dir / 'events.jsonl'
    event_log = EventLog(log_path)
    event_log.load()

    if event_log.seq > built_at_seq:
        stale = False
        for ev in event_log.events:
            ev_type = ev.get('type') or ev.get('event')
            if ev.get('seq', 0) > built_at_seq and ev_type in ('unit_completed', 'job_complete', 'item_discovered'):
                stale = True
                break
        if stale:
            print("error: plan_stale: events updated since plan creation. Re-run 'samosa jobs organize <job.json>'", file=sys.stderr)
            return 2

    plan_moves = [json.loads(line) for line in lines[:-1] if 'dst' in json.loads(line)]
    plan_skips = [json.loads(line) for line in lines[:-1] if 'skip' in json.loads(line)]

    print(f"Organize Plan Summary for job {job['job_id']}:")
    print(f"  Moves to execute: {len(plan_moves)}")
    print(f"  Skips: {len(plan_skips)}")

    if not auto_yes:
        if not sys.stdin.isatty():
            print("error: non-interactive invocation requires --yes", file=sys.stderr)
            return 2
        try:
            resp = input("Apply plan? [y/N]: ").strip().lower()
        except EOFError:
            resp = ''
        if resp != 'y':
            print("aborted.")
            return 1

    event_log.append('plan_approved', plan_sha256=plan_sha)

    input_folder = job['input']['folder']

    # Crash recovery: resolve any orphaned move_applying events lacking terminal move_applied/move_skipped
    applying_map = {}
    terminal_srcs = set()
    for ev in event_log.events:
        ev_type = ev.get('type') or ev.get('event')
        src = ev.get('src')
        if not src:
            continue
        if ev_type == 'move_applying':
            applying_map[src] = ev
        elif ev_type in ('move_applied', 'move_skipped'):
            terminal_srcs.add(src)

    orphaned_srcs = [src for src in applying_map if src not in terminal_srcs]
    plan_moves_by_src = {m['src']: m for m in plan_moves}

    for src in orphaned_srcs:
        ev = applying_map[src]
        dst = ev.get('dst')
        sha = ev.get('input_sha256', '')
        plan_item = plan_moves_by_src.get(src, {'src': src, 'dst': dst, 'input_sha256': sha})

        # Check if destination file exists and matches size/hash
        dst_valid = False
        if os.path.exists(dst):
            if verify_hash and sha:
                try:
                    with open(dst, 'rb') as f:
                        dst_valid = (sha256_bytes(f.read()) == sha)
                except OSError:
                    dst_valid = False
            else:
                expected_size = plan_item.get('size')
                if expected_size is not None:
                    try:
                        dst_valid = (os.path.getsize(dst) == expected_size)
                    except OSError:
                        dst_valid = False
                else:
                    dst_valid = True

        if dst_valid and not os.path.exists(src):
            # Rename won the crash
            event_log.append('move_applied', src=src, dst=dst, input_sha256=sha)
        elif os.path.exists(src):
            # Move didn't happen yet or failed before rename; retry move
            ok, reason = apply_move(plan_item, input_folder=input_folder, verify_hash=verify_hash)
            if ok:
                event_log.append('move_applied', src=src, dst=dst, input_sha256=sha)
            else:
                event_log.append('move_skipped', src=src, dst=dst, skip=reason)
        else:
            event_log.append('move_skipped', src=src, dst=dst, skip='unresolved_crash')

    applied_count = 0
    skipped_count = 0

    applied_srcs = set()
    skipped_srcs = set()
    for ev in event_log.events:
        ev_type = ev.get('type') or ev.get('event')
        if ev_type == 'move_applied':
            applied_srcs.add(ev.get('src'))
        elif ev_type == 'move_skipped':
            skipped_srcs.add(ev.get('src'))
    batch_counter = 0

    for item in plan_moves:
        src = item['src']
        dst = item['dst']
        if src in applied_srcs or src in skipped_srcs:
            continue

        batch_counter += 1
        if batch_counter % 50 == 0:
            serve_url = get_serve_url()
            ok, gate_reason = gate_check(job, serve_url)
            if not ok:
                event_log.append('job_paused', reason=gate_reason)
                print(f"gate paused move execution: {gate_reason}")

        event_log.append('move_applying', src=src, dst=dst, input_sha256=item['input_sha256'])
        ok, reason = apply_move(item, input_folder=input_folder, verify_hash=verify_hash)
        if ok:
            event_log.append('move_applied', src=src, dst=dst, input_sha256=item['input_sha256'])
            applied_count += 1
            applied_srcs.add(src)
        else:
            event_log.append('move_skipped', src=src, dst=dst, skip=reason)
            skipped_count += 1
            skipped_srcs.add(src)

    event_log.append('organize_complete', applied=applied_count, skipped=skipped_count)
    print(f"organize complete: {applied_count} applied, {skipped_count} skipped")
    send_local_notification("Samosa Jobs", f"Organize complete: {applied_count} applied, {skipped_count} skipped")
    return 0


def cmd_undo(args):
    """samosa jobs undo <job.json> [--yes]"""
    if len(args) < 1:
        print("Usage: samosa jobs undo <job.json> [--yes]", file=sys.stderr)
        return 2

    job_file = args[0]
    auto_yes = '--yes' in args

    job, errors = load_and_validate_job(job_file)
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

    applied_events = [ev for ev in event_log.events if (ev.get('type') or ev.get('event')) == 'move_applied']
    if not applied_events:
        print("nothing to undo")
        return 0

    reverted_srcs = set()
    for ev in event_log.events:
        if (ev.get('type') or ev.get('event')) == 'move_reverted':
            reverted_srcs.add(ev.get('src'))

    pending_undo = [ev for ev in applied_events if ev.get('src') not in reverted_srcs]
    if not pending_undo:
        print("nothing to undo")
        return 0

    print(f"Undo Plan Summary for job {job['job_id']}:")
    print(f"  Moves to revert: {len(pending_undo)}")

    if not auto_yes:
        if not sys.stdin.isatty():
            print("error: non-interactive invocation requires --yes", file=sys.stderr)
            return 2
        try:
            resp = input("Undo applied moves? [y/N]: ").strip().lower()
        except EOFError:
            resp = ''
        if resp != 'y':
            print("aborted.")
            return 1

    reverted_count = 0
    skipped_count = 0
    input_folder = job['input']['folder']

    for ev in reversed(pending_undo):
        src = ev['src']
        dst = ev['dst']
        sha = ev.get('input_sha256', '')

        reverse_plan = {'src': dst, 'dst': src, 'input_sha256': sha}
        ok, reason = apply_move(reverse_plan, input_folder=input_folder, verify_hash=True)
        if ok:
            event_log.append('move_reverted', src=src, dst=dst, input_sha256=sha)
            reverted_count += 1
        else:
            event_log.append('undo_skipped', src=src, dst=dst, skip='changed_since_apply')
            skipped_count += 1

    print(f"undo complete: {reverted_count} reverted, {skipped_count} skipped")
    return 0


def cmd_suggest_job(args):
    """samosa jobs suggest-job [--description "..."] [--folder "..."] [--output-job "job.json"]"""
    desc = None
    folder = "./inputs"
    output_job = None

    i = 0
    while i < len(args):
        if args[i] in ('--description', '-d') and i + 1 < len(args):
            desc = args[i + 1]
            i += 2
        elif args[i] in ('--folder', '-f') and i + 1 < len(args):
            folder = args[i + 1]
            i += 2
        elif args[i] in ('--output-job', '-o') and i + 1 < len(args):
            output_job = args[i + 1]
            i += 2
        elif not desc and not args[i].startswith('-'):
            desc = args[i]
            i += 1
        else:
            i += 1

    if not desc:
        print("Usage: samosa jobs suggest-job [--description '...'] [--folder '/path'] [--output-job 'job.json']", file=sys.stderr)
        return 2

    folder_abs = os.path.abspath(folder)
    desc_lower = desc.lower()

    # Intent template keyword matching
    template_name = None
    if re.search(r'\b(sort|organize|arrange)\b.*\b(by extension|by type|type)\b', desc_lower):
        template_name = 'sort-by-type.job.json'
    elif re.search(r'\b(report|explore|count|summary)\b', desc_lower):
        template_name = 'folder-report.job.json'
    elif re.search(r'\b(two|2)\b.*\b(people|humans|persons)\b', desc_lower):
        template_name = 'photos-two-people.job.json'
    elif re.search(r'\b(receipt|receipts)\b', desc_lower):
        template_name = 'receipts-by-date.job.json'

    compiled_job = None
    if template_name:
        example_path = Path(__file__).parent.parent / 'docs' / 'examples' / 'jobs' / template_name
        if example_path.exists():
            try:
                compiled_job = json.loads(example_path.read_text())
                compiled_job['input']['folder'] = folder_abs
            except Exception:
                compiled_job = None

    if not compiled_job:
        # LLM compile call
        serve_url = get_serve_url()
        sys_prompt = (
            "You are a Samosa Jobs specification compiler. Given a user description and folder path, "
            "generate a complete, valid job.json object matching schema version 1.\n"
            "Return ONLY the raw JSON object."
        )
        user_prompt = f"Description: {desc}\nFolder: {folder_abs}"
        body = {
            'model': 'qwen3.6-35b-a3b',
            'messages': [
                {'role': 'system', 'content': sys_prompt},
                {'role': 'user', 'content': user_prompt},
            ],
            'thinking': 'off',
            'temperature': 0,
            'max_tokens': 1024,
            'stream': False,
        }
        resp, err = call_serve(body, serve_url, timeout=120)
        if not err and resp:
            content = resp.get('choices', [{}])[0].get('message', {}).get('content', '')
            val = validate_output(content, {'type': 'object'})
            compiled_job = val.get('record')

        if not compiled_job:
            # Fallback job spec synthesis
            slug = re.sub(r'[^a-z0-9]+', '-', desc_lower).strip('-')[:30] or 'suggested-job'
            compiled_job = {
                "schema_version": 1,
                "job_id": slug,
                "name": f"Suggested Job — {slug}",
                "input": {
                    "folder": folder_abs,
                    "recursive": True,
                    "types": ["image/jpeg", "image/png", "text/plain", "application/pdf"],
                    "max_file_bytes": 26214400
                },
                "unit": "auto",
                "instruction": f"Extract fields based on: {desc}",
                "reduce": {"mode": "deterministic"},
                "inference": {"thinking": "off", "seed": 11, "temperature": 0, "max_tokens": 512},
                "output_schema": {
                    "type": "object",
                    "required": ["summary"],
                    "properties": {
                        "summary": {"type": ["string", "null"]}
                    }
                },
                "output": {"dir": f"{folder_abs}/results", "format": "jsonl"},
                "resources": {"max_attempts": 3, "run_on_battery": False, "pause_when_user_active": True, "min_free_gb": 5}
            }

    # Validate compiled job
    _, errors = validate_job(compiled_job)
    if errors:
        print(f"warning: compiled job had validation errors: {errors}", file=sys.stderr)

    job_json_str = json.dumps(compiled_job, indent=2)
    if output_job:
        out_path = Path(output_job)
        atomic_write(out_path, job_json_str)
        print(f"suggested job written to: {out_path}")
    else:
        print(job_json_str)
    return 0


def cmd_review_patch(args):
    """samosa jobs review-patch <job.json> --unit <unit_id> --field <name> --val <value>"""
    if len(args) < 1:
        print("Usage: samosa jobs review-patch <job.json> --unit <unit_id> --field <name> --val <value>", file=sys.stderr)
        return 2

    job_file = args[0]
    unit_id = None
    field_name = None
    field_val = None

    i = 1
    while i < len(args):
        if args[i] == '--unit' and i + 1 < len(args):
            unit_id = args[i + 1]
            i += 2
        elif args[i] == '--field' and i + 1 < len(args):
            field_name = args[i + 1]
            i += 2
        elif args[i] in ('--val', '--value') and i + 1 < len(args):
            field_val = args[i + 1]
            i += 2
        else:
            i += 1

    if not unit_id or not field_name or field_val is None:
        print("error: --unit, --field, and --val are required", file=sys.stderr)
        return 2

    job, errors = load_and_validate_job(job_file)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2

    job_dir = Path(get_jobs_root()) / job['job_id']
    items_dir = job_dir / 'results' / 'items'
    safe_uid = unit_id.replace('#', '_').replace('/', '_')
    result_path = items_dir / f"{safe_uid}.json"
    prov_path = items_dir / f"{safe_uid}.provenance.json"

    if not result_path.exists():
        print(f"error: unit result file not found: {result_path}", file=sys.stderr)
        return 2

    try:
        rec = json.loads(result_path.read_text())
    except Exception as e:
        print(f"error: failed reading unit result: {e}", file=sys.stderr)
        return 2

    props = job.get('output_schema', {}).get('properties', {})
    f_schema = props.get(field_name, {})
    ftypes = f_schema.get('type', [])
    if isinstance(ftypes, str):
        ftypes = [ftypes]

    parsed_val = field_val
    if 'number' in ftypes or 'integer' in ftypes:
        try:
            parsed_val = int(field_val) if 'integer' in ftypes else float(field_val)
        except ValueError:
            pass
    elif 'boolean' in ftypes:
        if field_val.lower() in ('true', '1', 'yes'):
            parsed_val = True
        elif field_val.lower() in ('false', '0', 'no'):
            parsed_val = False

    rec[field_name] = parsed_val
    atomic_write(result_path, json.dumps(rec, indent=2))

    input_sha256 = ''
    input_path = ''
    if prov_path.exists():
        try:
            prov = json.loads(prov_path.read_text())
            prov['validation'] = 'passed'
            prov['manual_correction'] = True
            atomic_write(prov_path, json.dumps(prov, indent=2))
            input_sha256 = prov.get('input_sha256', '')
            input_path = prov.get('input_path', '')
        except Exception:
            pass

    review_file = job_dir / 'results' / 'review' / f"{safe_uid}.json"
    if review_file.exists():
        try:
            review_file.unlink()
        except OSError:
            pass

    log_path = job_dir / 'events.jsonl'
    event_log = EventLog(log_path)
    event_log.load()
    event_log.append('unit_patched', unit_id=unit_id, field=field_name, val=parsed_val)
    event_log.append('item_complete', unit_id=unit_id, artifact=unit_id, validation='passed', input_sha256=input_sha256, input_path=input_path)

    write_merged_output(job, str(job_dir), event_log)
    render_view_html(job, event_log.events, str(job_dir))

    print(f"patched unit {unit_id}: field '{field_name}' updated to {json.dumps(parsed_val)}")
    return 0


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

PLIST_LABEL = "com.samosa.jobsd"
PLIST_PATH = Path.home() / "Library" / "LaunchAgents" / f"{PLIST_LABEL}.plist"


def generate_launchd_plist(script_path):
    python_exe = sys.executable
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>{PLIST_LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>{python_exe}</string>
        <string>{script_path}</string>
        <string>daemon</string>
        <string>run-loop</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>{Path.home()}/.samosa/jobsd.log</string>
    <key>StandardErrorPath</key>
    <string>{Path.home()}/.samosa/jobsd.err</string>
</dict>
</plist>
"""


def cmd_daemon(args):
    """samosa jobs daemon [install|uninstall|status|run-loop]"""
    sub = args[0] if args else 'status'
    if sub == 'install':
        return daemon_install()
    elif sub == 'uninstall':
        return daemon_uninstall()
    elif sub == 'status':
        return daemon_status()
    elif sub == 'run-loop':
        once = '--once' in args
        return daemon_run_loop(once=once)
    else:
        print("Usage: samosa jobs daemon [install|uninstall|status|run-loop]", file=sys.stderr)
        return 2


def daemon_install():
    if platform.system() != 'Darwin':
        print("launchd daemon installation is currently supported on macOS.", file=sys.stderr)
        return 1
    PLIST_PATH.parent.mkdir(parents=True, exist_ok=True)
    script_path = str(Path(__file__).resolve())
    plist_content = generate_launchd_plist(script_path)
    PLIST_PATH.write_text(plist_content)
    subprocess.run(['launchctl', 'unload', str(PLIST_PATH)], capture_output=True)
    res = subprocess.run(['launchctl', 'load', str(PLIST_PATH)], capture_output=True, text=True)
    print(f"installed launchd agent {PLIST_LABEL} at {PLIST_PATH}")
    return 0


def daemon_uninstall():
    if PLIST_PATH.exists():
        subprocess.run(['launchctl', 'unload', str(PLIST_PATH)], capture_output=True)
        try:
            PLIST_PATH.unlink()
        except OSError:
            pass
        print(f"uninstalled launchd agent {PLIST_LABEL}")
    else:
        print(f"plist not found: {PLIST_PATH}")
    return 0


def daemon_status():
    installed = PLIST_PATH.exists()
    status_str = "installed" if installed else "not installed"
    print(f"daemon status: {status_str}")
    if installed:
        print(f"plist path: {PLIST_PATH}")
    return 0


def run_job_with_caffeinate(job_file):
    """Run job wrapped with caffeinate -i -s on macOS to prevent sleep."""
    cmd = [sys.executable, str(Path(__file__).resolve()), 'run', str(job_file)]
    if platform.system() == 'Darwin':
        cmd = ['caffeinate', '-i', '-s'] + cmd
    return subprocess.run(cmd)


def daemon_run_loop(once=False):
    """Background execution loop for scheduled jobs with missed-window handling."""
    print("samosa-jobsd scheduler starting...")
    jobs_root = get_jobs_root()
    while True:
        try:
            if jobs_root.exists():
                for job_dir in jobs_root.iterdir():
                    if job_dir.is_dir() and (job_dir / 'job.json').exists():
                        _check_and_run_scheduled_job(job_dir / 'job.json')
        except Exception as e:
            print(f"[jobsd error] {e}", file=sys.stderr)
        
        if once:
            break
        time.sleep(30)
    return 0


def _check_and_run_scheduled_job(job_file):
    try:
        data = json.loads(job_file.read_text())
        schedule = data.get('schedule')
        if not schedule:
            return
        policy = schedule.get('missed_window_policy', 'catch_up')
        interval = schedule.get('interval_seconds', 3600)
        state_file = job_file.parent / 'schedule_state.json'
        last_run = 0
        if state_file.exists():
            st = json.loads(state_file.read_text())
            last_run = st.get('last_run', 0)
        
        now = time.time()
        elapsed = now - last_run
        if elapsed >= interval:
            if last_run > 0 and elapsed > (interval * 2) and policy == 'skip':
                print(f"[jobsd] Missed window for {data.get('job_id')}, skipping per policy 'skip'")
                state_file.write_text(json.dumps({'last_run': now}))
                return
            print(f"[jobsd] Triggering scheduled job: {data.get('job_id')}")
            state_file.write_text(json.dumps({'last_run': now}))
            run_job_with_caffeinate(job_file)
    except Exception as e:
        print(f"[jobsd schedule error] {job_file}: {e}", file=sys.stderr)


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
    'suggest-job': cmd_suggest_job,
    'review-patch': cmd_review_patch,
    'organize': cmd_organize,
    'report': cmd_report,
    'apply': cmd_apply,
    'undo': cmd_undo,
    'daemon': cmd_daemon,
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
