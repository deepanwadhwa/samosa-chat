# Samosa: simpler UI and useful file workflows

Created: 2026-10-01. Status: FW-1–5 in progress; FW-6–7 open.
These are repository tickets, not published GitHub issues.

First FW-1 increment: selections are saved with existing job state, restored
when reopening a saved search, and resolved by the gateway for follow-ups.
Saving selections requires no model. Invalid selections are rejected; save
failure preserves the user's choices and question for retry. New searches
retain source modification times, and follow-up reads reject changed files.
The current preview/refinement controls now describe their actual behavior.
The connected increment adds conversation association, real selected-document
answers, and reviewed copy/move/undo through the same selection. Compiled
restart/scope/action/model-clone tests pass; Ornith and Qwen passed the small
real-model two-question fixture. Neutral styling and simpler match labels are
implemented. Model forks passed a real Qwen-to-Ornith check. Discovery no longer
excludes subfolders by name scores, and bounded answers reuse complete matching
extractions from the reader shared with Chutni. Ornith passed the larger 12-file fixture, including the scanned PDF, copying,
moving, reopening, cited follow-up, and undo. Qwen also passed all three larger-fixture questions after a timeout exposed
excessive per-question reading; the appointment answer still took 286 seconds. Browser
acceptance, portable memory registration exceptions, and
broader semantic grounding qualification are outstanding; no ticket is closed.
The connected 12-file flow also passed with a ready index and deliberately
incomplete index coverage. Shared extraction reuse, stale-source rejection,
forgetting, stopped-batch restart/resume, partial undo, permission refusal and
staging interruption/retry have evidence; the final full `make test` passed.
Connected-workflow evidence:
[2026-10-01 connected increment](regressions/file-workflows/2026-10-01-connected/README.md).
Initial persistence evidence:
[2026-10-01 selection increment](regressions/file-workflows/2026-10-01-selection/README.md).

Automatic memory continuation: folder recipes and saved discovery now hand off
to the existing incremental Chutni worker, with durable waiting handoffs and
current scope status on reopen. Automatic builds save document/OCR evidence
without optional model summaries/captions. Scan errors and incomplete policy
coverage are reported as partial. Unregistered portable stores and changed
portable exclusions remain explicit registration/review cases. Later PDF previews
reuse complete matching host-reader entries with current-byte hash and reader
fingerprint checks. Browser acceptance remains open. See
[automatic memory evidence](regressions/file-workflows/2026-10-01-connected/automatic-memory.md).

UI cleanup continuation, 2026-10-02: grouped existing actions, simplified
sorting/progress, removed the stale folder-memory disclaimer, and fixed Show
all and keyboard-focus continuity in the shortlist. DOM regression evidence
is recorded in [UI cleanup](regressions/file-workflows/2026-10-01-connected/ui-cleanup.md).
That browser connection exposed no browser. The later folder-chat walkthrough
uses an isolated Playwright fallback; broader workflow visual acceptance remains open.

## Product direction

Continuation, 2026-10-03: theme contrast and document-answer wording were
cleaned up, and stale app-name/slogan checks were corrected. A larger-section
Ornith experiment improved one run from 397 to 315 seconds, but two retries
failed; it was reverted to the proven review limits. Browser setup remains
unavailable, report/inventory parity failed, and the corrected installer smoke
check still needed rerunning at that checkpoint. The later harness qualification
below supersedes these folder-chat browser, installer and local parity blockers.
FW-6 stays open; FW-7 stays gated. See
[continuation evidence](regressions/file-workflows/2026-10-03-fw6-continuation/README.md).

Walkthrough correction, 2026-10-03: folder chat now uses the existing local
decision model to choose folder/file scope and overview, inventory, metadata
and content actions. Inventory membership and reader page totals no longer
come from a lexical search shortlist. Raw thinking is filtered before display
or playback. The corrected runtime-only installer and full local regression
suite pass. Earlier browser checks passed seven folder questions, but manual
review found an unsupported unique-file assumption that their gate missed.
Subsequent checks found false subject associations and an erroneous root-only
interpretation of duplicate basenames. Grounded quoted membership proofs and
authoritative inventory filename resolution address these failures. Separate
subject extraction prevents document-selection words from becoming a person's
name. Semantic membership uses OpenDecision over every supplied literal preview;
unverified content is withheld from final generation. The final installed Qwen
multipart browser check passed: 12 files, two PDFs of 40 and 1 page, no thinking
text, neutral composer focus and one Ask action. Fast general cases cover changed
names, duplicate basenames, missing subjects/files, empty folders, content, page
totals and follow-up references; negative runs and manual corrections are retained.
OpenDecision routing passed 10/11 cases; Laya passed
5/11 and has not replaced it. See the
[harness contract](FOLDER_FILE_DECISION_HARNESS.md) and
[evidence](regressions/file-workflows/2026-10-03-decision-harness/).
These results do not close the mixed-document review and broader FW-6 gates.

