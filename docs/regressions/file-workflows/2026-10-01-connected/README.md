# Connected selection, conversation, and guarded organizing increment

Latest continuation: [manual workflow UI cleanup, 2026-10-02](ui-cleanup.md).
Urgent follow-up: [40-page document reading/OCR repair](../2026-10-02-document-coverage/README.md).

Date: 2026-10-01. Baseline: `2017598`; uncommitted working tree, including
pre-existing recovery work. Host: local macOS arm64. No CI or ticket-closure claim.

## Implemented

- A saved search has one durable conversation association. Opening Ask recovers
  the server's conversation ID, including when the browser-side link was lost.
  Other conversations cannot bind the same search and share its mutable scope.
- Ask passes the saved selection to the existing attachment/document reader.
  Each source is opened beneath the saved root without following directory/file
  symlinks; device, inode, size, and exact saved modification time are checked
  before and after snapshotting. Changed, missing, legacy identity-incomplete,
  empty, and unsupported selections produce an explicit limitation.
- Text/PDF snapshots reuse the content-addressed attachment store and extraction
  cache. They are not merged into uploaded documents, so deselection cannot
  leave an old workflow file bound as an upload. Source labels are per selected
  path, including byte-identical sources at different paths.
- File discovery now visits all subfolders allowed by the bounded source policy.
  Folder-name classification and its hard relevance exclusions were removed.
  Saved model-rejected folders are revisited; mechanical exclusions remain.
  All checked files are visible by default, with source snippets and optional
  relevance filters, so a low file score cannot hide a candidate from review.
- A complete extraction from the shared reader cache (including Chutni enrichment)
  can supply a requested page range without new extraction/OCR. Byte hash, reader
  fingerprint, full page count/order and successful coverage are checked; only
  the requested pages reach the conversation. This reuses the host reader cache,
  not arbitrary portable index summaries or search snippets.
- Explicit single-page questions use the existing bounded page reader directly.
  Small text files up to 4,000 bytes require no planning call. Fast document
  planning has a 30-second control deadline; detailed mode retains its existing
  larger budget. Default selected-file questions use fast reading; the existing
  detailed action is available for saved files too.
- Compatible model forks clone the saved search and selection into an independent
  task without rerunning discovery. Clone retries recover the same task; each
  conversation can subsequently change its own selection.
- Neutral surfaces, one system UI typeface, visible keyboard focus, wrapped review
  paths, and simpler match labels replace the prior warm/serif treatment. Detailed
  classifier scores and inputs remain available under Match details. Visual
  layout acceptance remains unverified without the browser.
- Preview Copy/Move defaults to Copy and shows every selected source/destination.
  Original subfolder paths are preserved beneath the chosen destination, so
  same-basename files from different subfolders remain distinct.
- Organize plans are existing durable child jobs. Approval names the exact plan
  hash and is checked against its persisted bytes and the parent selection.
  No source-folder writes occur during preview. Existing apply/undo/event/history
  machinery executes the plan, reports conflicts, and saves its outcome.
- Copy uses anchored directory descriptors, no-follow opens, no-clobber hard-link
  publication, full byte hashing, and operation receipts under the authorized
  root's hidden `.samosa-actions` directory. Receipts are hard links to created
  copies: they establish ownership and recovery across a publication/journal
  crash window. This is necessary because the old move journal cannot identify
  a newly allocated copy inode before copying. Undo preserves edits/replacements.
  Interrupted staging files and receipt lock files may remain in that hidden
  directory; cleanup policy is not yet implemented.
- Successful moves/undo update saved path references without changing source
  identity/evidence. The UI refreshes the original selection after an action.
  Saved actions can be reviewed, resumed, or undone after reopening the search.

## Verification and actual failures

The deterministic gateway fixture seeds search inventory to isolate state and
mutation contracts from classifier quality. It verifies native conversation
association, approval/selection rejection, exact copy bytes, repeated apply with
one journal entry, move-follow-up/undo, and copy recovery/undo after restart.
`tests/test_selected_copy.py` additionally exercises destination collisions,
later edits/replacements, symlink escape, changed source, and idempotent undo.
The document harness verifies the explicit page read bypasses failed planning
and small text is read without a planning request.

