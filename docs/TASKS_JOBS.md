# Issue #7 — Samosa Jobs (batch, scheduled, local multimodal work)

**Status: J1 implementation is landed on `issue-7-jobs`; its offline suite is
green. E-J1 is closed for the labeled JSS PDF batch and the live chat interlock.
Single-image extraction now produces passing records — Qwen vision read a real
rendered JSS page and returned a valid object (title + journal correct); the two
structural blockers (Qwen thinking not disabled; ```json-fenced output not
recovered) are fixed, see
[`qwen-image-thinking-fix-2026-07-22.md`](regressions/jobs/e-j1/qwen-image-thinking-fix-2026-07-22.md).
Still open: full-resolution field accuracy (small text needs a full-res image,
~8 min/inference on the 16-GiB host) and multi-image/page reduction.** The
compiled definition route now emits active inference timing and has offline
interlock coverage; see
[`compiled-interlock-telemetry-2026-07-22.md`](regressions/jobs/e-j1/compiled-interlock-telemetry-2026-07-22.md).
The 2026-07-16 whole-file long-PDF preview hit unsafe memory pressure on the
16-GiB reference host; see
[`pdf-preview-aborted-2026-07-16.md`](regressions/jobs/e-j1/pdf-preview-aborted-2026-07-16.md).
The later 2026-07-22 run rebuilt/installed with the reviewed PDFium archive and
completed all four labeled JSS PDFs through the compiled `/v1/jobs/definition/*`
routes on Ornith: 4/4 `passed`, 46/48 fields correct, 0 review/failed records,
and zero swap/throttled pages; see
[`jss-pdf-ornith-2026-07-22/`](regressions/jobs/e-j1/jss-pdf-ornith-2026-07-22/)
and the superseded setup note
[`ornith-metadata-2026-07-22.md`](regressions/jobs/e-j1/ornith-metadata-2026-07-22.md).
The clean live interlock rerun completed the same four PDFs with an interactive
chat opened mid-batch: 4/4 `passed`, 44/48 fields correct,
`active_inference_seconds=124.93`, and one observed
`job_paused`/`job_resumed` pair; see
[`jss-pdf-ornith-interlock-clean-2026-07-22/`](regressions/jobs/e-j1/jss-pdf-ornith-interlock-clean-2026-07-22/).
No broad image or multi-image reduction acceptance claim is implied until the
vision-backend run passes. A narrow Qwen image smoke reached the
vision-capable backend through the compiled route but returned
`review_required`/`invalid_model_output`; see
[`qwen-image-smoke-2026-07-22/`](regressions/jobs/e-j1/qwen-image-smoke-2026-07-22/).
Claims about the *current* engine, by contrast, are verified below with
`file:line` evidence.

This card is written to be executed by an agent with **no prior context on this
repo**. Every task states its goal, its exact interface (file formats, request
shapes), its definition of done, and a **runnable test with expected output**.
Where a design choice existed it has been made and recorded — do not reopen a
resolved decision; implement it.

Program bar (see [ISSUE_TASKS.md](ISSUE_TASKS.md)): acceptance criteria are
measured, a negative result is a result, "should work" is not a status, and no
claim is scoped wider than what was run. Read [ISSUE_TASKS.md](ISSUE_TASKS.md)
first. This card **absorbs the internet work** — [TASKS_INTERNET.md](TASKS_INTERNET.md)'s
verified security groundwork is reused, not replaced (§J3) — and sits on top of
the document extractor from [TASKS_DOCUMENTS.md](TASKS_DOCUMENTS.md) (#5) and the
vision tower from [TASKS_VISION.md](TASKS_VISION.md) (#3, landed). The general
document-reading tool (`doc.read` — OCR cascade, handwriting escalation, read
cache, `low_confidence_read` wiring) is a **Tools-layer contract specified
separately in [TASKS_READER.md](TASKS_READER.md)**, not part of this card.

## Start here — handoff preflight

This card is the primary spec but **not** self-contained. Before writing code:

### Read, in this order
1. **This card**, end to end.
2. **[ISSUE_TASKS.md](ISSUE_TASKS.md) — mandatory.** The Working Agreement
   (branch-per-issue, definition-of-done, evidence-not-assertion, commit
   conventions, run the RUN-FIRST experiment first) and the shared ground truth
   (model, two quant schemes, reference machine, prefill cost). The repo's rules
   live here.
3. **[CLAUDE.md](../CLAUDE.md)** — non-negotiables, the two-quant-scheme model
   facts, `build/test/run` commands, git rules. (Auto-loaded by Claude Code; read
   it directly otherwise.)
4. **[SERVE_API.md](SERVE_API.md)** — the exact serve request/response contract
   F-J8 defers to. Verify field names here **before** J1.4.
5. **[TASKS_HARDWARE.md](TASKS_HARDWARE.md)** H5/H6 (skim) — the host profile and
   machine-safety governor J1.13 reads; both now exist in code (below).
6. **The source the F-J findings cite** — `src/qwen36b.c`, `src/samosa_http.h`,
   `src/kernels.h`, `src/vision.c`, `src/tok.h`. **Verify each F-J at its
   `file:line` before building on it**, and read the serve request handler +
   session/snapshot code: the three engine additions are specified as
   *contracts, not C walkthroughs*.

### Repo state (main @ `f51fe39`, 2026-07-16)
- **Branch:** cut **`issue-7-jobs` from `main`**. Implementation lives there; the
  card / `ISSUE_TASKS.md` / `CLAUDE.md` stay on `main` (Working Agreement §1).
- **#3 vision: LANDED** on main (single-image — F-J4). **#5 PDF/text core: LANDED**
  as `samosa-extract`: PDFium text/page metadata, bounded PPM rendering, native
  UTF-8 text, and optional exact Qwen tokenizer counts. DOCX remains deferred;
  HTML waits for #4's shared extractor; RTF is explicitly unsupported. The Jobs
  adapter must use the sidecar when it is installed/on `PATH`, retaining its
  existing controlled review result when it is absent.
- **Host profiler + gated x86 SIMD merged** (`8b3e813`): `host_profile_init()` /
  `g_host` / the `[host]` startup line exist — **J1.13 reads `g_host`**, do not
  re-derive. (x86 AVX2/VNNI is opt-in and unvalidated; irrelevant to Jobs, just
  don't be surprised by `[simd] path=scalar` on x86.)

### The three engine additions are contracts, not code
`samosa tokenize --count`, `GET /internal/v1/status`, `X-Samosa-Priority:
background` (§"Engine additions") are specified by interface only. Implementing
them is real C in `qwen36b.c` / `samosa_http.h` / `tok.h`; read those first. They
are additive and read-only — **a run with no jobs must stay byte-identical** to
today (the same gate #3/#4 use).

### #5 PDF-metadata adapter contract

`samosa-extract --json PATH --tokenizer TOKENIZER` returns `text_layer`, whole
document `text`/`tokens`, and page objects with `{index, text, tokens,
has_raster_figure}`. The Jobs adapter maps `tokens` to its `text_tokens` field:

```
extract_meta(path) -> { "text_layer": bool,
                        "pages": [ {"index": int, "text_tokens": int, "has_raster_figure": bool}, … ] }
```
`needs_image` (J1.2) = `text_tokens < LOW_TEXT_TOKENS OR has_raster_figure`. If
the sidecar binary is absent, J1.2 and J1.3 retain
`review_required reason:"extractor_unavailable:application/pdf"`; this is clean
capability degradation, not a host-tool fallback. Page rendering uses
`samosa-extract --render-ppm PATH PAGE OUTPUT.ppm`.

### Scaffolding to create (does not exist yet)
- `tests/jobs/` — start with `fake_serve.py` (§Test harness).
- A **`jobs-test`** Makefile target running `tests/jobs/*`; J1 is gated on
  `make jobs-test` exit 0. Keep it separate from `make test` (which stubs the C
  engine) and say which.

### Do NOT touch
- The web app / `app_html_path`: J1 is **CLI + a static `view.html` only**. The
  interactive "Jobs tab" is J2.

### Recommended first increment
`fake_serve.py` → `samosa tokenize --count` → J1.0 → J1.1 → J1.2 → … → J1.13, each
against its offline test, until `make jobs-test` is green → then **E-J1** on the
real model (respecting machine-safety, HR-6/J1.13).

## Decisions locked (do not reopen)

- **Durable state = append-only JSONL** with a per-job process lock and event
  sequence numbers, not SQLite (§Durable state).
- **Implementation language = Python 3, standard library only** (no `pip`).
- **Default unit granularity = `auto`** — software decides per-file vs per-page
  deterministically (§J1.2), forced to per-page where the single-image engine
  (F-J4) or the context cap requires it.
- **Resource use adapts to the host, and is not hardcoded** (§HR-6): thread count
  and aggressiveness derive from detected RAM / performance cores / power source.
  The 16 GB → 2-thread reference config is the **measured floor**, not a cap; no
  higher tier may be described as "fast" until it is measured.
- **Unattended safety is enforced in J1** by a resource gate + chat interlock
  (§J1.13), not deferred to J2.
- **Results surface = a dedicated Jobs view** — static, fully-escaped HTML in v1
  (§J1.12), interactive (live/create/pause) with the J2 daemon.
- **Filesystem post-actions exist, and they never delete (owner, 2026-07-19).**
  Jobs may create directories and move files inside one user-granted root, via
  the organize stage (§Phase JO). The runner contains **no delete path** for
  user files: a move is a no-clobber atomic rename, cross-volume moves are
  refused (a copy-then-delete "move" smuggles a delete), and every applied move
  is journaled and reversible with `undo`. Plans are shown and approved before
  anything is touched. Do not reopen; the exact contract is §Phase JO.
- **Product surface stays browser + headless server (owner, 2026-07-19).** No
  dmg/Electron/exe shell: the capability lives in the server, a bundled
  Chromium costs more RAM than a tab, and there is no native Windows port (#2
  is Docker). Long jobs run with **no UI open**; completion is announced by a
  local notification. An optional native **menu-bar shim** (small `.m`, polls
  serve over loopback — the [metal_expert.m](../src/metal_expert.m) pattern) is
  future polish, not a prerequisite. UI direction: [UI_DESIGN.md](UI_DESIGN.md).

## Engine additions J1 requires (small, additive, read-only)

J1 cannot be done by an out-of-process runner alone; three minimal engine
surfaces are needed. Each is additive, loopback-only, and does not change
existing behavior (so it respects the vision/tool "byte-identical when unused"
gate other cards set):

1. **`samosa tokenize --count <file>` / `--count-stdin`** — exact token count from
   the already-loaded tokenizer. Needed because a character heuristic is not a
   safe context bound (§J1.2). Deterministic; unit-testable without the 24 GB
   model.
2. **`GET /internal/v1/status`** (loopback only) → `{"interactive_active": bool,
   "last_interactive_ts": <rfc3339>, "queue_depth": int, "inference_busy": bool,
   "threads": int}`. Needed for the chat interlock and for the post-timeout
   "wait until the slot clears" step (§J1.13, §J1.4). Read-only; no model state
   change.
3. **A low-priority admission class for job requests** — a request header/flag
   (e.g. `X-Samosa-Priority: background`) that makes `serve_scheduler_acquire`
   (F-J1) yield to any interactive request. Optional for the very first cut (the
   interlock in §J1.13 can gate coarsely without it) but strongly preferred; if
   omitted, say so and rely on the interlock alone.

Serve already returns the two error signals the runner depends on: **`400
context_limit`** ([qwen36b.c:4848](../src/qwen36b.c#L4848)) and **`429
queue_full`** ([:4853](../src/qwen36b.c#L4853)). Treat both as authoritative.

## The bet

Samosa's one structural weakness is speed: ~5–7 tok/s decode and, more
bindingly, **prefill** — a 5,000-token artifact costs ~3.5–6 minutes to read
once ([ISSUE_TASKS.md](ISSUE_TASKS.md), verified 2026-07-14). Interactive chat is
the single arena where local+slow always loses to a hosted API, because there
latency _is_ the product.

Samosa Jobs picks the opposite job shape — **work where wall-clock time does not
matter** — and there the weakness disappears and the strengths carry:

- **No metered API fee.** Recurring, high-volume work is where hosted per-token
  cost accumulates. "Slow, but no per-token charge, and it runs while I sleep" is
  a genuine trade. It is **not** free — see Hard requirement 7.
- **Private.** Resumes, receipts, financial and medical paperwork are the
  documents people least want to paste into a hosted model. Local is the feature.
- **Multimodal.** With vision landed (#3), a job can process photographs,
  screenshots, scans, and charts — not just extracted text.

Positioned precisely: **not an autonomous agent.** A *bounded workflow engine* —
folder in, repeatable task, checkpointed progress, deterministic validation,
reviewable artifacts out. Agentic behavior is deferred (§Non-goals).

**Why the bounded shape is also the UX decision (recorded 2026-07-16).** A
plan→tool→observe→replan agent loop is the worst possible shape for this engine:
every turn re-prefills a growing context at ~14–24 tok/s, so a 10-step LLM-planned
job pays hours of prefill overhead before any useful work, on plans a 35B-A3B q4
model produces unreliably. The map-shaped job — N inputs × one bounded model call
each, deterministic reduce — is the only shape where "slow but local and
unmetered" wins. Generality comes from **more intents on the same shape** (and
deterministic post-actions over validated fields, e.g. rename-by-extracted-date
— now specified as **§Phase JO**), never from runtime planning. Pitch wording must match (HR-7): not "the model
breaks the request into steps" — honest phrase: **"Samosa turns your description
into a reviewable job definition, then executes it deterministically."**
Reviewable-and-repeatable is the claim hosted agents cannot make; lead with it.
The plain-English entry point is real but lives at *definition time*
(`suggest-job`, §J2), one supervised model call the user edits and previews —
not at runtime.

**The actor split (recorded 2026-07-16) — who does what; do not blur these:**

| Actor | Does | Never does |
|---|---|---|
| **Runner** (the compiled gateway `src/samosa_gateway.c` + `samosa-fs` sidecar — deterministic C, no AI; the original `samosa_jobs.py` prototype was retired at Gate 11) | Reads files, magic-byte typing, hashing, planning units, building prompts, validation, joins/merges, the event log | Judgment calls; nothing here consults the model |
| **Intent author** (a human, at development time) | Decomposes a task class into its shape **once** — e.g. Reconcile = extract-each → deterministic join on declared match rule → exception report — tests it, ships it as a template | — |
| **Qwen, definition time** (`suggest-job`, one supervised call) | Intent *selection* + parameter *filling*: maps the user's English onto a shipped intent, proposes schema/rules; user edits and previews | Inventing a new workflow shape |
| **Qwen, runtime** | Answers one bounded, pre-built prompt per unit (read this receipt, summarize this page) | Touching raw files, choosing tools, planning, deciding what "done" means |

The model never reads the filesystem — by the time it is involved, the runner
has prepared one tidy prompt (instruction + schema + extracted text or one
decoded image). Task *understanding* flows: human author → shape (once) → Qwen
matches words to a shape and fills blanks (per job) → deterministic code
executes (per run). Corollary: **a request that fits no shipped intent gets an
honest "no job shape for that yet" — never an improvised plan.** The intent
library growing is the generality roadmap; a confidently wrong plan discovered
eight hours later is the failure mode this split exists to prevent. Pitch
accordingly: "Samosa recognizes which of its proven workflows fits your
request, configures it from your description, and shows you the plan before
running it" — not "Samosa plans the work."

## Verified codebase foundations

Verified by source read, 2026-07-16 (`main`):

- **F-J1 — Requests are already serialized behind a bounded admission
  scheduler.** `serve_scheduler_acquire()` grants exclusive model admission or
  returns `queue_full` (429) / `shutting_down` (503); `serve_scheduler_release()`
  frees it ([qwen36b.c:4852-4880](../src/qwen36b.c#L4852-L4880)). The worker
  **preserves one-inference-at-a-time**; non-model work may pipeline.
- **F-J2 — Cooperative cancellation exists.** `atomic_int *cancel_flag`
  ([qwen36b.c:3119](../src/qwen36b.c#L3119)), surfaced as `stats->cancelled`;
  serve wires `ctx->cancel` ([:4829](../src/qwen36b.c#L4829)). Client abort of the
  connection is the trigger the runner uses (§J1.4).
- **F-J3 — Sessions are sealed, resumable snapshots.** `session.qws` save/resume,
  revalidated under exclusive admission
  ([qwen36b.c:4831-4867](../src/qwen36b.c#L4831-L4867)); KV ~40 KB/token.
- **F-J4 — Vision is single-image, by construction.** One `vision_pixels` buffer
  ([qwen36b.c:249](../src/qwen36b.c#L249)); a second image part frees/replaces the
  first ([:4683](../src/qwen36b.c#L4683)); freed+nulled after every turn
  ([:4883](../src/qwen36b.c#L4883)). Makes the planner (J1.2) sometimes *forced*.
- **F-J5 — Request body cap is 4 MiB.** `SAMOSA_HTTP_MAX_BODY (4u << 20)`
  ([samosa_http.h:20](../src/samosa_http.h#L20)). Governs J1.4's oversize rule.
- **F-J6 — Portable readers already chosen.** Images: vendored `stb_image`
  ([stb_image.h](../src/stb_image.h)). PDFs: the pdfium sidecar (#5), which
  rasterizes pages. The LLM never parses PDF/image bytes; sidecars do.
- **F-J7 — Serve caps `max_tokens` at 8192; exposes last-run stats; there is no
  live status route today** ([qwen36b.c:4795-4799](../src/qwen36b.c#L4795-L4799),
  `ctx->last_stats` [:4879](../src/qwen36b.c#L4879)) — hence the additive
  `/internal/v1/status` above.
- **F-J8 — The serve request contract** (authority: [SERVE_API.md](SERVE_API.md)).
  `POST /v1/chat/completions`, OpenAI-shaped; uses **the first system + the last
  user message only** ([qwen36b.c:4073-4081](../src/qwen36b.c#L4073-L4081) via
  [TASKS_INTERNET.md](TASKS_INTERNET.md) F3); accepts `thinking`, `seed`,
  `temperature`, `top_p`, `top_k`, `max_tokens` (≤8192), `stream`, optional
  `conversation_id`; user `content` may be a string or an array of `{"type":
  "text"}` / `{"type":"image_url","image_url":{"url":"data:<mime>;base64,…"}}`.
- **F-J9 — The context limit is enforced server-side and returns `400
  context_limit`.** `SAMOSA_MAX_CONTEXT_TOKENS = 24576`
  ([qwen36b.c:3564](../src/qwen36b.c#L3564)); preflight rejects over-budget turns
  ([:4844-4850](../src/qwen36b.c#L4844-L4850)). The runner treats this rejection
  as **ground truth** and splits/retries (§J1.4), rather than trusting any
  client-side token estimate.

## Hard requirements — tight, and non-negotiable

1. **No authentication. Ever. Public sources only.** No logins/cookies/
   credentialed requests/paywalled content. Local files: the user's own,
   path-validated. Web (J3): only what a logged-out browser sees.
2. **The user supplies inputs; Samosa does not discover them.** Folder or explicit
   public-URL list. No crawling/discovery/cross-domain link-following in v1.
3. **Every web fetch goes through the SSRF-hardened path**
   ([TASKS_INTERNET.md](TASKS_INTERNET.md)); E-I2 suite 100% blocked for the job
   fetcher; polite (robots.txt, per-host rate cap, honest `User-Agent`, back-off).
4. **One model request at a time (F-J1).** Concurrency only in non-model stages.
5. **Output is reviewable, structured, and validated by software — not by the
   model's self-reported confidence.** Explicit schema + domain checks; failures/
   cross-attempt disagreement/truncation → **REVIEW_REQUIRED**. The morning review
   is the product.
6. **Resource use adapts to the host; unattended safety is enforced, not
   assumed.** The runner **derives** its thread count and aggressiveness from
   detected RAM, physical performance-core count, and power source — it does
   **not** hardcode a number. The 16 GB M3 Air → 2 threads is the **only measured
   point** and is the conservative floor; higher tiers scale up but may **not** be
   described as "fast" until measured (this repo's non-negotiable). The runner
   enforces machine safety every unit via the resource gate + chat interlock
   (§J1.13): honor memory-pressure and thermal state, **pause rather than compete
   with an interactive chat**, respect AC/battery and free-storage policy. (The
   general host-adaptivity policy — the `host_profile()` and its measured tiers —
   is **[TASKS_HARDWARE.md](TASKS_HARDWARE.md) H5**; this card carries the
   jobs-scoped rule and the enforcement task, and the J1.13 gate should read H5's
   profile once it exists rather than re-deriving the budget.)
7. **Do not call it free; do not overstate model quality.** Honest phrase:
   **"local inference with no metered API fee."** Jobs spend electricity, SSD
   bandwidth (endurance is consumed by **writes**/swap, not streaming reads — see
   [CLAUDE.md](../CLAUDE.md) published-claim defect,
   [TASKS_HARDWARE.md](TASKS_HARDWARE.md) H1), battery, storage, and review time.