Find files → keep a selection → ask grounded questions → organize files →
reopen and continue. Only then make the workflow repeatable.

Success means less work for the user. Code volume, new screens, model calls,
and passing mock tests are not measures of utility. Prefer deleting a competing
path to adding another abstraction. Reuse the current browser UI, document
reader, conversation persistence, durable jobs, Chutni store, and guarded
filesystem operations where they meet the outcome. No new framework, generic
agent platform, workflow language, or separate indexing service is in scope.

This program supersedes conflicting product/UI direction in UI_DESIGN.md,
TASKS_JOBS.md, TASKS_JOBS_INTELLIGENCE.md, and CHUTNI_JOBS_RECOVERY_PLAN.md for
the workflow above. Existing filesystem protections, runtime limits, and
protocol contracts still apply. Older completion claims do not close these
tickets. Verify current code rather than treating historical plans as shipped.

## Ticket order

| ID | Outcome | Priority | Depends on |
|---|---|---|---|
| FW-1 | Keep and reopen the files being worked on | P0 | — |
| FW-2 | Answer questions from those documents with citations | P0 | FW-1 |
| FW-3 | Copy or move the selection into a folder | P0 | FW-1 |
| FW-4 | Make folder memory useful without a separate workflow | P1 | FW-1, FW-2 |
| FW-5 | Simplify the interface around the work | P0 | FW-1; finish with FW-2–4 |
| FW-6 | Remove redundant paths and qualify the complete workflow | P0 | FW-1–5 |
| FW-7 | Repeat a proven workflow on new files | P2 | FW-6 passes |

FW-2 and FW-3 can proceed independently after FW-1. Visual work can begin
alongside them. Do not delay the manual workflow to build scheduling.

## Shared acceptance fixture and evidence

Use one small, redistributable synthetic folder about two fictional people.
Include 12 relevant documents for Person X, 8 distractors (including Person Y
and similar names), nested folders with generic names, two same-basename files,
a text PDF of at least 40 pages with a unique fact on page 37, a scanned PDF,
and plain text. Include an unreadable/unsupported file and a symlink outside
the authorized folder as separately labeled coverage cases. No personal
immigration records are required. Record ground truth paths, facts, and pages.

Reuse existing fixture generators and test harnesses. Add only missing
behavioral cases. Tests must assert outcomes, source evidence, saved state,
and filesystem contents; matching UI strings or request shapes alone is
insufficient. Run the relevant existing regression gates plus the repository's
required checks. A documentation-only change does not require model tests.

Implementation evidence belongs in docs/regressions/file-workflows/: commands,
commit, fixture version, model IDs, machine, outcomes, elapsed times, memory/swap
observations for model runs, and screenshots for UI changes. Record failures
and untested cases. Fake backends establish deterministic behavior; actual
document answers require a real supported local model through the app, including
the reference Qwen path required by the repository working agreement. Test a
second compatible installed model for switching when available; otherwise mark
that gate unverified. No claim of broad retrieval accuracy from this fixture.

Each ticket closes with: acceptance evidence, relevant regressions, and a short
list of code/UI/dependencies removed or reused. If new machinery was necessary,
explain why existing machinery could not do the job. Do not impose a line-count
quota or remove working protections to shrink code.

## FW-1 — Keep a persistent file selection across actions

**Problem:** a saved shortlist is not yet a shared context for conversation,
document reading, and filesystem actions. The user must reconstruct the task.

**Scope:** extend existing conversation/job persistence with the smallest
durable representation of the working files, original request, selection,
source identities, and completed actions. Resolve “these files” against that
selection on the server. Support adding/removing files and reopening the work.
Preserve available search results separately from the chosen subset so later
refinement does not silently discard the user's choices.

**Simplify:** reuse existing document references and job IDs. Replace parallel
selection state where possible. No separate collection-management screen or
second conversation engine. Existing saved results must remain readable or
offer an explicit conversion without deleting them.

**Acceptance criteria**