The opt-in real test uses the public discovery endpoint, then selection, binding,
and two questions. Fixture v1 is three synthetic files: a 40-page Person X PDF
with `QZ-731` on page 37, an appointment text with 17 May 2031 / `WX-482`, and an
excluded Person Y text with `YD-991`. It is **not** the complete 12/8/scanned
shared acceptance fixture.

Initial Ornith AUTO run failed after 152.25 seconds: the planner read PDF page 1,
a visual specialist provided a localization rather than the code, and synthesis
misattributed `WX-482` from the appointment text to page 37. This prompted explicit
page reading and the fast default. The subsequent Ornith run passed: discovery
17.74 seconds, page-37 answer/citation 26.61 seconds, appointment follow-up 67.82
seconds; the answer explicitly named unread PDF coverage. Ornith model version:
`Ornith-1.0-9B-Q4_K_M.gguf`. Observed model RSS 5,036,288 KiB; gateway 53,904 KiB;
swap 0 (point observations, not peak measurements).

Initial Qwen run found the fixture in 17.17 seconds and correctly answered/cited
page 37 in 147.19 seconds, but its second question hit the client's 300-second
timeout. It is recorded as failed/incomplete. After direct small-text reading
and the shorter fast planning deadline, the complete Qwen retry passed:
discovery 18.11 seconds, page-37 answer/citation 94.51 seconds, appointment
follow-up/citation 143.28 seconds. Both answers used the selected evidence. Qwen
version reported by the app: `qwen` (existing expert-streamed local installation).
Observed Qwen RSS during the retry: 2,936,624 KiB; gateway 13,232 KiB; swap 0.

The latest live v1 run cloned the successful Qwen discovery task into a new
Ornith conversation, without rerunning discovery. Both questions passed (19.72
and 70.62 seconds). Two selected files were copied byte-for-byte, reopened and
undone, then moved, reopened, asked about using their new relative paths (56.40
seconds), and undone. Original task selection remained independent. The answer
about the moved text correctly cited its new path, but also phrased a missing
fact too broadly for the partially inspected PDF. The source instruction now
requires “not found in the inspected portion” and excludes attachment IDs from
citations; semantic absence/exhaustive qualification remains open.

Native two-file batch recovery passed: an existing second destination stayed
unchanged; the first copy remained intact; retry reused its inode, completed the
second file, retained exactly two journal entries, and undo removed both created
copies. A manually invoked shell gate initially lacked its required jobsd
environment variable; the supported `make compiled-gateway-test` invocation
subsequently passed. This was a test invocation failure, not a passing app gate.

The document harness counted extractor calls for a saved task: the first named
page read used one extraction; repeat and independent fork used zero additional
calls. A page-37 question naming one PDF did not read a second selected PDF.
Light/dark primary text, muted text, accent-button text and sidebar-muted pairs
were calculated at 6.61:1 or higher. This does not establish full visual contrast
or responsive layout acceptance.

Commands used for this increment:

```sh
make test
make compiled-gateway-test
make test-document-harness
python3 tests/test_selected_copy.py
node tests/test_jobs_ui.mjs
node tests/test_sidebar_ui.mjs
python3 tests/test_file_workflow_live.py --backend ornith --clone-from job-1790886803-42291-371713364 --organize
.venv/bin/python tools/gen_file_workflow_fixture.py build/file-workflow-live/acceptance-v2
```

The first larger v2 discovery run failed before questioning: only 3 of 12
relevant paths were available because folder-name relevance decisions excluded
`01`, `02`, and `misc`. Removing that semantic traversal gate made all 12 paths
available among 22 checked candidates; excluded symlink content remained outside
the inventory. The reviewed ground-truth selection is explicit, not an assertion
that classifier ranking alone selected exactly the right files. File relevance
ranking remains imperfect (including a similar-name distractor and a low-ranked
relevant PDF). The UI therefore shows all checked files and their previews.

