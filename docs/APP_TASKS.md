# Samosa Chat app — task program

A lightweight local chat application on top of the existing engine: opens on
the user's machine, chats like ChatGPT/Claude, reads documents the user
points at, and can reach the internet. This document is the working plan;
every task carries acceptance criteria that are measured, not assumed, in the
tradition of the engine's own task program.

> **Phases A2 and A3 are extended and corrected by the issue task program**
> (2026-07-15). Read [ISSUE_TASKS.md](ISSUE_TASKS.md) before starting either.
> A2 (documents) → [TASKS_DOCUMENTS.md](TASKS_DOCUMENTS.md): A2.1's
> `textutil`/PDFKit design is macOS-only and has been replaced.
> A3 (internet) → [TASKS_INTERNET.md](TASKS_INTERNET.md): A3.3 is blocked on an
> unscoped API change, and A3.1's SSRF list has a correctness bug.

**Ground truth this plan builds on (measured, 2026-07-13):**

- Engine: `qwen36b` — one-shot CLI chat, streaming, EOS-bounded output,
  thinking mode, sessions (`QWSESS01`: ~70–100 MB snapshots, byte-identical
  resume, ~40 KB/token KV + fixed 63 MB DeltaNet state).
- Speed on the 16 GB MacBook Air M3 reference machine: decode 7–8 tok/s
  (2-thread cool default) / ~9.5 (4T); prefill ~14 (2T) / ~24 tok/s (4T).
  **Prefill is the document-chat constraint**: a 5,000-token document costs
  ~3.5–6 minutes to read once. Sessions make it pay-once-per-document.
- RAM: the resident app's macOS physical footprint measured 2.51 GiB fresh and
  4.07 GiB after a real two-turn continuation. KV grows ~40 KiB/token; the
  enforced 24,576-token total-context cap bounds that variable component to
  ~960 MiB. One conversation state is resident at a time. Zero new swap remains
  a standing guardrail.
- Context safety: contexts > 4,096 tokens are exercised and safe as of the
  2026-07-13 `attention_gqa` fix; long-context generation coverage is still
  thin (see A0.4).
- Thinking control: general mode defaults to 1,024 internal tokens and
  precise-code to 2,048, both inside the 8,192 outer ceiling. Budget exhaustion
  appends Qwen's published natural-language early-stop transition rather than
  a bare `</think>` token. A recalibrated 933-token group-32 arithmetic run
  closed naturally and passed; broader quality is a parity gate, not an
  app-development blocker.
