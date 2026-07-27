# Samosa browser UI overhaul and Chutni — implementation task program

**Status:** planned

**Product surface:** local browser application + headless local server

**Last scoped:** 2026-07-25

This document is the standalone source of truth for three connected pieces of
work:

1. Replace the current interface with a minimal, clean Samosa UI.
2. Add first-run profile and model-download onboarding.
3. Build **Chutni**, Samosa's durable, local memory for explicitly selected
   folders, drives, or readable user-document locations.

Gigatoken is the pinned bulk-tokenization engine for Chutni. The audited
upstream baseline is `marcelroed/gigatoken` v0.10.0 at commit
`34a1599f0c0ae7d7cd0d1c530e6522320158b360`. That pin is a source and behavior
reference, not permission to ship its Python package or enable its network
loader. T0.5 defines the native, local-only integration and its adoption gates.

An implementation agent should be able to work from this document without
needing the conversation that produced it. Existing task documents may provide
useful implementation history, but they do not define or override this
program.

---

## 1. Product in one page

### First run

```text
Name → Welcome → Choose a model → Download and verify → Chat
```

- The first screen asks: **“What should Samosa call you?”**
- The next screen says: **“Welcome, <name>.”**
- Samosa explains that it uses local models and keeps the user's chats and
  Chutni data on this computer.
- The user chooses a compatible model from a factual, server-provided catalog.
- Samosa downloads, verifies, and activates that model without requiring the
  user to leave the app.
- Returning users resume the first incomplete step. A completed setup opens
  directly into Chat.
- An existing valid model installation is detected and is never downloaded
  again merely because the UI changed.

### Normal use

The primary navigation is:

- **Chat**
- **Chutni**
- **Jobs**

The composer has one `+` menu for image, document, web page, and directory
context. Model and local-runtime status remain visible without dominating the
interface.

### Chutni

Chutni is an explicitly authorized, read-only memory scope. It can cover:

- one folder;
- one drive; or
- **This computer**, meaning a visible collection of readable user-document
  locations with safe exclusions—not an indiscriminate scan of `/`, system
  files, credentials, caches, or Samosa's own data.

The ingestion path is:

```text
Authorized inventory
        ↓
Safe extraction/OCR with provenance
        ↓
Gigatoken: exact token IDs and counts in bounded batches
        ↓
Deterministic model-sized chunks
        ├──→ publish raw chunks to SQLite FTS5 → searchable Chutni is Ready
        └──→ bounded local-model prefill → cards and directory summaries
```

Gigatoken makes the `text → tokens` stage fast. It does not walk directories,
extract PDFs, create a searchable index, run the model forward pass, or enlarge
the model's context window. A large folder or drive is therefore never loaded
into one prompt. Chutni preserves the complete searchable evidence outside the
model and sends only bounded chunks through model prefill or retrieval.

When a question has an explicit directory context:

```text
Directory question
        ↓
Find the most-specific matching Chutni
        ↓
Check its manifest for filesystem changes
        ↓
Incrementally update added, changed, renamed, or removed files
        ↓
Retrieve useful, cited local memory
        ↓
Answer, or ignore Chutni if it is not useful
```

A normal refresh is incremental. A complete rebuild happens only when the
schema or parser contract changes, the index is damaged, or the user explicitly
requests it.

---

## 2. Decisions locked for v1

Do not reopen these decisions during implementation without owner approval.

1. **Browser only.** The supported product remains a browser UI served by the
   local Samosa gateway. There is no DMG, Mac App Store package, Electron app,
   Swift/AppKit window shell, launch-at-login daemon, or separately installed
   operating-system service in this program. Gateway-managed C sidecars remain
   part of the local browser runtime.
2. **Local first.** Profile, chats, models, indexes, extracted text, summaries,
   and citations stay on the user's computer.
3. **No hidden scanning.** A Chutni scope is created only after the user selects
   and confirms it.
4. **Read-only source access.** Chutni never edits, moves, renames, or deletes
   source files. Forgetting a Chutni removes only Samosa's derived memory.
5. **No symlink traversal.** Chutni must not follow symlinks or escape a
   selected root through aliases or path tricks.
6. **Incremental freshness is the default.** Full rebuild is exceptional.
7. **Lexical retrieval first.** V1 uses SQLite FTS5/BM25 over paths, chunks,
   memory cards, and directory summaries. It does not claim embeddings or
   vector search.
8. **Raw evidence is retained.** Chutni keeps provenance-bearing extracted
   chunks. Model-written cards and summaries are aids, never the only memory.
9. **Useful-or-nothing retrieval.** Irrelevant Chutni results are not injected
   into a prompt merely because an index exists.
10. **Honest status.** Unknown progress is shown as indeterminate. Stale or
    partially checked memory is identified as such. The UI never invents a
    percentage, ETA, citation, or “up to date” claim.
11. **One active model process.** Model switching may unload the current model.
    Chats are bound to the model that created them.
12. **No runtime web framework.** Keep the frontend dependency-free and
    same-origin. Splitting HTML, CSS, and JavaScript into static files is
    allowed only if the gateway, packaging, CSP, and tests are updated
    together; a build system is not required for the app to run.
13. **Closing the tab is not cancellation.** Model downloads and Chutni builds
    are server-owned durable jobs. They continue or pause according to policy
    and can reconnect after the browser is reopened.
14. **Interactive work wins.** Chutni's model-summary work yields while the
    user is chatting or while the machine is under resource pressure.
15. **Gigatoken is the bulk tokenizer, not the memory.** Chutni pins the
    audited Gigatoken source, removes its Python and network runtime paths,
    and uses a bundled native adapter for exact tokenization, chunk budgeting,
    and eligible pretokenized Qwen ingestion. Inventory, extraction, SQLite,
    freshness, retrieval, and citations remain Chutni responsibilities.
16. **Finite, bounded model ingestion.** Gigatoken output is consumed under
    explicit byte, token, context, memory, and cancellation bounds. Tokenizer
    throughput and model-prefill throughput are reported separately. No UI or
    documentation may imply that tokenization speed lets an entire drive fit
    into the model context.
17. **Pretokenized input is capability-gated.** A backend may accept
    Gigatoken token IDs only after exact parity is proven for the immutable
    tokenizer artifact and prompt template. A fingerprint mismatch, unsupported
    backend, or failed gate uses the bounded text path; it never guesses that
    token IDs are compatible.

---

## 3. Existing foundation and known blockers

The implementation should reuse the repository's working pieces rather than
replace them wholesale:

- `assets/app.html` is the current dependency-free browser application.
- `docs/UI_DESIGN.md` defines the visual language: warm flat paper, hairlines,
  one orange accent, a small radius/type scale, restrained motion, SVG icons,
  and explicit local status.
- `src/samosa_gateway.c` already serves the app and contains backend selection,
  durable Jobs machinery, event logs, checkpoints, and a primitive job skim.
- `src/samosa_fs.c` already contains safe file opening, recursive traversal,
  metadata, hashing, type detection, and no-follow checks.
- `src/read_cache.h`, `src/samosa_extract.c`, and `src/samosa_ocr.c` already
  provide content-addressed reading, document extraction, OCR, and provenance.
- `src/tok.h` and `qwen36b tokenize --count` already provide the exact current
  Qwen tokenizer and form the compatibility oracle for Gigatoken parity.
- `src/qwen36b.c` already prefills from token arrays internally and has bounded
  prefill cancellation. That path can be factored into a trusted
  pretokenized-ingestion surface after the T0.5 parity and fingerprint gates.
- `src/qwen36b.c` already has background-yield behavior that Chutni can reuse.

The following are blockers, not optional cleanup:

- The gateway currently refuses to start when no model is installed. Onboarding
  cannot work until the control plane can run without a model.
- The installer currently treats a large Qwen payload and the runtime as one
  release. Model packages must become independently installable.
- Model choices are hardcoded in the frontend. There is no install, download,
  verification, pause, resume, repair, or removal API.
- The current scanner materializes and sorts the complete traversal, hashes too
  much content, loses later paths when bytes are duplicated, and handles some
  unreadable descendants too broadly. That is unsuitable for drive-scale work.
- Capped-prefix hashes cannot prove that the un-hashed portion of a large file
  is unchanged.
- The document caller can request a page batch larger than the extractor's
  supported per-call bound. The caller must page correctly.
- The read cache needs explicit writer locking, parser fingerprints, bounded
  cleanup, and protection against a file changing between hash and extraction.
- There is no scope registry, file manifest, deletion/rename reconciliation,
  lexical index, Chutni API, or chat-directory context.
- Browser file inputs cannot reliably give the local gateway a stable absolute
  directory path. Browser mode therefore needs a safe, server-powered directory
  chooser plus a typed-path headless API.
- Gigatoken upstream is a Python-first PyO3 package built with nightly Rust,
  a process-global Rayon pool, a broad dependency graph, and optional
  Hugging Face network loading. It has no stable C ABI, offset-map API,
  cooperative cancellation callback, Chutni job protocol, or production
  ingestion CLI. Samosa therefore needs a pinned, minimized, precompiled
  adapter; the user must not need Python, Rust, Cargo, or Homebrew.
- The existing `POST /v1/chat/prefill` route only counts tokens and returns a
  placeholder status. It does not construct or retain model KV state. Chutni
  cannot claim fast model ingestion until a real internal bounded
  pretokenized-prefill path exists and passes the T4.4 acceptance gates.

---

## 4. User experience contract

### 4.1 Name screen

- Full-screen setup view with no sidebar or application chrome.
- Heading: **“What should Samosa call you?”**
- One text field, one primary **Continue** button.
- Supporting copy: **“Your name stays on this computer.”**
- A name is required, trimmed, valid UTF-8, and limited to 80 Unicode scalar
  values and 256 UTF-8 bytes.
- Names are always rendered as text, never interpolated as HTML.
- The name is UI personalization by default. It is not silently inserted into
  model prompts.

### 4.2 Welcome screen

- Heading: **“Welcome, <name>.”**
- Brief copy explains:
  - models run locally;
  - chats and Chutni remain local;
  - the next step downloads a model;
  - model downloads can be large and take time.
- The screen has **Choose a model** and **Back** actions.
- It is a real setup step, not a timed splash screen.

### 4.3 Model chooser and download

Every model card is generated from the server catalog and shows:

- name and factual purpose;
- model version;
- download bytes and installed bytes;
- required free space;
- text/image capabilities;
- compatibility with the current machine;
- license and required attribution link;
- one measured recommendation, when the machine facts justify it;
- current state: unavailable, not installed, queued, downloading, paused,
  verifying, installing, ready, or failed.

Download UI shows exact completed and total bytes when the server knows both,
the current artifact, verification state, and pause/resume/cancel/retry actions.
Refreshing or closing the browser does not lose the job.

The user enters Chat only after one model is verified and ready. Existing
installations skip downloading and can select the valid installed model.

### 4.4 Main application

- Sidebar order: Chat, Chutni, Jobs.
- Conversation creation and history appear beneath the navigation.
- Active model and local-runtime status appear at the bottom.
- The primary header does not contain a destructive **Kill** action. Process
  termination belongs in a clearly labeled Troubleshooting surface.
- The empty chat screen says **“Welcome, <name>”** and offers a few restrained
  examples.
- The composer `+` menu contains Image, Document, Web page, and Directory.
- An active directory is visible as a removable context chip above the
  composer. Samosa never guesses an active directory solely from conversational
  wording when filesystem access is required.
- Telemetry remains a stable, single-line local status bar with tabular
  numbers.
- A conversation records the model ID and version that created it. Reopening a
  conversation under a different active model requires an explicit switch or a
  new conversation.

### 4.5 Chutni library

The Chutni view contains:

- an **Add Chutni** action;
- a list of scopes showing display name, root, kind, state, last successful
  check, file counts, indexed coverage, memory size, and skipped items;
- scope detail with exclusions, supported/unsupported counts, recent errors,
  build/refresh progress, and management actions;
- pause, resume, stop, refresh, rebuild, copy-path/open-in-chooser, and
  forget-memory actions when applicable. Browser v1 does not promise to open
  Finder, Explorer, or another native file manager.

Visible states are plain language:

- Learning
- Ready
- Checking for changes
- Updating
- Paused while chatting
- Paused by user
- Needs permission
- Drive disconnected
- Needs attention
- Rebuilding

Learning and Updating show the real subphase—Reading files, Extracting text,
Tokenizing, Publishing memory, or Improving summaries—without turning those
subphases into contradictory top-level states. Tokenizing means Gigatoken is
forming exact model-ready tokens; it does not claim that model prefill or
evidence publication is complete.

Forgetting requires confirmation with this exact promise:

> This removes Samosa's memory. Your files are not changed.

### 4.6 Add-Chutni flow

1. Choose Folder, Drive, or This computer.
2. Select a root using the server-powered directory chooser or enter a path.
3. Show the canonical root and a preflight summary.
4. Show default exclusions, filesystem-boundary policy, supported file types,
   available disk space, and a clearly labeled estimate when one is possible.
5. Let the user confirm.
6. Create the scope and durable build job.
7. Return to the scope detail view; the build continues if the tab closes.

“This computer” must show the concrete roots it will include. It must never be
a hidden alias for scanning the filesystem root.

---

## 5. Persistence and API contracts

Exact JSON field names below are the v1 contract. Additive fields are allowed;
renaming or removing a field requires a documented API-version change.

Durability depends on the storage type:

- mutable JSON snapshots use `temporary file → fsync file → rename → fsync
  parent` where the platform supports it;
- append-only JSONL writes exactly one complete line under the writer lock and
  fsyncs it before SSE delivery; recovery ignores only a torn final line;
- SQLite uses transactions, WAL/checkpoint rules, integrity checks, and the
  publication protocol in §5.6.