The shared full-cache contract test disables extraction and proves the requested
page is returned from complete matching cached evidence without leaking another
page. A partial entry under a full-document contract is rejected. This establishes
cache interoperability with the reader used by Chutni enrichment; full portable
index ready/partial/stale/forgotten acceptance is still pending.

Browser runtime setup/discovery succeeded but reported no browser available;
`agent.browsers.list()` returned `[]`. No visual/screenshots/user-walkthrough
acceptance is claimed. DOM fixtures establish interaction behavior only.

## Outstanding acceptance

FW-1–3 remain in progress. Staging interruption/retry, permission refusal,
concurrent native copy, overlapping gateway apply, and stopped-batch
restart/resume now have native evidence. Qualify broader grounding of
absent/exhaustive questions and the browser find → ask → organize → reopen
workflow. The larger reviewed fixture and scanned document passed with Ornith;
Qwen subsequently passed all three larger-fixture questions. Model
forking preserves independent scope in native regression tests and a real
Qwen-to-Ornith fork; browser qualification remains outstanding. Same-basename nested
copy, concurrent native copy retry, and clone retry/scope fixtures passed.
FW-4–5 are in progress; FW-6–7 remain open. Portable-index passage reuse,
index freshness/forget qualification, and the browser walkthrough remain open.

No new framework, QA stack, model catalog, index service, organizer screen, or
conversation engine was introduced. Existing previews remain refinement tools;
no classifier removal or broad retrieval-accuracy claim is made.

## Larger fixture and bounded question scope

Ornith passed the larger fixture with all 12 relevant paths available among 22
checked candidates. The three cited answers found the page-37 verification code,
appointment date, and image-only scan code in 64.74, 70.96, and 53.02 seconds.
Copy and move each handled all 12 selected files, preserved relative subfolders,
reopened the saved action, and restored original paths and bytes through undo.
The moved appointment follow-up cited the new path (112.20 seconds). Regression
compilation ran concurrently, so these timings are not isolated benchmarks.

The first larger Qwen run timed out at 300 seconds on the page question. The
reader now reads explicitly named sources directly; broader fast questions use
a bounded source-selection control call. Exhaustive/comparison questions and
detailed reading retain all selected sources. Omitted sources are explicitly
marked as uninspected, never treated as negative evidence, and remain selected.
Invalid source numbers from the planner fail with 422 and preserve the selection.
Native regression tests cover source isolation and invalid planner output.

Folder-memory asking opens a separate conversation when a selected-file task
is active. The gateway rejects combining both scopes with 409. This prevents
indexed folder context from broadening the user's saved selection. Source paths
that exceed the reader's 299-byte citation-label limit are refused explicitly
and remain selected for recovery, rather than producing truncated citations.

Latest focused evidence: `selected-reader-gateway.log` and
`bounded-selection-harness.log`. Larger real-model evidence:
`shared-v2-ornith-passed.log` and `shared-v2-qwen-timeout.log`. The bounded Qwen
retry passed all three questions in 100.75, 286.36, and 118.91 seconds, preserving
the full saved selection. These are correct cited answers, not a responsiveness
claim; the appointment response remains slow. See `shared-v2-qwen-bounded-passed.log`.
The final `make test` run passed (exit 0) on the source containing bounded
selection reading, scope isolation, and long-path refusal. `full-suite.log`
contains that final run. Installed release: `dev-ebc3cd7eeadc`; the app was
returned to the original Ornith backend and `/healthz` confirmed readiness.

## Coverage probes and backend-independent document timeout

A real Ornith absent-fact question about `Person X appointment.txt` correctly
reported no passport number in the inspected portion, cited bytes 0–61, and
quoted the actual appointment/reference text (20.89 seconds). This establishes
the named-source absence case only.

The first “every mention” request closed without an answer. The gateway and
backend remained healthy and the exact selection survived; saved evidence
disclosed the failed PDF planner and bounded page-1 read. Backend logs showed
continued generation until cancellation. The document receive limit had been
detected through the native `pinned_context` payload, leaving Ornith selected
work on the ordinary 120-second path. The document handler now marks selected
work internally, so the existing 600-second document bound applies across
backends. The flag cannot be supplied through HTTP headers.

