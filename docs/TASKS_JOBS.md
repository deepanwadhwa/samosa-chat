# Issue #7 — Samosa Jobs (batch, scheduled, local multimodal work)

**Status: design. Nothing here is built or measured. Every performance number is
marked _unverified_ until an experiment produces it.** Claims about the *current*
engine, by contrast, are verified below with `file:line` evidence — that is the
line this card holds: the codebase foundations are proven, the job system on top
of them is not.

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
vision tower from [TASKS_VISION.md](TASKS_VISION.md) (#3, landed).

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

### Repo state (main @ `8b3e813`, 2026-07-16)
- **Branch:** cut **`issue-7-jobs` from `main`**. Implementation lives there; the
  card / `ISSUE_TASKS.md` / `CLAUDE.md` stay on `main` (Working Agreement §1).
- **#3 vision: LANDED** on main (single-image — F-J4). **#5 documents: NOT built**
  — no pdfium sidecar exists (grep: zero `FPDF_`/`pdfium`). The PDF path is stubbed
  (below); build and test J1 on **images + text**, which need no sidecar.
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

### #5 PDF-metadata stub contract (define now; coordinate with whoever owns #5)
Until the sidecar exists, J1.2's planner and J1.3's extractor need a metadata
shape #5 has not defined. Define it as the interface J1 expects, so #5 implements
*to it*:
```
extract_meta(path) -> { "text_layer": bool,
                        "pages": [ {"index": int, "text_tokens": int, "has_raster_figure": bool}, … ] }
```
`needs_image` (J1.2) = `text_tokens < LOW_TEXT_TOKENS OR has_raster_figure`. Until
the sidecar binary is on `PATH`, J1.2 (PDF inputs) and J1.3 both return
`review_required reason:"extractor_unavailable:application/pdf"`. **This is the one
genuine cross-issue seam — flag it to #5.**

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

`<jobs_root>` default `~/.samosa/jobs` (override `SAMOSA_JOBS_DIR`), mode `0700`.
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

**Dependency note:** the PDF/DOCX path needs the #5 pdfium sidecar. Until it
lands, J1 runs on **images (via #3) and text/markdown**; the PDF path returns
`review_required reason:"extractor_unavailable:application/pdf"`. E-J1 can run
**today** on image+text inputs. The planner (J1.2) is testable today with
synthetic PDF metadata.

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

## Experiments — run the cheapest, most decisive one first

### E-J1 — Does the runner work on the real model?  ~1–2 days  **RUN FIRST (after J1 offline tests are green)**

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
- **J3 — Public-web job input (folds in #4).** Scheduled input on the reused SSRF
  fetcher + extractor ([TASKS_INTERNET.md](TASKS_INTERNET.md)); user-provided
  public URLs (HR-1/2); polite fetch, extract, change-detect, feed only new items.
  The resume-vs-public-postings screen lands here. Out of scope: search engines,
  crawling, discovery, login.

## First release — deliberately narrow

Ships: JSONL durable state with process lock + seq; one-shot runner (J1.0–J1.13)
preserving F-J1 serialization; auto per-file/per-page granularity with exact token
accounting; stb_image + text ingestion now, pdfium hybrid when #5 lands;
deterministic page reduction (model reduce only for named narrative fields); the
resource gate + chat interlock; status/errors/warnings validation; content-hash
idempotency; crash-durable atomic artifacts + provenance; a fully-escaped static
Jobs view; `validate`/`arm`/`preview`/`run`/`status`/`view`/`suggest-schema`/
`delete`/`archive`; **host-derived** thread budget (floor = 2 on the reference
machine). Two intents: **Extract Document Data**, **Analyze Image Folder**.

Does **not** ship: daemon/scheduler, interactive Jobs view, folder-watching, web
input, asset snapshots, auto-downscale, measured host-adaptive tiers, multi-image
prompts, code-repo jobs, tool/action adapters, agents.

## Non-goals

- Any login/cookie/credential/paywall bypass. Public sources only.
- Autonomous site discovery / open-web crawling (user supplies inputs in v1).
- Real-time / interactive browsing — the anti-pattern this concept avoids.
- Autonomous agents or model-initiated actions (delete/send/publish/modify)
  without an explicit human-approval boundary.
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