8. **Best-effort scheduling on a laptop, stated as such.** Define the
   missed-window policy; never claim reliability the hardware cannot provide.
9. **Vision only where visual information matters.** Route scans/charts/
   handwriting/screenshots/photographs to vision; everything else to text.
10. **Local-data confidentiality (jobs carry receipts, medical, financial data).**
    Job directories `0700`, result/log files `0600`; **no source content in
    general application logs**; the static view **HTML-escapes every
    interpolated value** (filenames, input paths, validation messages, model
    output, instructions, job names) and loads **no external resources**;
    provide an explicit `delete`/`archive` command and a defined retention rule
    for rendered PDF pages and base64 intermediates (deleted after the unit's
    terminal event unless the job opts to keep them).

## Architecture

The load-bearing decision (from F-J1/F-J3): **separate orchestration from
inference.** A jobs controller — `samosa-jobsd` (J2) — owns job state; `samosa
serve` stays the small resident model server. J1 ships this as a **one-shot CLI
runner**, not a daemon.

**Language — DECIDED: Python 3, stdlib only.** Orchestration is HTTP + file I/O +
JSON + subprocess to sidecars; the schema check is a **hand-rolled minimal
validator** (J1.5). Product packaging (embed in C vs. ship Python) is a J2 call.

**Per-item state machine** — recovery is explicit; abandoned `RUNNING` → `READY`
on startup:

```
DISCOVERED → INGESTING → READY → RUNNING → VALIDATING → COMPLETE
                                                  ↘ REVIEW_REQUIRED → COMPLETE
any active state → PAUSED · CANCELLED · RETRY_WAIT · FAILED
```

**Hybrid PDF (F-J6) + map-reduce (F-J4), chosen automatically (J1.2), reduced
deterministically where possible (J1.9).**

**Read-once/query-many asset snapshots** and a **prefill-only endpoint** are
**Phase 2** engine work, not J1.

## Durable state — JSONL (DECIDED 2026-07-16)

**Decision: append-only JSONL event logs + plain result files, guarded by a
per-job process lock and monotonic event sequence numbers. Not SQLite.**

**Rationale:** the project's first constraint is *"no framework, no build system,
no dependencies"* ([CLAUDE.md](../CLAUDE.md)); SQLite is a new dependency. J1's
write volume is trivial and single-writer (F-J1). An append-only log with `fsync`
is crash-safe at this volume; recovery is a linear replay, easy to test. **Revisit
only** if a job exceeds ~10^5 items or concurrent jobs need shared querying.

### Directory layout (one job)

```
<jobs_root>/<job_id>/
  job.json                          # immutable frozen definition (copied at arm time)
  job.lock                          # advisory flock held for the whole run
  events.jsonl                      # append-only event log — the source of truth
  results/
    items/<unit_id>.json                 # validated per-unit output
    items/<unit_id>.provenance.json      # provenance record
    pages/<unit_id>.json                 # per-page partials (split files)
    documents/<input_sha256>.json        # reduced document record
    review/<unit_id>.json                # copy of units needing review
    intermediates/<unit_id>.*            # rendered pages / decoded images (retention rule, HR-10)
    output.jsonl | output.csv            # merged output (deterministic order)
    view.html                            # static Jobs view (J1.12)
  preview/                          # preview-only artifacts, never confused with a real run
    result.json  provenance.json
```

`<jobs_root>` default `~/.samosa/jobs` (override `SAMOSA_JOBS_ROOT`), mode `0700`.
`<input_sha256>` = the file's SHA-256; `<unit_id>` = `<input_sha256>` (whole
file), `<input_sha256>#p<N>` (page N), `<input_sha256>#c<N>` (text chunk N).
Files stay on the filesystem; the log stores paths + hashes + metadata, never
blobs.

### Process lock (enforces the single-writer assumption)

`run`/`preview`/`arm` acquire an **advisory `flock`** on `job.lock` held for the
entire operation. If another process holds it, fail with exit 3 and
`error: job <job_id> is already being run by pid <n>`. This prevents two
`samosa jobs run job.json` invocations from racing on the same job.

### `job.json` (schema_version 1)

```json
{
  "schema_version": 1,
  "job_id": "receipts-2026-07",
  "name": "Receipt extraction",
  "created_at": "2026-07-16T18:00:00Z",
  "input": { "folder": "/abs/path", "recursive": true,
             "types": ["image/jpeg","image/png","text/plain","application/pdf"],
             "max_file_bytes": 26214400 },
  "unit": "auto",
  "instruction": "Extract the receipt fields. Return ONLY JSON matching the schema. Do not guess values that are not visible.",
  "reduce": { "mode": "deterministic", "model_fields": [] },
  "inference": { "thinking": "off", "seed": 11, "temperature": 0, "max_tokens": 512, "timeout_s": null },
  "output_schema": {
    "type": "object",
    "required": ["merchant","date","total","currency"],
    "properties": {
      "merchant": {"type":["string","null"]},
      "date":     {"type":["string","null"]},
      "subtotal": {"type":["number","null"]},
      "tax":      {"type":["number","null"]},
      "total":    {"type":["number","null"]},
      "currency": {"type":["string","null"],"maxLength":3}
    }
  },
  "validation": { "domain_rules": ["subtotal + tax ~= total"] },
  "output": { "dir": "/abs/path/results", "format": "jsonl" },
  "resources": { "max_attempts": 3, "run_on_battery": false,
                 "pause_when_user_active": true, "min_free_gb": 5 }
}
```