Persistent JSON must include a schema version. Samosa-owned state directories
use mode `0700`; profile, job, event, database, cache, and token files use mode
`0600`, subject to stricter platform equivalents.

### 5.0 Common HTTP, authentication, and error rules

The gateway remains bound to loopback. At each start it creates 32 random bytes,
stores their 64-character lowercase hexadecimal form at
`~/.samosa/run/ui-token` with mode `0600`, and substitutes that value for one
fixed placeholder in the served root HTML. The HTML response is
`Cache-Control: no-store`; its script reads the token from a `<meta>` element
and sends it as `X-Samosa-Token`.

- Every `/v1/*` read or mutation except the minimal `/healthz` probe requires
  this token. This includes profile, model install/select/remove, filesystem,
  Chat, Jobs, and Chutni routes.
- Browser requests must also carry the exact loopback `Origin`. Requests with
  an absent Origin are allowed only with a valid token for headless/CLI use.
- `Host` must resolve to the bound loopback host/port. CORS is not enabled.
- The bundled `samosa` CLI reads the token file automatically. It must not print
  the token in normal output or logs.
- A gateway restart rotates the token. A stale browser receives
  `401 stale_ui_session` and reloads the root document once.
- The root HTML substitution, CSP, response length, and cache headers have
  automated tests. No external script, stylesheet, font, or runtime asset is
  loaded.

New API errors use:

```json
{
  "error": {
    "code": "stable_machine_code",
    "message": "Safe user-facing sentence.",
    "retryable": false,
    "details": {}
  }
}
```

HTTP mapping:

- `400` malformed JSON, field, path, or identifier;
- `401` missing/invalid/stale UI token;
- `403` valid request but denied path or policy;
- `404` unknown resource;
- `409` state, binding, duplicate, writer, or activation conflict;
- `413` request or configured source limit exceeded;
- `422` well-formed but incompatible model/scope request;
- `429` bounded queue full;
- `500` unexpected internal failure;
- `503` temporarily unavailable, paused by system, or backend not ready;
- `507` insufficient destination space.

Timestamps are RFC 3339 UTC with a trailing `Z`. IDs are server-generated
128-bit random lowercase hexadecimal strings unless a client request ID is
explicitly allowed. Display names are valid UTF-8, at most 256 bytes, and are
always returned separately from canonical path bytes.

### 5.1 Profile and setup state

Store the profile at:

```text
~/.samosa/profile.json
```

Minimum shape:

```json
{
  "schema_version": 1,
  "name": "Deepan",
  "onboarding": {
    "welcome_completed_at": "2026-07-25T12:01:00Z",
    "selected_model_id": "qwen",
    "selected_model_version": "immutable-version",
    "active_install_job_id": null,
    "active_selection_operation_id": null
  },
  "created_at": "2026-07-25T12:00:00Z",
  "updated_at": "2026-07-25T12:00:00Z"
}
```

Endpoints:

```text
GET /v1/profile
PUT /v1/profile
GET /v1/setup/status
POST /v1/setup/welcome/complete
```

`GET /v1/setup/status` returns:

```json
{
  "profile_complete": true,
  "installed_model_count": 1,
  "active_model_id": "qwen",
  "active_model_version": "immutable-version",
  "active_model_ready": true,
  "selected_model_id": "qwen",
  "selected_model_version": "immutable-version",
  "active_install_job_id": null,
  "active_selection_operation_id": null,
  "next_step": "chat"
}
```

`next_step` is one of `name`, `welcome`, `model`, `download`, or `chat`.
Welcome completion must be persisted; it cannot depend on a timer or an
in-memory frontend flag. `POST /v1/setup/welcome/complete` is idempotent and
records `welcome_completed_at`; changing the user's name does not reset it.
Starting an install or selecting an installed model persists the selected
model/version. `active_install_job_id` is repaired from the durable job registry
at startup rather than trusted blindly. The same rule applies to
`active_selection_operation_id`.

`next_step` is derived in this order:

1. missing/invalid name → `name`;
2. missing `welcome_completed_at` → `welcome`;
3. a selected model has a nonterminal or recoverable-failed install job →
   `download`;
4. no selected, verified model → `model`;
5. selected model verified but a selection operation is loading, failed, or not
   yet active → `download`;
6. selected model ready → `chat`.

### 5.2 Conversation schema and migration

At minimum, each browser conversation gains:

```json
{
  "id": "stable-id",
  "schema_version": 2,
  "model_id": "qwen",
  "model_version": "immutable-catalog-version",
  "model_binding_source": "explicit",
  "created_at": "ISO-8601",
  "updated_at": "ISO-8601",
  "directory_context": null,
  "messages": []
}
```

The model binding is also stored canonically by the gateway at:

```text
~/.samosa/chats/<conversation-id>/metadata.json
```

When `conversation_id` is present, Chat requests must also include `model_id`
and `model_version`. The gateway validates these fields against canonical
metadata before forwarding the request. A mismatch returns
`409 conversation_model_mismatch`; it can never silently run through the
currently active backend. A conversation binding is create-once. Continuing
with a different model creates a new/forked conversation rather than rewriting
history. OpenAI-compatible stateless requests with no `conversation_id` use the
active model and create no canonical conversation metadata.

Validation and inference admission occur under the same gateway selection lock:
the active backend cannot switch between checking the binding and admitting the
turn. Model selection likewise waits for or rejects Chat, Jobs, scratch
relevance, and Chutni-summary inference using that backend.

Binding endpoints:

```text
GET /v1/conversations/<conversation_id>/binding
PUT /v1/conversations/<conversation_id>/binding
```

`PUT` is idempotent when the requested binding is identical and returns `409`
when a different binding already exists.

Migration rules:

- Read the existing `samosa-chat-v1` browser state.
- Copy it to the new schema without deleting or overwriting the old key until
  the new state has been written and read back successfully.
- Register each legacy binding with the gateway. Existing server metadata wins;
  otherwise infer a binding only when the exact historical model/version is
  unambiguous. Mark it `model_binding_source:"inferred"`.
- If no model is ready, more than one model could have produced the turns, or
  the immutable version cannot be established, use `model_id:null`,
  `model_version:null`, and `model_binding_source:"unknown"`. Do not create a
  false binding.
- Preserve conversation IDs so existing Qwen snapshots remain addressable.
- Migration is idempotent.
- A malformed top-level state or browser-quota failure leaves the original key
  untouched and produces a recoverable export/reset choice.
- Malformed individual conversations are skipped with a recoverable diagnostic;
  one bad record must not erase the rest.

Reopening an unavailable exact binding offers only: install that exact version,
switch to it if installed, fork visible messages into a new conversation under
the current model, or cancel. A fork always receives a new conversation ID and
cannot address the old Qwen snapshot.

Full server-side transcript persistence can be added in this program, but
gateway-enforced binding metadata is required. If transcript persistence is
added, migration must be acknowledged by the server before browser backup data
is removed.

### 5.3 Model catalog and lifecycle

Endpoints:

```text
GET  /v1/models/catalog
POST /v1/models/install
GET  /v1/models/installs
GET  /v1/models/installs/<job_id>
GET  /v1/models/installs/<job_id>/events
POST /v1/models/installs/<job_id>/pause
POST /v1/models/installs/<job_id>/resume
POST /v1/models/installs/<job_id>/cancel
POST /v1/models/installs/<job_id>/retry
POST /v1/models/repair
POST /v1/models/remove
POST /v1/models/select
GET  /v1/models/selections
GET  /v1/models/selections/<operation_id>
```

V1 trusts one catalog bundled with the runtime release. Remote catalog updates
are out of scope until a complete signature, canonicalization, key-rotation,
and rollback-protection contract exists.

Top-level catalog:

```json
{
  "schema_version": 1,
  "catalog_revision": "immutable-release-revision",
  "runtime_abi": "samosa-model-runtime-v1",
  "models": []
}
```

Catalog entries include:

```json
{
  "id": "stable-model-id",
  "version": "immutable-version",
  "preferred_for_backend": true,
  "label": "Human name",
  "description": "Factual one-sentence description.",
  "capabilities": ["text", "image"],
  "backend_kind": "qwen_native",
  "supported_platforms": [
    { "os": "macos", "architecture": "arm64" }
  ],
  "required_runtime_abi": "samosa-model-runtime-v1",
  "minimum_ram_bytes": 0,
  "download_bytes": 0,
  "installed_bytes": 0,
  "required_free_bytes": 0,
  "launch_profile_id": "compiled-trusted-profile",
  "runtime_dependencies": [
    {
      "package_id": "qwen36b-runtime",
      "version": "immutable-version",
      "source": "bundled"
    }
  ],
  "tokenization": {
    "tokenizer_artifact_name": "tokenizer.json",
    "tokenizer_sha256": "hex",
    "vocabulary_size": 0,
    "prompt_template_sha256": "hex",
    "gigatoken_profile": "qwen3_5",
    "direct_token_ingestion_candidate": true
  },
  "license": {
    "name": "license-name",
    "url": "https://license-source"
  },
  "artifacts": [
    {
      "name": "file-name",
      "role": "weights",
      "required": true,
      "url": "https://approved-host/path",
      "install_path": "weights/file-name",
      "file_mode": "0600",
      "bytes": 0,
      "sha256": "hex"
    },
    {
      "name": "tokenizer.json",
      "role": "tokenizer",
      "required": true,
      "url": "https://approved-host/path",
      "install_path": "tokenizer/tokenizer.json",
      "file_mode": "0600",
      "bytes": 0,
      "sha256": "hex"
    }
  ],
  "compatible": true,
  "compatibility_reason": null,
  "install_state": "not_installed",
  "active": false
}
```

Every artifact has an exact size and SHA-256. `install_path` is a validated
relative path: absolute paths, empty components, `.`/`..`, duplicate targets,
case-folded collisions on a case-insensitive destination, and symlinks are
rejected before any network or filesystem mutation.

`runtime_dependencies` represents executables or shared runtimes required to
launch the model, including the shared `llama-server` used by multiple GGUF
models. `artifacts.role` distinguishes weights, tokenizer, configuration,
vision projector, and other required data. Optional capability artifacts, such
as a vision projector, must not be mistaken for the minimum text install.
`launch_profile_id` selects a launch recipe compiled into the trusted runtime;
downloaded catalog data must not inject arbitrary executable paths or command
arguments. `backend_kind` is a closed enum initially containing `qwen_native`
and `llama_cpp`; unknown values are rejected.

`tokenization` is required for a model that participates in exact Chutni
budgeting. It names the locally verified tokenizer artifact and immutable
template/vocabulary facts; it never contains a repository ID. The catalog may
declare a direct-ingestion *candidate*, but installed status reports
`direct_token_ingestion_state` as `unsupported`, `unverified`, `verified`, or
`failed`. Only T0.5/T4.4 runtime proof can produce `verified`; catalog text
alone cannot.

Every v1 runtime dependency has `source:"bundled"` and is installed/upgraded by
T1.0 on each supported target. The model manager downloads data artifacts only;
there is no separate executable-runtime downloader in v1. Exactly one catalog
version per stable model/backend ID is marked `preferred_for_backend`.

Conditional capabilities name their required optional artifacts. For example,
image capability is Ready only when the catalog's vision-projector artifact
verifies; the minimum text package may still be Ready without it.

Install request and creation response:

```json
{ "model_id": "stable-model-id", "version": "immutable-version" }
```

```json
{
  "job_id": "random-id",
  "model_id": "stable-model-id",
  "version": "immutable-version",
  "state": "queued",
  "status_url": "/v1/models/installs/random-id",
  "events_url": "/v1/models/installs/random-id/events"
}
```

One artifact transfer runs at a time; additional distinct installs may wait in
FIFO `queued` state. A duplicate install request for the same model/version
returns the existing nonterminal job instead of creating another. A
`client_request_id` may be supplied and is idempotent.

Legal install transitions:

```text
queued → downloading → verifying → installing → installed
queued|downloading → paused → queued
queued|downloading|paused|verifying → canceled
any nonterminal state → failed
failed|canceled → retry creates a new job that may reuse safe retained bytes
```

Install job status includes:

```json
{
  "job_id": "random-id",
  "model_id": "stable-model-id",
  "version": "immutable-version",
  "state": "downloading",
  "completed_bytes": 123,
  "total_bytes": 456,
  "current_artifact": "weights",
  "current_artifact_completed_bytes": 123,
  "current_artifact_total_bytes": 456,
  "retained_partial_bytes": 0,
  "partial_expires_at": null,
  "error": null,
  "created_at": "ISO-8601",
  "updated_at": "ISO-8601"
}
```

`GET /v1/models/installs` returns `{ "active_transfer_job_id": null,
"jobs": [...] }`, including all nonterminal jobs and recent terminal jobs
needed for setup recovery. Pause/resume/cancel return the updated job. Retry
returns `201` with a new job ID and `reused_bytes`. Repair accepts
`{ "model_id", "version", "client_request_id" }`. Remove accepts
`{ "model_id", "version", "confirm_bound": false }`.

Immediately before publication, the worker checks cancellation and enters the
noncancelable `installing` commit phase. From that point, cancel returns
`409 invalid_state`; the atomic directory/registry commit finishes or rolls
back and can never report Canceled after a successful publication.

Cancel stops earlier work and retains verified or resumable partial bytes for
seven days; the response states the retained byte count and expiry. Repair creates a
verification/redownload job for an installed package. Removal is denied while
the exact package is active. Removing a package referenced by conversations
requires `confirm_bound:true` and leaves those chats visible but unavailable
until the exact version is reinstalled. A legacy package can be unregistered
but is never moved, modified, or deleted by Samosa.

`required_free_bytes` is calculated on the actual model destination and
includes remaining download bytes, verification/publication amplification,
required runtime dependencies, and a 2 GiB safety reserve. Recommendations
never start a download automatically.

Model storage:

