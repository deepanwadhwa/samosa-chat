#!/usr/bin/env python3
"""The Tool layer: capabilities a model invokes to do a job.

Separation of concerns (owner's model): the *model* is the foundation; *tools*
are the things a model uses — search the web, list a folder, read a file, make a
directory, move a file; a *job* is the utility software that drives a model
through those tools. web_search is not special — it is one tool alongside the
filesystem tools.

This module owns three things and nothing else:

  1. The **tool-call protocol** — a model asks for a tool by emitting one line of
     JSON `{"samosa_tool": "...", ...args}`; the runner executes it and feeds the
     result back beginning `SAMOSA_TOOL_RESULT`. (Same convention the gateway
     already primes every model with.)
  2. A **registry** of tools, each with a name, an arg spec used to build the
     model-facing menu, a `run(args, ctx)`, and a `mutating` flag.
  3. A **permission boundary** (`ToolContext`): every path is resolved inside a
     rooted working directory (escapes rejected), and mutating tools are refused
     unless the context is in `execute` mode. A plan you can watch is only safe
     if the tools underneath it cannot wander off the folder or move files while
     you are still reviewing.

The filesystem tools are registered here (backed by the deterministic
`jobs_fs`). Web tools are registered by whoever owns their implementation
(the gateway) via `register_web_tools`, so this layer stays free of network,
SSRF, and provider config. `run_tool_loop` is the shared agent loop used by both
chat (web tools) and jobs (fs + web tools).
"""

import datetime
import json
import os
import re

import jobs_fs as fs


# --- Permission boundary ---------------------------------------------------

class ToolError(Exception):
    """A tool refused to run (bad path, wrong mode, missing arg)."""


class AwaitUser(Exception):
    """A tool paused the loop until the user provides an answer."""

    def __init__(self, question):
        super().__init__(question)
        self.question = question


class AwaitApply(Exception):
    """A mutating tool call was staged until the user approves it."""

    def __init__(self, call):
        super().__init__("await apply")
        self.call = call


class ToolContext:
    """Execution context for a run of tools.

    root  — the working directory tools are jailed to (realpath'd).
    mode  — 'preview' (read-only only) or 'execute' (mutating allowed).
    emit  — callback(event_type, **fields) for the live action stream.
    """

    def __init__(self, root, mode='preview', emit=None, job_dir=None, stage_mutations=False):
        self.root = os.path.realpath(root)
        if not os.path.isdir(self.root):
            raise ToolError(f"working directory does not exist: {root}")
        self.mode = mode
        self.job_dir = job_dir
        self.stage_mutations = stage_mutations
        self._emit = emit or (lambda *a, **k: None)

    def emit(self, event_type, **fields):
        self._emit(event_type, **fields)

    def resolve(self, path, must_exist=False):
        """Resolve a tool-supplied path inside the jail, rejecting escapes.

        Relative paths are taken against root. The resolved realpath (of the
        path, or its parent if it does not exist yet) must stay under root.
        """
        if path is None or path == '':
            candidate = self.root
        elif os.path.isabs(path):
            candidate = path
        else:
            candidate = os.path.join(self.root, path)

        # Anchor the containment check on an existing ancestor so that a
        # not-yet-created destination (e.g. a new folder) is still checked.
        probe = candidate
        while not os.path.exists(probe):
            parent = os.path.dirname(probe)
            if parent == probe:
                break
            probe = parent
        real = os.path.realpath(candidate) if os.path.exists(candidate) else \
            os.path.join(os.path.realpath(probe), os.path.relpath(candidate, probe))

        rel = os.path.relpath(real, self.root)
        if rel == '..' or rel.startswith('..' + os.sep):
            raise ToolError(f"path escapes the working folder: {path}")
        if must_exist and not os.path.exists(real):
            raise ToolError(f"no such path: {path}")
        return real


# --- Tool + registry -------------------------------------------------------

class Tool:
    def __init__(self, name, description, params, run, mutating=False):
        self.name = name
        self.description = description
        self.params = params            # list of (name, required, help)
        self.run = run                  # run(args: dict, ctx: ToolContext) -> str
        self.mutating = mutating