`document-timeout-gateway.log` passed the compiled gateway and dependent gates.
Installed revision `dev-ab13622abf71` contains this fix. The real exhaustive
retry completed in 190.81 seconds, beyond the former 120-second cutoff. It
explicitly disclosed that only page 1 of 40 was inspected and that the requested
code could appear on unread pages; it did not claim exhaustive results. However,
it also associated record IDs from other text files with the unread PDF pages
without source evidence. That wording is a grounding defect, so broader semantic
qualification remains open. `coverage-exhaustive-retry.log` contains the exact
answer and source evidence. The full `make test` suite on this revision passed (exit 0);
`full-suite.log` contains that run. The earlier full-suite pass predates this final
timeout fix.

## Real shared-index qualification

The existing preflight and build APIs created an index for the same v2 synthetic
folder. Status reported 22 candidates, 21 indexed files, 20 readable files, one
metadata-only file, and one scan error. Enrichment recorded 40 PDF pages and one
OCR output. A new page-36 request recorded `shared_full_cache_hit` in the native
developer trace, proving reuse of the complete extraction generated by index
enrichment without extraction/OCR. Trace logging was restored to its previous
disabled state after the probe.

The first page-36 question failed semantic validation: the model returned the
title instead of the first line despite receiving correct page text. A precise
page-number-line retry returned “Page 36 of 40” with the correct file/page
citation (18.53 seconds). Both outcomes are retained.

While a synthetic appointment source was temporarily changed, the index query
withheld its stale original-date artifacts and selected-file QA returned 409.
The source bytes and original timestamp were restored, and selection stayed
unchanged. The full organize/reopen flow with this existing index passed: three cited
answers (15.50, 77.77, 17.15 seconds), 12-file copy/reopen/undo, 12-file
move/reopen/current-path follow-up (136.38 seconds)/undo. While files were moved,
the old-path index query returned no stale evidence. Forgetting the app index
association preserved the portable store, selected-source hashes and exact
selection; a cited appointment follow-up passed (20.76 seconds).

A confirmed index-policy rebuild deliberately excluded `01`, leaving coverage
of 19 candidates and 17 readable files. The index returned no `X-101` hit, but
the explicitly selected `01/report.txt` remained answerable from current source
text with its relative-path/byte citation (19.54 seconds). This is deliberately
incomplete coverage relative to the task, not an interrupted or safety-budget
partial build. Full organize/reopen qualification in that state passed: all three questions
(15.75, 78.36, 17.29 seconds), 12-file copy/reopen/undo and move/reopen/cited
follow-up (101.72 seconds)/undo. The moved answer also printed an attachment ID
despite the citation instruction; source-label cleanup is needed. The synthetic
app index association was forgotten after qualification; portable evidence and
source files remain. Interrupted/safety-budget partial-index qualification is
still open.

## Stop, interrupted staging, and citation cleanup

Selected-file evidence labels now contain the relative filename and coverage,
without attachment IDs. Uploaded-document identity and private tracing remain
in the existing attachment machinery. Gateway regression assertions verify
that selected evidence has no `attachment_id=` labels.

File apply/undo emits its current run ID and offers “Stop after current file.”
A stop request is tied to that run, finishes the current native operation,
preserves the durable journal, and reports completed/pending counts. Stale run
IDs and overlapping apply are rejected. The UI offers resume and undo of
completed work. A stopped two-file copy survived gateway restart, reused the
first destination inode, retained one journal entry per operation, and undid
both copies. Partial undo stop/resume passed in the final suite: the first destination was
removed, the second remained intact, the journal survived, and resumed undo
completed safely.

Native permission tests refused an unreadable source and inaccessible
destination without changing the existing user file. A 64 MiB native CLI copy
was killed while bytes were in private staging: no partial destination appeared;
retry published the correct SHA-256 bytes and undo restored the original state.
This native crash fixture exceeds the app's per-document read bound and does
not claim larger app-file support. Unfinished staging and lock files are
conservatively retained. Receipt ownership validates recovery/undo; unknown
staging is not automatically deleted, and lock files are retained to preserve
interprocess lock identity.