```text
~/.samosa/models/<model-id>/.partial/<job-id>/
~/.samosa/models/<model-id>/<version>/
~/.samosa/models/registry.json
```

Activation occurs only after every required artifact verifies. A failed
download never replaces a working model. Existing legacy model locations are
detected and registered by resolved immutable path without copying
multi-gigabyte files. Registry entries record model/version, source
(`managed` or `legacy`), resolved path, package-manifest hash, validation
state, file identities, sizes, and mtimes. Never register a legacy package
through a movable `current` symlink.

Legacy discovery performs a fast identity/size/mtime check at startup, then a
visible one-time background full validation against exact catalog hashes.
Subsequent starts use the stored identity tuple; **Deep verify** rehashes all
bytes. Only an exact catalog match becomes Ready.

Model selection is asynchronous:

```json
{ "model_id": "stable-model-id", "version": "immutable-version" }
```

returns `202` with:

```json
{
  "operation_id": "random-id",
  "state": "loading",
  "status_url": "/v1/models/selections/random-id"
}
```

Selection states are `queued`, `loading`, `ready`, and `failed`. Status also
returns `model_id`, `model_version`, `package_manifest_sha256`, timestamps, and
the verified tokenizer SHA-256, prompt-template fingerprint,
`direct_token_ingestion_state`, and the shared error envelope when failed.
Those fingerprint fields are server evidence, not client assertions.
`GET /v1/models/selections` returns the active nonterminal operation plus
recent terminal operations. Duplicate select requests for the same exact
model/version return the existing operation. Startup repairs
`active_selection_operation_id` from this durable registry.

The gateway persists the new active selection only after readiness succeeds.
`/healthz` and selection status expose `backend_state`, `model_id`,
`model_version`, `package_manifest_sha256`, tokenizer/template fingerprints,
direct-token-ingestion state, and the trusted launch-profile ID.
For `llama_cpp`, readiness means the expected process is responsive and its
verified package path/model alias match the requested package. Fingerprint or
readiness failure restores the prior working backend.

The existing `GET /v1/backends` and `POST /v1/backends/select` remain
compatibility routes during migration and delegate to the same registry and
selection operation. A legacy backend ID selects its one catalog entry marked
`preferred_for_backend`; no Ready preferred version returns `409 unavailable`,
and ambiguous preferred entries invalidate the catalog. Removal requires a
separate versioned deprecation.

### 5.4 Browser directory chooser

Endpoints:

```text
GET /v1/fs/roots
GET /v1/fs/directories?path=<encoded-canonical-path>
```

`GET /v1/fs/roots` returns:

```json
{
  "roots": [
    {
      "chooser_root_id": "stable-for-this-gateway-start",
      "label": "Home",
      "path": "/Users/example",
      "kind": "home",
      "volume_identity": "platform-stable-opaque-value",
      "readable": true,
      "connected": true
    }
  ]
}
```

`GET /v1/fs/directories` returns:

```json
{
  "path": "/Users/example/Documents",
  "chooser_root_id": "id",
  "parent": "/Users/example",
  "directories": [
    {
      "name": "Research",
      "path": "/Users/example/Documents/Research",
      "readable": true
    }
  ]
}
```

Rules:

- Return directories only; do not leak file contents or filenames that the
  chooser does not need.
- Start from explicit safe roots such as the user's home and connected volumes.
- Canonicalize every request and reject traversal outside an allowed chooser
  root.
- Do not follow symlinks.
- Hidden and protected directories are excluded by default.
- A typed absolute path remains available for headless use and is subjected to
  the same canonicalization and Chutni policy checks.
- Filesystem and Chutni endpoints require a per-launch UI token injected into
  the served page and sent in a custom request header. Reject invalid
  Host/Origin values and continue binding only to loopback.

The chooser grants no lasting permission by itself. Creating the scope is the
explicit authorization step.

### 5.5 Chutni scope API

Endpoints:

```text
GET  /v1/chutni/scopes
POST /v1/chutni/preflight
POST /v1/chutni/scopes
GET  /v1/chutni/scopes/<scope_id>
POST /v1/chutni/scopes/<scope_id>/build
POST /v1/chutni/scopes/<scope_id>/refresh
POST /v1/chutni/scopes/<scope_id>/rebuild
POST /v1/chutni/scopes/<scope_id>/pause
POST /v1/chutni/scopes/<scope_id>/resume
POST /v1/chutni/scopes/<scope_id>/cancel
GET  /v1/chutni/scopes/<scope_id>/events
POST /v1/chutni/query
POST /v1/chutni/scopes/<scope_id>/forget
```

Scope kinds are exactly `folder`, `drive`, and `computer`. The schema always
uses `roots[]`: folder and drive have exactly one root; computer has one or
more visible roots.

Preflight request:

```json
{
  "client_request_id": "caller-random-id",
  "kind": "folder",
  "roots": [
    { "path": "/user-selected/absolute/path" }
  ],
  "policy": {
    "cross_filesystems": false,
    "include_hidden": false,
    "maximum_file_bytes": 268435456,
    "exclusions": []
  }
}
```

Preflight returns a ten-minute, single-configuration token:

```json
{
  "preflight_id": "random-id",
  "expires_at": "ISO-8601",
  "kind": "folder",
  "roots": [
    {
      "root_id": "random-id",
      "path": "/canonical/absolute/path",
      "volume_identity": "opaque-platform-value",
      "root_file_identity": "opaque-platform-value",
      "readable": true,
      "connected": true
    }
  ],
  "effective_policy": {
    "policy_fingerprint": "sha256",
    "cross_filesystems": false,
    "include_hidden": false,
    "maximum_file_bytes": 268435456,
    "mandatory_exclusions": [],
    "user_exclusions": []
  },
  "estimate": {
    "sampled": true,
    "files": 100,
    "source_bytes": 1234,
    "peak_additional_disk_bytes": 5678,
    "available_disk_bytes": 9999,
    "confidence": "low"
  },
  "warnings": []
}
```

Estimates may be `null`; their confidence is `low`, `medium`, or `high` and the
UI labels them as estimates. The server returns its effective policy, including
mandatory exclusions the client cannot remove.

Creating the scope accepts:

```json
{
  "client_request_id": "same-caller-random-id",
  "preflight_id": "random-id",
  "display_name": "Research"
}
```

Creation atomically records the scope and starts generation 1, returning
`201 { "scope": {...}, "job": {...} }`. Retrying the same
`client_request_id` returns the same scope/job. The same canonical roots and
volume identities under a different request return `409 scope_exists` with the
existing scope ID. Nested scopes are allowed; the most-specific segment-aware
canonical root wins. `POST .../build` is valid only for an `unbuilt`,
`failed_initial`, or `canceled_initial` scope and starts a new generation-1
job.

A scope summary includes:

```json
{
  "id": "random-stable-id",
  "schema_version": 1,
  "kind": "folder",
  "roots": [
    {
      "root_id": "random-id",
      "path": "/canonical/absolute/path",
      "volume_identity": "opaque-platform-value",
      "root_file_identity": "opaque-platform-value",
      "state": "connected"
    }
  ],
  "display_name": "Research",
  "state": "ready",
  "freshness_state": "complete",
  "evidence_generation": 7,
  "enhancement_revision": 3,
  "current_job_id": null,
  "last_successful_build_at": "ISO-8601",
  "last_successful_check_at": "ISO-8601",
  "regular_files_seen": 100,
  "files_indexed": 80,
  "files_skipped": 20,
  "chunks_indexed": 420,
  "source_bytes_indexed": 1234,
  "extracted_text_bytes": 1000,
  "tokenizer": {
    "engine": "gigatoken",
    "engine_version": "0.10.0",
    "engine_commit": "34a1599f0c0ae7d7cd0d1c530e6522320158b360",
    "tokenizer_sha256": "hex",
    "chunker_fingerprint": "hex"
  },
  "enhancement_state": "improving",
  "chunks_total": 420,
  "chunks_summarized": 180,
  "summary_tokens_pending": 96000,
  "index_bytes": 5678,
  "warnings": []
}
```

`files_indexed + files_skipped == regular_files_seen`. Directories and
mandatory-excluded descendant names are not counted as seen files.
`index_bytes` includes the active database, WAL/SHM, cards, and summaries but
not the shared extraction cache. Warnings are objects with `code`, `message`,
`root_id`, and an optional aggregate `count`; they are not free-form strings.
`enhancement_state` is `not_started`, `pending_model`, `improving`, `complete`,
`paused_chat`, or `failed`. Evidence can be Ready while enhancements remain
incomplete. Tokenization progress and model-summary progress are never blended
into one percentage or ETA.

Scope states are:

```text
unbuilt | building | ready | checking | updating | rebuilding |
paused_user | paused_chat | needs_permission | disconnected |
needs_attention | failed_initial | canceled_initial | forgetting
```

Freshness states are:

```text
unchecked | complete | partial | failed | stale
```

Known excluded/unsupported files do not make a completed traversal partial.
An unenumerated subtree, volume loss, permission failure, unstable source, or
other gap makes it `partial` or `failed`; neither may display **Checked now**.
The scope summary's `last_successful_check_at` advances only after a complete
full-scope walk. Complete contextual-subtree checks are stored separately by
root ID plus normalized boundary and appear as `checked_at` only in that query/
context response.

Build/refresh/rebuild returns `202` with a durable job object. Pause, resume,
and cancel require `{ "job_id": "current-job-id" }`; a stale job ID returns
`409 job_changed`. One writer job is active per scope. Repeated identical
actions are idempotent. Forget requires:

```json
{
  "job_id": "random-id",
  "scope_id": "scope-id",
  "kind": "build",
  "state": "queued",
  "phase": "inventory",
  "evidence_generation_target": 1,
  "status_url": "/v1/chutni/scopes/scope-id",
  "events_url": "/v1/chutni/scopes/scope-id/events?job_id=random-id"
}
```

Event replay requires `job_id` and accepts `after=<seq>` or SSE
`Last-Event-ID` under §5.7. Refresh targets the current evidence generation + 1;
rebuild stages a complete replacement with the same target rule.

Chutni job phases are:

```text
preflight | inventory | hashing | extracting | tokenizing |
indexing | validating | publishing | summarizing | finalizing
```

`tokenizing` means normalized extracted text is being converted into exact
model-token counts/IDs. It does not mean evidence is published or that the
model has prefetched it. The scope becomes Ready only after `publishing`;
`summarizing` may continue as a separate enhancement revision.

Forget requires:

```json
{ "confirm": true, "client_request_id": "caller-random-id" }
```

and returns `202` while crash-safe cleanup runs.

Chutni query request:

```json
{
  "query": "Which report mentions the renewal date?",
  "directory_context": {
    "scope_id": "optional-explicit-id",
    "root_id": "root-id",
    "relative_directory": "contracts/2026"
  },
  "freshness": "require_current",
  "maximum_results": 20,
  "maximum_context_tokens": 4096
}
```

`freshness` is `require_current` or `allow_previous_once`. Query response:

```json
{
  "scope_id": "id",
  "root_id": "id",
  "relative_directory": "contracts/2026",
  "evidence_generation": 7,
  "enhancement_revision": 3,
  "checked_at": "ISO-8601",
  "freshness_state": "complete",
  "used": true,
  "reason_code": "useful_evidence",
  "results": [
    {
      "chunk_id": "deterministic-id",
      "rank": 1,
      "citation": {
        "citation_id": "deterministic-id",
        "scope_id": "id",
        "root_id": "id",
        "evidence_generation": 7,
        "file_id": "id",
        "content_sha256": "hex",
        "relative_path": "renewal.pdf",
        "relative_path_bytes_base64": null,
        "page": 2,
        "section": null,
        "offset_unit": "utf8_bytes",
        "start_offset": 120,
        "end_offset": 380,
        "excerpt_sha256": "hex"
      },
      "text": "bounded source excerpt"
    }
  ]
}
```

`used:false` returns an empty `results` array and a stable reason such as
`no_scope`, `scope_not_ready`, `freshness_incomplete`, `no_candidates`,
`not_relevant`, or `relevance_check_failed`. Page numbers are one-based.
Offsets address the exact normalized UTF-8 text stored for that content.
Non-UTF-8 path bytes use the Base64 field plus an escaped display path.

Stable error codes include:

```text
scope_not_found
path_outside_scope
path_denied
permission_required
volume_disconnected
index_busy
index_corrupt
model_required
tokenization_failed
tokenizer_asset_missing
tokenizer_mismatch
tokenizer_unsupported
token_frame_invalid
model_ingestion_unsupported
unsupported_file
insufficient_space
job_not_found
invalid_state
scope_exists
job_changed
scope_limit_exceeded
changed_during_read
```

### 5.6 Chutni ingestion and storage

#### 5.6.1 Gigatoken adapter boundary

Pin Gigatoken v0.10.0 at
`34a1599f0c0ae7d7cd0d1c530e6522320158b360`. Record the upstream source URL,
commit, unmodified MIT license, Samosa patch set, build toolchain, target, and
complete transitive SBOM in release evidence. The pin changes only through a
reviewed dependency update.

Production Samosa ships a precompiled `samosa-gigatoken` child process built
from a minimized, local-only Rust adapter. It must:

- require no Python, NumPy, Awkward, Typer, Rust, Cargo, or nightly toolchain on
  the user's machine;
- remove or compile out Hugging Face Hub lookup, `ureq`/network access,
  training, benchmarks, Parquet/Arrow, compression, and Python bindings unless
  a measured required path proves otherwise;
- load only a model-manager-verified local `tokenizer.json` whose real path,
  length, and SHA-256 match the immutable model/catalog manifest;
- receive already extracted and validated UTF-8 over private inherited pipes;
  it never receives a Chutni root, recursively walks source paths, opens
  arbitrary user files, or writes a shared Hugging Face cache;
