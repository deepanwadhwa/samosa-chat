# Document-type search regression — 2026-10-03

All source documents are generated test fixtures. The folder from the user's
failed run and every file in it were excluded. The user's PDF, existing
conversations and default-app logs were not inspected. API and browser tests
use `/private/tmp/fw6-walkthrough-home`, gateway 18642 and backend 18643.
The visible fixture folder is `~/Documents/Samosa Walkthrough/Document Type Search v4`.
It contains 15 files: two text-layer application forms, an image-only form,
an instruction guide, a receipt, other form types, a suffix variant, unrelated
papers, a misleading filename, meeting notes and unreadable binary data.

## Cause and change

The original classifier included the user request in its evidence premise and
used a generic “plausible match” hypothesis. Its seven-document probe admitted
all seven files, including unrelated research, investing and writing documents.
The 100-character preview compounded that error.

The new hierarchy separates query interpretation, requested role, document
identity/type evidence, extra properties and selected-file actions. It reuses
the installed OpenDecision model and existing stateless local JSON judgement.
There is no production list of form numbers or question-specific answer code.
An invented form identifier is a positive generality control. Filename-only
candidates cannot be confirmed as document-type matches. Actual form IDs,
source roles and additional properties are checked independently. Similar IDs,
receipts and instruction guides cannot pass as an application form solely by
mentioning it. Secondary judgements require literal quotes and respect scope.

Initial previews have a 1,200-character limit, with the existing 3,200-character
longer-reading limit for ambiguous previews or follow-ups. Overlapping NLI
windows preserve later evidence; a later reference cannot replace a different
primary form identifier. OCR uncertainty remains visible for review. Neither a
preview rejection nor an NLI score proves complete-document absence.

The results table initially shows every checked file and its status. Both
progress and results use native tables with bounded scrolling, sticky headers,
counts, filters, expandable evidence and durable checkboxes. A selected
refinement persists its own query, so reading more preserves its constraints
without changing unselected files.

## Evidence and negative results

- `original-classifier.log` preserves the original false positives.
- `document-role-probe.log` and `literal-type-probe.log` separate identity and
  role evidence from generic relevance.
- `initial-live-v3.json` preserves uncertain instruction/tax-form results and
  a missed employment application before the independent identity/role gate.
- `initial-live-v4.json` preserves the selected-deepen regression: an abbreviated
  query target had weak expected-role scores, causing two confirmed forms to
  lose match status on rereading. It also preserves a missed employment form
  caused by treating accepted blank/filled alternatives as an extra filter.
- `initial-live-v5.json` and `query-interface-failure.log` preserve structured
  query failures: the local model put role labels into the search-kind field.
  The bounded adapter accepts those only when the separate role agrees.
- `type-variant-probe.log` shows why a bare additional-property hypothesis can
  fail on an accepted union of type variants. `type-variant-coverage-probe.log`
  contrasts that union with approval, signature and person requirements.
  Production requires a literal role-definition gate and strong independent
  entailment before treating an inclusive union as already covered by the type.
- `initial-full-tests.log` preserves an earlier compiled-test failure during
  overlapping test runs. `compiled-repeat.log` and `full-tests.log` record the
  subsequent successful runs. Later per-file-intent and UI refinements are
  additionally covered by final focused tests and installed browser checks.

`installed-hashes.json` identifies the final installed gateway, adapter and UI.
The Python adapter remains the existing OpenDecision dependency; no Laya or
second Python runtime was added. Web enrichment of search descriptions was
left for later. Scheduling and the remaining FW-6 gates are unchanged.

## Acceptance and deployment

`live-search-matrix.json` records six successful real local searches: the
original I539 wording plus selected-file rereading, instruction guides, W-9,
I-765, an invented AB-731 identifier and semiconductor research. No unrelated
file became a likely match. `live-receipt-search.json` adds a successful receipt
search on the final gateway and per-file-intent adapter. The six-query matrix
preceded the per-file refinement persistence change; that change is covered by
its regression test and the final installed browser's selected reread.
`acceptance-summary.json` contains the expected matches and measured timings.

`browser/results.json` records actual pending/checking/checked rows, all 15 file
statuses, selected-file rereading, unchanged matches, durable selection after
reload and no JavaScript errors. The first page screenshots left the table
below the viewport; the later `checked-files-table-visible.png` and
`narrow-table-visible.png` show the actual table after scrolling into view.
`browser/visual-results.json` also checks the table bounds and each column at a
390-pixel viewport. The final CSS widens the selection column so its heading
fits, and the refinement placeholder is independent of document subject.

`full-tests.log` records a successful `make test`. After the per-file-intent
change, `compiled-tests.log`, `memory-contract-tests.log`,
`decision-unit-tests.log` and `ui-dom-tests.log` record the relevant focused
checks. The decision unit suite has 23 checks including three optional skipped
older-model/reader checks; the new installed searches exercise the real model,
PDF extraction and OCR. Tests used generated data only.

The normal application now runs release `dev-6e7e7e0b8193` on port 8642 with
Ornith 9B ready. `default-installed-hashes.json` matches the tested source and
isolated installed gateway, adapter and UI. Deployment consulted only health
and runtime control state; no default conversations, job results or logs were
opened. `SAMOSA_DEFER_PENDING_MEMORY=1` keeps earlier automatic memory handoffs
queued during this process. The isolated test service was stopped after QA.
This does not complete the remaining FW-6 gates or enable scheduling.
