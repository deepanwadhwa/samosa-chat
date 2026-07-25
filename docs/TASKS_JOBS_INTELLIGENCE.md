# Phase JI — generally intelligent document jobs (find, rebuilt)

> **Status: COMPLETE — approved direction from the owner, 2026-07-23, after the
> second dogfood failure of the Titli scenario.** Evidence:
> [regressions/jobs/titli-find-2026-07-23.md](regressions/jobs/titli-find-2026-07-23.md)
> (five compounding root causes, one reproduced C bug, and a
> spec-vs-implementation divergence table).
>
> **Implementation status (2026-07-23): complete.** JI.0–JI.8 pass their
> offline gates; E-JI1/E-JI2 pass on the owner-approved 50-file real-model
> fixture. Thermal telemetry required privileged `powermetrics`, so the gate
> records its unavailability rather than making an unmeasured thermal claim.
>
> This card **supersedes the Phase JF implementation** (`jobs_find`,
> `candidate_score`, `build_candidates`, the canned ask fallback, the
> restart-on-answer path — all in `src/samosa_gateway.c` as of `ad825d1`).
> It does **not** supersede the JF spec's intent — it enforces it. JF said:
> give the model the goal and the lowest-level tools and let it compose them;
> the implementation instead hardcoded language processing in C. That
> inversion is the defect class this card exists to remove.

## The owner's design statement (2026-07-23 — this is the requirement)

> "My query is a natural-language query to an LLM. The LLM should identify
> that it needs to find files, from a given list of files in the given
> folder, which might look like a medical record of a pet. First it should
> have checked the filenames and seen which files might qualify. Then it
> could have prepared a dictionary like `filename: <first few lines of the
> file>` and gone through those 250 PDFs easily."

That is the pipeline. Filenames first (model-judged, all of them), then a
**skim index** — first-page text of every still-plausible file, extracted
through `doc.read`'s OCR cascade and made cheap by the content-addressed
read cache — then model classification over the skim, then bounded deep
verification of the survivors. Jobs must be *generally intelligent about
documents*: a scanned vet record with a meaningless filename must be
findable, because its **content** is reachable.

## Design law (non-negotiable, enforced by JI.0 and JI.8)

1. **The gateway executes; the model decides.** No C code may tokenize,
   keyword-match, score, or otherwise interpret natural-language goal text or
   file contents. C owns: the path jail, budgets, caching, event streaming,
   persistence, sidecar dispatch, and approval gates. The model owns: what
   the goal means, which files are plausible, what evidence confirms a match,
   what to ask the user, and how to summarize. The delimiter bug that
   shredded "Titli" (RC1) was only *possible* because C was doing a job it
   should never have had.