- [ ] Find, select, deselect, and reopen a file set after browser reload and
  gateway restart with the same request and exact selection.
- [ ] “Ask about these” and “put these in a folder” resolve the identical
  selected set without re-entering paths, re-uploading, or repeating inventory.
- [x] Two conversations have independent selections; no cross-task leakage.
- [ ] Changed, missing, or inaccessible files are marked explicitly; operations
  never silently substitute a different file at the same path.
- [ ] The user can inspect and change scope before an action; unsupported
  actions explain the limitation instead of being reinterpreted as refinement.

**Testing:** extend Jobs UI/persistence and compiled gateway tests. Exercise
reload, restart, two conversations, rename/change/delete between turns, empty
selection, and a request containing a path outside the saved authorized set.
Manually reopen the fixture and ask a follow-up without selecting files again.

**Starting points:** assets/app.html (`decisionSelectedPaths`,
`renderDecisionShortlist`, conversation documents), src/samosa_gateway.c
(`jobs_decision_*`, document persistence), docs/DEEP_FILE_CHAT_PLAN.md.

## FW-2 — Replace excerpt re-scoring with real document answers

2026-10-02 priority repair: [complete PDF reading and OCR evidence](regressions/file-workflows/2026-10-02-document-coverage/README.md).
Independent 40-page fixtures now cover automatic OCR, late-page evidence,
small model contexts, detailed follow-up, and bounded failures. FW-6 remains open.

**Problem:** `deepen` currently expands 100 characters to 400 and re-runs a
relevance classifier. “Ask about file” promises more than this delivers.

**Scope:** send questions over the saved selection through the existing
document reading/retrieval and answering path. Retrieve exact passages, read
additional pages when needed, and cite file/page (or stable text location).
Retain the question, evidence, and coverage for further turns. Offer a deeper
pass using a compatible available model without losing task context. A model
change must not substitute for reading the required evidence.

**Simplify:** remove the 400-character re-score as the implementation of deep
reading. Explicit UI actions need no classifier to decide their meaning.
Evaluate whether DeBERTa/OpenDecision adds measured discovery value; remove
that dependency if an existing path meets or improves the fixture outcomes.
Do not create a second document QA stack or hard-code a new model catalog.

**Acceptance criteria**

- [x] A question produces an answer or a specific evidence limitation, not
  another shortlist presented as an answer.
- [x] The page-37 fact is found and correctly cited; a second question about
  another passage works without re-upload or reselection.
- [x] All 12 readable relevant fixture documents are discoverable, including
  those in generically named folders. Final reviewed selection excludes all
  eight distractors; the user can inspect uncertain matches and correct them.
- [ ] Search reports examined, skipped, unreadable, and remaining coverage.
  A heuristic folder rejection never becomes an unsupported “all files” claim.
- [ ] A scoped answer never draws facts from deselected Person Y documents.
  Absent facts are reported as not found within the stated coverage.
- [ ] “Every mention” either completes the required coverage or identifies
  what remains unchecked; top retrieval hits do not imply exhaustive reading.
- [ ] Scanned-document OCR succeeds when available; unavailable OCR produces
  an explicit incomplete result. Citations resolve to supporting source text.
- [ ] Switching to a compatible installed model preserves selection, question,
  and evidence. Unavailable models and memory limits yield a usable explanation;
  cancellation leaves the task resumable and chat usable.

**Testing:** ground-truth late-page, multi-file, absent-fact, distractor,
scanned-page, exhaustive-query, and changed-source cases through the compiled
gateway; then the real app/model path. Check every factual answer against its
citations. Compare discovery coverage and time before/after classifier removal
if proposed. Exercise cancellation and model-switch failure deterministically;
measure actual model switching separately.

**Starting points:** tools/samosa_decision.py (`next_action`, excerpt limits),
assets/app.html (`runDecisionFollowup`), src/samosa_gateway.c document planner,
existing document harness tests.

## FW-3 — Organize selected files with a reviewable result

**Problem:** discovery does not carry the selected files into a useful folder
operation, despite existing guarded move/undo primitives.

**Scope:** support “put these in immigration_files_for_person_X” from the
existing selection. Show a compact plan with destination, exact files, and an
explicit Copy/Move choice; default to Copy. Apply the reviewed operation through
existing filesystem/job machinery, save the outcome, and keep the task usable.

**Simplify:** reuse move, journal, review, and undo paths. Add only the missing
copy behavior and selection handoff. No general shell execution or separate
organizer screen. Remove a superseded organize flow after parity is verified.