class Registry:
    def __init__(self):
        self._tools = {}

    def register(self, tool):
        self._tools[tool.name] = tool
        return tool

    def get(self, name):
        return self._tools.get(name)

    def names(self):
        return list(self._tools)

    def subset(self, names):
        return [self._tools[n] for n in names if n in self._tools]


REGISTRY = Registry()


# --- The tool-call protocol (shared by chat and jobs) ----------------------

MAX_TOOL_ROUNDS = 8


def ability_prompt(tools, locality=''):
    """Build the model-facing tool menu from a list of Tool objects."""
    if not tools:
        return f"\n\nThe user's approximate location is {locality}." if locality else ""
    lines = [
        "\n\nYou have real abilities this app runs for you. To use one, reply with ONLY a "
        "single line of JSON — no other words, no code fences:",
    ]
    for tool in tools:
        arglist = ", ".join(
            f'"{name}":"..."' for name, _req, _help in tool.params
        )
        head = f'{{"samosa_tool":"{tool.name}"' + (", " + arglist if arglist else "") + "}"
        lines.append(f"{head} — {tool.description}")
    lines.append(
        "The app will run the tool and reply with a message beginning SAMOSA_TOOL_RESULT; "
        f"use that output. You may use at most {MAX_TOOL_ROUNDS} tool calls per message. "
        "Do not repeat the same call. When you have done everything the task needs, reply "
        "with a short plain-text summary instead of a tool line."
    )
    if locality:
        lines.append(f"The user's approximate location is {locality}.")
    return "\n".join(lines)


def classify_reply(text):
    """Classify streamed/assistant text: a tool call, ordinary text, or undecided.

    Ported from the gateway so chat and jobs share one convention.
    """
    check = text.strip()
    check = re.sub(r"^```(?:json)?\s*", "", check)
    if not check:
        return "wait", None
    if not check.startswith("{"):
        return "text", None
    for candidate in (re.sub(r"\s*```\s*$", "", check), check.split("\n", 1)[0]):
        try:
            value = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and isinstance(value.get("samosa_tool"), str):
            return "tool", value
        return "text", None
    if '"samosa_tool"' not in check[:40] and len(check) > 40:
        return "text", None
    if len(check) > 700:
        return "text", None
    return "wait", None


def execute_tool(call, ctx, tools):
    """Execute one tool call against the registry, enforcing the boundary.

    Returns a text result for the model. Errors are returned as text (never
    raised) so a single bad call does not abort the loop. `ctx` may be None
    when `tools` is known to contain no mutating (filesystem) tools — chat's
    web-only toolset has no working folder to jail, so it calls with no
    context rather than faking one.
    """
    name = str(call.get("samosa_tool", ""))
    available = {t.name: t for t in tools}
    tool = available.get(name)
    if tool is None:
        return f"unknown tool {name!r}; available tools: {', '.join(available) or 'none'}"
    if tool.mutating and ctx is None:
        return f"tool {name} is not allowed here; it changes files"
    if tool.mutating and ctx.mode != 'execute':
        if getattr(ctx, 'stage_mutations', False):
            raise AwaitApply(call)
        return f"tool {name} is not allowed in preview mode; it changes files"
    try:
        if ctx is not None:
            ctx.emit('tool_call', tool=name, args={k: v for k, v in call.items() if k != 'samosa_tool'})
        result = tool.run(call, ctx)
        return result if isinstance(result, str) else json.dumps(result)
    except AwaitUser:
        raise
    except AwaitApply:
        raise
    except ToolError as e:
        return f"tool {name} refused: {e}"
    except Exception as e:  # defensive: a tool bug must not crash the job
        return f"tool {name} failed: {e}"