2. **No hardcoded user-facing questions.** Every question shown to the user
   is model-authored via `ask_user`, with the full conversation (which
   contains the goal) in context. Mechanical *status* lines ("Paused:
   skimmed 180/271 files") are permitted only when clearly system-authored
   progress reporting — never phrased as the model asking for information.
3. **Pause is not restart.** Any pause (question, budget checkpoint, crash)
   persists the full loop conversation and phase state; resume continues in
   place. Work already paid for (prefill, OCR, reads) is never repaid.
4. **Honest events.** A progress line may only claim what mechanically
   happened: "Checked 510 filenames" requires the model to have actually
   received 510 filenames. Every ✓ maps to a completed action with a real
   count.
5. **Sweep semantics.** "Find all X" returns a *list* of matches with
   per-file evidence, plus an explicit list of files that could not be read
   and why. Never a single-file answer to a plural question, and never
   silence about unreadable files.

## Architecture — the five-phase pipeline

All phases run inside one persistent job conversation (one `convo.json`),
driven by the active text backend (Ornith 9B is the expected orchestrator;
the 24 GB Qwen tower stays out of the reading path per reader decision 9).
`doc.read` internally escalates to Bonsai+mmproj for hard crops when the
vision backend is available; under text-only Ornith it parks pages with
`vision_backend_required` (already implemented, R5) — parked files surface
in the result as unverified, they are never silently dropped.

```
A. TRIAGE    model reads ALL filenames+metadata, in batches   → verdicts.jsonl
B. SKIM      doc.read page 1 of every plausible file (cached) → skim.jsonl
C. CLASSIFY  model reads filename + first-lines, in batches   → shortlist
D. VERIFY    model loop: doc.read more pages of shortlist     → evidence
E. FINISH    structured finish() tool → result card           → result.json
```

**Why this is affordable on the reference M3.**
Phase A is pure prompt tokens (~510 names ≈ 6–9k tokens, batched under the
context cap). Phase B's per-file cost is `samosa-ocr` C inference plus
possible Bonsai crop escalations. E-JI1 measured a 50-file cold skim
increment of 56.826 s (1.14 s/file) and a warm skim increment of 0.037 s;
the report records its RSS and machine-wide swap observations. The read cache
(`~/.samosa/cache/read/`, content-addressed) makes the skim **once per file
content, ever**: the first find job on a folder pays for it, every later job
on that folder — and any chat `doc.read` of the same files — hits the cache.
This is the same amortization argument the whole architecture is built on
(sessions amortize prefill; the cache amortizes reading). First-run honesty:
the UI must show skim progress per file and the checkpoint mechanism (JI.3)
must make a long first skim interruptible and resumable. The measured
reference-M3 calibration emits an event-backed remaining-time estimate in the
UI; it is an estimate, not a promise, and can vary with document mix.

### Durable state (all under `<jobs_root>/<job_id>/`, 0600/0700)

| file | contents | written by |
|---|---|---|
| `job.json` | goal, folder, created, schema_version | arm/run |
| `convo.json` | full model conversation (messages array incl. tool results) | every model round |
| `phase.json` | `{phase: A..E, cursor, budgets_spent, counts}` | every phase step |
| `verdicts.jsonl` | one line per file: triage verdict + reason | phase A |
| `skim.jsonl` | one line per file: `{path, sha256, type, size, mtime, page_count, first_lines, source, needs_review, parked}` | phase B |
| `result.json` | the structured `finish()` payload | phase E |
| `events.jsonl` | append-only SSE mirror (existing shape) | all |

`skim.jsonl` **is** the owner's dictionary (`filename: first few lines`),
made durable. `first_lines` is capped (`JI_SKIM_CHARS`, initial 400) and
comes from `doc.read {pages:[1,1], detail:"text"}` — text layer if present,
OCR if not, tier-2 crops if available. It contains extracted user-document
text, so it lives in the job dir under the same trust boundary as the read
cache, and never leaves the machine.

---

## Tasks

### JI.0 — Demolition (do this first; nothing else lands on top of the old code)

Delete from `src/samosa_gateway.c`:

- `candidate_score()` and `build_candidates()` (lines ~1845–1894) including
  the stopword list and the cat/pet/medical bonus tables. **No replacement
  in C.** Filename judgment moves to the model (JI.2).
- The `ask:` fallback block in `jobs_find` (lines ~2466–2476) including the
  canned strings `"What is your pet's name?"` and `"What filename, name,
  date, or phrase should I use…"`. Budget exhaustion gets a checkpoint
  (JI.3/JI.6), not a fake question.
- The goal-restart body of `jobs_answer` (lines ~2588–2594) — replaced in
  JI.6.
- The `"Read PDFs only with fs_read_pages"` system-prompt language (line
  ~2356) — replaced in JI.4's prompts, which route document reading through
  `doc.read`.

`find_intent()` (substring routing at line ~1812) may remain **only** as a
fast-path router into the find pipeline, with the JF.1 three-way model
classifier as fallback for goals it misses; it must never gate semantics
beyond routing. `fs_read_pages`/`fs_read_text` remain as raw tools (text
layer is tier 0 and free) but stop being the *instructed* path for PDFs.

**Acceptance:** `grep -c "What is your pet" src/` returns 0; a fake-backend
find job whose goal contains "education" (substring "cat") runs to a finish
or a *model-authored* question — asserted by JI.8's tests. The `simplify`
altitude rule applies: no dead helpers left behind.

### JI.1 — Listing enrichment + honest counts plumbing

`samosa-fs list` rows must carry `{name, rel_path, size, mtime, magic_type}`
(JF's table already required size+mtime; verify against current sidecar
output, extend if missing — this is Track A sidecar work, C only). The
gateway serializes the *complete* listing for phase A; nothing filters it.
Emit `indexing {total}` when listing starts and `index_complete {total,
batches}` only after the last triage batch returns — the UI's "Checked N
filenames" line binds to *model-confirmed* batches, not to a C loop.

**Acceptance:** unit test over a fixture folder asserts every file appears
in exactly one phase-A batch payload (count the names actually embedded in
model request bodies via the fake backend's request log).

### JI.2 — Phase A: model filename triage, batched

**Status: passed offline.** The compiled gateway test drives 510 files,
SIGKILLs the gateway while triage batch two is in flight, and resumes to
exactly 510 durable verdict rows (the first 16 are not repeated).

Build batches of listing rows sized by *measured* tokens (reuse the
engine tokenizer via the existing serve path or a conservative
chars/4 estimate) with budget `JI_TRIAGE_BATCH_TOKENS` (initial 500;
calibrated in E-JI1 against the orchestrator's context). Per batch, one
model call, strict contract:

```
system: You are triaging filenames for a local file-finding job. For each
        numbered file, output a compact JSON object
        {"items":[{"i": <index>, "c": "h"|"m"|"l"}, …]}. Judge only from name, type,
        size, date. "unknown" is the correct verdict when a name (e.g. an
        anonymous scan) says nothing about content. Output JSON only.
user:   Goal: <goal>\nFiles:\n1. CamScanner 03-15-2024.pdf (pdf, 2.1 MB,
        2024-03-15)\n2. …
```

- Batch cap: at most 16 files. The response is a fixed-shape internal routing
  code, so it must never ask the model to explain itself or impose a token cap
  as a substitute for a precise request.
- Parse with the existing fenced/prose JSON recovery (`50ad6e7`); one
  malformed retry per batch, then the batch's files default to `"unknown"`
  (fail open into the skim, never silently drop).
- Append verdicts to `verdicts.jsonl`; emit `triage_progress {done, total}`
  per batch; update `phase.json` cursor so a killed job resumes at the next
  batch.
- **The anonymous-scan rule is the point:** a CamScanner name must come back
  `"unknown"`, not `"no"` — it flows into phase B where its *content* gets
  read. This single behavior is what makes the Titli scenario winnable and
  is asserted by JI.8-b.

**Acceptance:** fake-backend test drives a 510-name fixture listing through
triage; asserts batch count, cursor resume after SIGKILL mid-triage, and the
unknown-flows-to-skim invariant.

### JI.3 — Phase B: the skim index (the owner's dictionary)

For every `likely` and `unknown` file whose `magic_type` is readable
(pdf, image, text — text files use `fs_read_text` head directly, no OCR):

1. `doc.read {path, pages:[1,1], detail:"text"}` — cache-backed; a warm
   cache makes this a JSON read.
2. Truncate to `JI_SKIM_CHARS` (400) at a line boundary; record
   `{…, first_lines, source: text_layer|ocr|vlm_crop, needs_review}`.
3. `vision_backend_required` or `ocr_unavailable` → record the file with
   `parked: true` and the error code; it appears in the final result under
   "could not read", it is not dropped.
4. Emit `skim_progress {done, total, current_name}` — the live "skimming
   CamScanner 03-15-2024.pdf (214/271)" line JF.2 promised.

**Ordering and budget:** `likely` files first, then `unknown` by mtime
descending (recent files are more likely to be sought). Per-run budget
`JI_SKIM_MAX_FILES` (initial 300) and wall budget `JI_SKIM_MAX_SECONDS`
(initial 1,800) — on exhaustion, checkpoint `phase.json`, emit
`await_continue {skimmed, remaining, found_so_far}`, and stop cleanly. The
UI renders a **Continue** button with the real numbers. This replaces the
canned question as the budget-exhaustion behavior: an honest mechanical
checkpoint, resumable because state persists (design law 2 and 3).

Skim runs are **background admission class** under the existing chat
interlock (TASKS_JOBS.md §J1.13): a folder-scale OCR sweep must not fight
the owner's live chat for the machine (CLAUDE.md machine-safety rule).

**Acceptance:** fixture folder with a scanned PDF (no text layer, pet name
only in pixels — reuse the R-series OCR fixtures under
`tests/fixtures/documents/`); test asserts `skim.jsonl` contains OCR-sourced
`first_lines` for it, that a second run is served from the read cache
(assert zero `samosa-ocr` invocations via a counting shim), and that
kill+resume continues from the cursor without re-reading.

### JI.4 — Phases C+D: skim classification and deep verification

**C — classify.** Batch `{rel_path, first_lines}` rows (budget
`JI_CLASSIFY_BATCH_TOKENS`, initial 500; maximum 16 files) with compact strict
JSON `{items:[{i,v}]}`, where `v` is
`m | y | n` for `match | maybe | no`. `match`
and `maybe` form the shortlist. Progress: `classify_progress {done,
total, shortlist}`.

**D — verify.** One agentic tool loop (the JF.2 shape — persistent
conversation, tools, events) over the shortlist, with tools:
`doc.read` (the instructed path for PDFs and images — pages come in [start,
1..5] chunks per the reader contract), `fs_read_text`, `fs_metadata`,
`ask_user`, `notes_append`/`notes_read` (jailed to the job dir, per JF),
and `finish` (JI.5). System prompt states the sweep contract: confirm or
reject every shortlisted file, cite page-numbered evidence quotes, call
`ask_user` only for genuine ambiguity *that the conversation cannot
resolve* — and the conversation contains the goal, so a detail present in
the goal is never a valid question. Round budget `JI_VERIFY_MAX_ROUNDS`
(initial 24 — sized to the shortlist, not to 8) with the same
checkpoint-not-question behavior on exhaustion.

**Acceptance:** fake-backend script drives a canned verify conversation:
asserts `doc.read` is what the loop calls for a PDF, that evidence quotes
land in `result.json`, and that exhausting the round budget yields
`await_continue`, never a question.

### JI.5 — Phase E: structured finish + the result card

New tool, the **only** legal way to end a find job:

```json
finish({
  "matches":   [{"path": "...", "evidence": "quote", "page": 3, "confidence": "high|medium"}],
  "rejected_count": 17,
  "unreadable": [{"path": "...", "reason": "vision_backend_required"}],
  "notes": "one short paragraph for the user"
})
```

The gateway validates the shape (unknown keys rejected, paths must be
inside the jail, evidence non-empty for every match) and writes
`result.json`. A content-only final message from the model is **not**
accepted: the loop replies once with "call finish or ask_user", then fails
the job honestly (`error: model_no_finish`). This kills the "Would you like
me to: Ask you…" class of ending (RC-secondary), and gives the UI a real
result card: matches with evidence quotes and page numbers, an unreadable
list with reasons ("needs the vision backend — switch backend and re-run;
the skim is cached, the re-run is cheap"), honest counts.

Find→move stays out of the find loop entirely (restores JF's v1
read-only rule): the result card offers "organize these matches" as a
follow-up that feeds the verified paths into the existing JO plan/apply
approval machinery.

**Acceptance:** shape-validation unit tests (bad paths, empty evidence,
unknown keys → rejected); fake-backend test for the no-finish retry path.

### JI.6 — Pause/resume done right (JF.3, actually built)

**Status: passed offline.** The compiled gateway test covers answer resume,
skim checkpoint resume, and SIGKILL during Phase D after a durable tool
result; the restart preserves `convo.json` and emits no duplicate tool
events.

One mechanism for all three pause kinds (`ask_user`, `await_continue`
checkpoints, crash):

- Every model round appends to `convo.json` (write-temp + rename, same
  atomic discipline as J1.6); every phase step updates `phase.json`.
- `ask_user` → emit `await_user {question}` (model-authored only), return.
- `/v1/jobs/answer {job_id, answer}` → load `convo.json`, append the answer
  **as the tool result of the pending ask_user call**, re-enter the loop at
  the saved phase/cursor. The goal is never mutated. Delete the
  `jobs_report(expanded_goal…)` restart path.
- `/v1/jobs/continue {job_id}` → same reload, no message appended, next
  cursor step (the Continue button for budget checkpoints and crash
  recovery — reuses the J1.7 recovery scan to find the last durable state).

**Acceptance:** three fake-backend tests — (1) answer-resume: run 1's tool
results are still in the conversation the model sees after the answer
(assert via fake-backend request log); (2) checkpoint-resume mid-skim:
no re-OCR, cursor advances; (3) SIGKILL mid-verify then continue: no
duplicate events, conversation intact. These are the tests whose absence
let RC3/RC4 ship.

### JI.7 — Honest progress events + UI copy audit

**Status: passed offline.** `tests/test_jobs_ui.mjs` executes the shipped
renderer against DOM fixtures for index, triage, skim, classify, tool,
continue, and result events, and asserts each progress branch consumes its
event field.

Bind every `assets/app.html` job line to the new event vocabulary
(`triage_progress`, `skim_progress`, `classify_progress`, `verify` tool
events, `await_continue`, result card). Audit rules (design law 4):

- "Checked N filenames" renders only from `index_complete` with
  model-confirmed batch counts (JI.1).
- Tool lines name the file and the *reader* ("Read page 1 (OCR) —
  CamScanner 03-15-2024.pdf"), sourced from `doc.read`'s `source` field.
- The question card shows the model's question verbatim; the Continue card
  shows checkpoint numbers and is visually a *status*, not a question.
- First-run skim shows a duration expectation once measured (E-JI1) —
  before that, it shows counts only. No invented time estimates.

**Acceptance:** DOM-level test (existing app.html test harness pattern)
mapping each event fixture to its rendered line; a grep-style check that no
progress string in app.html is emitted without a backing event field.

### JI.8 — The test program (the coverage whose absence shipped this)

All offline, `make test` lane, fake backend (`tests/fake_openai_backend.c`)
plus scripted verdicts; fixtures under `tests/fixtures/jobs/find/`:

- **(a) Well-named target:** `titli_vaccination_2023.pdf` (text layer) in
  200 junk files → triage `likely`, skim, classify `match`, finish lists it
  with evidence. The 2026-07-21 dogfood scenario, mechanized.
- **(b) Anonymous scan target (the Titli killer):** scanned PDF, no text
  layer, "Patient: Titli (feline)" only in pixels, CamScanner-style name →
  must reach the skim via `unknown`, get OCR `first_lines`, classify
  `match`. Asserts RC1/RC5 stay dead.
- **(c) Clutter realism:** 300+ junk names incl. `medicare_and_you.pdf`,
  `medical_coding_reference.pdf`, `wallpaper_gallery.zip`,
  `training_schedule.txt` (the RC1 score-table names) → none may appear in
  `matches`.
- **(d) No-canned-question:** goal "find my education certificates" →
  assert no event ever contains "pet" (regression lock on RC2).
- **(e) Already-provided detail:** goal contains a name; scripted model
  attempts `ask_user("what is the name?")` — allowed through (the model
  authored it) but the JI.6 answer-resume test asserts the conversation
  the model then sees contains both the goal and the answer. (The *fix*
  for redundant questions is context, not C censorship — law 1.)
- **(f) Sweep contract:** two planted matches → both in `matches`; one
  parked file → in `unreadable` with its error code.
- Resume suite from JI.6.

### E-JI1 — real-model verification gate (JF.4's gate, finally run)

**Status: passed on the owner-approved 50-file synthetic fixture, 2026-07-23.**
Evidence: [`e-ji1-2026-07-23/report.md`](regressions/jobs/e-ji1-2026-07-23/report.md).

On the reference M3, real Ornith (and Bonsai available for tier 2), a
**synthetic** cluttered folder (50 files: 25 scanned junk images, 15 images,
7 text files, and 3 planted targets from JI.8-a/b —
never the owner's real Downloads, and never while the owner is chatting):

- Run the Titli goal end-to-end through the app's Jobs tab. "Works" =
  both planted files in the result card with correct evidence, through the
  real interactive path.
- Measure and record: triage tokens+wall, skim files/min (cold and warm
  cache), classify wall, verify wall, total; peak RSS; thermal/swap notes
  per the machine-safety rule. Publish under `docs/regressions/jobs/` and
  copy the honest numbers into this card and the UI expectation (JI.7).
- **No performance claim of any kind before this runs.** Until then the
  card's only claim is the cache-amortization *argument*, labeled as such.

Observed in the completed gate: cold wall 182.144 s (triage through 43.398 s,
skim increment 56.826 s, classify increment 38.033 s, verify increment
43.888 s); peak gateway-plus-backend RSS 6,211,984 KiB. Swap was 365.31 MiB
at the largest sample (machine-wide, not attributed to the job); thermal
telemetry was unavailable without privileged `powermetrics` and is not claimed.

### E-JI2 — warm-folder re-run

**Status: passed on the same 50-file fixture, 2026-07-23.** The warm run
returned the planted Titli certificate; no OCR worker was observed while it
skimmed the already cached folder. Warm wall was 174.125 s; its 50-file skim
increment was 0.037 s. See the E-JI1 report for phase boundaries and raw SSE
evidence.

Immediately after E-JI1: change the goal ("find Titli's rabies certificate
specifically"), re-run on the same folder. Expected: zero OCR invocations
(all skim from cache), majority of wall time in model batches. This is the
measured proof of the amortization story — the number that justifies the
architecture to the README someday, with the E-JI1 caveats.

---

## Parameters (initial values — calibrate in E-JI1, never hardcode meaning)

| knob | initial | note |
|---|---|---|
| `JI_TRIAGE_BATCH_TOKENS` | 500 | compact 16-file coded verdict batches; confirmed against Ornith's 8,192-token context |
| `JI_CLASSIFY_BATCH_TOKENS` | 500 | same |
| `JI_SKIM_CHARS` | 400 | first-lines cap per file |
| `JI_SKIM_MAX_FILES` | 300 | per-run checkpoint budget |
| `JI_SKIM_MAX_SECONDS` | 1800 | wall checkpoint budget |
| first-skim reference | 1.14 s/file | E-JI1's 50-file cold run on reference M3; emitted as an honest remaining-time field |
| `JI_VERIFY_MAX_ROUNDS` | 24 | per-run, checkpoint-not-question on exhaustion |

## Non-goals (v1)

- Embeddings, vector stores, or any index beyond `skim.jsonl` + the read
  cache — the pure-C no-dependency constraint stands, and the skim gets a
  fair chance to prove it's enough before anything heavier is argued for.
- Filesystem-wide background indexing daemons. Jobs index the folder they
  were given, when they run.
- Mutation inside the find loop. Organize follow-ups go through JO's
  plan/apply approval, fed by verified matches only.
- Network anything.

## Execution order

JI.0 (demolition + regression locks) → JI.1 → JI.2 → JI.3 → JI.5 → JI.6 →
JI.4 → JI.7 → JI.8 green offline → **E-JI1 before any "works" claim** →
E-JI2. JI.5/JI.6 land before JI.4 because the verify loop is the main
consumer of finish and resume; building it against stubs of both avoids
rework.