**Acceptance criteria**

- [x] A reviewed selection can be copied or moved into the named folder without
  typing source paths or rerunning discovery.
- [x] No writes occur before approval of the concrete plan. A changed plan or
  changed source cannot reuse stale approval silently.
- [x] Exactly the selected files arrive, byte-identical. Copy preserves originals;
  move updates saved references so subsequent questions still work.
- [x] Same-basename collisions, existing destinations, and out-of-scope paths
  have explicit outcomes; no silent overwrite, escape, or omitted file.
- [x] Partial failure and cancellation identify completed/pending operations.
  Restart/resume does not duplicate completed work.
- [x] Undo restores moved files; copy undo removes only unchanged files created
  by that operation. Later user edits are preserved and conflicts reported.

**Testing:** filesystem manifests/hashes before and after copy, move, undo,
collision, source change after preview, permission failure, interrupted batch,
restart, and retry. Include symlink escapes and destination changes before undo.
End the app test by asking a cited question about a moved document.

**Starting points:** src/samosa_fs.c, src/durable_job.h, gateway job apply/undo,
assets/app.html (`applyJob`, `undoJob`), existing Jobs filesystem tests.

## FW-4 — Make Chutni support the same work

**Problem:** the app exposes a separate memory workflow while Jobs explicitly
does not query that memory. Users pay setup and conceptual costs twice.

**Scope:** use Chutni as reusable folder evidence behind the same file workflow.
Reuse eligible extraction/index data, show freshness and coverage where they
affect the answer, and read current source files when indexed evidence is stale
or insufficient. Keep task selection/action history in task persistence.

**Simplify:** remove mandatory “build memory → use in chat” navigation from
ordinary file work. Put index management in source details/settings. Reuse the
portable Chutni protocol; avoid a second index or replacing sources with summaries.

**Acceptance criteria**

- [x] Successful folder parsing automatically requests an incremental shared
  memory build; saved tasks expose its status after reopening.
- [ ] The same find/ask/organize flow works with no index, an existing index,
  and an incomplete index, with truthful coverage in each case.
- [x] Repeating work on unchanged files reuses valid extraction/index artifacts;
  evidence demonstrates which expensive work was avoided.
- [ ] Modified/deleted/moved sources invalidate stale evidence appropriately.
  A previous version can only be cited with an explicit version indication.
- [x] Index availability never expands the authorized or selected file scope.
- [x] Forgetting an index leaves source files intact and the saved task reopenable;
  missing evidence can be rebuilt through the existing path.
- [ ] The ordinary workflow requires no knowledge of “Chutni,” build phases,
  index budgets, or protocol storage paths.

**Testing:** run the shared questions with index absent, ready, partial, stale,
and forgotten; compare facts/citations and coverage. Assert cache reuse with
instrumented extraction counts. Retain Chutni protocol, inventory parity,
forget, and crash-recovery regression gates.

**Starting points:** assets/app.html (`renderJobMemoryAccess`, Chutni views),
gateway folder retrieval, vendor/chutni, docs/CHUTNI_PROTOCOL.md.

## FW-5 — Reduce visual noise and expose the next useful action

**Problem:** warm brown/beige/orange surfaces, mixed display/UI typography,
many boxes, and technical details compete with the actual work.

**Scope:** one neutral light/dark palette, one system sans-serif family,
consistent spacing/corners, and restrained borders. Present conversation,
selected files, and their contextual actions together. Jobs shows ongoing or
completed work; source/index administration belongs in details/settings.

**Simplify:** remove serif display styling, redundant cards, duplicated status
blocks, and default-visible classifier scores/prompts. Keep diagnostic details
available on demand. No extra dashboard, parallel composer, theme framework,
or new frontend dependency. Consolidate existing CSS instead of layering overrides.

**Acceptance criteria**

- [ ] The user can see current scope and reach Ask, Copy/Move, and Continue
  from the results without switching between Jobs, Chat, and Chutni screens.
- [ ] The default view emphasizes files, answers, progress, and outcomes;
  internal model prompts, classification probabilities, and index mechanics
  do not occupy the main workflow.
- [ ] Existing chat, model selection, attachments, and task history remain usable.
- [ ] Light/dark views work at 390px and 1440px widths and 200% zoom without
  clipped actions or page-level horizontal scrolling. Long paths remain inspectable.