def iter_tool_loop(model_call, messages, tools, ctx, max_rounds=MAX_TOOL_ROUNDS,
                   add_ability_prompt=True):
    """Drive a model through a tool-using conversation, yielding loop events.

    model_call(messages) -> assistant_text. `messages` is an OpenAI-style list;
    the ability prompt is appended to the system message here. Each tool call
    fires ctx.emit('tool_call', ...) and, when it returns, is fed back as the
    next user turn beginning SAMOSA_TOOL_RESULT.
    """
    convo = [dict(m) for m in messages]
    if add_ability_prompt:
        system_add = ability_prompt(tools)
        if convo and convo[0].get('role') == 'system':
            convo[0]['content'] = str(convo[0].get('content', '')) + system_add
        else:
            convo.insert(0, {'role': 'system', 'content': system_add.lstrip()})

    last_result = ''
    for round_i in range(max_rounds):
        text = model_call(convo)
        kind, call = classify_reply(text or '')
        if kind != 'tool':
            yield {'type': 'final', 'text': text, 'convo': convo}
            return
        try:
            result = execute_tool(call, ctx, tools)
        except AwaitUser as pause:
            yield {'type': 'await_user', 'question': pause.question, 'call': call,
                   'convo': convo, 'round_i': round_i}
            return
        except AwaitApply as pause:
            yield {'type': 'await_apply', 'call': pause.call, 'convo': convo,
                   'round_i': round_i}
            return
        yield {'type': 'tool_result', 'call': call, 'result': result}
        last_result = result
        remaining = max_rounds - round_i - 1
        note = ("\n\n(No tool calls remain; answer now.)" if remaining <= 0
                else f"\n\n({remaining} tool call(s) left.)")
        convo.append({'role': 'assistant', 'content': (text or '').strip()})
        convo.append({'role': 'user',
                      'content': f"SAMOSA_TOOL_RESULT {call.get('samosa_tool', '')}\n{result}{note}"})
    yield {'type': 'final', 'text': last_result, 'convo': convo}


def run_tool_loop(model_call, messages, tools, ctx, max_rounds=MAX_TOOL_ROUNDS):
    """Compatibility wrapper returning only the final answer."""
    final = ''
    for event in iter_tool_loop(model_call, messages, tools, ctx, max_rounds=max_rounds):
        if event.get('type') == 'final':
            final = event.get('text') or ''
    return final


# --- Filesystem tools (backed by jobs_fs) ----------------------------------

def _read_header(path, n=16):
    try:
        with open(path, 'rb') as f:
            return f.read(n)
    except OSError:
        return b''


def _tool_fs_survey(args, ctx):
    """Count files under the working folder, grouped by detected type."""
    recursive = bool(args.get('recursive', True))
    payload, error = fs.fs_sidecar_survey(ctx.root, recursive=recursive)
    if error:
        raise ToolError(error)
    by_type = payload.get('by_type', {})
    counts = {k: v.get('count', 0) for k, v in by_type.items() if isinstance(v, dict)}
    ctx.emit('survey', total=payload.get('total', 0), skipped=payload.get('skipped_count', 0),
             by_type=counts)
    parts = [f"{v} {k}" for k, v in sorted(counts.items())]
    return f"{payload.get('total', 0)} files under the folder: " + (", ".join(parts) or "none")


def _tool_fs_list(args, ctx):
    """List files in a folder with detected type, size, and mtime."""
    target = ctx.resolve(args.get('path', '.'), must_exist=True)
    limit = int(args.get('limit', 200))
    recursive = bool(args.get('recursive', False))
    items, skipped, error = fs.fs_sidecar_list(target, recursive=recursive)
    if error:
        raise ToolError(error)
    rows = []
    for item in items[:limit]:
        rel = os.path.relpath(item['input_path'], ctx.root)
        rows.append(f"{rel}\t{item['media_type']}\t{item['size']} bytes\t{_format_mtime(item.get('mtime'))}")
    more = '' if len(items) <= limit else f"\n… {len(items) - limit} more"
    if skipped:
        more += f"\n… {len(skipped)} skipped"
    return "\n".join(rows) + more if rows else "(empty)"


