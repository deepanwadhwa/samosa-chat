# Latest-chat reliability audit — 2026-08-11

Branch: `codex/latest-10-chat-analysis-20260811`

## Scope and evidence limits

Samosa keeps the message text in browser `localStorage` (`samosa-chat-v2`), not
under `~/.samosa/chats`. The local in-app browser bridge was unavailable during
this audit, and no exported Samosa backup was present. I therefore did not read
or infer the users' prompts or the assistants' answers.

The ten records below are the latest *created* server-side conversation
bindings, ordered by the `metadata.json` filesystem timestamp. They are not a
reliable latest-activity ordering: `updated_at` is currently written only when
the binding is created and none of the UUIDs are included in the runtime logs.
The findings correlate model bindings, backend timing/crash evidence, installed
binary layout, and shipped source. Per-conversation answer-quality review still
requires the browser state or an exported backup.

## Latest ten server-side bindings

| Created (America/New_York) | Conversation | Model |
|---|---|---|
| 2026-08-11 18:02:13 | `chat-550430d2-f3f4-4d87-9f4b-8e214461eadb` | Bonsai 27B Q1_0 |
| 2026-08-11 17:58:48 | `chat-1ce2f3ae-bca6-462a-8345-8e6c90a64e9d` | Ornith 9B Q4_K_M |
| 2026-08-11 17:54:30 | `chat-53308b42-600a-456c-a964-ccbe55570deb` | Maple |
| 2026-08-11 17:46:20 | `chat-5818b4a0-36b9-4f2f-bd66-709e3bee247a` | Maple |
| 2026-08-11 16:17:17 | `chat-cf465541-b6fc-4b99-8680-8279146012e9` | Maple |
| 2026-08-11 15:50:22 | `chat-4d776762-513d-4c04-af1f-892bacef36f4` | Maple |
| 2026-08-11 15:42:26 | `chat-e9685d78-f912-41db-918d-67fe69358dc1` | Maple |
| 2026-08-09 17:50:10 | `chat-3821918f-931b-4850-b467-72dbd14efe40` | Maple |
| 2026-08-09 17:48:52 | `chat-af3efa37-5da3-4b5d-8660-0fa87a4e7f03` | Maple |
| 2026-08-09 17:46:47 | `chat-812573bd-81a4-4d27-9aed-db13aa899550` | Maple |

Model distribution: Maple 8, Ornith 1, Bonsai 1.

## Confirmed findings

### 1. Maple requests could crash the entire backend

Severity: high.

`backend.log` contains four uncaught Maple failures at its rotating-cache
boundary. The most specific diagnostic is:

```text
Maple prefill chunk [512,576) cache_offset=576: Maple layer=0
query_len=64 cache_offset=576 mask=64x512: [broadcast_shapes]
Shapes (64,512) and (1,16,64,575) cannot be broadcast.
```

The attention-mask width defect is already corrected in ancestor commit
`4937479` and covered by the native 512+64 cache-boundary regression. However,
the HTTP worker still allowed any generation exception to escape the thread,
which invokes `std::terminate` and takes down Maple for every chat.

Fix in this branch: contain generation exceptions, log the private diagnostic,
return a safe structured stream/API error, and emit an OpenAI-compatible
terminal `finish_reason` (`stop` or `length`) on successful responses.

### 2. A crashed/truncated stream looked like a successful answer

Severity: high.

The browser finalized the assistant message whenever the response body reached
EOF. It did not require either a `[DONE]` sentinel or a non-null
`finish_reason`, and it ignored structured SSE errors. Because Maple sent HTTP
200 streaming headers before inference, a backend crash produced a normal EOF
from the browser's perspective. Blank or partial answers could therefore be
saved without an error.

Fix in this branch: track protocol completion, render structured backend
errors, and turn premature EOF into a visible retryable error while preserving
any partial text.

### 3. Installed PDF reading was broken by a non-relocatable rpath

Severity: high for document chats.

`server.log` contains 268 `Library not loaded: @rpath/libpdfium.dylib`
occurrences. The installed extractor and `libpdfium.dylib` were both in
`release/bin`, but the extractor's sole `LC_RPATH` pointed to the developer SDK
directory, which the runtime sandbox could not read.

Fix in this branch: build the extractor with a loader-relative rpath
(`@loader_path` on macOS, `$ORIGIN` on Linux), repair older locally built
extractors while staging them, and include the optional extractor and library
in the local release hash so a rebuilt sidecar cannot silently reuse a stale
release.

### 4. The latest Bonsai run consumed the full output cap

Severity: UX/performance observation; needs transcript review.

The latest Bonsai backend task processed a 1,777-token prompt and generated
exactly 2,048 tokens in 410.327 seconds. Generation throughput declined from
about 11.8 to 5.48 tokens/second. Hitting exactly the configured cap strongly
suggests a length-limited answer, but without the saved message text it is not
possible to distinguish a genuinely long requested answer from repetition,
rambling, or a missing stop condition. No behavioral change is made from this
evidence alone.

### 5. Chat diagnostics are not currently auditable end to end

Severity: medium, developer experience.

Server metadata has model identity but no messages or outcome, `updated_at`
does not track subsequent turns, and backend logs omit `conversation_id`.
Consequently, logs can establish aggregate failures and timing but cannot
attribute them to one of the eight Maple conversations. A privacy-conscious
follow-up should add explicit user-controlled transcript export and structured
per-turn diagnostics (conversation ID, model, terminal reason, timing, and
error class; never prompt text by default).

## Verification completed

- `node tests/test_web_activity_ui.mjs`
- `make test-ui-setup` (outside the restricted runner so its localhost fixtures could bind)
- `make samosa-maple`
- real two-token Maple HTTP stream emitted `finish_reason: length` followed by `[DONE]`
- `make test-maple-cache-boundary` (outside the restricted runner so MLX could use Metal)
- `make extract-test PDFIUM_DIR=...` (outside the restricted runner so its nested sandbox test could execute)
- clean-room local development install with no staged models; the installed
  extractor resolved `@rpath/libpdfium.dylib` via `@loader_path` and extracted
  the PDF fixture successfully
- `git diff --check`