- `job_id`: `^[a-z0-9][a-z0-9_-]{0,63}$`. `unit`: `auto` (default) | `file` | `page`.
- `reduce.mode`: `deterministic` (default) | `model`; `model_fields` = narrative
  fields the model merges even in deterministic mode (J1.9).
- `inference.timeout_s`: `null` ⇒ the runner **derives** the timeout from the
  estimated prefill + output budget (J1.4), not a fixed 600 s.
- `resources.max_attempts` = **total attempts** (initial + retries), not "retries
  after the first." All paths absolute.

### `output_schema` — the exact supported subset (unknown keywords are REJECTED)

Supported keywords, and nothing else:

| Location | Keyword | Meaning |
|---|---|---|
| top level | `"type": "object"` | required |
| top level | `"required": [names]` | listed keys must be present |
| top level | `"properties": {name: rule}` | per-field rules |
| field rule | `"type": T` / `[T,…]` | `T ∈ {string, number, integer, boolean, null}` |
| field rule | `"enum": [values]` | value must equal one listed, **compared by JSON type** |
| field rule | `"minimum"` / `"maximum"` | inclusive numeric bounds |
| field rule | `"maxLength"` | string length bound |

**At job-validation time (J1.0), REJECT** — never silently ignore — any unknown
top-level or field keyword (a `maxLenght` typo must fail loudly, not disable the
rule), any nested object/array, and any unsupported type name. **Python type
traps the validator must handle:** `bool` is a subclass of `int` in Python — a
`True`/`False` must **not** satisfy `"type":"integer"`/`"number"`; and enum
comparison must observe JSON types, so `True` must **not** equal `1`.

### `events.jsonl` — one JSON object per line

Every line: `{"seq": <monotonic int>, "ts": <rfc3339 UTC>, "type": <…>, …}`.
`seq` starts at 1 and strictly increases; append order remains authoritative,
`seq` aids corruption detection and debugging. Required fields per type:

| type | required fields (besides seq/ts/type) |
|---|---|
| `job_created` | `job_id`, `job_sha256` |
| `item_discovered` | `input_sha256`, `input_path`, `media_type` |
| `item_planned` | `unit_id`, `input_sha256`, `granularity`, `plan_reason` (`page_index`/`chunk_index` when split) |
| `item_ingested` | `unit_id` |
| `item_running` | `unit_id`, `attempt` |
| `item_complete` | `unit_id`, `artifact`, `validation` |
| `item_review_required` | `unit_id`, `reasons` (array) |
| `item_retry_wait` | `unit_id`, `attempt`, `error` |
| `item_failed` | `unit_id`, `attempt`, `error` |
| `doc_reduced` | `input_sha256`, `artifact`, `validation`, `method` (`deterministic`/`model`) |
| `job_paused` / `job_resumed` | `reason` |
| `job_cancelled` | `reason` |
| `job_complete` | `processed`, `review`, `failed` |

### Recovery / replay algorithm (implement exactly; tested by J1.7)

1. Read `events.jsonl`; **ignore a non-JSON final line** (torn write) — never
   abort.
2. Per `unit_id`, state = last event. Terminal: `item_complete`,
   `item_review_required`, and `item_failed` with `attempt == max_attempts`.
3. Last event `item_running` with no terminal follow-up → reset to `READY`.
4. **Orphaned-artifact recovery (the rename-succeeded / event-not-appended
   window):** for every `<unit_id>.json` (non-`.partial`) present with a valid
   provenance file but **no** terminal event, verify it parses and re-validate it;
   if valid, append the missing terminal event; if not, delete both and reset the
   unit to `READY`. **Absence of an event does not imply absence of output.**
5. **Processed set** = inputs all of whose planned units are terminal (and whose
   reduce, if any, emitted `doc_reduced`). `run` skips inputs whose
   `input_sha256 ∈ processed set` — this is what makes "100 more tomorrow" touch
   only the new 100.
6. Writes: append + `fsync` the log; result/provenance write `*.partial` →
   `fsync(file)` → `rename` → **`fsync(parent dir)`** (durability across the
   rename), then append the terminal event.

### `<unit_id>.provenance.json` and the run fingerprint

```json
{
  "unit_id":"…","input_sha256":"…","input_path":"…","granularity":"file",
  "media_type":"image/jpeg","run_fingerprint":"…","instruction_sha256":"…",
  "prompt_sha256":"…","schema_version":1,"seed":11,"attempt":1,
  "input_tokens":814,"output_tokens":129,"prefill_seconds":11.7,
  "decode_seconds":21.2,"validation":"passed","runner_version":"j1-0.1"
}
```

`run_fingerprint` = `sha256(` manifest.json bytes `‖` engine build id
(git commit or version string) `‖` tokenizer file hash `‖` quant-format version
`)`. Hashing `manifest.json` alone is sufficient **only if** it cryptographically
enumerates every shard and the runtime geometry; since that is not guaranteed,
use the composite. Token counts/timings come from the serve response `usage`/
stats if present, else `null` + runner wall-clock.

## Test harness — the fake serve (build before J1.4)

**`tests/jobs/fake_serve.py`** — stdlib `http.server` on `127.0.0.1` answering
`POST /v1/chat/completions` from a canned map keyed by request-body hash, shaped
like serve (`choices[0].message.content`, `usage`). Modes: "hang N s" (timeout),
"500 twice then 200" (retry), "return 400 context_limit" (split test), "return
429 queue_full" (interlock/backoff test); it **counts requests** and exposes a
stub `GET /internal/v1/status` whose fields tests can set (interactive_active,
inference_busy, queue_depth) to drive J1.13 and the J1.4 wait-for-slot step.
**Self-test:** `python3 tests/jobs/fake_serve.py --self-test` → exit 0.

## Phase J1 — Local document/image extraction  **PROOF OF CONCEPT — build first, no network**

Proves the concept with zero network surface, on the owner's case: a folder of
receipts/medical records → structured fields → JSON; **100 more tomorrow** handled
idempotently; **auto per-file/per-page**; **safe to leave running unattended**.
One-shot runner (daemon = J2).

**Dependency note:** the #5 PDFium sidecar is now packaged with a capable
release. J1 uses it for exact PDF page metadata and bounded one-page rendering;
a release without it still returns the controlled
`review_required reason:"extractor_unavailable:application/pdf"` result. DOCX
remains deferred. Text-prefill cancellation is now measured, but it is not safe
to infer long-PDF or image-prefill readiness from the offline suite alone.

**Schema decision (resolved):** the explicit `output_schema` is the validation
contract, always. Schema *suggestion* is a **separate command** (`samosa jobs
suggest-schema`, J1.11), not part of `preview`.

Sub-tasks — each **Goal / Interface / Done / Test**; offline against the fake
serve unless noted; build in order.

### J1.0 — Runner skeleton, `job.json` loader/validator, immutable arm
- **Goal.** Strictly validate a `job.json`; freeze it into the job dir.
- **Interface.** `samosa jobs validate <job.json>` → exit 0 + normalized job; any
  violation → exit 2 + `error: <field>: <reason>`; no disk change on failure.
  Schema validation rejects unknown keywords, nested structures, bad type names
  (per §output_schema). **Immutable arm:** the first `run`/`arm` copies the
  definition to `<job_id>/job.json` and records `job_sha256`; every later
  operation uses the **frozen copy**. Supplying a different file with the same
  `job_id` (mismatched `job_sha256`) → exit 4 + `error: job <id> already armed
  with different content; use a new job_id or 'clone'`.
- **Done.** Rejects: missing key, bad `job_id`, relative path, unknown `unit`/
  `format`, `max_tokens > 8192`, unknown/typo'd schema keyword, nested schema.
- **Test.** `tests/jobs/test_validate.sh`: 1 valid + 8 malformed (incl.
  `{"maxLenght":3}` and a nested-object property) → exit codes; then `arm`, then
  re-`run` with an edited file same id → exit 4; no dir created for malformed.

### J1.1 — Input discovery (TOCTOU-safe, magic-byte typing)
- **Goal.** Enumerate inputs deterministically; emit `item_discovered` per unique
  file, immune to swap-after-check.
- **Interface.** For each candidate: **open with no-follow** (`O_NOFOLLOW`),
  `fstat` the descriptor, require a **regular file** (reject symlinks/FIFOs/
  devices/dirs), then **hash and read through that same descriptor**; re-`fstat`
  after read and drop the item if size/mtime changed (still being written, or
  swapped). Type by **magic bytes** (JPEG `FF D8 FF`, PNG `89 50 4E 47`, PDF
  `%PDF`); a file that is none of these becomes `text/plain` **only if it decodes
  as strict UTF-8 and passes a control-character check**, else `unsupported` (a
  skip with reason, not a silent text coercion). Skip content duplicates
  (same hash earlier this run) and files `> max_file_bytes`. Order by
  `input_path` ascending.
- **Done.** Deterministic; symlink/binary/oversized/unstable excluded with logged
  reasons; typing never guesses text for binary.
- **Test.** `tests/jobs/test_discovery.sh`: folder with 2 images, 1 byte-identical
  copy, 1 symlink to an image, 1 `.txt` renamed `.jpg`, 1 26 MiB+ file, 1 random
  binary blob. Assert: 3 items; copy hash == original; symlink excluded;
  mislabeled file → `text/plain`; binary blob → `unsupported`; oversized excluded;
  re-run identical.