- run as a supervised, low-priority child with a fixed thread budget, bounded
  input/output frames, bounded pretoken cache, and no inherited network
  capability;
- return machine-readable errors and deterministic results for identical
  bytes, tokenizer fingerprint, special-token policy, and adapter build; and
- check cancellation between bounded frames. The gateway may terminate and
  restart a stuck child without losing the last published evidence generation.

Use a versioned length-prefixed binary protocol rather than JSON arrays of
millions of token IDs. Every request binds:

```text
protocol version
request ID
operation
Gigatoken build commit
model ID and immutable version
tokenizer SHA-256 and vocabulary size
prompt-template/special-token policy fingerprint
source-content SHA-256
document count, byte lengths, and total byte/token ceilings
```

The required operations are `health`, `encode_batch`, `encode_prompt`,
`cancel`, and `shutdown`. `encode_batch` returns one flat little-endian `u32`
token buffer plus per-document token lengths and always treats source text as
untrusted evidence. `encode_prompt` accepts the gateway's typed trusted-template
and untrusted-evidence segments and returns one complete bounded prompt token
sequence. No production operation accepts a repository ID or URL. Start with a
maximum 8 MiB input frame and lower it if the measured two-second cancellation,
RSS, or Chat-admission gates require it. Never place a complete drive-sized
token array in memory.

There are two distinct consumers:

1. **Evidence chunking.** The provenance-aware reader first produces bounded
   page/section/paragraph spans with exact UTF-8 byte offsets. Gigatoken
   tokenizes those spans in batches; the chunker combines complete spans up to
   its target. Gigatoken v0.10.0 does not expose source offset maps, so it does
   not own provenance boundaries. An oversized span is split at UTF-8-safe
   candidate boundaries and re-counted until it fits. Arbitrary independently
   encoded byte slabs must not be concatenated because BPE merges can cross a
   slab boundary.
2. **Bounded model ingestion.** The gateway constructs the complete logical
   prompt as typed trusted-template and untrusted-evidence segments and calls
   `encode_prompt` once. For the native Qwen backend, a private engine call may
   pass those IDs directly into the existing bounded prefill/generation
   machinery, eliminating a second `tok_encode` pass. The public browser API
   never accepts raw token IDs. Other backends use the same exact Gigatoken
   budgeting where compatible and otherwise receive bounded text through their
   native tokenizer.

Untrusted document text and trusted chat-template/control text are different
protocol segment types. Document text cannot create role, image, end-of-turn,
or other control tokens merely by containing their printed spelling. Only the
gateway creates trusted control segments. Segment concatenation is allowed
only across tokenizer-proven safe/special-token boundaries and is covered by
full-prompt parity fixtures.

Direct token-ID ingestion is enabled for one immutable backend package only
when all of the following match the activated proof:

```text
model package manifest SHA-256
tokenizer artifact SHA-256
vocabulary size and special-token map
prompt-template fingerprint
Gigatoken commit and Samosa adapter patch fingerprint
token-parity corpus version
```

The engine validates frame length, every token ID against the embedding
vocabulary, cumulative context arithmetic, reserved output space, and the
active memory-fit check before allocating or prefilling. Mismatch returns
`tokenizer_mismatch` and uses the bounded text path; it never partially
prefills or treats incompatible IDs as text.

The canonical Chutni v1 chunk profile uses the verified Qwen3.6 tokenizer
artifact and a versioned 600–800-token policy. Persist text, byte provenance,
exact token counts, and fingerprints—not corpus-wide token-ID blobs. A
different active model receives fresh exact prompt budgeting; if one stored
chunk is too large under that tokenizer, it is deterministically subdivided
for that request without changing its citation. Switching models does not
silently reinterpret a count produced by another tokenizer.

Gigatoken activation requires exact differential parity against `src/tok.h`
for the shipped Qwen tokenizer over templates, control tokens, multilingual
Unicode and normalization, code, whitespace, long documents, random data, and
adversarial boundary cases. Family-name support or an upstream benchmark is
not proof. If parity is incomplete, Samosa still uses Gigatoken only where the
proven special-token-disabled evidence-counting profile is exact and retains
`tok_encode` for model input.

#### 5.6.2 Durable Chutni storage

Use a vendored SQLite build with FTS5 compiled into a dependency-free C
sidecar. The user must not need Homebrew, Python, or a system SQLite package at
runtime.

Storage layout:

```text
~/.samosa/chutni/
  scopes.json
  scopes/<scope-id>/
    scope.json
    active.json
    index.sqlite3
    state.json
    events.jsonl
    checkpoints/
    forgetting.json
```

Sources of truth and recovery precedence:

1. The committed SQLite metadata row is authoritative for published
   `evidence_generation` and `enhancement_revision`.
2. `scope.json` is the authoritative confirmed roots and effective policy.
3. `events.jsonl` is the durable history of a job.
4. `state.json` and `scopes.json` are rebuildable lookup/UI snapshots. On
   disagreement they are reconstructed from scope directories, SQLite
   metadata, and terminal job events.
5. `active.json` identifies the validated database file after a full
   build/rebuild. It is atomically replaced only after the new database and its
   parent directory are durable.

For a full build or rebuild, create a uniquely named staging database, run
`quick_check`, an FTS integrity check, foreign-key validation, and required
queries, checkpoint/remove its WAL, fsync it, then atomically update
`active.json`. Retain the previous database until all pinned readers close and
the new state snapshot is durable.

For a normal incremental refresh, use one WAL transaction against the active
database. Queries start and hold one SQLite read transaction from retrieval
through prompt construction, thereby pinning one evidence generation. Configure
foreign keys on, a bounded busy timeout, and explicit WAL checkpoint policy.
If estimated WAL/copy amplification exceeds the available-space calculation or
the measured large-update threshold, build a staging database and use the full
publication path instead. Cancellation happens between bounded batches before
the publication transaction; it never exposes a partial logical diff.

Minimum logical tables:

- `metadata`: schema, reader, OCR, canonical chunker, Gigatoken
  commit/adapter, tokenizer artifact, summarizer, and prompt fingerprints;
- `tokenizer_profiles`: immutable model/version, tokenizer SHA-256, vocabulary
  size, special-token policy, prompt-template fingerprint, parity-corpus
  version, and direct-ingestion capability;
- `files`: relative path, stable file identity when available, size,
  nanosecond mtime, content hash, type, status, skip reason, generation;
- `contents`: content hash and content-addressed extraction reference;
- `chunks`: file ID, deterministic ordinal, page/section/offset provenance,
  text, exact canonical token count, tokenizer/chunker fingerprint, generation;
- `memory_cards`: short query-independent statements with source chunk IDs,
  generator model ID, and prompt version;
- `directory_summaries`: relative directory, bounded summary, source IDs,
  generator model ID, and prompt version;
- one or more FTS5 tables covering relative path, title, chunks, cards, and
  summaries.

Duplicate bytes at different paths remain separate `files` rows while sharing a
`contents` record. Chutni must never discard a path merely because another file
has the same hash.

Token IDs are transient between the adapter and a bounded model call in v1.
Persisting corpus-wide token blobs is forbidden until a later measured design
defines their storage cost, invalidation, encryption/permissions, and garbage
collection. Fast re-tokenization plus immutable counts is preferred to making
the evidence database model-format-dependent.

`evidence_generation` changes only when paths, file state, extracted chunks, or
their freshness changes. Paths/chunks publish first. Later cards and summaries
commit as `enhancement_revision` updates tied to that exact evidence generation.
A late worker rechecks generation and source hashes immediately before commit;
if either changed, its output is discarded and rescheduled.

Cards/summaries have foreign-key source links. A source change or deletion
invalidates all dependent derived rows in the same evidence transaction.
Retrieval of a card/summary expands at least one current raw source chunk into
the prompt; generated memory is never the sole evidence.

The shared read cache maintains durable owner references from scopes and active
jobs. Forgetting a scope removes its references; crash-safe garbage collection
deletes extraction/OCR entries once no scope or job references them. Forget
deletes the scope database, WAL/SHM files, events, checkpoints, cards,
summaries, and state. A `forgetting.json` tombstone makes cleanup resume after a
crash. Shared content remains only when another live owner still references it.

### 5.7 Durable job events

Model downloads and Chutni builds use the same event guarantees:

- each event receives a monotonically increasing sequence number scoped to one
  job and allocated under that job's writer lock;
- the event is persisted before it is sent over SSE;
- reconnect accepts `Last-Event-ID` or an explicit `after` sequence; `after`
  takes precedence when both are supplied;
- state can be reconstructed without an open browser;
- a crash leaves the last complete model/index active;
- pause, resume, and cancel are idempotent.

Minimum event shape:

```json
{
  "seq": 42,
  "time": "ISO-8601",
  "job_id": "stable-id",
  "kind": "chutni_build",
  "state": "running",
  "phase": "extracting",
  "completed": 120,
  "total": 300,
  "unit": "files",
  "current_item": "relative/path.pdf",
  "message": "Reading documents"
}
```

`total` may be `null`. If it is unknown, the UI uses an indeterminate state.
Logs and diagnostics must not contain extracted content, credentials, or
unredacted secrets.

Job states are `queued`, `running`, `paused_user`, `paused_chat`, `canceling`,
`canceled`, `failed`, and `completed`. Every job has exactly one durable
terminal event. Recovery ignores a torn final JSONL line, verifies that
sequences are contiguous through the last valid record, and rebuilds
`state.json`. An `after` value older than retained detail receives the latest
snapshot followed by newer events; a value beyond the last sequence returns
`409 event_cursor_ahead`.

Progress is coalesced to at most ten persisted UI progress events per second and
at least one event per checkpoint/phase transition. Event compaction writes a
durable state snapshot plus terminal/diagnostic landmarks before removing old
progress detail. Replaying a compacted job must yield the same current state as
replaying the original log.

### 5.8 Chat context and attachment contract

The browser never performs Chutni retrieval and then concatenates arbitrary
text into a user message. The gateway owns freshness, retrieval, relevance
gating, prompt construction, and citation metadata.

Chat requests add:

```json
{
  "conversation_id": "stable-id",
  "turn_id": "new-idempotency-id",
  "model_id": "expected-id",
  "model_version": "expected-immutable-version",
  "directory_context": {
    "scope_id": "optional-id",
    "root_id": "selected-root-id",
    "relative_directory": "contracts/2026"
  },
  "attachment_ids": ["server-attachment-id"],
  "messages": []
}
```

The gateway canonicalizes the directory against the confirmed root, performs
Check → Update → Query, pins one SQLite read snapshot/evidence generation,
constructs a clearly delimited **untrusted local evidence** block, and only
then invokes the selected model. SSE metadata reports whether Chutni was used,
the reason code, freshness/generation, and citation objects before/with the
answer.

For Qwen session snapshots, the effective augmented turn is recorded exactly
once under `turn_id`; the visible browser transcript retains the user's
original wording plus structured context/citations. Retrying the same turn
cannot append evidence twice. Scratch relevance judgments never mutate the
conversation snapshot.

Binary attachments no longer live as Base64 inside the single browser
`localStorage` record. Add:

```text
POST   /v1/attachments
GET    /v1/attachments/<attachment_id>
DELETE /v1/attachments/<attachment_id>
```

The upload body is a bounded raw `Blob` with declared media type and a
separately encoded display filename. The server streams it to a
content-addressed, mode-`0600` staging file, verifies size/type, atomically
publishes it, and returns ID/hash/capabilities. Conversations store IDs only.
Unreferenced attachments follow a documented local garbage-collection grace
period.

The composer capability-gates actions from server truth:

- Image requires an image-capable ready model and attachment endpoint.
- Document requires a supported reader/extractor.
- Web page requires implemented `/v1/web/*` routes and explicit network
  configuration; otherwise it is disabled with a reason.
- Directory requires filesystem chooser/context APIs.

The UI must not display a working action for a route absent from the packaged
gateway.

---

## 6. Chutni indexing and retrieval contract

### 6.1 Scope policy

Mandatory exclusions include:

- Samosa's own state and model directories;
- keychains, credential stores, SSH/GPG key directories, browser profiles, and
  known password-manager stores;
- system caches, device files, sockets, and virtual filesystems;
- hidden files/directories unless the user explicitly opts in and the path is
  not a mandatory exclusion;
- symlinks;
- model weights, build artifacts, and obviously generated dependency trees by
  default.

Exclusions are versioned rules, not prose-only checks:

```json
{
  "id": "credentials.ssh.v1",
  "kind": "mandatory",
  "matcher": "path_component",
  "pattern": ".ssh",
  "reason": "credential_store"
}
```

`kind` is `mandatory` or `user_overridable`. Matching is component-aware over
canonical path bytes using the selected volume's case-sensitivity rules;
substring matching is forbidden. The ordered effective rules, supported-type
table, mount policy, and size limits produce the stored `policy_fingerprint`.
A stricter mandatory-policy update retires formerly indexed evidence before a
scope can return to Ready. Mandatory-excluded trees are counted in aggregate
without persisting their sensitive child names.

V1 supported source classes are:

- valid UTF-8 text-family files (plain text, Markdown, JSON, CSV, source code)
  after bounded validation;
- PDF through `samosa-extract`;
- PNG, JPEG, and PPM through the available OCR path.

Detection uses bytes/magic plus parser validation, never extension alone.
Office documents, archives, mail stores, browser databases, and other formats
remain `unsupported_type` until a bounded parser and fixtures are added.

Stable skip reasons are:

```text
mandatory_exclusion | user_exclusion | hidden_excluded |
cross_filesystem | unsupported_filesystem | unsupported_type |
not_regular_file | symlink | unreadable | permission_denied |
too_large | changed_during_read | extraction_failed | ocr_unavailable |
quota_reached
```

The preflight UI shows the effective list. Unsupported, excluded, unreadable,
oversized, or transient files are counted with stable skip reasons. One
unreadable child never fails the entire scope.

