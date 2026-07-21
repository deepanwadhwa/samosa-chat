# Samosa Sidecar Contract

This document freezes the contract for small compiled tools that Samosa runs
outside the resident model process. It codifies the existing `samosa-extract`
shape rather than introducing a second protocol.

## Purpose

Sidecars are constrained executables for one low-level domain: PDF extraction,
filesystem metadata, image conversion, and similar operations. The model never
generates code for them. It emits a structured tool call, the gateway selects a
registered tool, and the tool may dispatch to a sidecar with validated
arguments.

For Ornith, the gateway sends standard OpenAI Chat Completions `tools` schemas
to llama-server and consumes its parsed `tool_calls`. Tool results return as
`role: "tool"` messages with the matching `tool_call_id`; they are not encoded
as visible JSON or synthetic user messages. The text protocol remains only as
a compatibility path for backends without native function-call parsing.

The primary safety property is containment. A malformed file or expensive scan
must fail inside a short-lived process with its own limits, not inside the
gateway or model server.

## Binary Shape

Each sidecar owns one domain and exposes subcommands or flags for that domain.
Existing example:

```sh
samosa-extract --json FILE
samosa-extract --json-pages FILE.pdf START COUNT
samosa-extract --json FILE --tokenizer tokenizer.json
samosa-extract --render-ppm FILE.pdf PAGE OUTPUT.ppm
```

The filesystem sidecar follows the same family shape:

```sh
samosa-fs survey [flags] ROOT
samosa-fs list [flags] ROOT
samosa-fs metadata [flags] PATH
```

Arguments are passed through `argv`, not stdin shell snippets. Paths are plain
path arguments after the caller has applied the registry permission model. A
sidecar still revalidates filesystem invariants itself.

`--json-pages` accepts a one-based start page and a count from 1 through 5.
The native sidecar rejects larger ranges. Model-driven document inspection must
prefer this operation so each additional range requires a new model decision;
whole-document extraction is reserved for bounded batch pipelines that have
already calculated and enforced an input-token budget.

## Output

A sidecar writes exactly one JSON object to stdout, followed by a newline.
Stderr is diagnostic only and is not part of the machine contract.

Successful operations use:

```json
{"ok": true}
```

The object may include operation-specific fields. For example, `samosa-extract`
returns `text_layer`, `pages`, `text`, and token counts when requested.

Failures use:

```json
{"ok": false, "error": "stable_code"}
```

`error` is a stable, lowercase machine code. Callers translate it to user-facing
language. A caller must treat malformed JSON, missing `ok`, or an unexpected
shape as `extract_invalid_response` or the equivalent sidecar-specific invalid
response code.

Sidecars must not print progress records, logs, or multiple JSON objects to
stdout.

## Exit Codes

Exit code is secondary to the JSON envelope when stdout contains a valid
envelope.

The convention follows `samosa-extract`:

- `0`: success with `{"ok": true, ...}`.
- `64`: command-line usage error.
- `65`: input, validation, parsing, or operation failure with
  `{"ok": false, "error": ...}`.
- `70`: sidecar setup failure, such as unavailable sandbox limits.
- `124`: wall timeout emitted by the sidecar alarm path, with
  `{"ok": false, "error": "wall_timeout"}` when possible.

Callers must also enforce their own wall-clock timeout and kill the whole child
process group on expiry, matching `jobs_fs.extract_document`.

## Resource Limits

Every sidecar applies its own resource limits before touching untrusted input.
`samosa-extract` sets CPU and address/data limits internally, then the Python
caller adds a watchdog timeout and uses `killpg` on expiry. New sidecars should
preserve that two-layer shape:

- The sidecar sets CPU limits.
- The sidecar sets address-space or data-segment limits where the OS permits.
- The sidecar caps output size before buffering or writing JSON.
- The caller starts the process in a new process group/session.
- The caller kills the process group if the wall-clock timeout fires.

Platform differences are acceptable only as explicit degradation. For example,
some Darwin kernels reject finite address-space limits; the sidecar may continue
with the limits the OS accepts, while the parent watchdog remains mandatory.

## Filesystem Invariants

Sidecars that inspect user files must avoid pathname time-of-check/time-of-use
mistakes.

The current file pattern is:

- `lstat` the path.
- Reject symlinks.
- Open with `O_NOFOLLOW` when available.
- `fstat` the descriptor.
- Require a regular file.
- Verify the descriptor still matches the earlier path identity where possible.
- Read from the descriptor, not by reopening the path.

Directory-scanning sidecars must reject symlink entries and skip non-regular
files. They must use bounded reads for metadata-only scans: type from magic
bytes, size from `stat`, and dedup hash from full content only when the file was
not truncated. When content is truncated by the scan cap, hash the prefix plus a
truncation marker and real size.

Mutation sidecars are not part of v1 of `samosa-fs`. When added, they must keep
the existing no-delete boundary: no-clobber atomic rename only, no cross-volume
copy-then-delete moves, and journaled undo.

## Versioning

Each sidecar should support a version query:

```sh
samosa-fs --version
```

The version string is for diagnostics and packaging checks. Compatibility is
defined by command behavior and JSON fields, not by parsing the version string.
Adding optional fields is compatible. Removing fields, renaming fields, changing
stable error codes, or changing command semantics requires a documented contract
revision.

## Registry Names

Model-facing tool names are namespaced by domain, such as `fs.list`,
`pdf.extract`, and `image.convert`. Python registry names may keep their current
underscored names during transition, but each shim should map cleanly to one
sidecar operation.

The approval boundary remains in the registry and `ToolContext`: preview-mode
tools may read, but mutating operations must refuse to run unless the caller has
entered the approved execute path.