### J1.2 — Granularity planner (`auto`) + exact token accounting
- **Goal.** Deterministically produce each input's **unit list** and record why.
- **Interface.** `plan_units(input_meta, unit_mode) -> [{unit_id, granularity,
  plan_reason, page_index?, chunk_index?, reduce_group}]`.
  - **Constants (pinned from the engine):**
    - `IMAGE_TOKENS = 576` — **max** vision tokens per rendered page. Derivation:
      page image capped at `max_pixels = 768×768 = 589,824`
      ([vision.c:156](../src/vision.c#L156)), resized to multiples of
      `SPATIAL_MERGE_SIZE = 32` ([vision.c:154](../src/vision.c#L154)); the LLM
      sees `(grid_h/2)×(grid_w/2)` pad tokens
      ([qwen36b.c:3459-3461](../src/qwen36b.c#L3459-L3461)) = `(768/32)² = 576`.
      Use the max so the planner never under-budgets (range 1–576).
    - `MAX_CONTEXT = 24576` ([qwen36b.c:3564](../src/qwen36b.c#L3564)),
      `SYSTEM_RESERVE = 1024`,
      `CONTEXT_BUDGET = MAX_CONTEXT − job.inference.max_tokens − SYSTEM_RESERVE`.
    - **Jobs prefill ceiling:** `MAX_JOB_INPUT_TOKENS = 8192`. The effective
      planning ceiling is the smaller of this and `CONTEXT_BUDGET`; a job may
      lower it through `resources.max_input_tokens` but may not raise it. The
      engine context is a correctness boundary, not an unattended-laptop
      performance target. A forced `unit:"file"` never bypasses this ceiling.
    - `LOW_TEXT_TOKENS = 20`.
  - **Text token counts are exact, not estimated.** Use `samosa tokenize --count`
    (Engine addition 1) on the extracted text and on the instruction+schema. A
    character heuristic (`ceil(chars/4)`) is **not** a safe bound (code, compact
    JSON, non-Latin, OCR, long numbers all break it) — it may be used only as an
    *advisory* pre-filter; the authority is the exact count, and ultimately the
    server's `400 context_limit` (F-J9), which the runner must honor by
    splitting and retrying (J1.4).
  - `SYSTEM_RESERVE` is likewise a floor, not a guarantee: if the exact
    instruction+schema count exceeds it, use the real count.
  - **Decision, `unit == "auto"`, per file:** single `image/*` → 1 unit
    `single_image`. text/markdown → exact tokens; `≤ CONTEXT_BUDGET` → 1 unit
    `fits_budget`, else split into chunks `over_context`. PDF (sidecar metadata
    per page `{text_tokens, needs_image}`, `needs_image = text_tokens <
    LOW_TEXT_TOKENS OR page has a raster figure`): `image_pages = Σ needs_image`,
    `total_tokens = Σ text_tokens + image_pages × IMAGE_TOKENS`; `image_pages ≥ 2`
    → **PER_PAGE** `multi_image_pages` (forced by F-J4); else `total_tokens >
    CONTEXT_BUDGET` → **PER_PAGE** `over_context`; else **PER_FILE** `fits_budget`.
  - **Chunking splits on paragraph/line boundaries with a small overlap** (e.g.
    ~64 tokens), never at arbitrary character offsets. `unit=="file"` on a
    multi-image doc → 1 unit + `warning:forced_file_multi_image`.
  - Split units set `reduce_group = input_sha256`; whole-file units
    `reduce_group = null`.
  - A PDF page that itself exceeds the Jobs prefill ceiling is not sent to the
    model; it receives `review_required reason:"unit_over_safe_prefill_budget"`
    until a page-text subchunker is available.
- **Done.** Pure over metadata + the tokenizer count; deterministic; no model.
- **Test.** `tests/jobs/test_planner.py`: (1) PNG → file/`single_image`;
  (2) **PDF 10 pages each `needs_image` → 10 units/page/`multi_image_pages`**
  (owner's anchor case); (3) PDF 3 text pages small → file/`fits_budget`;
  (4) PDF 1 scanned page small → file/`fits_budget`; (5) PDF 40 text pages
  `total_tokens > CONTEXT_BUDGET`, 0 images → per-page/`over_context`; (6) a text
  file whose **exact** count (stubbed tokenizer) exceeds `CONTEXT_BUDGET` → chunk
  units split on line boundaries with overlap; (7) `unit:"file"` on case 2 →
  1 unit + `forced_file_multi_image`.

### J1.3 — Extraction dispatch (per unit)
- **Goal.** Turn one unit into `{text?, image_data_uri?}`.
- **Interface.** text/markdown → UTF-8 text (chunk range per J1.2). `image/*` →
  base64 `data:` URI. PDF whole-file → sidecar text (+ ≤1 rendered image). PDF
  page → that page's text and, if `needs_image`, its rendered image. Sidecar
  absent → `extractor_unavailable`. Rendered pages / decoded intermediates written
  under `results/intermediates/` and deleted after the unit's terminal event
  unless the job opts to keep them (HR-10).
- **Test.** `tests/jobs/test_extract.sh`: `.txt` → exact text; PNG → round-trip
  `data:` URI; `.pdf` w/o sidecar → `extractor_unavailable:application/pdf`;
  intermediates removed after completion.

### J1.4 — Model call: request, timeout+cancel, split-on-limit, retry
- **Goal.** Call serve safely; never leave a zombie inference holding the slot.
- **Interface.** Build the F-J8 request (system = instruction + compact schema +
  "Return ONLY a JSON object … no prose, no fences"; user = text and/or one
  image_url; `thinking:"off"`, `temperature:0`, `seed`, `max_tokens`,
  `stream:false`, header `X-Samosa-Priority: background`). POST to
  `${SAMOSA_SERVE_URL:-http://127.0.0.1:8642}`.
  - **Oversize (F-J5):** encoded body `> 4 MiB` → `review_required
    reason:"image_too_large"`, **no POST** (auto-downscale is the first J2 item).
  - **Timeout is derived** when `timeout_s` is null: `timeout = base + (est_input_
    tokens / prefill_rate) + (max_tokens / decode_rate)` with conservative rates
    and a generous multiplier; a fixed 600 s may be too low for the largest inputs.
  - **On timeout the inference is still running (F-J1 single slot).** The runner
    must: (1) **abort the HTTP connection** to trigger server-side cancellation
    (F-J2); (2) **poll `GET /internal/v1/status` until `inference_busy` is false**
    (the slot is clear) — do **not** retry while it is busy or the retry queues
    behind / duplicates the abandoned request; (3) append `item_retry_wait`;
    (4) retry, up to `max_attempts` total, else terminal `item_failed`.
  - **`400 context_limit` (F-J9)** → split this unit further (halve on a paragraph
    boundary) and re-enqueue the pieces; if already minimal, `review_required
    reason:"context_limit_irreducible"`. **`429 queue_full`** → exponential
    back-off and re-attempt (not counted against `max_attempts`).
- **Test.** Fake serve: (a) registered content captured verbatim; (b) >4 MiB body
  → `image_too_large`, **0** POSTs; (c) "hang 1 s" + derived timeout 0.5 s →
  connection aborted, status polled until `inference_busy=false`, **then**
  `item_retry_wait`, then success on retry — assert the retry POST is sent only
  after the slot clears; (d) `max_attempts:3` + "500 twice then 200" → two
  `item_retry_wait` then `item_complete`; (e) `400 context_limit` once → the unit
  is split and both halves POSTed; (f) `429` → back-off then success, attempt
  count unchanged.

### J1.5 — Output validation (status / errors / warnings)
- **Goal.** Classify deterministically, separating hard failures from advisories.
- **Interface.** Return `{status: "passed"|"review_required", errors:[…],
  warnings:[…]}`. **`review_required` iff `errors` is non-empty**; warnings never
  force review.
  - **Parse:** `json.loads` the content; on failure, recover the first JSON object
    with a **proper string-aware scanner** — a real state machine that tracks
    quoted strings, escaped quotes, and backslashes so braces *inside strings*
    (`{"note":"use {braces}"}`) don't break brace-counting. If exactly one
    unambiguous object is recovered and there is trailing non-whitespace →
    `warnings += ["trailing_prose"]` (a warning, not an error). No object → `errors
    += ["unparseable"]`.
  - **Schema (subset):** missing required → `missing_required_field:<name>`; wrong
    type → `type_mismatch:<name>` (with the `bool`-is-not-`int` and JSON-typed
    enum rules from §output_schema); enum/bounds/maxLength → `constraint:<name>`.
    All → `errors`.
  - **Domain rules** (`"<a> + <b> ~= <c>"`): if all three are numbers, require
    `|a+b−c| ≤ 0.01 × max(1,|c|)` else `errors += ["domain:<rule>"]`.
- **Done.** Pure function; no model, no I/O.
- **Test.** `tests/jobs/test_validate_output.py`: valid → `passed`, no errors;
  missing `total` → error `missing_required_field:total`; `total:"x"` →
  `type_mismatch:total`; **`total:true` → `type_mismatch:total`** (bool≠number);
  enum `"USD"` vs value `true` → constraint (JSON-typed, not `True==1`);
  `currency:"USDD"` → `constraint:currency`; `subtotal 10,tax 2,total 99` →
  `domain:…`; `{"note":"use {x}"} thanks` → `passed` + `warnings:["trailing_prose"]`;
  `"sorry"` → error `unparseable`.

### J1.6 — Atomic artifact + provenance write
- **Goal.** No half-run mistaken for done; durable across power loss.
- **Interface.** `*.partial` → `fsync(file)` → `rename` → **`fsync(parent dir)`**
  for both the result and provenance; REVIEW copies to `results/review/`; append
  the terminal event **only after both files exist and are fsynced**. (Recovery
  step 4 handles the crash between rename and event.)
- **Test.** `tests/jobs/test_atomic.sh`: raise between the two renames → only a
  `.partial` remains, no terminal event; separately, simulate rename-done/
  event-missing (a full artifact + provenance, no event) → next `run` verifies and
  appends the missing `item_complete` rather than reprocessing.

### J1.7 — Event log + recovery + process lock
- **Goal.** Rebuild state from `events.jsonl`; resume after a kill; forbid
  concurrent runners.
- **Interface.** Implement the Recovery algorithm (incl. orphan recovery, step 4);
  acquire the `job.lock` `flock` for the run and fail exit 3 if held; `samosa jobs
  status <job.json>` prints counts by state from the log.
- **Test.** `tests/jobs/test_recovery.sh`: fake-serve run over 5 units; `SIGKILL`
  after 3 `item_complete`; append a truncated 6th line; restart `run` → units 1–3
  not re-POSTed, 4–5 processed, torn line ignored, `status`=5. Concurrency:
  start two `run`s → the second exits 3 with the lock message.

### J1.8 — Idempotent re-run ("100 more tomorrow")
- **Interface.** Discovery (J1.1) ∖ processed-set (recovery step 5).
- **Test.** `tests/jobs/test_idempotent.sh`: `run` 3-file folder → 3 processed;
  immediate re-`run` → **0** POSTs; add 1 file → exactly 1 POST; `status`=4.

### J1.9 — Page reduction: deterministic scalar merge (+ optional model, explicit failure semantics)
- **Goal.** Recombine a split file's page units into one document record, without
  letting the model paper over missing/failed pages.
- **Interface.** Runs when **all** units of a `reduce_group` are terminal.
  **Document-level rule:**
  - all page units `passed` → reduce, document `passed` (unless reduce itself
    fails validation);
  - some units `review_required` but with a **parseable record** → reduce,
    document **`review_required`** (carry the union of page reasons);
  - any unit with **no usable record** (`unparseable`/`failed`) → **do not hide
    it**: document `review_required`, and the missing page is listed.
  - **Reducer = deterministic scalar merge (default `reduce.mode`):** per schema
    field, gather non-null page values; one non-null → use it; all equal → use it;
    conflicting → `null` + `errors += ["reduce_conflict:<field>"]`. Only fields in
    `reduce.model_fields` (narrative/summary) use a **model** reduce call; scalars
    never need the model. `reduce.mode:"model"` sends the whole set to the model.
  - **Reducer input always carries page status + provenance**, so a model reduce
    cannot silently drop pages:
    ```json
    [{"page":1,"status":"passed","record":{…}},
     {"page":2,"status":"review_required","reasons":["missing_required_field:date"],"record":{…}}]
    ```
  - **Reducer context ceiling:** cap the combined page-record size; if the set
    would exceed `CONTEXT_BUDGET`, reduce hierarchically (merge in batches) — this
    is trivial for the deterministic path and required for the model path.
  - Validate the document record (J1.5); write `results/documents/<input_sha256>
    .json`; emit `doc_reduced` with `method`.
- **Done.** Deterministic path needs no model; failure semantics never fabricate a
  complete document from incomplete pages.
- **Test.** `tests/jobs/test_reduce.sh`: (a) 3 passed pages, `name` on p1, `dob` on
  p2 → deterministic merged record, **0 model POSTs**, `doc_reduced
  method:deterministic`; (b) p2 conflicts on `total` → `reduce_conflict:total`,
  document `review_required`; (c) p3 `unparseable` → document `review_required`
  listing the missing page, still no fabricated value; (d) a `model_fields:
  ["summary"]` job → exactly one model POST for `summary`, scalars still merged
  deterministically.

### J1.10 — Preview mode (own namespace)
- **Goal.** Prove this exact job definition on one representative input.
- **Interface.** `samosa jobs preview <job.json> [--file <path>]` runs **one** unit
  end-to-end writing to **`preview/`** (never `results/`), prints the record + its
  validation, and appends **no** `events.jsonl` item entries and touches no
  real-run outputs. It does **not** generate a schema suggestion (that is
  `suggest-schema`, J1.11).
- **Test.** `tests/jobs/test_preview.sh`: 3-file folder; `preview` → exactly one
  extraction POST, artifacts only under `preview/`, no `item_*` events, and a
  later `run` still processes all 3.

### J1.11 — Merged output + CLI surface
- **Goal.** User-facing outputs and commands.
- **Interface.** On `job_complete`, write `results/output.jsonl` (one record per
  passed **document**: reduced record for split files, unit record otherwise;
  `{"input_sha256","input_path",…fields}`) or `output.csv` (header
  `input_sha256,input_path` + schema property names in `properties` order).
  **Deterministic ordering: by `input_path`, then `page_index`/`chunk_index`** —
  never event-completion order. Commands: `validate`, `arm`, `preview`, `run`,
  `status`, `view` (J1.12), `suggest-schema <job.json|--instruction …>` (a single
  model call proposing a schema, written to `suggested_schema.json`),
  `delete <job.json>` / `archive <job.json>` (HR-10 retention).
- **Test.** `tests/jobs/test_output.sh`: 2 passing + 1 review → `output.jsonl` has
  exactly 2 lines in `input_path` order; CSV mode → header + 2 rows parseable by
  `csv.DictReader`; `suggest-schema` writes a file and emits no run events.

### J1.12 — Static Jobs view (fully escaped, self-contained)
- **Goal.** A browsable, safe local report — no server, no external requests.
- **Interface.** `samosa jobs view <job.json>` renders self-contained
  `results/view.html` (inline CSS, **no** JS framework, **no** external
  resources) mode `0600`: summary with the **REVIEW_REQUIRED queue first** (each
  reason shown); a per-item table (unit_id, input_path, granularity, state, links
  to result/provenance); and **both** timings shown separately — **wall time**
  (first→last event) and **active inference time** (Σ model-call durations).
  **Every interpolated value is HTML-escaped** (filenames, input paths, validation
  messages, model output, instructions, job names) so a hostile filename or
  extracted string cannot inject markup.
- **Test.** `tests/jobs/test_view.sh`: after 2 passed + 1 review, `view.html`
  exists, shows the review count/reason and 3 rows, lists wall vs active time; a
  file named `<img src=x onerror=alert(1)>.jpg` appears **escaped** (assert the
  literal `&lt;img` is present and the raw tag is not).

### J1.13 — Resource gate + chat interlock  **(the unattended-safety enforcement HR-6 requires)**
- **Goal.** Before every model request, and between units, confirm it is safe and
  polite to run; pause rather than compete with a human.
- **Interface.** A `gate_check()` run **before each unit and re-checked between
  units**:
  1. **Chat interlock.** `GET /internal/v1/status`; if `interactive_active` or
     `last_interactive_ts` is within a cool-down (e.g. 60 s), **pause** (emit
     `job_paused reason:"interactive_chat"`), poll until clear, then `job_resumed`.
     With Engine addition 3 (background priority) the scheduler also yields; the
     interlock is the coarse guarantee even without it.
  2. **Free storage** ≥ `resources.min_free_gb`, else pause `reason:"low_disk"`.
  3. **Power policy.** If on battery and `run_on_battery` false → pause
     `reason:"on_battery"`.
  4. **Memory pressure.** Read the OS pressure state; under WARN/CRITICAL, pause
     `reason:"memory_pressure"` (this is where the Linux G9 caveat in
     [CLAUDE.md](../CLAUDE.md) applies — record the raw signal, do not act on a
     known-bad ratio; on macOS use the real pressure level).
  5. **Thread/resource budget.** Confirm serve's `threads` (from status) matches
     the **host-derived budget** (HR-6), not a hardcoded 2; log the chosen budget
     and the host facts it came from. Perfect thermal measurement is **not**
     required for v1 — an enforceable boundary matching the published claim is.
  Any pause is logged; the runner never silently proceeds through a failing gate.
- **Done.** No unit starts while a gate condition holds; pauses/resumes are in the
  event log; the thread budget is derived and recorded, never hardcoded.
- **Test.** `tests/jobs/test_gate.sh` (fake serve drives `/internal/v1/status`):
  set `interactive_active=true` → runner emits `job_paused
  reason:"interactive_chat"` and sends **0** unit POSTs; flip to false → resumes
  and processes. Set a tiny `min_free_gb` above actual free space → `low_disk`
  pause. Assert the chosen thread budget is logged with its host inputs.

**J1 acceptance (offline):** `make jobs-test` runs every `tests/jobs/*` and exits
0. This is "tests pass" — **not** "works." "Works" is E-J1.

## Phase JO — Organize: deterministic filesystem post-actions  **(owner ask, 2026-07-19)**

The owner's three anchor tasks, verbatim shape:

1. *"Arrange this folder by document type — PDFs, JPGs, JPEGs, PNGs, DOCX, CSV,
   each in its own folder titled accordingly."*
2. *"Separate out all pictures with two humans into one folder."*
3. *"Check every receipt and separate out the ones for Saturday, June 5th."*

All three are the same pipeline — **enumerate → classify each file → group →
move** — and differ only in the classifier: (1) pure metadata, zero model
calls; (2) one bounded vision call per image; (3) one bounded extraction call
per document. The filesystem verbs are identical: list/stat, mkdir, move.
**Never delete.** JO adds the missing write stage to J1's read-only runner. It
is still not an agent: the model never chooses a path, names a folder, or
initiates an action — it only fills validated fields the runner then maps to
moves deterministically.

**Dependency shape.** JO.0–JO.5 and the two metadata intents (JO.6) touch no
model and are gated only on J1.0/J1.1/J1.7 landing. Field-based organize
(intents 2 and 3) consumes J1 results and is gated on **E-J1's accuracy
number**: a wrong extraction that merely sits in a JSON file is an error; a
wrong extraction that *moves someone's receipt* erodes trust. Field rules
therefore only ever consume **`passed`** documents (never `review_required`),
and E-JO1 measures move precision before the intent is described as working.

### Decisions locked (owner, 2026-07-19 — do not reopen)

- **JO-D1 — No delete path exists.** The runner never calls `unlink`/`rmdir`/
  `rmtree` on anything under the user's folder — not "gated", absent. The one
  audited exception is inside the move fallback (JO.3), which may remove the
  *source name* of a file only after proving the destination is the same inode.
- **JO-D2 — Move = no-clobber atomic rename.** An overwriting move is a delete
  by another name. macOS: `renamex_np(..., RENAME_EXCL)`; Linux:
  `renameat2(..., RENAME_NOREPLACE)`; both via `ctypes` (stdlib — the
  Python-stdlib-only decision holds). Destination exists ⇒ the move is **not**
  performed; it is a logged skip.
- **JO-D3 — Same-volume only.** `EXDEV` ⇒ skip with reason `cross_device`,
  never copy+delete.
- **JO-D4 — Scope jail.** Every source and destination stays under the job's
  `input.folder`, checked on the **realpath**, descriptors opened `O_NOFOLLOW`.
  Field values never become path components without passing the JO.1 whitelist.
- **JO-D5 — Plan → approve → apply.** `organize` only ever writes a plan file.
  User files change only in `apply`, after the frozen plan is shown and
  approved. The default invocation touches nothing.
- **JO-D6 — Journal + undo.** Every applied move is an event before and after
  execution; `undo` replays exact reversals with the same no-clobber rename.
  Never-delete **plus fully-reversible** is the promise, and E-JO1 verifies it
  by hash inventory, not assertion.

### `job.json` additions

**Note (2026-07-19):** J1 implementation is already in flight on
`issue-7-jobs` (validator/runner/PDF-corpus commits exist there, and that
branch carries its own card edits that must be reconciled with this section
on merge). The `organize` block is therefore an **additive** change JO.0
lands in that validator: optional top-level `organize`; **absent ⇒ behavior
byte-identical to a J1 job** (same gate pattern as the engine additions):

```json
"organize": {
  "rule": {"by": "extension"},
  "dest_root": null,
  "on_collision": "skip",
  "unmatched": "leave"
}
```

- `rule` — exactly one of:
  - `{"by":"extension"}` — destination folder = upper-cased extension (`PDF/`,
    `JPG/`, `JPEG/` — jpg and jpeg stay distinct, per the owner's task 1);
    extensionless files use the magic-byte type name (`JPEG/`, `PNG/`, `PDF/`,
    `TEXT/`), else `OTHER/`. Optional `"map": {"jpg":"Photos", …}` overrides
    folder names; keys are lowercase extensions, values pass the JO.1 whitelist.
  - `{"by":"media_type"}` — destination = magic-byte type; extension ignored.
  - `{"by":"field","field":"date"}` — one folder per distinct validated value
    of a schema field (task 3 grouped by day: `2027-06-05/`).
  - `{"by":"where","field":"people","op":"eq","value":2,"dest":"Two people"}` —
    matching documents move to `dest`, everything else follows `unmatched`.
    `op ∈ {eq, ne, lt, le, gt, ge}`; comparison is JSON-typed (the J1.5
    `bool≠int` rule applies).
- `dest_root` — `null` ⇒ `<input.folder>/Organized`; if set, must be an
  absolute path inside `input.folder` (validator-enforced, realpath).
- `on_collision` — `skip` (default) | `suffix_sha8` (append `.<first 8 hex of
  input_sha256>` before the extension — deterministic, collision-proof; never a
  mutable ` (2)` counter).
- `unmatched` — `leave` (default) | a folder name passing the whitelist.

### JO.0 — Validator extension + metadata-only jobs
- **Goal.** Accept the `organize` block strictly; allow jobs with **no model
  stage** for metadata rules.
- **Interface.** Extends J1.0. Reject: unknown keys inside `organize`, unknown
  `rule.by`/`op`, `dest_root` outside `input.folder` (checked by realpath, and
  rejected if any path component is a symlink), a `map` value or `dest` or
  `unmatched` folder name failing the JO.1 whitelist, `rule.by ∈
  {field,where}` naming a field absent from `output_schema.properties`.
  **Metadata-only form:** when `rule.by ∈ {extension, media_type}`,
  `instruction`, `output_schema`, and `inference` may all be `null` — the
  pipeline is discovery → plan, zero model calls, and `unit`/`reduce` are
  ignored. For metadata-only jobs discovery relaxes J1.1's typing in one way:
  a file that is neither magic-typed nor UTF-8 (e.g. DOCX — ZIP `PK`) is
  **included** with type `application/octet-stream` instead of skipped
  `unsupported` — it is being *sorted*, not sent to the model. Everything else
  in J1.1 holds (O_NOFOLLOW, regular-file check, hashing, stability re-fstat;
  `max_file_bytes` does not apply to metadata-only jobs).
- **Done.** A J1 job with no `organize` block validates and runs byte-identical
  to before (existing J1 tests untouched and green).
- **Test.** `tests/jobs/test_organize_validate.sh`: 1 valid extension job with
  nulls + 8 malformed (`dest_root` outside the folder, `dest_root` reached
  through a symlink, `"by":"extenson"` typo, unknown `op`, `where` on a field
  not in the schema, `map` value `"../up"`, `unmatched` value `".hidden"`,
  `organize` with an unknown key) → exact exit codes/messages; plus: every
  pre-existing J1 fixture still validates.

### JO.1 — Plan compiler (pure, deterministic, model-free)
- **Goal.** Turn results (or discovery records) into an exact, reviewable move
  list. Never touches user files.
- **Interface.** `samosa jobs organize <job.json>` reads `events.jsonl` +
  `results/` (or, metadata-only, runs/reuses discovery), computes one decision
  per input, writes `results/organize_plan.jsonl`, prints the human summary,
  and appends `plan_created`. Rules:
  - Source of truth per input: metadata rules → the `item_discovered` record;
    field rules → the **passed** document record (reduced record for split
    files). `review_required`/`failed`/unreduced → skip `not_validated`.
  - **Destination-name whitelist** (the only gate between extracted text and
    the filesystem): after trimming, a folder or mapped name must match
    `^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$` — no leading dot/dash, no path
    separators, no control characters, never `..`. A field value failing it →
    skip `unsafe_dest` (the hostile-value path: `"../../etc"`, `"a/b"`, a
    300-char value, an empty string all land here, listed in the plan).
  - Already in place (source dir == destination dir) → skip `already_sorted`.
  - Collision handling per `on_collision`; a `skip` collision is computed
    against both **existing files** and **other planned moves** (two different
    `IMG_001.jpg` from sibling subfolders in a recursive job must not both be
    planned into `JPG/IMG_001.jpg` — second one skips or suffixes
    deterministically by `input_path` order).
  - Plan lines: `{"input_sha256","src","dst","size","mtime"}` (src/dst
    absolute, realpath); skips: `{"input_sha256","src","skip":<reason>}`.
    Final line: `{"plan_sha256": <sha256 of all preceding lines>,
    "built_at_seq": <last events.jsonl seq consumed>, "moves": n, "skips": n}`.
  - Deterministic: same inputs ⇒ byte-identical plan (ordering: `input_path`
    ascending, same as J1.1).
- **Done.** Pure function of records; re-running `organize` with unchanged
  inputs rewrites an identical file; zero model calls, zero mkdir/rename.
- **Test.** `tests/jobs/test_organize_plan.py`: canned results for all four
  rule forms; assert byte-identical plans across two runs; hostile field
  values → `unsafe_dest`; review'd unit → `not_validated`; recursive name
  clash → exactly one planned + one deterministic skip/suffix; `where` with
  `value:2` does not match `true` (JSON-typed); a file already in its
  destination → `already_sorted`; filesystem untouched (tree hash before ==
  after).

### JO.2 — Shipped intents (the owner's three tasks + folder report)
- **Goal.** Encode the anchor tasks as reviewed templates, not ad-hoc jobs.
- **Interface.** `docs/examples/jobs/`:
  - `sort-by-type.job.json` — metadata-only, `{"by":"extension"}` (task 1).
  - `folder-report.job.json` — metadata-only, **no organize block executed as
    moves**: `samosa jobs report <job.json>` (tiny JO command) prints counts,
    total bytes, and largest files per type from discovery records — the
    "explore a directory" ask (count files, types, sizes) with zero inference
    and zero writes.
  - `photos-two-people.job.json` — vision intent: schema `{"people":
    {"type":"integer","minimum":0,"maximum":20}}`, fixed instruction ("Count
    the people visible…"), organize `{"by":"where","field":"people","op":"eq",
    "value":2,"dest":"Two people"}` (task 2).
  - `receipts-by-date.job.json` — extraction intent: the §job.json receipt
    schema, organize `{"by":"field","field":"date"}` (task 3; the "Saturday"
    check is deterministic — the runner, not the model, knows 2027-06-05's
    weekday, and a mismatch between a claimed weekday and the extracted date is
    a domain-rule review, not a move).
- **Suggest-job mapping (J2 §1).** These templates are what `suggest-job`
  selects among; "arrange this folder by type" must compile to
  `sort-by-type` + the folder path — **not** to a bespoke plan (§actor split).
- **Test.** `tests/jobs/test_intents.sh`: all four templates pass `validate`;
  `sort-by-type` end-to-end offline on a fixture folder (pdf + jpg + jpeg +
  png + docx-as-zip + csv + extensionless png + a `.txt` renamed `.jpg`) →
  plan groups match the magic-byte/extension contract exactly, **0 POSTs** to
  the fake serve; `report` prints the fixture's exact counts/bytes and creates
  no plan; `photos-two-people` against fake serve returning canned
  `{"people":N}` → only the `N==2` files planned.

### JO.3 — Move engine (the only code that changes the user's filesystem)
- **Goal.** One audited function performs every mkdir and rename; everything
  else is forbidden from touching user files.
- **Interface.** `apply_move(plan_line)`:
  1. `open(src, O_NOFOLLOW | O_RDONLY)` — symlink or non-regular ⇒ skip
     `not_regular_file`. `fstat` and compare `(size, mtime)` to the plan line —
     drift ⇒ skip `changed_since_scan` (the job may have run overnight;
     `--verify-hash` upgrades this check to a full re-hash).
  2. Destination directory: created with `mkdir -p` semantics, each component
     realpath-verified inside the jail before creation, default umask.
  3. Rename: `renamex_np`/`renameat2` no-clobber (JO-D2); `EEXIST` ⇒ skip
     `dest_exists`; `EXDEV` ⇒ skip `cross_device` (JO-D3).
  4. **Fallback** (non-macOS/Linux only): `os.link(src, dst)` (atomic
     no-clobber by contract), `fstat` both and **assert same inode**, only then
     remove the source name. This is the single permitted `unlink` in the
     runner (JO-D1) and it is wrapped in the inode assertion.
- **Done.** Grep-auditable: `os.remove`/`os.unlink`/`shutil.rmtree`/`os.rmdir`
  appear nowhere in the runner except the guarded fallback branch.
- **Test.** `tests/jobs/test_move_engine.py`: dst exists → skip, **both**
  files byte-identical after; src replaced by a symlink post-plan → skip; src
  touched post-plan → `changed_since_scan`; injected `EXDEV` (monkeypatched
  rename) → `cross_device`, src intact; fallback path unit-tested with the
  inode assertion firing on a simulated mismatch (no source removal); an audit
  fixture monkeypatches `os.unlink`/`os.remove`/`shutil.rmtree` to raise —
  the full JO test suite passes with the traps armed (except the one guarded
  call site, which the trap whitelists by stack inspection).

### JO.4 — Apply: approval boundary, events, crash safety, gate
- **Goal.** Execute a frozen plan exactly once, resumable, polite, journaled.
- **Interface.** `samosa jobs apply <job.json> [--yes] [--verify-hash]`:
  - Refuses without a plan; prints the summary (moves by destination, skips by
    reason) and requires typed confirmation or `--yes`; appends `plan_approved
    {plan_sha256}`. If `events.jsonl` has terminal events newer than
    `built_at_seq`, warn `plan_stale` and require `organize` to be re-run.
  - Per move: append `move_applying {src,dst,input_sha256}` → `apply_move` →
    append `move_applied` or `move_skipped {reason}`; fsync discipline of
    J1.6. Completion: `organize_complete {applied, skipped}`.
  - **Recovery** (extends J1.7's replay): `move_applying` without a follow-up →
    if dst exists with the plan line's size (or hash under `--verify-hash`)
    and src is gone ⇒ the rename won the crash: append the missing
    `move_applied`; if src still exists ⇒ retry the move; neither ⇒
    `move_skipped reason:"unresolved_crash"` for the morning review. Re-`apply`
    after completion ⇒ 0 actions (idempotent).
  - **Gate:** run J1.13's `gate_check()` between batches (default 50 moves) —
    renames are cheap but the interlock and pressure rules still apply; pauses
    log `job_paused` exactly as in runs.
- **Test.** `tests/jobs/test_apply.sh`: full offline apply of a 6-move plan →
  tree matches expected exactly, 6 `move_applied`; `SIGKILL` mid-apply (fault
  injection after rename, before event) → re-`apply` appends the missing
  event, completes the rest, no move performed twice; immediate re-`apply` →
  0 actions; without `--yes` and with stdin closed → exit non-zero, tree
  untouched; stale plan → refused with `plan_stale`.

### JO.5 — Undo
- **Goal.** Reverse an applied plan exactly, with the same safety rules.
- **Interface.** `samosa jobs undo <job.json> [--yes]` replays `move_applied`
  events in **reverse order** as `dst → src` no-clobber renames via JO.3
  (same jail, same skip taxonomy). User moved/renamed/replaced a file since ⇒
  skip `changed_since_apply` and report; each reversal appends `move_reverted`.
  Undo of an undo is out of scope — state it in the CLI help. Empty
  destination directories created by apply are **left in place** (removing
  them would require `rmdir` — JO-D1 wins over tidiness).
- **Test.** `tests/jobs/test_undo.sh`: apply → undo → recursive tree+hash
  identical to the pre-apply snapshot (empty created dirs excepted, asserted
  present-but-empty); touch one moved file → that one skips
  `changed_since_apply`, the rest revert; events show matching
  applied/reverted pairs.

### JO.6 — View + notification
- **Goal.** The plan and its outcome are first-class in the Jobs view; the
  finish is announced without any UI open.
- **Interface.** `view.html` (J1.12 contract: static, inline CSS, no external
  resources, every value escaped) gains a **Moves** section. Its first screen
  obeys the **bakery test** ([UI_DESIGN.md](UI_DESIGN.md) §3.0, owner rule
  2026-07-19): plain sentences — what happened, what needs a look (each
  reason as one plain-language sentence, never a J1.5 taxonomy string),
  where the files are, and the never-deleted + undo safety card. The full
  src → dst manifest (grouped by destination, monospace, dimmed common
  prefixes), skip reasons, per-unit table, exact undo command, and
  provenance all live in one collapsed "Details for the record" section. On `organize_complete` (and
  `job_complete`), the runner posts a local notification — macOS:
  `osascript -e 'display notification …'`, best-effort, never fails the job;
  content is counts only, **no filenames** (HR-10: notifications persist in
  Notification Center logs).
- **Test.** `tests/jobs/test_view_moves.sh`: after an apply with 2 moves +
  1 skip, `view.html` shows the grouped table and counts; a file named
  `<img src=x onerror=alert(1)>.jpg` appears escaped (literal `&lt;img`
  present, raw tag absent); notification command is invoked via a stubbed
  `osascript` on PATH and its argv contains no input filename.

**JO acceptance (offline):** all `tests/jobs/test_organize_*`, `test_move_*`,
`test_apply*`, `test_undo*`, `test_intents*`, `test_view_moves*` green under
`make jobs-test`, with the no-delete audit traps armed. This is "tests pass" —
"works" is E-JO1.

## Experiments — run the cheapest, most decisive one first

### E-J1 — Does the runner work on the real model?  ~1–2 days  **RUN FIRST (after J1 offline tests are green)**

**Current result (updated 2026-07-22): labeled JSS PDF batch and live chat
interlock completed through the compiled gateway.** The sidecar correctly
measured/planned all four supplied JSS PDFs, but the 2026-07-16 run used a
20,817-token whole-file preview on the 24 GB model and caused heavy
compression/swap on the 16-GiB host. The branch now uses a PDFium-backed
installed `samosa-extract --json-pages`, and the full four-PDF JSS corpus was
rerun through the compiled `/v1/jobs/definition/*` routes on the lighter Ornith
backend:
[`jss-pdf-ornith-2026-07-22/`](regressions/jobs/e-j1/jss-pdf-ornith-2026-07-22/).
Result: 4/4 records `passed`, 46/48 labeled fields correct, 0
`review_required`, 0 failed, preview 21.888 s, run 135.149 s, and
`Pages throttled=0`, `Swapins=0`, `Swapouts=0` before/after. The only labeled
misses were one grouped email-order field and one affiliation over-inclusion.

The PDF release-path blocker is closed for this branch by installing from the
reviewed PDFium archive and by installer smoke tests in `dist/install.sh` and
`tools/install_local_dev.sh`. The E-J1 harness now drives the compiled
`/v1/jobs/definition/*` routes instead of the deleted Python runner. Follow-up
on 2026-07-22 closed the compiled-observability gap: definition runs emit
per-item `model_call_seconds`, total `active_inference_seconds`, and
`job_paused`/`job_resumed` interlock events when
`resources.pause_when_user_active:true`; see
[`compiled-interlock-telemetry-2026-07-22.md`](regressions/jobs/e-j1/compiled-interlock-telemetry-2026-07-22.md).
A clean live real-model rerun with an interactive chat opened mid-batch is now
captured in
[`jss-pdf-ornith-interlock-clean-2026-07-22/`](regressions/jobs/e-j1/jss-pdf-ornith-interlock-clean-2026-07-22/):
4/4 records `passed`, 44/48 fields correct, 0 `review_required`, 0 failed,
`active_inference_seconds=124.93`, one `job_paused` and one `job_resumed`, and
zero swap/throttled pages.

The compiled route now has single-image request plumbing: PNG/JPEG definition
items are base64 encoded and sent as OpenAI-compatible `image_url` content, with
fake-backend coverage in `tests/test_compiled_gateway.sh`. The first live Qwen
smoke on a 1x1 PNG reached the vision-capable backend and completed with active
inference timing, but the model returned a scalar (`0`) instead of the requested
schema object, so the record was `review_required`; see
[`qwen-image-smoke-2026-07-22/`](regressions/jobs/e-j1/qwen-image-smoke-2026-07-22/).
Remaining acceptance coverage: labeled image inputs and multi-image reduction.

Real `samosa serve` + the real 24 GB model; full runner + `preview` over 10–20
real inputs (images+text; PDFs if #5 landed) with a hand-labeled reference.
Include at least one **multi-page image record** so the auto-planner's per-page +
reduce path (J1.2/J1.9) runs end-to-end, and drive the **interlock** by opening an
interactive chat mid-run and confirming the job pauses.

**Measure and record** (commit under `docs/regressions/jobs/e-j1/`): per-field
correctness vs. labels; malformed/`review_required` rate; per-unit and total
wall-clock **and** active inference time; peak footprint (`rss_gb`); whether the
resource gate paused on chat/low-disk and resumed; memory-pressure/thermal state;
**the swap delta** (`vm_stat` before/after — the real wear signal, HR-7); and the
**host-derived thread budget actually chosen** on this machine (the measured floor
point for HR-6).

**Acceptance:** correct structured output with a stated malformed rate; the
auto-planner chose the right granularity on every labeled file; the interlock
paused for the interactive chat and resumed; run stayed inside the machine-safety
envelope (no OOM, thermals bounded); a measured cost table; exact commands +
outputs pasted in. **"Correct but overnight wear too high" is a successful,
publishable result** — it goes in the ledger and may resize J2.

**E-J1's accuracy number also decides the product framing** (per the
never-overstate rule): if per-field correctness is high, "extracts your data"
is honest; if it is, say, ~80%, the feature must be framed as **"drafts every
field, you confirm"** — or it will feel like it lies. Record the number and pick
the sentence.

### E-JO1 — Does organize keep its promises on a real folder?  ~0.5–1 day  (after JO offline tests; sort intent needs no model)

Two halves, cheapest first:

**(a) Metadata half — no model.** A real copy of a messy user folder (≥100
mixed files including nested dirs, duplicate names, an alias/symlink, at least
one file with the wrong extension). Run `sort-by-type`: `organize` → review the
printed plan → `apply` → `undo` → `apply` again.

**Acceptance (a):** the **hash inventory law** — `find … -type f | sha256sum`
before and after every step: the *multiset of content hashes is identical at
every point* (moves change paths, never the set of contents). Zero
`unlink`-audit trips. `undo` restores every path (`changed_since_apply` count
0 on an untouched folder). Re-`apply` after `undo` = re-plan required or
identical plan applies cleanly. Plan preview counts matched what `apply` did,
exactly. Wall-clock for plan/apply/undo recorded (expected: seconds — say so
only after measuring).

**(b) Field half — real model, gated on E-J1.** The owner's receipt folder +
`receipts-by-date`, and ~30 personal photos + `photos-two-people`. Hand-label
ground truth first.

**Acceptance (b):** report **move precision** (files moved that belonged
there / files moved) and **recall** separately per intent; the review/skip
queue accounts for every file not moved; the hash inventory law holds; the
same machine-safety telemetry as E-J1 (footprint, swap delta, pauses).
Precision on labeled data decides the honest verb (per the E-J1 framing rule):
high ⇒ "sorts your photos"; mediocre ⇒ "proposes a sort you confirm" — record
the number and pick the sentence. Commit everything under
`docs/regressions/jobs/e-jo1/`.

### E-J2 — Public-fetch politeness & extraction  ~1 day  (gates J3, after J1)

Reuse E-I2 (SSRF 100% blocked for the job fetcher) and E-I3 (extraction) against
~15–20 real, public, logged-out careers pages; robots.txt honored per host; rate
limiter holds; JS-rendered postings detected and reported, never passed as the
posting. **Acceptance:** SSRF 100% blocked; robots/rate-limit honored; ≥15/20
extract cleanly with JS failures flagged.

## Phases J2 / J3 — kept at design level until E-J1 returns

Per the program's rule (experiments resize tasks — [ISSUE_TASKS.md](ISSUE_TASKS.md)
§5), J2/J3 are not expanded to J1 depth yet.

- **J2 — Daemon, scheduler, interactive view, host-adaptive tuning.**
  `samosa-jobsd`; launchd on macOS first (cross-platform-ready, not repeating the
  `textutil` mistake, [ISSUE_TASKS.md](ISSUE_TASKS.md) conflict 1); `caffeinate`
  keep-awake; missed-window policy (HR-8); background-priority admission (Engine
  addition 3) if not already in J1; the **interactive** Jobs view; **auto-downscale
  oversized images** (removes J1.4's `image_too_large` floor); and consuming the
  **host-capability profile** ([TASKS_HARDWARE.md](TASKS_HARDWARE.md) **H5**,
  gated on **E-H5**) so the resource budget comes from measured tiers, not the
  ad-hoc J1.13 derivation. Also asset snapshots + prefill-only endpoint (Phase 2
  engine work, F-J3).

  **J2 priority order (recorded 2026-07-16) — ranked by where user frustration
  actually comes from, build in this order:**
  1. **`suggest-job`** — extend `suggest-schema` into a full compiler: one
     interactive model call turning a plain-English description into a complete
     `job.json` (instruction + schema + domain rules + intent), which the user
     edits and `preview`s. Hand-writing `job.json` + JSON schema is dev-grade
     friction and the adoption killer for general users; this one command is the
     pitched "describe the outcome" experience, supervised and cheap.
  2. **Pre-arm time/cost estimate.** Before `arm`, print projected wall-clock
     (exact `tokenize --count` totals × E-J1-measured prefill/decode rates),
     battery/AC policy in effect, and unit count: "~6.4 h, run overnight."
     Expectation-setting is the entire difference between "slow" and "broken."
     Extend `preview` to ~3 diverse samples (new flag; J1.10's one-unit contract
     and its locked test stay valid as the default).
  3. **Side-by-side review view.** The review queue must show the **source next
     to the extraction** (receipt image beside the fields) and let the user
     correct a field and mark the unit done — the patch flows into
     `output.jsonl`; never "fix the instruction and rerun everything." If the
     review experience is three JSON files per item, users churn; the morning
     review is the product, not the punishment.
  4. Daemon/scheduler last — a job kicked off manually at night is 90% of the
     value; launchd is polish.

  **"Ask when uncertain" stays asynchronous in J2**: REVIEW_REQUIRED is the
  ask-when-uncertain surface. A job that blocks at 2 a.m. on a mid-run prompt is
  strictly worse than one that flags the unit and keeps going. Do not add
  interactive mid-run questions.
- **J3 — Public-web job input (folds in #4).** Scheduled input on the reused SSRF
  fetcher + extractor ([TASKS_INTERNET.md](TASKS_INTERNET.md)); user-provided
  public URLs (HR-1/2); polite fetch, extract, change-detect, feed only new items.
  Keep Jobs general-purpose: domain-specific workflows and screens do not belong
  in this surface. Out of scope: search engines, crawling, discovery, login.

## First release — deliberately narrow

Ships: JSONL durable state with process lock + seq; one-shot runner (J1.0–J1.13)
preserving F-J1 serialization; auto per-file/per-page granularity with exact token
accounting; stb_image + text ingestion now, pdfium hybrid when #5 lands;
deterministic page reduction (model reduce only for named narrative fields); the
resource gate + chat interlock; status/errors/warnings validation; content-hash
idempotency; crash-durable atomic artifacts + provenance; a fully-escaped static
Jobs view; `validate`/`arm`/`preview`/`run`/`status`/`view`/`suggest-schema`/
`delete`/`archive` plus the JO surface `organize`/`apply`/`undo`/`report`;
**host-derived** thread budget (floor = 2 on the reference machine). Four
intents: **Extract Document Data**, **Analyze Image Folder**, and the JO pair
**Sort Folder by Type** (no model) and **Folder Report** (no model); the two
field-based organize templates ship as examples gated on E-J1/E-JO1 accuracy.

Does **not** ship: daemon/scheduler, interactive Jobs view, folder-watching, web
input, asset snapshots, auto-downscale, measured host-adaptive tiers, multi-image
prompts, code-repo jobs, tool/action adapters, agents.

## Non-goals

- Any login/cookie/credential/paywall bypass. Public sources only.
- Autonomous site discovery / open-web crawling (user supplies inputs in v1).
- Real-time / interactive browsing — the anti-pattern this concept avoids.
- Autonomous agents or model-initiated actions (delete/send/publish/modify)
  without an explicit human-approval boundary. §Phase JO's organize stage is
  the sanctioned form of "modify": deterministic, plan-approved, journaled,
  and **file deletion is absent from the runner entirely** (JO-D1), not merely
  approval-gated.
- Model-initiated tool calls (A3.3) — parked behind E-I1 in
  [TASKS_INTERNET.md](TASKS_INTERNET.md).
- Parallel model inference (violates F-J1); multi-image prompts (F-J4).
- Cloud offload; always-on server-grade scheduling claims on a laptop.
- Hardcoded resource use — thread count/aggressiveness always derive from the host
  (HR-6), and no unmeasured tier is described as "fast."

## Open questions

- **Change detection for web inputs (J3)** — content hash per URL, or per
  extracted posting on a page listing many?
- **Host-adaptive resource tiers (HR-6/J2)** — owned by
  **[TASKS_HARDWARE.md](TASKS_HARDWARE.md) H5** (host-capability profile) and gated
  on **E-H5**: no tier above the 16 GB/2-thread floor is described until measured
  on real non-reference hardware. J1.13 derives a budget ad hoc until H5 lands,
  then reads H5's profile.
- **Cross-file synthesis** ("summarize all receipts this month") reintroduces the
  context cap per-file sessions sidestep. Out of scope for v1.
- **Scale ceiling of JSONL** — revisit SQLite only above ~10^5 items/job or
  concurrent shared querying.
- **Cross-platform scheduling** once #1/#2 land — launchd → systemd timers / Task
  Scheduler.
- **Product packaging** — embed the Python runner in the C binary vs. ship
  alongside (J2).

---

## Phase JF — model-driven "find" jobs (owner-approved direction, 2026-07-21)

> **STATUS UPDATE 2026-07-23:** an implementation of this phase shipped
> (`7eab55a`, `c9c99a0`) and **failed the Titli dogfood a second time** — it
> diverged from this spec on exactly the points the spec called out (C-side
> candidate filtering the spec never asked for, a hardcoded fallback
> question, restart-instead-of-resume despite JF.3, `fs_read_pages` instead
> of `doc.read` for scans). Root-cause evidence:
> [regressions/jobs/titli-find-2026-07-23.md](regressions/jobs/titli-find-2026-07-23.md);
> CLAUDE.md defect **J11**. The rebuild spec is
> [TASKS_JOBS_INTELLIGENCE.md](TASKS_JOBS_INTELLIGENCE.md) (**Phase JI**),
> which supersedes the shipped `jobs_find` code while keeping JF.1–JF.4 as
> the intent baseline. Do not extend the shipped find path — demolition is
> JI.0.

> **Status: SPEC — nothing below is built.** Approved by the owner on
> 2026-07-21 after dogfooding: "find my cat's medical record… his name is
> Titli" against a real ~/Downloads (508 files) fell through `decode_intent`'s
> two-kind vocabulary (organize | report) to a useless type count. The owner's
> stated model: give the model a goal and the *lowest-level* tools (count,
> metadata, read, read-a-page, notes, ask-the-user), and let it compose them
> freely — metadata-first to filter candidates, content reads only to confirm.
>
> **This supersedes the "no model-initiated tool calls" non-goal above** — for
> read-only tools only. The human-approval boundary moves from "the model may
> not call tools" to "the model may not *mutate* without approval," which is
> the boundary `samosa_tools.ToolContext` already enforces mechanically
> (mutating tools refuse to run in `preview` mode).

### What already exists (verified in code, 2026-07-21)

The rebuild on `issue-7-jobs` means most of this phase is wiring, not building:

| Owner's tool ask | Status |
|---|---|
| count files | `fs_survey` — exists |
| metadata of all files in a folder | `fs_list` — exists; **extend** rows with size + mtime so the metadata-first filter pass works |
| metadata of one file | **new** `fs_metadata` (size, dates, magic-byte type) — trivial over `os.stat` + `detect_media_type` |
| read a file | `fs_read_text`, `fs_read_document` (PDF via samosa-extract) — exist |
| read a particular page | **new** `fs_read_page` — `extract_document()` already returns per-page text; this is a slice, and it is the main prefill-cost mitigation |
| write notes in a tmp file | **new** `notes_append`/`notes_read` — jailed to the *job dir* (`<job_dir>/notes.txt`), never the user's folder |
| ask the user a clarifying question | **new** `ask_user` — the one genuinely new mechanism (below) |
| the agent loop itself | `run_tool_loop` — exists (registry, ability prompt, `SAMOSA_TOOL_RESULT` protocol, `MAX_TOOL_ROUNDS = 8`, per-round budget notes to the model) |
| jail + read-only enforcement | `ToolContext` — exists (realpath jail, preview/execute gating, `tool_call` emit) |

### JF.1 — third intent kind: `find`

`decode_intent` gains `_FIND_RE` (find / locate / search / look for / where is
/ which file) → `{'kind': 'find'}`, and the ambiguous-goal model classifier
becomes three-way (organize | report | find). Safety invariant preserved and
extended: the model classifier can *choose* `find`, but `find` always runs a
`preview`-mode ToolContext, so escalation to file mutation remains impossible
by construction — same shape as today's "model cannot upgrade report to
organize" test.

### JF.2 — run_job routes `find` through run_tool_loop

`ToolContext(root=folder, mode='preview', emit=log.append)`; toolset = the
read-only fs tools + notes + ask_user (never fs_mkdir/fs_move in v1);
system prompt = the job framing + goal; every `tool_call` event lands in the
existing events.jsonl → SSE → Jobs-tab feed, so the user watches "reading
titli_vaccination_2025.pdf …" live. Final model text becomes the `done`
summary (bakery test applies: plain sentences, paths, why).

The gateway needs a second model entrypoint: `jobs_model_call` is a one-shot
64-token classifier; the loop variant needs ~512 tokens/turn, same
lock/busy-fallback discipline.

### JF.3 — ask_user (pause/resume)

`ask_user` emits `await_user {question}`, persists the loop conversation to
`<job_dir>/convo.json`, and the generator returns — the same
stop-and-persist shape as `await_apply`. New endpoint `/v1/jobs/answer
{job_id, answer}` reloads the conversation, appends the answer as the tool
result, and re-enters the loop. UI: question + one text input in the feed.
**This is the phase's only real mechanism risk** — everything else is
registration and routing.

### JF.4 — honest constraints (measure, then claim)

- **Prefill is the binding constraint, again.** Every loop round re-prefills
  the growing conversation at ~14–24 tok/s on the reference M3. A bounded
  508-file listing is thousands of tokens; each document read is hundreds to
  thousands more. A realistic find job is **minutes, not seconds** — the
  cloud-model transcript that motivated this phase describes the *strategy*,
  not the local latency. Page-level reads (`fs_read_page`), bounded listings
  (`fs_list` limit), and notes-instead-of-context are the mitigations; the
  24,576-token cap is the wall.
- **Verification gate:** "works" means the real 24 GB Qwen model, through the
  app's Jobs tab, finds a real planted file in a real cluttered folder and
  answers with its path — evidence under `docs/regressions/jobs/`. The
  existing real-model evidence (decode_intent round trip, chat tool loop)
  makes this plausible, not proven.
- **v2, not v1:** find→act combos ("find X and move it into Y") — mutating
  tools in `execute` mode behind an Apply-style confirmation. Keep v1
  strictly read-only.

---

## Tool runtime decision — compiled sidecars, not Python (owner, 2026-07-21)

> **Owner's architecture statement:** Samosa's execution layer is
> `LLM → structured tool call → small compiled executable` (Rust, C, Swift, or
> Go; stable JSON or binary interface; one constrained operation per tool —
> `pdf.extract`, `files.rename`, `image.convert`, …). Python may remain an
> *optional compatibility layer* for third-party extensions, but the
> lowest-level tools must not rely on it and Samosa must not ship a Python
> runtime as its primary execution environment. Samosa should resemble a
> compact operating environment for AI agents, not a Python distribution.
>
> **Product frame (owner):** most boring laptop work reduces to five
> operations, and jobs should cover all five —
> **retrieve → compare/classify → transform → enter/send → escalate to a
> person.** "Enter/send" is outward action and sits behind the same approval
> boundary as file mutation; "escalate" is JF.3's `ask_user`.

### What is already true vs. what actually depends on Python (audited 2026-07-21)

Being precise about the starting point, per the evidence bar:

- **The protocol half of the target architecture already exists.** The model
  never generates or executes Python — it emits one line of structured JSON
  (`{"samosa_tool": …}`) and the app runs a named, constrained tool
  (`samosa_tools.classify_reply`/`execute_tool`). Nothing about the
  model-facing contract changes.
- **The pattern for the implementation half already exists too:**
  `samosa-extract` ([src/samosa_extract.c](../src/samosa_extract.c)) is a
  compiled C sidecar — one operation, `--json` out, its own CPU/memory
  sandbox, `@rpath`-linked libpdfium. `fs_read_document` already dispatches
  to it. This is the template, not a research question.
- **At the decision point, the Python layer was stdlib-only** (gateway + tools +
  jobs ≈ 2,900 lines, zero pip dependencies) — the "fragile package/version
  management" critique did not bite that code, and honesty required saying so.
  What **did** bite then: the release hard-required the host's `python3`;
  version drift was observable in-tree (both `cpython-310` and `cpython-314`
  bytecode in `__pycache__/`); in-process Python tools had no OS-enforced
  resource limits. The 2026-07-21 15 GB memory spike was a logic bug any
  language could have written — but a sidecar under rlimits would have been
  *contained* by the OS instead of eating 15 GB + swap. That containment
  argument, not "Python is slow," was the strongest concrete case for this
  decision.
- **Status update (2026-07-22):** Gate 11 removed the shipped Python
  gateway/jobs runtime. The installed path now uses compiled `samosa-gateway`,
  `samosa-jobsd`, `samosa-fs`, and `samosa-extract`; the compiled gateway test
  runs with `python3` unavailable. Python remains repo tooling only (packaging,
  tests, and regression/evidence harnesses).

### The sidecar contract (freeze before building more tools)

- One binary, one domain; subcommands for operations
  (`samosa-fs survey|list|metadata|move|undo …`), namespaced registry names
  (`fs.list`, `pdf.extract`, `image.convert`).
- Interface: args via argv/flags, JSON envelope on stdout —
  `{"ok": true, …}` / `{"ok": false, "error": "<stable_code>"}` — exactly the
  `samosa-extract` convention (`jobs_fs.extract_document` shows the caller
  side, including the wall-clock watchdog + `killpg`).
- Every sidecar sets its own rlimits (CPU, address space, output size), as
  `samosa-extract` does. C is the default implementation language (the
  toolchain the repo already builds with); Rust/Swift/Go are acceptable where
  they pay for themselves.

### Migration order (JF does not stall)

1. **JF tools are born against the contract, not added to Python.**
   `fs_read_page` *is* `samosa-extract` (per-page output already exists).
   `samosa-fs` starts with the metadata-first trio JF leans on: `survey`,
   `list` (with size/mtime), `metadata` — the deterministic logic ports from
   `jobs_fs.py`, whose tests become the port's acceptance tests.
2. **Port the mutation core** (`move` with atomic no-clobber rename, `undo`,
   the journal) into `samosa-fs`. During transition the Python `Tool` entries
   become thin dispatch shims (build subprocess argv, parse envelope) — the
   registry, jail, and preview/execute gating stay where they are.
3. **DONE (2026-07-22): Fold orchestration into compiled glue.** The C gateway
   now owns HTTP/SSE, agent loop, backend supervision, scheduling/jobsd, and the
   deterministic job routes. `dist/install.sh` builds and installs the compiled
   executables directly, so the installed runtime no longer depends on Python.

Non-goal, restated from the owner's framing: the model composing *tools* is
the feature; the model generating *programs* (arbitrary Python/shell) stays
out — a tool is a constrained verb, not an interpreter.

---

## Execution order — Phase JF + samosa-fs handoff (2026-07-21)

Owner-assigned sequencing for the work specced in §Phase JF and §Tool runtime
decision. **Read [ISSUE_TASKS.md](ISSUE_TASKS.md)'s Working agreement first**;
branch is `issue-7-jobs`; agent shells have no push credentials — commit and
stop. The evidence bar applies to every task: paste commands + output, log
runs under `docs/regressions/jobs/`, and never claim "works" above what was
actually run.

Two independent tracks until T6 joins them. A single agent does T0→T10 in
numeric order.

### T0 — Preflight (blocking, 30 min)
Confirm the branch already contains the 2026-07-21 session work: the
`discover_files` memory-cap fix in `tools/jobs_fs.py` (`read_up_to`, capped
metadata-only reads) + its two tests, and this spec's JF/runtime/order
sections. Then prove the baseline: `make jobs-test` green **and** `make`
compiles the C engine. If either fails, stop and report — do not build on a
red baseline.

### Track A — the compiled tools

**T1 — Freeze the sidecar contract (doc-only).** Write
`docs/SIDECAR_CONTRACT.md` by *codifying what `samosa-extract` already does*
(JSON envelope `{"ok": …}` / `{"ok": false, "error": "<stable_code>"}`,
argv conventions, self-applied rlimits, exit codes, versioning) — do not
invent a divergent scheme. The caller-side pattern to preserve is
`jobs_fs.extract_document` (watchdog + `killpg`).

**T2 — `samosa-fs` v1 in C: `survey`, `list`, `metadata`.** Port the
deterministic logic from `jobs_fs.py` — including O_NOFOLLOW symlink
rejection, magic-byte typing, dedup hashing, and **the capped-read behavior
from the 2026-07-21 fix** (max-bytes cap in metadata-only scans; truncated
files hashed over prefix+size, typed by magic bytes, sized by stat). `list`
gains size + mtime columns (JF's metadata-first pass needs them). Makefile
target + a test target. Acceptance: the discovery/typing scenarios of
`tests/jobs/test_jobs_fs.py` re-run against the binary with matching results
— the Python tests are the port's spec.

**T3 — Python tools become shims.** `fs_survey`/`fs_list`/new `fs_metadata`
dispatch to `samosa-fs` via subprocess (copy the `extract_document` caller
pattern); `run_job`'s internal folder survey switches too. Registry, path
jail, preview/execute gating: untouched. Acceptance: `make jobs-test` green
end-to-end through the shims, plus one containment demo: a folder with a
deliberately huge file shows the sidecar bounded by its rlimits instead of
the gateway's memory growing.

### Track B — the find-job plumbing (no sidecar dependency)

**T4 — JF.1, the `find` intent.** `_FIND_RE` + three-way model classifier in
`decode_intent`; `find` can never map to a mutating context (extend the
existing "model cannot upgrade report to organize" test to cover it).

**T5 — Gateway loop entrypoint.** A ~512-token variant of the 64-token
`jobs_model_call`, same lock/busy-fallback discipline. Test with the fake
backend (the `tests/test_gateway_chat_tools.py` pattern).

### Joined

**T6 — JF.2, route `find` through `run_tool_loop`.** Preview-mode
`ToolContext`; toolset = read-only fs tools + `fs_read_page` (a page slice
of `extract_document` output — no new binary) + `notes_append`/`notes_read`
(jailed to the job dir — job state, like `events.jsonl`, so Python is fine
here); `tool_call` events → events.jsonl → SSE; model's final text = the
`done` summary (bakery test). UI: render tool events live in the Jobs feed.
Acceptance: an integration test with a scripted fake model, then a live
app run on a scratch folder.

**T7 — Real-model checkpoint 1 (gate for calling find "working").** Plant a
plausible record (e.g. `titli_vaccination_2025.pdf` with a text layer) in a
cluttered scratch folder; the real 24 GB model, through the app's Jobs tab,
must find it and answer with the path. Evidence doc under
`docs/regressions/jobs/`. Machine-safety rules apply (never while the owner
is chatting; watch memory pressure + swap delta before/after).

**T8 — JF.3, `ask_user`.** Persist the loop conversation to
`<job_dir>/convo.json`, emit `await_user`, stop the generator; new
`/v1/jobs/answer {job_id, answer}` resumes; UI question + input box. This is
the phase's highest-risk mechanism — do it after T7 proves the loop, not
before. Acceptance: fake-model pause/resume integration test + live resume.

**T9 — Real-model checkpoint 2.** An ambiguous goal → the model asks a
clarifying question → answer → job completes. Evidence doc.

**T10 — Packaging.** `samosa-fs` ships alongside `samosa-extract` in
`dist/install.sh` + `tools/package_hf.py --gateway` (the `--pdfium-dir`
opt-in pattern), and a full local package→install→serve dry run — the step
that caught the two real release bugs on 2026-07-20.

### Explicitly deferred (do not start without the owner)
- Porting the mutation core (`move`/`undo`/journal) to `samosa-fs` — wave 2,
  alongside find→act (`execute` mode behind Apply).
- `enter/send` tools (email, forms) — outward actions, need their own
  approval-boundary spec.
- Rewriting the gateway/orchestration in a compiled language.
- Anything touching the HF release itself (owner publishes; H1 README defect
  is a separate owner decision).

---

## Native Jobs completion estimate (2026-07-21)

This section supersedes the older statement above that compiled gateway
orchestration was deferred. The compiled gateway now ships and runs without a
Python runtime. The remaining gap is specifically the **native background
scheduler** and **native public-URL input pipeline**, followed by deletion of
the obsolete Python orchestration implementation.

### Current compiled baseline

Implemented and tested in the compiled gateway:

- persisted Find clarification and continuation;
- native Ornith tool calls with metadata-first candidate selection;
- PDF reads limited to explicit ranges of 1–5 pages;
- editable review items with corrections persisted atomically to
  `output.jsonl`, including accept-as-is completion;
- one-sample preview by default and deterministic expanded preview of up to
  three samples, with artifacts isolated under `preview/`;
- definition execution and reviewable output generation;
- reviewed move plans, Apply, Undo, and process-wide Kill;
- compiled installation and packaging with no shipped Python runtime;
- all generated build outputs under `build/`, not the repository root.

Relevant commits: `57d96d9`, `be6057b`, `cf61342`, `2cf5bd8`, `e544ac8`,
and `7e97a4c`. The locally installed acceptance release at the time this
section was written is `dev-6d62f84ec2a4`.

### Remaining work and effort

This is an engineering estimate, not a deadline. It assumes one engineer who
already understands this repository and includes implementation, focused
tests, real acceptance checks, packaging, and cleanup.

| Work item | Scope | Estimate |
| --- | --- | ---: |
| Native scheduler state and commands | Freeze/arm a definition, manual overnight command, persisted schedule state, idempotent one-shot polling | 1.5–2.5 days |
| macOS `launchd` integration | Compiled `samosa-jobsd`, plist install/uninstall/status, logs, `caffeinate`, no hidden duplicate daemon | 1–1.5 days |
| Scheduling policy | Cross-midnight windows, missed-window behavior, AC/battery policy, host checks, queued review instead of blocking prompts | 1–1.5 days |
| Native public fetch boundary | HTTP(S), DNS and redirect SSRF checks, robots.txt, response limits, per-host rate limiting, readable extraction, stable errors | 2–3 days |
| Public input change state | User-supplied URL list, content hashes, first-seen/changed/unchanged records, atomic state, feed only new or changed pages | 1–1.5 days |
| General comparison workflow | Pair a local document with changed public pages without adding search, crawling, login, credentials, or job-board-specific UI | 0.5–1 day |
| End-to-end hardening | Real public-site check, sleep/wake and missed-window checks, Kill behavior, package/install test, documentation | 1–1.5 days |

Expected total for the missing native functionality: **7–11 focused
engineering days**. Removing the superseded Python gateway/Jobs modules and
consolidating duplicate tests after native parity is proven adds approximately
**1–2 days**. A realistic completion range is therefore **8–13 focused days**.

The public fetch boundary is the largest uncertainty. It must preserve the
existing security behavior across DNS resolution and every redirect; invoking
`curl` and trusting the initial URL is not an acceptable implementation.

### Required acceptance gates

The work is complete only when all of the following are true:

1. `samosa-jobsd` is a compiled executable and runs with `python3` unavailable.
2. Arming the same definition is idempotent; arming a changed definition under
   the same job ID is rejected instead of silently replacing it.
3. Overnight windows work across midnight, and missed windows follow the
   configured `skip` or `run_next_start` policy.
4. Battery/AC policy is enforced before work starts. On macOS, keep-awake uses
   `caffeinate` only for the lifetime of an active run.
5. Review-required records are persisted and queued. The daemon never waits on
   an interactive question.
6. The Kill control terminates the daemon's active job, model request,
   sidecars, and keep-awake process without leaving listeners or child PIDs.
7. Public URLs are explicitly supplied by the user. There is no discovery,
   crawling, search-engine integration, login, credential use, or paywall
   handling.
8. SSRF checks cover resolved addresses and every redirect; robots.txt,
   per-host rate limits, byte/time limits, and readable extraction are tested.
9. A repeated unchanged fetch produces no job unit. A changed page produces
   exactly one new unit and updates state atomically.
10. The installed release passes scheduler and public-input integration tests
    with Python unavailable, followed by one real public-page change check.
11. **DONE (2026-07-22).** Gates 1–10 passed, so `tools/samosa_gateway.py`,
    `tools/samosa_jobs.py` and their runtime-only helpers (`tools/samosa_tools.py`,
    `tools/jobs_fs.py`) were **removed** (owner-approved "retire duplicates" path),
    along with the duplicate Python tests they backed (`tests/test_gateway_jobs*`,
    `test_gateway_chat_tools`, `test_gateway_web`, `tests/jobs/test_run_job`,
    `test_tools`). The shipped `samosa-fs` sidecar keeps direct CLI coverage in the
    new `tests/jobs/test_samosa_fs.py`; every C job route stays covered by
    `tests/test_compiled_gateway.sh`. `make jobs-test` and `make test` are green.
    Retired coverage and evidence: [`gate11-python-removal-2026-07-22.md`](regressions/jobs/gate11-python-removal-2026-07-22.md).

### Deliberate non-goals

- search engines or URL discovery;
- crawling a site beyond an explicitly supplied page and its robots policy;
- authenticated, credentialed, private, or paywalled pages;
- email, form submission, or other outward actions;
- job-board-specific fields or UI. Public URLs remain general-purpose job
  inputs.