- [ ] Keyboard selection/actions, visible focus, accessible labels, and normal
  text contrast of at least 4.5:1 are verified; state is not conveyed by color alone.
- [ ] Empty, loading, partial, failed, cancelled, and completed states each offer
  a clear next step. Progress describes actual completed work.
- [ ] Before/after screenshots and a walkthrough with someone unfamiliar with
  implementation show they can find, select, ask, and organize without developer
  guidance. Record confusion and resolve blockers before closing.

**Testing:** existing browser UI tests for changed behavior, plus rendered app
inspection at the specified sizes/themes/zoom, keyboard use, and the user walkthrough.
Avoid snapshots tied to incidental HTML or tests that merely assert new colors.

**Starting points:** assets/app.html, tests/test_jobs_ui.mjs,
tests/test_chutni_ui.mjs, tests/test_sidebar_ui.mjs, docs/UI_DESIGN.md.

## FW-6 — Delete redundant paths and prove the complete workflow

**Problem:** individually passing features can still leave the user at a dead end.
Old routes and competing plans can also bring removed complexity back.

**Scope:** qualify one complete workflow and remove superseded implementations,
controls, tests, dependencies, and documentation claims after checking callers.
Reuse tests that cover retained guarantees; replace implementation-shaped tests
with outcome checks. Preserve old saved work through explicit compatibility handling.

**Acceptance criteria**

- [ ] Through the actual app: find Person X files → correct selection → ask
  about page 37 → copy to the named folder → reopen after restart → ask a second
  question → move/undo a subset. No repeated upload, path entry, or manual
  context reconstruction is needed.
- [ ] Every answer is grounded and every file operation matches the selected set.
  Interrupted work resumes without lost selection or duplicated mutations.
- [x] An [implementation inventory](FILE_WORKFLOW_IMPLEMENTATION_INVENTORY.md) names
  competing paths removed or retained with concrete reasons and removal conditions.
  Browser acceptance and broader historical-format migration remain pending.
- [ ] Removed routes/controls have no live first-party callers. Existing saved
  jobs and conversations are readable; no silent data deletion or reset.
- [ ] Current user documentation describes only verified behavior, and conflicting
  historical plans clearly link to this program. No unsupported “deep read” claim remains.
- [ ] Required regression checks pass. Real-model/browser evidence records time
  to first useful result, total workflow time, user corrections, avoidable repeated
  work, and runtime resource observations against the starting baseline.
- [ ] Zero continuity blockers remain. Performance regressions have a documented
  cause and resolution or an explicit unresolved acceptance failure.

**Testing:** automate the reusable integrated fixture through the compiled app
interfaces, then perform the browser/real-model walkthrough. Exercise one interrupted
operation and a saved legacy job. Static caller searches support deletion review;
they do not replace the end-to-end run. Stop broadening tests once required gates
and concrete risks are covered.

## FW-7 — Repeat a proven task on new files

**Problem:** grunt work only becomes offloadable when the user can trust repeat
runs and review changes without rebuilding the task.

**Scope:** from a successful workflow, save a repeatable task for the same
authorized source: find new matching files and propose copying them to the
chosen destination. Offer Run again first, then scheduling through existing
durable jobs. Show only new results and unresolved items on subsequent runs.
Keep uncertain matches and file mutations in review by default.

**Simplify:** one narrow reusable task, not an automation builder. Reuse existing
scheduling/checkpoint infrastructure. Do not add triggers, plugins, or general
tool execution to qualify this ticket.

**Acceptance criteria**

- [ ] A completed workflow becomes a repeatable task without re-entering its
  source, intent, destination, or established review choices.
- [ ] Adding two relevant files and one distractor yields exactly two new
  proposed copies; an unchanged rerun proposes none and writes nothing.
- [ ] A changed existing file is identified separately from a new file; uncertainty
  is reviewable and never silently approved.
- [ ] Pause, cancel, restart, and overlapping run requests do not lose progress
  or execute a mutation twice. Missed schedules have a documented recovery behavior.
- [ ] Results show what changed, what failed, and what needs attention. The user
  can inspect a result, ask about its files, and disable recurrence in the same app.
- [ ] Two consecutive fixture runs pass through the actual app with a real model
  before this is described as dependable recurring work.

**Testing:** controlled-clock scheduler tests, changed/new/unchanged fixtures,
concurrent requests, unavailable source, interruption/restart, and mutation
approval boundaries; then the two real runs. Use existing scheduler test helpers
rather than real-time sleeps.