V1 rejects network filesystems during preflight unless a later task adds
bounded-call timeouts and disconnect recovery for that filesystem. Local
removable media is allowed only with stable volume identity.

### 6.2 Initial build

The build pipeline is:

1. Stream an inventory of relative paths and metadata without buffering the
   complete tree in memory.
2. Apply exclusions and the selected filesystem-boundary rule.
3. Identify supported regular files.
4. Hash supported content that is not already reusable.
5. Reuse extraction/OCR by full content hash and reader fingerprint.
6. Extract text with source provenance.
7. Produce provenance-bearing page/section/paragraph spans and send only
   bounded, valid UTF-8 batches to the pinned local Gigatoken adapter.
8. Use exact Gigatoken counts to assemble deterministic canonical
   600–800-token chunks, splitting oversized spans safely. Record the
   Gigatoken, tokenizer, special-token policy, and chunker fingerprints.
9. Insert paths, chunks, and exact canonical token counts into FTS5.
10. Validate and atomically publish the evidence generation.
11. Queue bounded, token-budgeted map jobs for short query-independent memory
    cards, then bounded reduce jobs for directory summaries. Use the verified
    direct Qwen token-ID path when eligible and the bounded text path
    otherwise.
12. Publish only generation-matched enhancement revisions.

The lexical index becomes usable once paths and chunks are committed. Summary
generation may continue as **Ready · improving summaries**. Folder evidence can
therefore become Ready without a model; summaries remain pending until a model
is available. Summary work must not make raw evidence unavailable or change
the freshness generation.

Gigatoken may finish tokenization far faster than Qwen can prefill or generate.
The durable enhancement queue is therefore capped by source bytes and estimated
input tokens, not just item count. It stores content/chunk references and
fingerprints rather than unbounded token arrays. Map/reduce prompts never exceed
the active context minus system, output, and memory-fit reserves; a context
overflow deterministically subdivides the work instead of failing the scope.

### 6.3 Provenance

Every retrievable item links back to:

- scope ID;
- file ID;
- relative path;
- content hash;
- chunk ID;
- page number when known;
- line range when genuinely available, otherwise section or character offsets;
- extraction and chunker fingerprints.

Page numbers are one-based. Text offsets are UTF-8 byte offsets into the exact
normalized text stored in `contents`; the normalization algorithm is part of
the reader fingerprint. A line number is stored only when the parser produced a
real line boundary.

`file_id` is a random stable record ID. It survives a rename only when stable
volume/root identity plus file identity and/or a full content hash proves
continuity; inode equality by itself is insufficient because inodes are reused.
`chunk_id` is a deterministic SHA-256 over content hash, chunker fingerprint,
page/section identity, and byte offsets.

Store relative path bytes losslessly in a BLOB. Store a separate escaped UTF-8
display path for JSON/UI use; invalid UTF-8 bytes use a replacement display and
the Base64 field in citations. Containment and uniqueness use canonical raw
components with the source filesystem's case and normalization semantics, not
display strings.

The UI must not manufacture line numbers for formats that do not provide them.
A valid citation can be a path plus page, section, or excerpt anchor.

### 6.4 Freshness

For a normal refresh:

1. Confirm that volume identity and root file identity still match, then walk
   only the applicable scope or contextual subtree.
2. Compare relative path, filesystem identity when available, file type, size,
   and nanosecond modification time.
3. Full-hash and reprocess additions or metadata changes.
4. Reuse content after a rename when stable identity or full hash proves it is
   the same.
5. Remove deleted paths and their unreferenced chunks.
6. Leave unchanged content unread.
7. Publish all changes in one transaction and increment the generation.

Subtree boundaries are normalized path components. Deletion reconciliation for
`foo/bar` can touch only that directory and descendants; it must not match
`foo/barley` or any sibling. A change outside a contextual subtree is left
untouched until that area is checked.

Hash/extraction checks source identity before and after the read. Retry an
unstable file twice with bounded backoff; if it still changes, record
`changed_during_read`, make freshness partial, and publish no new evidence from
it. Re-stat traversed directories and perform one bounded quiescence pass for
directories that changed during the walk. Continued churn produces a partial
check, never a false complete claim.

If a changed file becomes unreadable, unsupported, or unextractable, retire its
old chunks from the new evidence generation and report the skip/error. Old
chunks remain available only through an explicitly selected previous
generation, never as current evidence.

If path, size, identity, and mtime are all unchanged, v1 may treat the file as
unchanged. The UI must not claim protection against an adversarial program that
rewrites bytes while restoring all metadata. A manual deep verification may
rehash everything.

A full rebuild is required only for:

- incompatible database schema;
- changed extraction/chunking contract that cannot migrate;
- failed integrity check;
- explicit user request.

### 6.5 Answering

When Chat has an explicit directory context:

1. Resolve the most-specific ready Chutni whose canonical root contains that
   directory.
2. Check freshness for the relevant subtree in a foreground budget of two
   seconds or 10,000 stat records, whichever is reached first.
3. If discovery and a small update finish inside that budget, publish before
   retrieval.
4. If the budget expires, continue as a durable refresh and default to waiting.
   The user may explicitly choose
   **Answer from the previous memory**; the answer must then disclose the
   generation and stale state. That choice applies to one answer only.
5. Search relative paths, filenames, chunks, cards, and directory summaries
   with a bounded result and exact active-tokenizer prompt budget. Gigatoken
   may perform this budgeting only under that model's verified tokenizer
   profile; otherwise use the backend's authoritative tokenizer.
6. Apply deterministic minimum-score/coverage rules, then ask the local model
   to judge whether the evidence addresses the question.
7. If useful, answer with citations and a small **Used Chutni · checked now**
   status.
8. If not useful, do not inject it. Offer bounded live file tools or explain
   that no relevant indexed evidence was found.

If the most-specific matching scope is disconnected, partial, corrupt, or
permission-blocked, do not silently fall back to a broader scope. Ask for an
explicit stale/broader-scope choice and disclose the selected scope and
generation.

If no matching Chutni exists, Samosa can offer to create one. It must not begin
a long scan from an ordinary chat message without confirmation.

Retrieval configuration for v1:

- pin the exact vendored SQLite version and compile options in build evidence;
  enable FTS5 and disable loadable extensions;
- use `unicode61 remove_diacritics 2` unless frozen multilingual fixtures prove
  a different bundled tokenizer is required;
- index path, title, raw chunk, card, and summary columns with provisional BM25
  weights `6.0, 4.0, 1.0, 0.75, 0.5`;
- remember that FTS5 `bm25()` sorts lower values first; do not treat its sign as
  a universal relevance probability;
- lex user text into bounded Unicode terms and bind safely quoted MATCH input;
  never pass raw quotes, `NEAR`, `*`, column selectors, or other user syntax to
  FTS;
- fetch at most 20 candidates, order by BM25 then canonical path bytes then
  chunk ID, and deduplicate identical content to at most two displayed paths
  unless the query is explicitly path-focused;
- cap injected evidence at the minimum of the request, 4,096 tokens, and 25% of
  the active model's remaining context;
- require at least one meaningful lexical term match before model relevance
  judgment. The judgment runs in a scratch/non-conversation context and returns
  a strict fixture-tested JSON decision. Timeout, malformed output, or model
  failure means `used:false`.

Indexed content is untrusted evidence, not instructions. Its prompt wrapper
must state that it cannot authorize tools, network access, scanning, or file
changes. A prompt-injection document is a required security fixture.

### 6.6 Resource behavior

- V1 defaults to at most 512 queued inventory records, two concurrent hash/read
  workers, one extractor/OCR child, one supervised Gigatoken child, one SQLite
  writer, and one summary request. The Gigatoken child defaults to at most two
  background worker threads and bounded frames; a measured release may lower
  these values. Raising them requires new machine evidence.
- Inventory, hashing, extraction, OCR, tokenization, indexing, model prefill,
  and summarization all yield to interactive Chat. Admission tests start a chat
  during every phase, not only during model summaries.
- Cooperative scan/hash/SQLite cancellation is observed within two seconds.
  Gigatoken observes cancellation between frames within two seconds. An
  extractor/OCR/Gigatoken child receives a stop request, then is forcibly
  terminated if it has not exited within ten seconds. Source files remain
  untouched.
- Only one writer operates on a scope. Queries may continue against the last
  complete generation.
- Summary inference uses background admission and pauses while interactive
  generation is active.
- Memory-pressure and thermal signals can pause background work with an honest
  reason; they do not silently mark the job complete.
- Network access is forbidden during inventory, extraction, indexing,
  tokenization, model ingestion, summarization, refresh, and query. The
  Gigatoken build must have no reachable Hub/network loader. Only an explicit
  model download or explicit web action may access the network.
- Before publication, available space must cover the active index retained for
  rollback, estimated staging DB/WAL, estimated new extraction data, and a
  2 GiB reserve. Crossing the reserve during work pauses with
  `insufficient_space`; it does not publish a partial generation.

---

## 7. Ordered implementation tasks

All tasks start as **PLANNED**. Complete them in dependency order. Each task
must land with its tests and evidence; a rendered mock alone does not complete
backend behavior.

Task numbers group related work; they are not a blanket phase barrier. The
critical dependencies are:

```text
T0.1 + T0.4
    → T1.1 → T1.2 → T2.1 → T1.0
    → T2.2/T2.3 → T1.4
    → T3.1 → T2.4 → T3.2

T0.1 → T0.5

T0.2 + T0.3 + T0.4 + T0.5
    → T4.1/T4.2 → T4.3 → T4.4/T4.5
    → T5.* → T6.*

T3.3 may build against frozen fixtures early, but server integration waits for
T4.5.
```

The Chutni-specific T0.2/T0.3 work does not block profile, design-system, or
model-manager work. T0.5 can proceed against frozen tokenizer fixtures while
the rest of the browser control plane is built.

### Phase 0 — correctness and test foundations

#### T0.1 — Freeze contracts and add test fixtures

**Status: DONE (2026-07-25).** Evidence:
[docs/regressions/ui-chutni/t0.1-evidence.md](../regressions/ui-chutni/t0.1-evidence.md).

**Work**

- Add shared test fixtures for an empty profile, existing legacy model,
  interrupted model download, folder tree, duplicate files, rename, deletion,
  unreadable directory, symlink escape, multi-page PDF, and corrupt index.
- Add JSON contract tests for every new endpoint and stable error envelope.
- Add deterministic fake model/download servers so ordinary tests never fetch
  multi-gigabyte artifacts or load a real model.
- Record baseline behavior for existing Chat and Jobs routes before modifying
  the gateway.
- Freeze a release capability matrix generated from actually packaged targets:
  native-local versus container, OS/architecture, and the latest plus previous
  stable Safari/Chrome/Firefox versions available on those targets. Containers
  expose only explicitly mounted paths; they do not offer host Drive or This
  computer scopes.

**Acceptance**

- All fixtures are generated from small in-repository data.
- Tests fail if an endpoint silently changes a locked field or state.
- Existing Chat and Jobs tests remain green.

#### T0.2 — Make scanning safe and streaming

**Status: DONE (2026-07-26).** Evidence:
[docs/regressions/ui-chutni/t0.2-evidence.md](../regressions/ui-chutni/t0.2-evidence.md).

**Work**

- Add a Chutni inventory mode to `samosa-fs`.
- Stream records instead of materializing and sorting the entire tree.
- Preserve every path even when content is duplicated.
- Skip unreadable descendants with stable reasons.
- Implement filesystem-boundary and exclusion policies.
- Hash only when requested by the reconciliation plan; a hash request means a
  complete SHA-256, never a misleading prefix hash.
- Keep no-follow open/fstat identity checks around every read.

**Acceptance**

- A symlink cannot escape the root.
- Duplicate content at two paths yields two path records.
- An unreadable nested directory is counted and the scan continues.
- An inventory-only unchanged refresh reads metadata but no file content.
- Memory is bounded by queues/path buffers rather than total file count.
- Cancellation stops within two seconds and leaves no published partial
  result.

#### T0.3 — Fix extraction and read-cache correctness

**Status: DONE (2026-07-26).** Evidence:
[docs/regressions/ui-chutni/t0.3-evidence.md](../regressions/ui-chutni/t0.3-evidence.md).

**Work**

- Page through PDFs using the extractor's supported per-call batch instead of
  requesting an invalid oversized batch.
- Include extractor/OCR/parser fingerprints in cache validity.
- Add one-writer locking and atomic cache publication.
- Verify file identity before and after hashing/extraction; retry or mark dirty
  if it changed.
- Add cache size accounting and bounded pruning that never removes a live
  referenced record.

**Acceptance**

- A PDF longer than five pages is processed without violating the extractor
  contract.
- Concurrent reads of the same content produce one valid cache entry.
- Changing a file during extraction cannot publish text under the old hash.
- A stale parser fingerprint triggers re-extraction.

#### T0.4 — Generalize durable background operations

**Status: DONE (2026-07-26).** Evidence:
[docs/regressions/ui-chutni/t0.4-evidence.md](../regressions/ui-chutni/t0.4-evidence.md).

**Work**

- Extract or reuse one durable job primitive for model downloads and Chutni
  builds: atomic state, append-only events, replay, checkpoints, pause, resume,
  cancellation, and crash recovery.
- Add one-writer scope locks and stale-lock recovery based on verifiable process
  identity, not PID alone.
- Wire the existing interactive-generation interlock into Chutni summary work.

**Acceptance**

- Closing and reopening the browser reconstructs exact job state.
- Killing the gateway mid-operation leaves the prior active model/index valid.
- Restart resumes from a safe checkpoint or reports a recoverable failure.
- Repeated pause/resume/cancel calls are idempotent.

#### T0.5 — Pin, minimize, and prove the Gigatoken adapter

