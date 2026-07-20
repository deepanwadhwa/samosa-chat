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

import json
import os
import re

import jobs_fs as fs


# --- Permission boundary ---------------------------------------------------

class ToolError(Exception):
    """A tool refused to run (bad path, wrong mode, missing arg)."""


class ToolContext:
    """Execution context for a run of tools.

    root  — the working directory tools are jailed to (realpath'd).
    mode  — 'preview' (read-only only) or 'execute' (mutating allowed).
    emit  — callback(event_type, **fields) for the live action stream.
    """

    def __init__(self, root, mode='preview', emit=None):
        self.root = os.path.realpath(root)
        if not os.path.isdir(self.root):
            raise ToolError(f"working directory does not exist: {root}")
        self.mode = mode
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
    raised) so a single bad call does not abort the loop.
    """
    name = str(call.get("samosa_tool", ""))
    available = {t.name: t for t in tools}
    tool = available.get(name)
    if tool is None:
        return f"unknown tool {name!r}; available tools: {', '.join(available) or 'none'}"
    if tool.mutating and ctx.mode != 'execute':
        return f"tool {name} is not allowed in preview mode; it changes files"
    try:
        ctx.emit('tool_call', tool=name, args={k: v for k, v in call.items() if k != 'samosa_tool'})
        result = tool.run(call, ctx)
        return result if isinstance(result, str) else json.dumps(result)
    except ToolError as e:
        return f"tool {name} refused: {e}"
    except Exception as e:  # defensive: a tool bug must not crash the job
        return f"tool {name} failed: {e}"


def run_tool_loop(model_call, messages, tools, ctx, max_rounds=MAX_TOOL_ROUNDS):
    """Drive a model through a tool-using conversation.

    model_call(messages) -> assistant_text. `messages` is an OpenAI-style list;
    the ability prompt is appended to the system message here. Each tool call
    fires ctx.emit('tool_call', ...) and, when it returns, is fed back as the
    next user turn beginning SAMOSA_TOOL_RESULT. Returns the model's final
    plain-text answer (or the last tool result if the round budget runs out).
    """
    convo = [dict(m) for m in messages]
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
            return text
        result = execute_tool(call, ctx, tools)
        last_result = result
        remaining = max_rounds - round_i - 1
        note = ("\n\n(No tool calls remain; answer now.)" if remaining <= 0
                else f"\n\n({remaining} tool call(s) left.)")
        convo.append({'role': 'assistant', 'content': (text or '').strip()})
        convo.append({'role': 'user',
                      'content': f"SAMOSA_TOOL_RESULT {call.get('samosa_tool', '')}\n{result}{note}"})
    return last_result


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
    items, skipped = fs.discover_files(
        {'folder': ctx.root, 'recursive': recursive}, is_metadata_only=True)
    summary = fs.count_by_type(items)
    ctx.emit('survey', total=len(items), skipped=len(skipped),
             by_type={k: v['count'] for k, v in summary.items()})
    parts = [f"{v['count']} {k}" for k, v in sorted(summary.items())]
    return f"{len(items)} files under the folder: " + (", ".join(parts) or "none")


def _tool_fs_list(args, ctx):
    """List entries in a folder (name + detected type), bounded."""
    target = ctx.resolve(args.get('path', '.'), must_exist=True)
    limit = int(args.get('limit', 200))
    rows = []
    try:
        entries = sorted(os.scandir(target), key=lambda e: e.name)
    except OSError as e:
        raise ToolError(str(e))
    for entry in entries[:limit]:
        if entry.is_symlink():
            kind = 'symlink'
        elif entry.is_dir():
            kind = 'dir'
        else:
            kind = fs.detect_media_type(_read_header(entry.path)) or 'file'
        rows.append(f"{entry.name}\t{kind}")
    more = '' if len(entries) <= limit else f"\n… {len(entries) - limit} more"
    return "\n".join(rows) + more if rows else "(empty)"


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
        [('path', False, 'subfolder to list (default the working folder)')],
        _tool_fs_list, mutating=False))
    registry.register(Tool(
        'fs_detect_type', "detect one file's type by its contents",
        [('path', True, 'file to inspect')],
        _tool_fs_detect_type, mutating=False))
    registry.register(Tool(
        'fs_read_text', "read the text of a file",
        [('path', True, 'file to read')],
        _tool_fs_read_text, mutating=False))
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