The first real workflow repeat after citation cleanup passed its three
questions and copy/undo, then its moved follow-up returned 422. Saved undo
restored all 12 source files. Backend timing showed the source planner consumed
its entire 256-token output budget; long-path filename lists can exceed that
bound. Planning now returns validated zero-based selected-source numbers or
the compact value `all`, so response length does not grow with path length.
The current installed revision is `dev-96088d96ac98`; the real repeat passed all three cited questions (18.39, 66.04, 19.11 seconds),
12-file copy/reopen/undo and move/reopen/cited follow-up (75.64 seconds)/undo.
Neither selected evidence nor final answers printed attachment IDs.
`compact-selector-workflow-passed.log` records the complete run.
The final full `make test` suite passed (exit 0), including the latest numeric
selector, stop/restart/resume, partial undo, permission and staging cases;
`full-suite.log` contains that run.

The latest full suite also passed the named exhaustive-question scope cases.
A real request for every mention in a named PDF inspected only that PDF,
disclosed page 1 of 40 coverage and unread pages 2–40, and did not introduce
facts from other selected sources (`named-exhaustive-coverage.log`). This
qualifies truthful limited coverage, not exhaustive retrieval.

A portable index scan bounded to one file returned `complete_for_policy: false`
and retired no unvisited sources. With that partial index, the real Ornith
workflow passed all three cited questions (16.93, 63.50, 18.70 seconds),
12-file copy/reopen/undo, and move/reopen/cited follow-up (77.92 seconds)/undo.
The application index association remained forgotten; this exercises portable
cache reuse and direct selected-source reading, not the browser's partial-index
status. The subsequent scan visited all 19 policy candidates, but still reported
`complete_for_policy: false` because the deliberately unreadable fixture
produced one scan error; no sources were marked missing.
See `safety-budget-partial-index.log`, `safety-budget-partial-workflow-passed.log`
and `safety-budget-index-restored.log`.

The current installed numeric-selector revision also passed real Qwen answers
for page 37, the appointment text, and scanned page 1: 93.24, 119.50 and 89.70
seconds. All answers named their source (PDF answers also named the page);
the appointment evidence was limited to its text file. This timing is not an
isolated benchmark. `qwen-current-selector-passed.log` records the run. Ornith
was restored as the active backend afterwards. The original 12 selections and
source paths remain intact.

The [implementation inventory](../../../FILE_WORKFLOW_IMPLEMENTATION_INVENTORY.md)
records removed and retained paths and explicit removal conditions. Browser
runtime discovery still returned an empty browser list on the final check.
No screenshot, keyboard, zoom or browser walkthrough acceptance is claimed;
FW-6 remains open and FW-7 has not started.

## Automatic folder memory continuation, 2026-10-02

Folder parsing now requests the existing incremental Chutni build automatically.
Waiting handoffs persist in job state; saved searches expose current memory
status on reopen. Registered exclusions are retained, changed files are
withheld/refreshed, and scan errors/incomplete policy coverage report partial
status. Automatic builds save text/OCR without optional model summaries or
captions. Later Jobs PDF previews reuse complete matching host-reader cache
entries after checking current bytes, reader fingerprint and full coverage.

The final full `make test` passed. Native checks cover creation, refresh,
waiting handoffs, incomplete coverage, forgetting and restart persistence.
The installed `dev-b90bd22349fd` passed real small-fixture Ornith and Qwen cited
answers; a later real Jobs search reported `shared_pdf_cache` and preserved the
original selection. Ornith is restored and ready. Browser discovery still
returned `[]`; unregistered portable stores, broader retrieval and full browser
acceptance remain open. FW-6 stays open and FW-7 remains gated.

See [automatic memory evidence](automatic-memory.md) for commands, timings,
memory observations, initial failures and exact limits. The larger shared
fixture was not rerun in this continuation.