- Upstream colibrì has a GLM-side reference for a persistent serve mode:
  OpenAI-compatible stdlib-only HTTP gateway, bounded FIFO admission with
  429/503, and isolated per-slot KV contexts (upstream commits #21/#28/#29).
  Port the pattern, not the code — the Qwen engine's session machinery
  replaces GLM's KV persistence.

**Product principles (user-set):** lightweight — no Electron, no framework,
no build system; dependency-free C server + single-file web UI; cool 2-thread
default with an explicit fast toggle; local-only by default (bind 127.0.0.1;
the internet features reach out, nothing reaches in); technical, plain UI
copy.

---

## Phase A0 — Serve mode: the engine becomes a resident process

The CLI pays model load + tokenizer parse per invocation. The app needs the
model resident once, serving turns.

### A0.1 `samosa serve`: persistent process with an HTTP API  ~2–3 days

**Status (2026-07-14): implemented and bounded-real tested.** The resident
loopback server, SSE/non-streaming responses, FIFO admission, 429/503 behavior,
cooperative cancellation, shared tokenizer/model cache, health telemetry, and
clean shutdown are in-tree. Component coverage includes 20 sequential socket
connections without RSS drift. Real-model checks cover streaming, cancellation,
queue-full rejection, and shutdown. The 15-minute real-model soak remains a
release gate, as do exact engine/artifact fingerprints in `/healthz`; the
current endpoint reports the model identifier but not those hashes. The soak
was deliberately not substituted with thousands of decode tokens during
implementation.

Add a serve mode to `qwen36b` (or a thin C server binary linking the same
objects): load model once, accept HTTP on `127.0.0.1:<port>` (default 8642),
stdlib sockets only.

Endpoints:
- `POST /v1/chat/completions` — OpenAI-shaped, `stream: true` via SSE;
  supports `temperature/top_k/top_p/seed`, a `thinking` switch (maps to the
  chat template), `thinking_budget`, and `max_tokens` defaulting to the 8,192
  safety ceiling (EOS-bounded, never a creative constraint). Stream telemetry
  reports natural closure versus Qwen budget transition versus repetition
  guard separately.
- `GET /v1/models`, `GET /healthz` (returns engine hash, model hash, RSS,
  tok/s of last turn).
- One generation at a time; a small bounded FIFO queue (port the upstream
  429/503 semantics: queue full → 429, shutting down → 503).

Concurrency model: single compute thread pool (the engine's own OMP);
socket accept loop + queue on a separate thread; no locks around the model —
turns are strictly serialized.

Cancellation is part of the server contract, not deferred to the UI: client
disconnect, `POST /v1/cancel`, and clean shutdown set a cooperative flag that
the engine checks between generated tokens.

**Acceptance:** `curl` streams a completion; 20 sequential requests leak no
RSS (< 1% drift); standing tiny oracles and the frozen-baseline comparison
are unaffected (serve code compiled but idle in oracle mode); guardrails
hold in a 15-minute serve soak driven by scripted requests (zero swap,
writes < 100 MB/h, thermal ≤ moderate at the 2T default).

### A0.2 Conversation slots on the session machinery  ~2 days

**Status (2026-07-14): partially implemented.** `conversation_id` uses sealed,
atomic `QWSESS01` snapshots, and two real HTTP turns verified exact restore
without history re-prefill. The four-slot in-RAM LRU, pressure eviction, write
batching, and four-conversation soak are still open; the current developer
preview restores the snapshot from disk on every later turn.

Per-conversation state without re-prefill: keep N in-RAM conversation slots
(N=4 default; each ≈ KV bytes of its context + 63 MB state). API:
`conversation_id` on the completions call; slot hit → continue exactly
(the byte-identical continuation property is already proven); slot miss →
restore from the conversation's `QWSESS01` snapshot on disk; snapshot
written on turn end (atomic tmp+fsync+rename, batched — write-hygiene
guardrail applies).

Eviction: LRU over slots; RAM budget for slots is explicit and reported in
`/healthz`; the engine's memory-pressure reflex must also drop cold slots
(WARN) before the OS swaps.

**Acceptance:** turn 2 of a resumed conversation starts decoding in < 2 s
(no prompt re-prefill — measure TTFT); slot eviction + snapshot restore
produces byte-identical continuations (extend the T4.4 A/B/C test across
the HTTP path); 4 interleaved conversations soak 15 minutes within RAM
budget and zero swap.

### A0.3 `samosa app` launcher  ~0.5 day

**Status (2026-07-14): implemented in-tree.** Double launch retained one PID,
the browser URL opened through the launcher, and `samosa serve --stop` shut the
real server down cleanly. The staged installer includes the new server header
and pthread build flag. The fresh/upgrade installer integration and its
corrupt-staging rollback both pass.

`samosa app`: starts serve if not running (single-instance lock file),
opens `http://127.0.0.1:8642` in the default browser, prints the URL.
`samosa serve --stop` stops it. No launchd/autostart in v1.

**Acceptance:** double invocation does not double-start; stop is clean
(snapshots flushed); works from a fresh installer run.

### A0.4 Long-context generation test (debt from the 4096 fix)  ~0.5 day

**Status (2026-07-14): safety cap implemented; long-context regression still
open.** The server now tokenizes and rejects any turn whose saved history + new
turn + requested generation can exceed 24,576 tokens, before KV allocation or
stream headers. Pure boundary tests cover the cap arithmetic. The acceptance
design below still requires a tiny attention fixture plus bounded real prefill;
neither long-context arm is being claimed as complete.

The stack-overflow class was invisible to every existing test. Add a tiny
fixture that crosses shrunken equivalents of both boundaries on every test
run. The real-model arm should cross 4,096 and 8,192 **total context primarily
through bounded prefill**, then generate only enough tokens to prove the fixed
path and stats line. Do not generate 8,192 real-model output tokens locally:
the measured 933-token control already requested 376.77 GB of expert reads.

**Acceptance:** the tiny case fails on the pre-fix binary and passes on current;
the bounded real-model prefill/generation case exits cleanly under the standing
machine guard; both are documented release requirements, while only the tiny
case runs in ordinary `make test`.

---

## Phase A1 — The chat app itself

### A1.1 Single-file web UI  ~3–4 days

**Status (2026-07-14): core interactive slice implemented and bounded-real
tested.** The 32 KB single-file UI is served directly by the C process and
uses no framework or external request. It includes responsive conversation
navigation, browser-local transcript persistence, SSE answer/reasoning
streaming, stop/cancel, direct/general/code settings, seed and token ceilings,
safe lightweight markdown rendering, and speed/RSS/closure telemetry. Together
with the 149 KB transparent logo the served payload is 181,552 bytes. A real
group-32 request returned the exact requested sentence, stopped naturally,
saved its session, decoded at 5.13 tok/s, and peaked at 3.28 GB RSS. Open work
inside the original acceptance is keyboard/Safari manual review, fast/cool
switching, server-side rename/delete, and the 1,000-message jank test. Streaming
auto-scroll follows only while the reader remains near the bottom; scrolling
up pauses follow mode and exposes a `Latest` control.

One `app.html` served by the C server at `/`. No framework, no build step,
no external requests (CSP: `default-src 'self'`). Contents:

- Conversation sidebar: list, create, rename, delete (delete asks; removes
  snapshot + transcript).
- Message view: streaming tokens as they arrive (SSE), markdown rendering
  (small hand-rolled renderer: headings, lists, bold/italic, links, tables,
  fenced code with a copy button — no external highlighter; monospace block
  is enough), a visible-but-collapsed "thinking" section when thinking mode
  is on.
- Composer: Enter sends / Shift-Enter newline; stop button (uses the A0.1
  cooperative cancellation endpoint); regenerate last answer.
- Footer telemetry, always visible (the product's honesty signature):
  tok/s of the current generation, engine RSS, model-on-disk size, and
  fast/cool mode indicator.
- Settings drawer: thinking off/general/precise-code, fast/cool (maps to the
  engine's Qwen task profiles and thread count;
  cool is default per the standing user preference), seed field (blank =
  random), offline mode (disables Phase-A3 features globally).

**Acceptance:** total payload served < 200 KB; works in Safari and Chrome
current; keyboard-only operation possible; a 1,000-message transcript
scrolls without jank (virtualize or paginate if not).

### A1.2 Conversation persistence  ~1 day

Per conversation, under `~/.samosa/chats/<id>/`: `transcript.jsonl`
(append-only, one message per line: role, text, timestamps, gen stats,
thinking text if any) + `session.qws` (latest snapshot) + `meta.json`
(title, created, doc/web attachments list). Title auto-generated from the
first user message (first 40 chars — no model call). Full-text search
across transcripts (server-side substring scan is fine at this scale).

**Acceptance:** app restart and machine reboot restore the full list and
any conversation continues instantly from snapshot; disk writes during a
1-hour chat stay < 100 MB/h including transcripts (batch fsync per turn,
not per token); deleting a conversation leaves no orphan files.

### A1.3 App-path quality soak  ~0.5 day

Drive the full stack (browser → server → engine) with a scripted 15-minute
mixed session (new chats, resumes, stops, regenerates) on AC.

**Acceptance:** all four standing guardrails green; no 5xx; no stuck slot;
TTFT and tok/s within 10% of the CLI path (the HTTP layer must be free).

---

## Phase A2 — Documents

Design position: **sessions are the document feature.** Reading a document
once into a conversation snapshot amortizes the prefill cost forever —
this is the architecture's genuine advantage; retrieval exists only for
documents that exceed the context budget.

### A2.1 Extraction  ~1–2 days

`POST /v1/documents` with a local path (from the UI file picker). Extract
text: `.txt/.md/source code` natively; `.docx/.rtf/.html` via
`/usr/bin/textutil` (ships with macOS); PDF via a ~50-line helper using
PDFKit, compiled by the installer with the CLT toolchain (fallback: clear
error naming the unsupported type — never silent garbage). Normalize
whitespace, keep page/paragraph markers for citations. Token-count the
result with the real tokenizer and report it to the UI before ingestion
("this document is 8,400 tokens — reading it will take about N minutes")
with measured, not optimistic, N.

**Acceptance:** extraction matrix over a 12-file corpus (2 each: txt, md,
pdf text-based, pdf scanned → must fail loudly not silently, docx, html);
no temp files left behind; a 100 MB file is rejected with a clear message
(size cap, default 20 MB).

### A2.2 Full-document ingestion into a session  ~2 days

Documents up to the context budget (default 24K tokens ≈ 1 GB KV; hard cap
configurable) are prefilled whole into the conversation: system-framed
header (`Document: <name>` + content) + user's question. Ingestion runs at
the fast thread setting by default (it's bounded work, worth the heat —
still user-overridable) with a progress bar fed by real prefill telemetry
(tokens done / total, tok/s, ETA). The resulting snapshot IS the document
session: every follow-up question costs zero re-reading, even after reboot.

**Acceptance:** 10-page text PDF (~6K tokens): ingestion completes with
accurate ETA (±20%); 10 scripted comprehension questions answered with doc
content (manual spot-check rubric, ≥8/10 grounded); follow-up TTFT < 2 s;
snapshot survives reboot; two documents in one conversation compose.

### A2.3 Retrieval mode for oversized documents  ~2–3 days

Documents beyond the budget: chunk (~800 tokens, overlap 120, split on
paragraph boundaries), lexical index (BM25 in C — no embedding model
exists locally; do not pretend otherwise). Per question: retrieve top-k
(k=6) chunks, prefill question + chunks into the session for that turn,
**show the used chunks as citations in the UI** (file + page/paragraph).
Retrieval-turn prefill ≈ 5K tokens ≈ 3.5–6 min at 2T / ~2–3.5 min at 4T —
surface this honestly in the UI before the user commits.

**Acceptance:** 100-page PDF: index builds < 30 s; 10-question grounding
spot-check ≥ 7/10 with correct citations; a question whose answer is absent
from the document is answered with an explicit "not in this document"
(system-prompt requirement, spot-checked).

### A2.4 Known-limitation surfacing  ~0.5 day

Document answers inherit the int4 doubling artifact and any model
hallucination. UI shows a one-line standing notice on document
conversations ("answers are grounded in retrieved text; verify citations").
No overclaiming in any copy.

---

## Phase A3 — Internet access

Design position: the **app** reaches the internet; the model consumes what
the app fetched. v1 is user-initiated (deterministic, safe); v2 is
model-initiated tool calls behind an approval gate.

### A3.1 URL ingestion (user-initiated)  ~1–2 days

Paste a URL in the composer (or `/read <url>`): server fetches (redirect
limit 5, timeout 20 s, size cap 5 MB, `text/html|text/plain|pdf` only),
extracts readable text (strip script/style/nav; a ~200-line heuristic
extractor is enough — title, headings, paragraphs), then ingests exactly
like a document (A2.2/A2.3 path chooses by size) with the URL as citation.

Security requirements (non-negotiable): resolve-then-connect with private
address rejection (127.0.0.0/8, 10/8, 172.16/12, 192.168/16, 169.254/16,
::1, fd00::/8 — block SSRF), no cookies, no auth forwarding, a distinct
User-Agent, and every fetch logged visibly in the conversation.

**Acceptance:** SSRF suite (10 crafted URLs incl. redirects to localhost)
all rejected; 5 real article URLs ingest and answer grounded questions;
offline mode blocks the path entirely with a clear message.

### A3.2 Web search (user-initiated)  ~1–2 days

`/web <query>`: pluggable backend, config in `~/.samosa/config.json`.
Default: **none configured** → the command explains how to set one (honest:
no key-free search API is dependable enough to hardcode). Supported
backends v1: SearXNG instance URL (self-hosted/public), Brave Search API
key. Flow: search → show top 5 results in-UI → user picks (or `auto` takes
the top result) → A3.1 ingestion of the picked pages → answer with source
citations.

**Acceptance:** with a configured backend, 10-query suite produces answers
citing fetched pages; with none, the UX degrades to instructions, never a
silent failure; all network activity is visible in the transcript.

### A3.3 Model-initiated tool calls (v2, explicitly gated)  ~3–4 days

Qwen3.6's chat template supports tool definitions and `<tool_call>` JSON.
Implement: tool schema injection for `{web_search, fetch_url,
read_document}`, a strict parser for tool-call spans in generation (abort
the span cleanly on malformed JSON), execution through the SAME code paths
and security gates as A3.1/A3.2, result injection as tool messages, loop
cap (3 calls/turn), and a per-conversation approval mode: `ask` (default —
each call shows a confirm chip in the UI) or `auto`.

**Gate before this ships:** with tools disabled, output must remain
byte-identical to pre-A3.3 behavior (the template injection must be
strictly opt-in); with tools on, a 15-task suite (current-events lookup,
doc+web synthesis) succeeds ≥ 10/15 with zero security-gate violations.
If the int4 model's tool-call JSON reliability is poor (measure malformed
rate; > 20% malformed = no-go), ship A3.1/A3.2 only and say so in the
README — a negative result is a result.

---

## Phase A4 — Packaging and release

### A4.1 Installer integration  ~0.5 day
`install.sh` gains the server/UI files (checksums as always), and the smoke
test also starts serve, hits `/healthz`, and one-shot completes a prompt.

### A4.2 Format versioning  ~0.5 day
`transcript.jsonl` schema version field; `QWSESS01` already versioned;
`config.json` versioned; document index format versioned. Refuse newer
versions loudly; migrate older ones or state plainly that migration is
manual.

### A4.3 Release gates (all measured on the reference machine)
- Fresh `curl | sh` → `samosa app` → first answer, on a clean account.
- 15-min app soak: guardrails green (zero swap, < 100 MB/h writes incl.
  transcripts, thermal ≤ moderate at cool default).
- Document E2E: PDF in, grounded answers, reboot, instant resume.
- Internet E2E: URL ingest + (if configured) search, SSRF suite green.
- README/model card updated with app screenshots and honest performance
  numbers; known-limitations section current.

### A4.4 Second-machine validation  ~when hardware is available
Everything above on a Mac that is not the dev machine (the M1/M2 16 GB
audience). Until then the README says "tested on one machine" — keep it
true.

---

## Non-goals (v1)

Remote/multi-user access (server binds loopback, full stop); accounts;
plugins; auto-update; vision input; Windows/Linux app (engine gate first —
see platform notes in the main README); model switching; embeddings-based
retrieval (no local embedder — BM25 is the honest tool at this size).

## Suggested order and effort

A0.1 → A0.2 → A1.1/A1.2 (usable chat app, ~1.5 weeks) → A0.4 + A1.3 gates →
A2.1 → A2.2 (document sessions, the flagship feature) → A2.3 → A3.1 → A3.2
→ A4 release pass → A3.3 last, behind its own go/no-go. Roughly 3–4 weeks
of focused work to a releasable document-capable app; internet tool-calling
is the only research-risk item and is deliberately last.