**Work**

- Vendor or reproducibly fetch Gigatoken v0.10.0 at commit
  `34a1599f0c0ae7d7cd0d1c530e6522320158b360`; preserve its MIT license and
  record every Samosa patch.
- Create a small `samosa-gigatoken` Rust binary around only local
  `tokenizer.json` loading plus bounded `encode_batch`/`encode_prompt`. Compile
  out the Python, Hub/network, training, benchmark, Parquet/Arrow, and
  compressed-file paths.
- Pin the build toolchain and lockfile, produce a transitive license/SBOM
  report, and bundle reproducibly checksummed target binaries so users need no
  language runtime or build tools.
- Implement the private versioned framed protocol in §5.6.1 with input/output
  ceilings, child supervision, low-priority fixed threads, timeouts,
  cancellation, restart, and structured errors.
- Add exact differential tests against `src/tok.h` and the shipped
  `tokenizer_qwen36.json`. Cover ordinary prose, all trusted control tokens,
  special-token-disabled document text, Qwen chat templates, NFC and
  decomposed Unicode, multilingual/CJK/emoji, code, whitespace, empty and long
  documents, random UTF-8, and adversarial split boundaries.
- Fuzz tokenizer loading and framed input. Reject malformed length fields,
  invalid UTF-8 where text is required, output-size overflow, unknown
  operations, wrong hashes, and token IDs outside the declared vocabulary.
- Benchmark separately: current `tok_encode`, raw Gigatoken, adapter including
  IPC, direct token-ID handoff, Qwen prefill, and complete extraction-to-index
  and extraction-to-summary paths. Label upstream figures as upstream
  tokenization results, never Samosa ingestion measurements.

**Acceptance**

- The adapter produces exactly the same token ID sequence as `tok_encode` for
  every frozen Qwen fixture and repeated serial/parallel runs.
- A one-byte tokenizer artifact or prompt-template change prevents direct-ID
  activation with `tokenizer_mismatch`.
- A clean supported machine runs the bundled adapter without Python, Rust,
  Cargo, Homebrew, or a network connection.
- A network-deny test plus binary/dependency inspection proves that the
  production adapter cannot invoke the upstream Hub loader.
- Peak child RSS, cache growth, IPC throughput, cancellation latency, and
  interactive-chat admission pass §6.6 and are recorded on the reference
  machine.
- Adapter-plus-IPC tokenization materially beats current `tok_encode` on the
  frozen large Qwen corpus. If it does not, Gigatoken remains available for
  measured batch workloads but the authoritative text path stays the release
  default; no unmeasured speed claim appears in the UI.
- Tokenizer and model-prefill throughput are reported as separate metrics, and
  end-to-end evidence makes clear which stage dominates.

### Phase 1 — model-free browser shell and durable setup

#### T1.0 — Produce a model-free browser runtime package

**Status: DONE (2026-07-27), with two disclosed gaps.** Evidence:
[docs/regressions/ui-chutni/t1.0-evidence.md](../regressions/ui-chutni/t1.0-evidence.md).
The gateway/fs sidecar moved from `package_hf.py`'s opt-in `--gateway` flag
into every release unconditionally; a new `--runtime-only` flag stages the
control plane (engine, gateway, app shell, catalog) with zero model
artifacts. `install.sh` stages `MODEL_FILES` only when the manifest lists
them, always compiles the gateway/jobsd/fs, and its smoke test no longer
requires or forces real generation — it checks health/root UI/profile/
setup-status/catalog and accepts either a real completion or `409
model_required` from Chat. `dist/samosa` requires the gateway for
`serve`/`app` (the separately-named one-shot CLI prompt path is untouched)
and `doctor` no longer reports failure merely because no model is
installed. Testing this for real (not `SAMOSA_INSTALL_TEST=1`) surfaced and
fixed two real bugs it would otherwise have shipped: `dist/samosa`'s
`RELEASE_DIR` heuristic used model-directory presence as a proxy for "real
release directory," which broke the moment that directory legitimately
didn't exist; and `install.sh` eagerly pre-created an empty `current/model`
even when nothing was ever staged there. It also found and fixed a real
packaging gap disclosed in T2.1's own evidence doc: no release ever staged
`assets/models.json`, so `GET /v1/models/catalog` 500'd on every release
built to date, runtime-only or not. **Disclosed, not fixed:** bundling
`llama-server` as a v1 runtime dependency (predates this task, still just
detected by `doctor`, not installed by anything) and container/Docker
release assembly (issue #2's D-3 territory — the repo's root `Dockerfile`
is stale and out of scope here).

**Depends on:** T1.1, T1.2, and T2.1. This task packages the already-working
control plane; it does not define zero-model gateway behavior.

**Work**

- Update `dist/install.sh`, `dist/samosa`, `tools/package_hf.py`, and browser/
  container release assembly so the gateway is the mandatory browser control
  plane rather than an optional package with a raw-Qwen fallback.
- Separate runtime artifacts from model weights. A runtime install or upgrade
  makes no model-artifact request.
- Bundle every trusted executable runtime dependency required by v1 launch
  profiles, including shared runtimes; do not create a second executable
  downloader.
- Replace the installer's mandatory real-model smoke with a control-plane smoke:
  health, root UI, profile, catalog, and structured `model_required` chat
  response.
- Preserve a deliberate raw engine/headless path only if it is named separately
  and cannot masquerade as the full browser app.

**Acceptance**

- The installed browser runtime boots with every model weight and backend model
  path absent and remains alive.
- `/healthz`, root UI, profile, setup, and catalog work; Chat returns
  `409 model_required`.
- A fixture server records zero model-artifact requests during runtime install
  and upgrade.
- Upgrading around a registered legacy model leaves its path, inode/identity,
  and bytes unchanged.

#### T1.1 — Start the gateway with zero installed models

**Status: DONE (2026-07-26), scoped to the control-plane/backend-state work
this task owns.** Evidence:
[docs/regressions/ui-chutni/t1.1-evidence.md](../regressions/ui-chutni/t1.1-evidence.md).
Setup UI screens (Name/Welcome/Model/Chat) are T1.2/T2.4's work, not this
task's; see the evidence doc for the exact scope line.

**Work**

- Separate the control plane from the inference backend.
- Report runtime-dependency availability separately from model-artifact
  availability.
- Serve the UI, profile, setup, model catalog/install, diagnostics, and Chutni
  management APIs without an installed model.
- Represent backend state as `none`, `loading`, `ready`, `generating`, or
  `failed`.
- Chat requests without a ready model return a structured `model_required`
  response rather than crashing or exiting.

**Acceptance**

- A clean Samosa data directory opens the Name screen.
- `/healthz` clearly distinguishes a healthy control plane from model
  readiness.
- No compiler, model weights, or running inference backend is required to
  render setup.
- A clean runtime install completes without downloading Qwen or any other model.
- A runtime-only upgrade does not stage, copy, or redownload existing model
  bytes.
- Existing installed backends continue to be discovered.

#### T1.2 — Implement profile and setup state

**Status: DONE (2026-07-26), with one interim bridge pending T2.1.** Evidence:
[docs/regressions/ui-chutni/t1.2-evidence.md](../regressions/ui-chutni/t1.2-evidence.md).

**Work**

- Implement `GET/PUT /v1/profile` and `GET /v1/setup/status`.
- Validate, atomically store, escape, edit, and reload the name.
- Persist completion of the Welcome step, selected model/version, and active
  install-job/selection-operation references; reconcile them with the durable
  registries at startup.
- Add an explicit frontend setup state machine; do not initialize a blank chat
  behind setup screens.

**Acceptance**

- Refreshing at every setup step resumes that step.
- Losing browser state during a download reconnects through setup status or
  `GET /v1/models/installs`.
- Names containing markup render literally and cannot execute script.
- Editing the name updates all welcome text without altering model prompts.
- Corrupt profile JSON yields a recoverable setup state and diagnostic.

#### T1.3 — Implement the safe browser directory chooser

**Status: DONE (2026-07-26), except real multi-browser interactive
verification (no browser-automation tool is available in this environment;
see the evidence doc).** Evidence:
[docs/regressions/ui-chutni/t1.3-evidence.md](../regressions/ui-chutni/t1.3-evidence.md).
Linux/Windows volume ("Drive") discovery is not implemented -- Home only
there for now; a container target doesn't offer Drive/This-computer scopes
at all per the T0.1 capability matrix, so this isn't a blocker for that
target. Not yet wired into the composer `+` menu or the Chutni Add-Chutni
flow -- neither exists yet (that's T2.4/T3.x and a later Chutni-specific
task); the dialog is reachable today via a labeled "Folder access
(preview)" control in Settings.

**Work**

- Add filesystem roots/listing APIs and per-launch UI-token validation.
- Build a keyboard-accessible directory-browser dialog.
- Show the canonical selected path before scope creation.
- Retain typed-path support for headless/API use.

**Acceptance**

- `..`, encoded traversal, symlink, and race attempts cannot leave allowed
  chooser roots.
- The API returns no file contents.
- A denied directory appears as unavailable rather than crashing the chooser.
- The selection works in supported Safari, Chrome, and Firefox versions.

#### T1.4 — Migrate and bind conversations

**Status: DONE (2026-07-26), model identity keyed to an interim placeholder
pending T2.1/T2.3 (owner-confirmed sequencing, same pattern as T1.2's
`setup_status_handler` bridge).** Evidence:
[docs/regressions/ui-chutni/t1.4-evidence.md](../regressions/ui-chutni/t1.4-evidence.md).
No real selection lock yet (T2.3); the reopen-under-another-model dialog
offers fork/cancel only -- install/switch-to-that-version need T2.1/T2.3 and
say so rather than faking a working button.

**Depends on:** T2.1 and T2.3 for immutable model identity and readiness.

**Work**

- Add conversation schema v2 and idempotent v1 migration.
- Record model ID/version in browser state and canonical gateway metadata.
- Validate the binding on every chat request so another browser tab or API
  client cannot bypass it.
- Prevent implicit use of an old conversation under another model; fork instead
  of rebinding.
- Preserve Qwen conversation IDs and existing visible history.

**Acceptance**

- A populated legacy browser profile migrates without lost messages.
- Refresh during migration is safe.
- Browser-quota failure and malformed top-level JSON preserve the original
  localStorage key.
- Reopening a conversation created by another model prompts for an explicit
  action.
- A direct request carrying the wrong active model receives
  `409 conversation_model_mismatch` and is not forwarded.
- A malformed conversation does not erase healthy conversations.

### Phase 2 — model manager and first-run vertical slice

#### T2.1 — Replace hardcoded model UI with a trusted catalog