def _format_mtime(value):
    try:
        return datetime.datetime.fromtimestamp(float(value), datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    except (TypeError, ValueError, OSError):
        return "mtime_unknown"


def _tool_fs_metadata(args, ctx):
    """Return metadata for one file."""
    target = ctx.resolve(args.get('path'), must_exist=True)
    payload, error = fs.fs_sidecar_metadata(target)
    if error:
        raise ToolError(error)
    rel = os.path.relpath(payload.get('path', target), ctx.root)
    return "\n".join([
        f"path\t{rel}",
        f"type\t{payload.get('media_type', 'application/octet-stream')}",
        f"size\t{payload.get('size', 0)} bytes",
        f"mtime\t{_format_mtime(payload.get('mtime'))}",
        f"sha256\t{payload.get('input_sha256', '')}",
    ])


def _tool_fs_detect_type(args, ctx):
    """Detect a single file's media type by content."""
    target = ctx.resolve(args.get('path'), must_exist=True)
    mt = fs.detect_media_type(_read_header(target))
    if mt is None:
        with open(target, 'rb') as f:
            mt = 'text/plain' if fs.is_valid_utf8_text(f.read()) else 'application/octet-stream'
    return mt


def _tool_fs_read_text(args, ctx):
    """Read up to max_bytes of a text file."""
    target = ctx.resolve(args.get('path'), must_exist=True)
    max_bytes = int(args.get('max_bytes', 8192))
    with open(target, 'rb') as f:
        data = f.read(max_bytes + 1)
    if not fs.is_valid_utf8_text(data[:max_bytes]):
        raise ToolError("not a UTF-8 text file")
    text = data[:max_bytes].decode('utf-8', errors='replace')
    return text + ("\n… (truncated)" if len(data) > max_bytes else "")


def _tool_fs_read_document(args, ctx):
    """Read the text of a document (PDF today; plain text too), any length.

    Unlike fs_read_text (plain text only, fast path, no subprocess), this
    dispatches through the samosa-extract sidecar, so it also handles PDFs —
    and reports a clear reason instead of garbage for formats not wired up yet
    (.docx, .html, .rtf).
    """
    target = ctx.resolve(args.get('path'), must_exist=True)
    max_chars = int(args.get('max_chars', 8000))
    result, error = fs.extract_document(target)
    if error:
        raise ToolError(error)
    text = result['text']
    pages = result['pages']
    header = f"[{len(pages)} page(s)] " if len(pages) > 1 else ""
    body = text[:max_chars] + ("\n… (truncated)" if len(text) > max_chars else "")
    return header + body if body.strip() else header + "(no extractable text)"


def _tool_fs_read_page(args, ctx):
    """Read one page of a document, using the document sidecar."""
    target = ctx.resolve(args.get('path'), must_exist=True)
    try:
        page_number = int(args.get('page', 1))
    except (TypeError, ValueError):
        raise ToolError("page must be an integer")
    if page_number < 1:
        raise ToolError("page must be 1 or greater")
    max_chars = int(args.get('max_chars', 4000))
    result, error = fs.extract_document(target)
    if error:
        raise ToolError(error)
    pages = result['pages']
    for page in pages:
        if page.get('index') == page_number:
            text = page.get('text', '')
            body = text[:max_chars] + ("\n... (truncated)" if len(text) > max_chars else "")
            return f"[page {page_number} of {len(pages)}]\n" + (body if body.strip() else "(no extractable text)")
    raise ToolError(f"page out of range: {page_number}")


def _notes_path(ctx):
    job_dir = getattr(ctx, 'job_dir', None)
    if not job_dir:
        raise ToolError("notes are only available inside a job")
    os.makedirs(job_dir, exist_ok=True)
    return os.path.join(job_dir, 'notes.txt')


def _tool_notes_append(args, ctx):
    """Append a note to the job-local notes file."""
    text = str(args.get('text', ''))[:4000]
    if not text.strip():
        raise ToolError("note text is required")
    path = _notes_path(ctx)
    with open(path, 'a', encoding='utf-8') as f:
        f.write(text.rstrip() + "\n")
    ctx.emit('notes', op='append', bytes=len(text.encode('utf-8')))
    return "note saved"


def _tool_notes_read(args, ctx):
    """Read the job-local notes file."""
    path = _notes_path(ctx)
    try:
        with open(path, encoding='utf-8') as f:
            text = f.read(12000)
    except OSError:
        return "(no notes yet)"
    return text if text.strip() else "(no notes yet)"


def _tool_ask_user(args, ctx):
    """Pause the job and ask the user a clarifying question."""
    question = str(args.get('question', '')).strip()
    if not question:
        raise ToolError("question is required")
    raise AwaitUser(question[:1000])


def _tool_fs_mkdir(args, ctx):
    """Create a directory (and parents) inside the working folder."""
    if not is_valid_reldir(args.get('path')):
        raise ToolError("mkdir path must be a simple folder name under the working folder")
    target = ctx.resolve(args.get('path'))
    os.makedirs(target, exist_ok=True)
    ctx.emit('mkdir', path=os.path.relpath(target, ctx.root))
    return f"created {os.path.relpath(target, ctx.root)}"


def _tool_fs_move(args, ctx):
    """Move a file within the working folder (atomic, no-clobber)."""
    src = ctx.resolve(args.get('src'), must_exist=True)
    dst = ctx.resolve(args.get('dst'))
    plan = {'src': src, 'dst': dst}
    ok, reason = fs.apply_move(plan, input_folder=ctx.root)
    rel_src = os.path.relpath(src, ctx.root)
    rel_dst = os.path.relpath(dst, ctx.root)
    ctx.emit('move', src=rel_src, dst=rel_dst, ok=ok, reason=reason)
    if ok:
        return f"moved {rel_src} -> {rel_dst}"
    return f"skipped {rel_src}: {reason}"


def is_valid_reldir(name):
    """A destination folder name/relative path that stays simple and inside root."""
    if not isinstance(name, str) or not name:
        return False
    if os.path.isabs(name) or '\x00' in name:
        return False
    parts = [p for p in name.replace('\\', '/').split('/') if p]
    return bool(parts) and all(fs.is_valid_folder_name(p) for p in parts)


def register_fs_tools(registry=REGISTRY):
    registry.register(Tool(
        'fs_survey', "count the files in the folder by type",
        [('recursive', False, 'walk subfolders (default true)')],
        _tool_fs_survey, mutating=False))
    registry.register(Tool(
        'fs_list', "list files in a folder with their types",
        [('path', False, 'subfolder to list (default the working folder)'),
         ('limit', False, 'maximum rows to return')],
        _tool_fs_list, mutating=False))
    registry.register(Tool(
        'fs_metadata', "inspect one file's size, modified time, SHA-256, and content type",
        [('path', True, 'file to inspect')],
        _tool_fs_metadata, mutating=False))
    registry.register(Tool(
        'fs_detect_type', "detect one file's type by its contents",
        [('path', True, 'file to inspect')],
        _tool_fs_detect_type, mutating=False))
    registry.register(Tool(
        'fs_read_text', "read a plain text file (not PDF/docx — use fs_read_document for those)",
        [('path', True, 'file to read')],
        _tool_fs_read_text, mutating=False))
    registry.register(Tool(
        'fs_read_document', "read the text of a PDF (or other document) — extracts the content",
        [('path', True, 'file to read')],
        _tool_fs_read_document, mutating=False))
    registry.register(Tool(
        'fs_read_page', "read one page from a PDF or document",
        [('path', True, 'file to read'), ('page', True, '1-based page number')],
        _tool_fs_read_page, mutating=False))
    registry.register(Tool(
        'notes_append', "save a short note for this job",
        [('text', True, 'note to save')],
        _tool_notes_append, mutating=False))
    registry.register(Tool(
        'notes_read', "read notes saved during this job",
        [],
        _tool_notes_read, mutating=False))
    registry.register(Tool(
        'ask_user', "ask the user one clarifying question and pause this job",
        [('question', True, 'question to ask the user')],
        _tool_ask_user, mutating=False))
    registry.register(Tool(
        'fs_mkdir', "create a folder",
        [('path', True, 'folder name to create')],
        _tool_fs_mkdir, mutating=True))
    registry.register(Tool(
        'fs_move', "move a file to another folder",
        [('src', True, 'file to move'), ('dst', True, 'destination path')],
        _tool_fs_move, mutating=True))


register_fs_tools()


# --- Web tools (implementation injected by the gateway) --------------------

def register_web_tools(search_fn, fetch_fn, registry=REGISTRY):
    """Register web_search/open_url, whose implementation lives in the gateway.

    search_fn(query) -> str, fetch_fn(url) -> str. Keeping the network/SSRF/
    provider code in the gateway (where its config lives) means this layer stays
    pure; the tools still appear in the same registry the model draws from.
    """
    registry.register(Tool(
        'web_search', "search the public web",
        [('query', True, 'search terms')],
        lambda args, ctx: search_fn(str(args.get('query', ''))), mutating=False))
    registry.register(Tool(
        'open_url', "read one public web page",
        [('url', True, 'https URL to read')],
        lambda args, ctx: fetch_fn(str(args.get('url', ''))), mutating=False))