**Status: DONE (2026-07-27).** Evidence:
[docs/regressions/ui-chutni/t2.1-evidence.md](../regressions/ui-chutni/t2.1-evidence.md).
`GET /v1/models/catalog` is real and tested against the actual installed
Qwen (24 GB, hard-linked) and Ornith models: real per-artifact bytes/SHA-256
(cross-checked against live files and, for Bonsai/Ornith, Hugging Face's
own tree API), validation that rejects malformed/unsafe/untrusted catalog
data before serving any of it, live compatibility/install-state detection,
shared-runtime-dependency gating, and live capability degradation when an
optional artifact (Bonsai's vision projector) is missing. **Frontend gap
closed:** `assets/app.html`'s model `<select>` is now rebuilt at runtime
from this endpoint (`renderModelOptions()`/`refreshModels()`) instead of a
hardcoded `<option>` list — the acceptance item "the frontend contains no
authoritative hardcoded model availability" is met, covered by
`tests/test_model_catalog_ui.mjs` (`make test-model-manager`), same
DOM-fixture pattern as `tests/test_chooser_ui.mjs`. Model-switching
mechanics themselves (`/v1/backends/select`) are unchanged — that safety
work is T2.3, still open. Deliberately deferred, not forgotten: content-hash
("Deep verify") corruption detection (byte-size check only),
`minimum_ram_bytes` (no measured lower bound exists), and Qwen's
`prompt_template_sha256` (left empty rather than fabricated; real parity
proof is T0.5/T4.4 scope).

**Work**

- Define the versioned catalog and artifact manifests.
- Represent required runtimes, shared dependencies, artifact roles, optional
  capabilities, and trusted launch-profile IDs.
- Add compatibility checks based on measured architecture, RAM, disk, and
  required runtime capabilities.
- Detect valid current and legacy installs with complete artifact validation.
- Expose licenses, sizes, capabilities, and factual recommendation reasons.

**Acceptance**

- The frontend contains no authoritative hardcoded model availability.
- Missing or corrupt artifacts are not shown as installed.
- Existing valid Qwen/Bonsai/Ornith paths are detected without redownload.
- Shared runtime dependencies are installed once and reused without making an
  incomplete model appear ready.
- Missing optional vision data disables only that capability; missing required
  text data makes the model unavailable.
- Compatibility reasons are deterministic and testable.
- Duplicate IDs/paths, malformed hashes/sizes, unsafe relative paths, unknown
  backend kinds, unsupported runtime ABI, and untrusted artifact hosts are
  rejected before any write or network request.

#### T2.2 — Build resumable server-owned downloads

**Status: DONE (2026-07-27) against this task's acceptance list, with
disclosed scope decisions.** Evidence:
[docs/regressions/ui-chutni/t2.2-evidence.md](../regressions/ui-chutni/t2.2-evidence.md).
Real curl-based resumable download (`-C -`, `-L`, `--proto-redir
-all,https`), native SHA-256 verification, atomic per-model activation,
preflight free-space check with required-vs-available reporting in the
error, pause/resume/cancel/retry, and live byte progress, all tested end to
end against `tests/fake_model_download_server.c` (14 scenarios in
`tests/test_model_install.sh`, `make test-model-manager`, ASan-clean).
Two real bugs were found and fixed by the testing itself before landing: a
route-matching off-by-one that broke every `GET
/v1/models/installs/<job_id>` request, and a missing `-L` flag that would
have broken every real Hugging Face download despite the redirect-rejection
test appearing to pass (for the wrong reason). **Disclosed scope
decisions, not silent gaps:** one active transfer at a time rather than a
persisted multi-model FIFO queue (this task's acceptance list doesn't test
cross-model queuing); real network disconnect and real disk-full were not
separately simulated (the code paths they'd hit are exercised by other
tests, but the exact conditions weren't reproduced); events are plain JSON
replay, not SSE (matches existing project convention, nothing consumes
this endpoint live yet). Qwen still cannot be installed via this path
(`artifact_not_downloadable`) since it has no public download URL yet — see
T2.1's evidence doc.

**Work**

- Implement free-space preflight, bounded retries, HTTP range resume, exact byte
  progress, pause/resume/cancel, checksum verification, and atomic activation.
- Persist state/events before responding to the UI.
- Restrict artifact URLs to the trusted catalog.
- Keep incomplete data in a named partial location with an explicit retention
  policy and repair action.

**Acceptance**

- A forced disconnect resumes without restarting completed bytes.
- Tests cover valid `206`, invalid `Content-Range`, a server ignoring Range with
  `200`, truncation, oversize, checksum mismatch, redirect to an untrusted host,
  preflight ENOSPC, and disk-full during transfer.
- A checksum mismatch never activates and provides a useful retry/repair error.
- Closing the browser does not stop the download.
- Insufficient space is detected before downloading and reports required versus
  available bytes.
- A working installed version survives every failed update path.

#### T2.3 — Make model activation readiness-safe

**Status: DONE (2026-07-27).** Evidence:
[docs/regressions/ui-chutni/t2.3-evidence.md](../regressions/ui-chutni/t2.3-evidence.md).
`POST /v1/backends/select` still forks and returns 202 synchronously
(unchanged timing contract), but a durable selection job + background
watchdog now wait for real readiness (`backend_probe()`, bounded timeout),
confirm the target model's weights file wasn't swapped mid-load (size+mtime
snapshot — disclosed limit: not a content hash), and detect an immediate
child crash via `waitpid(WNOHANG)` rather than waiting out the timeout. Any
failure — timeout, crash, fingerprint mismatch, or a durable-commit
failure — rolls back to the previously-working backend; the
`model-backend` selection file is only rewritten after readiness and the
fingerprint both pass. `GET /v1/models/selection/active` lets a reloading
client reconnect to an in-flight switch without ever having known a
`job_id`. Two real bugs were found and fixed by testing before this landed:
`json.h`'s `double`-typed numbers silently lost precision on a
nanosecond-epoch mtime, permanently breaking every real fingerprint check
(fixed by splitting mtime into seconds + nanosecond-remainder fields); and
killing the gateway while a watchdog thread was still mid-flight could
orphan a freshly-forked backend process (main()'s shutdown-triggered
`backend_stop()` raced the watchdog's own crash-detection, which then
forked a rollback right as the process was exiting — fixed by never forking
during shutdown and having the watchdog check `g->stopping` first). A third
issue was an *added* `switching` interlock flag (beyond this task's literal
acceptance list) that caused a real, intermittent regression in the
pre-existing `tests/test_compiled_gateway.sh` vision-backend scenario — it
lagged behind `backend_probe()`'s own live readiness signal on an
uncoordinated clock and could reject an ordinary chat/Jobs request for a
moment after the backend was already genuinely answering. Removed entirely;
the pre-existing `backend_probe()` gate already interlocks Chat/Jobs
inference against an in-progress switch with no such lag, unchanged by this
task. "Relevance checks"/"Chutni summary inference" (named in this task's
Work list) don't exist as code paths yet (Chutni is Phase 4) — nothing to
interlock with until they're built.

**Work**

- Wait for process readiness and model fingerprint confirmation before
  reporting selection success.
- If switching fails, restore or keep the last working backend.
- Monitor the child process and surface startup/log errors without exposing
  sensitive content.
- Block switching during active generation with an actionable state.
- Interlock switching with Chat, Jobs inference, relevance checks, and Chutni
  summary inference—not only the currently visible chat tab.

**Acceptance**

- Selection never returns “ready” merely because a process was forked.
- A forced exec/load failure leaves a usable prior model or a clear no-model
  state.
- Readiness timeout, fingerprint mismatch, immediate child crash, and registry
  commit failure all preserve or restore the prior active selection.
- No duplicate or orphan backend remains after repeated switches.
- Health and UI agree on the active model and version.
- Refreshing or reopening during model load reconnects through the durable
  selection-operation registry.

#### T2.4 — Ship Name → Welcome → Model → Chat

**Depends on:** T1.2, T2.1–T2.3, and the T3.1 design-system foundation.

**Work**

- Implement the four setup views with the visual language in
  `docs/UI_DESIGN.md`.
- Connect model cards and durable download events.
- Add recovery UI for offline, paused, insufficient-space, checksum, and
  backend-start failures.
- Detect and skip completed steps.

**Acceptance**

- A clean profile completes the entire flow using a tiny fake model fixture.
- An existing valid model reaches Chat without a download.
- Browser refresh/close/reopen during download resumes exact state.
- Keyboard-only and screen-reader navigation can complete setup.
- No setup view exposes the normal sidebar prematurely.

### Phase 3 — main UI overhaul

#### T3.1 — Apply the design system and information architecture

**Work**

- Implement the warm flat surfaces, hairlines, tokenized color/type/spacing/
  radius system, one accent, and semantic status colors.
- Remove decorative washes, gradients, glow, routine shadows, hover lifts,
  mixed Unicode icons, and sub-legible type.
- Use one inline SVG icon family.
- Add Chat/Chutni/Jobs navigation, conversation region, bottom model status,
  and Troubleshooting surface.
- Produce checked-in visual acceptance captures for Name, Welcome, Model, Chat,
  and Chutni in light/dark and desktop/narrow layouts.

**Acceptance**

- Light and dark modes use the same semantic roles.
- Focus is always visible; reduced-motion is respected.
- Status color is never the only carrier of meaning.
- There are no layout jumps as telemetry values change.
- The app loads no external scripts, styles, fonts, or UI assets and retains its
  same-origin CSP.
- Existing Chat and Jobs capabilities remain reachable.

#### T3.2 — Rebuild Chat and the composer

**Work**

- Add personalized empty state, model-bound conversation state, unified `+`
  menu, attachment chips, directory context, streaming/reasoning display,
  cancellation, and stable telemetry.
- Move binary attachments to server IDs and capability-gate menu items from the
  packaged gateway's reported routes.
- Move web-page attachment out of Settings.
- Preserve escaped rendering and follow-output behavior.

**Acceptance**

- Image, document, web page, and directory contexts have distinct accessible
  labels and removable chips.
- Directory access is never inferred or started without an explicit context.
- Streaming stop/cancel still works.
- A 1,000-message synthetic conversation remains usable within the agreed
  browser performance budget.

#### T3.3 — Build Chutni library and scope views

**Work**

- Implement empty, add, preflight, building, ready, paused, permission,
  disconnected, attention, and forget-confirmation views first against the
  frozen T0.1 contract fixtures.
- Connect all state to server truth and replayed events only after T4.5.
- Show counts, coverage, timestamps, exclusions, skips, memory size, and
  current phase without fake precision.
- Label extraction, tokenization, evidence publication, and model-summary
  improvement separately. Show exact completed/total units only when known;
  never blend fast Gigatoken progress with slower model-prefill progress or
  publish a speculative ETA.

**Acceptance**

- Reloading any long-running view reconstructs it from the server.
- Every error state has a safe next action.
- Forget confirmation states that user files are unchanged.
- Unknown totals render indeterminate progress.
- The scope can display **Ready · improving summaries** while independently
  showing `chunks_summarized / chunks_total`; it never calls tokenized but
  unpublished evidence Ready.

#### T3.4 — Responsive and accessibility gate

**Work**

- Test desktop and narrow browser layouts.
- Complete semantic landmarks, headings, labels, focus order, modal trapping,
  live-region announcements, contrast, zoom, and reduced-motion behavior.
- Keep paths and metrics readable without horizontal page overflow.

**Acceptance**

- Core flows work keyboard-only at 200% zoom.
- Automated accessibility checks have no serious violations.
- Manual tests on the exact latest/previous-stable Safari, Chrome, and Firefox
  builds recorded by T0.1 cover setup, chat, model-download reconnection, and
  Chutni management.

### Phase 4 — folder Chutni MVP

#### T4.1 — Build the SQLite/FTS5 Chutni sidecar

**Work**

- Vendor and compile SQLite with only required features including FTS5.
- Pin the exact SQLite source/version and compile options, including FTS5,
  thread safety, foreign keys, and disabled loadable extensions.
- Implement schema creation, version checks, integrity checks, transaction
  publication, query, and machine-readable errors.
- Keep the sidecar network-free and constrained to gateway-owned scope/index
  paths.

**Acceptance**

- It runs on a clean supported machine without a system SQLite dependency.
- Database corruption is detected and never reported as Ready.
- Readers see the previous generation throughout a failed update.
- FTS5 ranking is deterministic for frozen fixtures.
- `quick_check`, FTS integrity, foreign-key, WAL checkpoint, busy-timeout, and
  generation-pinning tests cover clean and corrupt databases.

#### T4.2 — Implement one-folder inventory and manifest

**Work**

- Create scope registry/storage.
- Run preflight and persist effective policy.
- Inventory one folder with relative paths and stable skip reasons.
- Store volume/root identity, raw relative path bytes, manifest identities, and
  full hashes only where required.

**Acceptance**

- A folder with text, PDF, image, unsupported, duplicate, unreadable, hidden,
  oversized, and symlink fixtures produces exact counts.
- No path outside the root enters the manifest.
- Samosa's own data is always excluded.
- Reopening the gateway preserves the scope and its last complete state.
- An external-drive folder is rejected after a different volume appears at the
  same mount path.

#### T4.3 — Extract and chunk evidence

**Work**

- Reuse content-addressed extraction and OCR.
- Produce bounded provenance-aware structural spans before tokenization.
- Send validated extracted UTF-8 through the T0.5 Gigatoken adapter in bounded
  batches with content hashes, special tokens disabled, cancellation, and
  token/byte backpressure.
- Add deterministic, versioned exact-token chunking and provenance. Combine
  structural spans to the canonical 600–800-token target and safely subdivide
  an oversized span without inventing source offsets.
- Store supported content and explicit failure/skip state.
- Store exact canonical token counts and the complete Gigatoken/tokenizer/
  chunker fingerprint; keep token IDs transient.
- Deduplicate extraction by hash without deduplicating paths.

**Acceptance**

- Identical files at different paths share extraction work but cite both paths.
- Rebuilding unchanged content does not repeat OCR/extraction.
- PDF citations use real page numbers.
- Unsupported or failed files cannot silently appear indexed.
- Frozen chunks have exact `tok_encode`-matching counts, deterministic byte
  boundaries, and identical IDs across adapter restarts and thread counts.
- A literal Qwen control-token spelling inside an indexed document remains
  untrusted document text; it cannot become a trusted prompt control segment.
- Gigatoken never opens the source file, follows a symlink, accesses the
  network, or receives bytes that failed the reader's UTF-8 contract.
- Canceling during a maximum-size tokenization frame stops within the §6.6
  bound and publishes no partial chunk set.
- Mutation during inventory, hash, and extraction either stabilizes within the
  retry contract or yields `changed_during_read` and partial freshness.
- Every citation resolves after restart to the exact stored excerpt, content
  hash, path bytes, and evidence generation.

#### T4.4 — Add lexical retrieval, cards, and summaries

**Work**

- Index paths and chunks with FTS5/BM25.
- Replace or rename the placeholder `/v1/chat/prefill` behavior so no route
  claims cached model state after merely counting tokens.
- Add a private, gateway-only native-Qwen request path that accepts one fully
  fingerprinted bounded token sequence, validates every ID/context/memory
  bound, and runs real cancellable prefill plus generation. Browser requests
  cannot select this path or provide IDs.
- Add token-budgeted map jobs for query-independent memory cards and
  hierarchical reduce jobs for directory summaries. Durable queues hold source
  references and budgets rather than unbounded token arrays.
- Attach every generated statement to source chunk IDs and generator
  fingerprints.
- Pause tokenization, background prefill, and summarization while interactive
  Chat runs.

**Acceptance**

- Frozen relevant queries retrieve expected source chunks in the top bounded
  result set.
- Frozen irrelevant queries pass the no-usefulness gate.
- For the verified Qwen package, text-tokenized and Gigatoken-pretokenized
  versions of each frozen prompt have identical IDs, prefill state, greedy
  output, usage counts, citations, and cancellation behavior.
- A mismatched model, tokenizer, template, adapter, or vocabulary fingerprint
  is rejected before prefill and safely uses the bounded authoritative text
  path. It never partially ingests the supplied IDs.
- Token IDs outside the active embedding vocabulary, malformed frames, context
  overflow, memory-fit failure, and document-created control tokens are
  rejected deterministically.
- Gigatoken can run ahead only to the configured token/byte queue ceiling;
  model prefill backpressure cannot exhaust memory or create a false progress
  percentage.
- One oversized directory is summarized through deterministic bounded
  map/reduce levels; no model request exceeds its fitted context and every
  final statement expands to current raw source chunks.
- Deleting summaries still leaves raw chunk retrieval functional.
- No card or summary exists without resolvable source evidence.
- A source change/delete invalidates its cards/summaries in the same evidence
  transaction; a late worker cannot attach output to a newer generation.
- Identical chunks at many paths preserve all citations without flooding the
  top result set.

#### T4.5 — Complete durable build lifecycle

**Work**

- Wire build phases, counts, events, checkpoint, pause/resume/cancel, browser
  reconnection, and atomic publication into the Chutni UI.
- Keep last complete memory queryable until the next generation publishes.

**Acceptance**

- Inject SIGKILL, ENOSPC, EIO, permission loss, and volume detach at inventory
  checkpoint, source hash, cache temp write/rename, Gigatoken request/response,
  tokenization checkpoint, model-prefill boundary, staging DB write, SQLite
  commit, DB fsync, active publication, event append, state snapshot, registry
  update, and forget tombstone boundaries.
- Every injected failure proves: source bytes unchanged; no partial generation
  queryable; the old generation remains queryable when one existed; restart
  reaches one deterministic state; retry duplicates no rows/work.
- Initial generation failure ends as `failed_initial` or `canceled_initial`;
  query returns `scope_not_ready` rather than pretending an old generation
  exists.
- Resume does not duplicate file/chunk records.
- User pause and chat pause are distinguishable.
- Cancel removes only incomplete derived state.

### Phase 5 — freshness and Chat integration

#### T5.1 — Implement incremental reconciliation

**Work**

- Diff add, change, delete, and rename cases.
- Rehash/re-extract only required content.
- Apply the diff in one transaction and update counts/generation.
- Add manual deep verification and exceptional rebuild paths.
- Apply the fingerprint matrix: schema may migrate/rebuild; extractor/OCR
  reprocesses affected contents; chunker rechunks/reindexes; policy purges newly
  excluded evidence; canonical tokenizer artifact or Gigatoken/chunker behavior
  rechunks/reindexes; adapter implementation changes with proven identical
  output update metadata without changing chunks; active-model-only tokenizer
  changes invalidate that model's prompt-budget/direct-ingestion proof but not
  canonical evidence; summarizer model/prompt regenerates derived rows only.

**Acceptance**

- One changed file reprocesses exactly that content.
- A rename reuses content when identity/hash proves equivalence.
- A deletion disappears from retrieval after commit.
- Segment-aware subtree refresh never deletes or changes sibling rows.
- Hard links, case-only renames, inode reuse, and same-path file replacement
  follow volume/filesystem identity rules.
- An unchanged refresh performs no extraction/model work.
- Failure halfway through a diff leaves all old results intact.

#### T5.2 — Implement scoped query and usefulness gating

**Work**

- Resolve the most-specific containing scope.
- Filter search to the explicit contextual subtree.
- Enforce result-count and exact active-tokenizer prompt budgets, using the
  authoritative backend tokenizer whenever no verified Gigatoken profile
  exists.
- Combine deterministic score/coverage checks with local-model relevance
  judgment.
- Return citations and a structured reason when Chutni is not used.
- Hold one SQLite read snapshot/evidence generation through prompt construction.

**Acceptance**

- Nested scopes select the most-specific match.
- A query cannot retrieve a sibling path outside its context filter.
- Irrelevant evidence is omitted from the prompt.
- All included evidence has resolvable provenance.
- A refresh committing concurrently with a query cannot mix generations.
- Relevance timeout or malformed fake-model output fails closed with no evidence
  injection.

#### T5.3 — Refresh before directory answers

**Work**

- Add the Check → Update → Retrieve → Answer orchestration.
- Implement the two-second/10,000-stat foreground budget from §6.5.
- For larger refreshes, default to waiting and offer the explicit previous-
  generation choice.
- Render `Checked now`, `Updated N files`, generation, or stale disclosure.

**Acceptance**

- A small changed fixture is updated before the answer.
- A large change never silently answers from stale memory.
- A failed freshness check cannot display `Checked now`.
- A partial traversal displays its uncovered/failed count and cannot update
  `last_successful_check_at`.
- No matching/useful Chutni falls back without fabricating memory.

#### T5.4 — Forget, permission, disconnect, and repair

**Work**

- Implement memory-only forget with confirmation.
- Detect missing roots, disconnected drives, changed volume identity,
  permissions, and corrupt indexes.
- Provide reconnect, retry, deep verify, and rebuild actions where safe.

**Acceptance**

- Forget removes the scope/index/cache references but changes no source file.
- Forget deletes the scope DB/WAL/SHM, events, checkpoints, cards, summaries,
  state, and unreferenced extraction/OCR cache data; another live owner's shared
  cache record survives.
- Killing the gateway during Forget resumes from the tombstone on restart.
- A different drive mounted at the same path is not mistaken for the original.
- Permission loss preserves the last index but clearly marks its freshness
  stale/partial.
- Repair cannot replace a valid index until the replacement passes validation.

### Phase 6 — drive and This-computer scopes

#### T6.1 — Drive scope

**Work**

- Apply the base scope's stable volume/root identity to mount-boundary behavior,
  disconnection/reconnect, quotas, and large traversal checkpoints.
- Show exact included/excluded mount policy before confirmation.

**Acceptance**

- Disconnect/reconnect resumes only when volume identity matches.
- Mounted child filesystems are excluded by default.
- Traversal remains bounded at the large-scale gates below.

#### T6.2 — This-computer aggregate

**Work**

- Present concrete readable roots such as selected user document locations and
  selected external volumes.
- Apply mandatory secret/system/Samosa exclusions.
- Aggregate status while keeping each root's permission and freshness visible.

**Acceptance**

- It never silently scans filesystem root.
- The confirmation screen lists every included root.
- One denied root does not fail other roots.
- The UI never promises access that the browser-launched local process does not
  have.

#### T6.3 — Scale and resource hardening

**Work**

- Add synthetic manifest/index tests and real filesystem tests at increasing
  scale.
- Measure inventory memory, refresh I/O, extraction throughput, current
  tokenizer throughput, raw Gigatoken throughput, adapter+IPC throughput,
  model-prefill throughput, summary-generation throughput, database growth,
  query latency, cancellation latency, model pause behavior, thermal state,
  and restart time. Never combine tokenization and prefill into one throughput
  number.
- Add quotas and actionable capacity errors from measured results.

**Acceptance gates**

- V1 enforces a visible per-scope envelope of 1,000,000 regular files and
  5,000,000 chunks; exceeding it pauses before publication with
  `scope_limit_exceeded`.
- A synthetic 1,000,000-file/5,000,000-chunk database indexes and queries with
  peak additional gateway+Chutni RSS at or below 384 MiB, excluding the model
  backend and bounded extractor child but including the supervised Gigatoken
  child and its caches.
- A real 100,000-file tree inventories within the same queue/RSS bound.
- An unchanged 1,000,000-record refresh performs zero content bytes hashed,
  zero extraction/OCR, and zero model calls.
- On the 16 GB Apple-silicon reference machine, warm top-20 query latency is
  p95 ≤ 500 ms and p99 ≤ 2 s; record cold latency separately.
- Cooperative background work yields/admission-pauses within two seconds of an
  interactive Chat request during inventory, hashing, extraction, OCR,
  Gigatoken tokenization, SQLite work, Qwen prefill, and summarization.
- A frozen large extracted-text corpus records raw and end-to-end tokenizer
  MB/s, tokens/s, peak RSS, and exact parity on the 16 GB Apple-silicon
  reference machine. Release notes quote only those Samosa measurements and
  identify the hardware, corpus, tokenizer, thread count, and warm/cold state.
- A frozen card/summary workload records model prefill and generation
  separately and demonstrates bounded queue backpressure. Gigatoken speed
  alone is never used to claim end-to-end Chutni speed.
- Progress is persisted at no more than ten events/second; a compacted job
  replays in ≤5 seconds and no single progress log exceeds 50 MiB.
- Query does not scan JSONL or every chunk.
- Results and machine measurements are recorded under
  `docs/regressions/ui-chutni/`.

### Phase 7 — migration and release

#### T7.1 — Upgrade safety

**Work**

- Test clean install, existing runtime/model, existing chats, existing Jobs,
  incomplete setup, interrupted download, interrupted Chutni build, and schema
  upgrade.
- Do not move or duplicate existing large model files automatically.
- Keep rollback compatible with the last released runtime wherever data format
  allows.

**Acceptance**

- Existing users keep visible chats and installed models.
- A UI-only release cannot trigger a model redownload.
- Failed migration keeps its source backup and provides a diagnostic.
- Setup and Chutni schema migrations are idempotent.

#### T7.2 — Privacy and security gate

**Work**

- Test path containment, symlink races, special files, UI token/Origin checks,
  JSON/HTML injection, oversized inputs, malicious archive/document fixtures,
  indexed prompt injection, untrusted text that spells model control tokens,
  malformed Gigatoken frames/token IDs, tokenizer fingerprint substitution,
  child-process limits, log redaction, and network isolation.
- Audit the minimized Gigatoken dependency graph and ship its MIT license,
  attribution, pinned source/patch record, lockfile, and transitive SBOM.
- Document effective Chutni exclusions and limitations in user-facing help.

**Acceptance**

- Chutni generates no network traffic.
- The production Gigatoken adapter contains no reachable Hub/repository loader
  and cannot read Hugging Face credentials or write a shared Hub cache.
- Source files are byte-identical before and after every test.
- UI names, paths, errors, summaries, and citations cannot inject markup.
- Indexed instructions cannot authorize a tool, network call, wider scan, or
  source-file mutation.
- Diagnostics contain no extracted content or credentials.

#### T7.3 — Product release gate

The program is complete only when:

- clean first run reaches Chat without terminal intervention after the runtime
  itself is installed;
- the gateway renders setup with zero models;
- one fake-model end-to-end test covers download, verification, activation,
  chat, and restart;
- one real supported model passes measured download/activation and chat smoke
  tests;
- one folder Chutni completes, reconnects, refreshes a change, retrieves useful
  evidence, and cites it;
- the packaged pinned Gigatoken adapter passes exact tokenizer parity,
  offline/local-only, cancellation, resource, license/SBOM, and target-platform
  gates;
- one supported native Qwen package proves real direct token-ID prefill, while
  one unsupported or deliberately mismatched package proves the safe bounded
  text fallback;
- irrelevant Chutni evidence is demonstrably withheld;
- browser reload and gateway restart preserve all durable operations;
- current Chat and Jobs regression suites remain green;
- accessibility, responsive, privacy, security, and scale gates above pass;
- documentation states supported platforms, browsers, file types, exclusions,
  disk requirements, model sizes, and known limitations without unmeasured
  claims.

---

## 8. Suggested workstream ownership

These tracks can run in parallel after Phase 0 contracts are frozen:

| Workstream | Primary tasks | Must coordinate with |
|---|---|---|
| Control plane | T0.4, T1.1–T1.2 | Model manager, frontend |
| Runtime packaging | T1.0 | Control plane, model manager |
| Bulk tokenization | T0.5, T4.3–T4.4 | Runtime packaging, Qwen engine, Chutni core |
| Frontend | T1.2–T1.4, T2.4, T3.* | Every API owner |
| Model manager | T2.1–T2.3 | Installer/packaging, frontend |
| Chutni core | T0.2–T0.3, T4.*, T5.* | Reader/OCR, frontend |
| Scale/release | T6.*, T7.* | All workstreams |

Do not implement the frontend against invented response shapes. Commit contract
fixtures first, then make both sides pass them.

---

## 9. Evidence and completion protocol for agents

For every task:

1. Mark the task in progress in the implementing change or tracking system.
2. State which locked contract it satisfies.
3. Add automated tests before claiming completion.
4. Run the narrow tests, then the relevant existing suite.
5. Save measured/manual evidence for real-model, browser, scale, or
   machine-pressure gates under `docs/regressions/ui-chutni/<task-id>/`.
6. Report files changed, tests run, results, and remaining limitations.
7. Do not mark a parent phase complete while any required acceptance item is
   unverified.

Suggested test entry points to add:

```text
make test-ui-setup
make test-model-manager
make test-chutni
make test-chutni-security
make test-chutni-scale
```

Ordinary CI tests must use tiny deterministic fixtures. Real model downloads,
large disk fixtures, thermal measurements, and multi-hour recovery tests are
explicit release gates, not hidden dependencies of every unit-test run.

---

## 10. Explicitly out of scope

- DMG packaging, code signing, notarization, Mac App Store distribution, or an
  Apple Developer membership.
- Electron, SwiftUI, AppKit, or any other native application shell.
- Bundling large model weights into the runtime download.
- Embeddings, vector databases, or claims of semantic search in v1.
- Cloud sync, multi-user accounts, remote Chutni storage, or telemetry.
- Automatic source-file organization, edits, moves, or deletion by Chutni.
- Always-on filesystem watchers as the source of truth. Watchers may later
  provide dirty hints; manifest reconciliation remains authoritative.
- Secret-content classification guarantees. V1 uses conservative path/type
  exclusions and must state their limits honestly.
- Silently indexing the whole computer.
- Treating Gigatoken as a filesystem crawler, parser/OCR system, persistent
  memory, search index, retriever, model runtime, or context-window extension.
- Loading an entire folder/drive into one model context or retaining an
  unbounded model KV cache for it. Large ingestion remains bounded map/reduce
  plus retrieval.
- Shipping Gigatoken's Python wheel, requiring a runtime Rust/Python toolchain,
  or allowing its Hugging Face Hub loader during Chutni work.
- Persisting corpus-wide model token arrays in v1.

---

## 11. Planning estimate

For one experienced engineer working mostly serially, the current planning
range is **8–14 focused engineering weeks**:

- browser shell, UI, profile, and model manager: 2–3 weeks;
- pinned/minimized Gigatoken adapter, parity, packaging, and safe native-Qwen
  token-ID handoff: 1–2 weeks;
- folder Chutni, retrieval, and freshness integration: 3–5 weeks;
- drive/This-computer hardening, migration, and measured release gates:
  2–4 weeks.

This is a planning range, not an acceptance criterion. Correctness, privacy,
recovery, and measured scale gates determine completion.
