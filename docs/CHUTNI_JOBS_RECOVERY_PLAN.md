# Chutni and Jobs recovery plan

**2026-09-23 Jobs direction:** The earlier deterministic-only decision and
"complete" language below no longer describes the current Jobs interface.
The user now enters a folder and a natural-language request. A local pinned
DeBERTa model through OpenDecision chooses among the allowlisted Jobs
actions. For a find request, Jobs scores direct files in the selected folder
before deciding which subfolders to enter. DeBERTa chooses whether each
subfolder is plausible before Jobs enters it. Application bundles and generated
dependency trees are excluded even if the model gives them a high score.
Jobs then inventories the selected subfolders, reads each filename and up to
100 characters of extracted content, and asks DeBERTa to choose `plausible`
or `unrelated` using OpenDecision `choice()`. The premise contains the
natural-language request, filename, and extracted excerpt. If extraction
returns no readable text, the file is labeled as filename-only evidence.
The checked-file panel is collapsible, searchable, sortable, and limited in
height. Each file exposes the exact premise and candidate descriptions sent to DeBERTa
on expansion. The user can select up to 50 files, ask a question about one or more,
or read more of just the selected files without repeating folder inventory.
A later natural-language request can narrow the shortlist, read up to 400
characters, continue with remaining files, or show all scores. During a find
run, Jobs writes each folder check, folder
decision, file check, and file decision to the durable event log. The Jobs
screen polls that log and shows paths, selected or skipped status, and model
scores. File excerpts are collapsed in the result panel. The
same trace and saved shortlist can be reopened from Recent jobs.
The model scores are ranking signals, not calibrated chances
that a file is relevant. This local development integration depends on the
OpenDecision Python environment and the pinned cached DeBERTa snapshot;
native packaging and broad accuracy qualification remain open.

**2026-09-23 fixture check:** `tests/test_samosa_decision.py` passed with the
cached DeBERTa model, an extracted PDF, an OCR image, and a text file. It also
checks that all direct files are scored before folder decisions, even when
there are more than 250 direct files. The existing PDFium routing, PDF OCR runtime, and
OCR smoke checks passed on repository fixtures. No personal Downloads content
was used for these checks.

**Status:** deterministic recovery implementation and its synthetic acceptance
scope are complete. The shared bounded inventory is enforced by Chutni and
Jobs; policy mismatches offer a confirmed rebuild; durable Jobs events survive
gateway restarts; and both pinned decision candidates fail the measured
development screen, so Jobs stays deterministic. The mixed-folder fixture
covers PDF and text evidence, report/inventory parity, unrelated-query misses,
full-tree move undo, and controlled interruptions during report inventory,
Chutni indexing, classification, and a move batch. Browser-based visual
acceptance and broader real-folder and format qualification remain outside the
verified scope; see §9.


**Updated:** 2026-09-22

**Primary target:** 16 GB Apple-silicon MacBook Air

**Scope:** safe folder inventory, useful Chutni retrieval, practical Jobs,
durable scheduling, and a small local decision-model runtime

This plan replaces optimistic completion claims in older Jobs documents for the
current app surface. Those documents remain useful implementation history, but
the acceptance gates below define whether Chutni and Jobs are useful now.

## 1. Outcome

Samosa should let a user select an ordinary mixed folder—even one containing a
software project—and safely do two things:

1. Build a searchable Chutni without entering virtual environments, dependency
   trees, caches, build output, repositories, or symlink escapes.
2. Run small, repeatable Jobs such as reporting, finding, classifying, and
   organizing files, with a visible plan, durable progress, review gates, and
   undo.

The filesystem remains the source of truth. Models may make bounded semantic
decisions; they may not construct paths, choose arbitrary tools, overwrite
files, delete files, or bypass the approval policy.

## 2. Pre-repair baseline and why the current behavior failed

The observations below describe the behavior that motivated this plan. The
2026-09-22 checkpoints in §§5–7 record which items now have regression
evidence; all listed acceptance gates remain authoritative.

### Chutni

- `vendor/chutni/src/chutni.c` records configurable exclusion policy but does
  not enforce `exclude_globs` during traversal.
- `vendor/chutni/src/scan.c` has unbounded depth when a maximum is absent.
- The scanner traverses broadly and creates source/hash work before deciding
  whether a file has useful text content.
- `src/samosa_gateway.c` sends folder activation without an enforceable
  exclusion policy or traversal budget. The UI can display an effective policy
  that is not the policy the scanner actually executes.
- The fixed ignored-directory list catches a few exact names such as `.venv`
  and `node_modules`, but it is not enough for generated trees, nested package
  managers, case variants, custom environments, or user rules.

### Jobs

- The simple Jobs UI sends `goal`, `folder`, and a requested mode, but the
  current backend ignores the mode.
- Intent routing collapses the model's `organize` answer into the report path.
- The ordinary Jobs path can therefore enumerate or summarize a folder but
  does not reliably create and execute a useful action plan.
- More capable compiled-definition machinery exists, but the normal app does
  not expose it as a small set of understandable, end-to-end workflows.
- Earlier regression evidence validates older paths and isolated endpoints; it
  is not proof that the current browser flow completes useful Jobs.

The repair starts below these UI symptoms: one shared, policy-enforcing folder
inventory must be used by Chutni preflight, Chutni ingestion, Jobs preview, and
Jobs execution.

## 3. Updated decision-model finding

### What OpenDecision contributes

The OpenDecision repository was reviewed at commit
[`20b2a78f9822130322125c0345976a12a87c76a4`](https://github.com/deepanwadhwa/OpenDecision/tree/20b2a78f9822130322125c0345976a12a87c76a4).
It is not a generative model. It is an Apache-2.0 Python decision engine over a
zero-shot classifier and exposes four useful result shapes:

- `Choice`: select one declared option;
- `Noul`: score whether a statement is supported;
- `Score`: choose a position on an ordered rubric;
- `Relation`: distinguish support, contradiction, and missing evidence.

Its default model is
[`MoritzLaurer/ModernBERT-large-zeroshot-v2.0`](https://huggingface.co/MoritzLaurer/ModernBERT-large-zeroshot-v2.0),
an approximately 0.4B-parameter ModernBERT classifier. The model configuration
has `max_position_embeddings: 8192`, and OpenDecision's direct entailment paths
explicitly tokenize up to 8,192 tokens.

The 8K claim needs three qualifications:

1. OpenDecision's document API defaults to 384-token chunks and restricts an
   individual chunk to at most 4,096 tokens. It retrieves passages before the
   final decision rather than routinely sending an entire 8K document.
2. `Choice` delegates tokenization to the Transformers zero-shot pipeline,
   while `Noul` and the evidence backend set their own limits. Samosa must use
   explicit token accounting and must not assume every upstream route behaves
   identically at 8K.
3. The model card says a later model was being prepared to make fuller use of
   the 8K window. Capacity is not proof that relevant evidence is used equally
   well at the beginning, middle, and end of a long input.

OpenDecision's long-document pattern is still valuable: split evidence,
retrieve question-relevant passages, pack them under a known token budget, make
a typed decision, and return the passages that caused it.

### OpenDecision/ModernBERT versus Laya

Laya and OpenDecision should be evaluated through one Samosa decision contract,
not integrated as two unrelated features.

| Candidate | Best expected role | Context actually used by current package | Important limitation |
| --- | --- | --- | --- |
| OpenDecision + ModernBERT zero-shot | intent, evidence relation, longer file/document classification | architecture supports 8,192; direct entailment uses 8,192; document chunks default to 384 and cap at 4,096 | upstream Python/PyTorch service is not an appropriate shipped Samosa dependency; scores remain uncalibrated for Samosa Jobs |
| Laya root | very short bounded routing/triage | Laya config uses `max_len: 512`, despite its ModernBERT encoder being 8K-capable | base checkpoint is not the strongest typed-decision checkpoint |
| Laya typed-decisions | short `Choice`/`Score`/`Noul` decisions | Laya config uses `max_len: 1024` | benchmark specialization may not transfer; higher-cardinality choices and domain calibration need testing |

The initial hypothesis is:

- prefer ModernBERT zero-shot for decisions that benefit from retrieved file
  evidence or explicit support/contradiction/unknown;
- prefer Laya typed-decisions only if it is materially faster or more accurate
  on short Samosa routing and classification cases;
- ship one default decision model, not both resident at once, on a 16 GB host;
- retain deterministic rules for cases that do not need a model.

This is a measured selection gate, not a pre-decided model addition.

## 4. Target architecture

```text
User-selected root
        |
        v
Policy-enforcing inventory (one shared implementation)
        |-- pruned directories and skipped-file reasons
        |-- bounded metadata for eligible files
        |-- stable file identity and content hash when required
        |
        +--> Chutni manifest -> extraction -> chunks -> FTS -> cited retrieval
        |
        +--> Jobs inventory -> deterministic recipe -> optional typed decisions
                                             |
                                             v
                                      reviewable action plan
                                             |
                                      journaled executor
                                             |
                                            undo
```

Chutni and Jobs may reuse content-addressed extraction results. Jobs must not
require a Chutni to exist, and building a Chutni must never authorize Jobs to
modify the source folder.

## 5. Phase A — build one safe folder inventory

### A1. Policy layers

Apply policies before opening a directory or file:

1. **Non-overridable safety rules**
   - do not follow symbolic links;
   - do not cross out of the selected root;
   - optionally remain on the selected filesystem for large scopes;
   - skip sockets, devices, FIFOs, and other non-regular files;
   - exclude Samosa's own state and job journals if they appear under a scope.
2. **Generated-tree defaults**
   - VCS: `.git`, `.hg`, `.svn`;
   - Python: `.venv`, `venv`, `env`, `__pycache__`, `.mypy_cache`,
     `.pytest_cache`, `.ruff_cache`, `site-packages`;
   - JavaScript: `node_modules`, `.next`, `.nuxt`, `.yarn`, `.pnpm-store`;
   - build/cache: `build`, `dist`, `target`, `DerivedData`, `.gradle`,
     `.cache`, coverage output, and common IDE indexes.
3. **Marker-based pruning**
   - recognize environments by markers such as `pyvenv.cfg`, not only by the
     directory name;
   - recognize package/build trees by a small, documented marker set.
4. **User policy**
   - add/remove exclusions in preflight;
   - persist the exact accepted policy with the Chutni or Job;
   - match path components with the selected filesystem's case behavior.

Defaults should be conservative and explainable. An expert may opt a generated
tree back in, but may not disable symlink/root-boundary protections.

### A2. Traversal and work budgets

Every inventory request has explicit limits:

- maximum depth;
- maximum directories and files visited;
- maximum eligible bytes and per-file bytes;
- deadline and cancellation token;
- maximum errors retained in the response;
- no materialization of the whole tree merely to sort it;
- backpressure between traversal, extraction, and indexing.

Reaching a limit produces `partial` plus the exact limiting reason. It never
silently claims completion.

### A3. Eligibility before expensive work

- Decide from file type, extension, and a bounded magic-byte probe whether a
  file can contribute searchable content.
- Do not hash or extract unsupported files during the initial walk.
- Hash eligible content only when needed for ingestion, change detection, or a
  duplicate-finding recipe.
- Re-stat before and after reading; if identity, size, or modification time
  changes, defer the file instead of publishing inconsistent content.
- Reuse `read_cache` by content hash and parser fingerprint.

### A4. One preflight contract

Preflight and execution consume the same inventory implementation and policy.
The preflight response includes:

- directories visited and pruned;
- eligible, skipped, unreadable, and unsupported file counts;
- eligible bytes and estimated extraction work;
- exclusions grouped by reason with representative paths;
- limits and whether the result is complete;
- a policy fingerprint that must accompany confirmation.

If the root or policy changes between preview and confirmation, execution
rejects the stale fingerprint and asks for a new preview.

### A5. Inventory acceptance gates

- A synthetic `.venv` and `node_modules`, each containing at least 10,000
  decoy files, are pruned at their roots. No descendant is opened or hashed.
- A differently named folder containing `pyvenv.cfg` is also pruned.
- A symlink to an outside secret and a symlink cycle are never followed.
- Preflight and execution report the same considered/skipped counts for an
  unchanged tree.
- Cancellation interrupts a large walk promptly and leaves a truthful partial
  checkpoint.
- Warm reruns inspect metadata and changed files only; unchanged content is not
  re-extracted.

## 6. Phase B — make Chutni demonstrably useful

### B1. Activation flow

1. Select folder.
2. Run the shared preflight.
3. Show what will be indexed and what will be ignored.
4. Let the user adjust non-safety exclusions.
5. Confirm the policy fingerprint.
6. Build a durable, reconnectable index with pause and cancel.

The build status reports real units: inventory, extraction, chunks indexed,
files deferred, and model-summary work if any. It does not invent a percentage
when total work is unknown.

### B2. Search and retrieval proof

- Keep raw, provenance-bearing chunks in SQLite FTS alongside path and parser
  metadata.
- Make incremental add/change/rename/delete reconciliation the normal refresh.
- Add **Test this Chutni** to the completion screen. It runs a real query and
  displays retrieved passages and paths before the user relies on chat.
- Chat injects only useful retrieved evidence and renders citations back to the
  file and passage.
- A retrieval miss stays a miss; the app must not imply the answer came from
  the selected folder.

### B3. Chutni acceptance gates

- Planted facts in Markdown, text, HTML, and supported PDFs are found with the
  correct file/path citation.
- A fact inside an excluded dependency tree is not indexed.
- Removing or changing a source file updates results after refresh.
- Restarting the app during a build resumes from a durable checkpoint.
- Existing indexes created under the unenforced policy are marked for rebuild;
  they are not silently labeled safe and current.

## 7. Phase C — replace the Jobs blank box with useful recipes

The main Jobs screen begins with a small set of productized recipes. A freeform
goal may map into one of them, but pasted internal JSON is not required.

### C1. First recipes

1. **Folder report**
   - counts by type, age, size, unreadable/unsupported reason, and duplicate
     candidates;
   - read-only and deterministic.
2. **Find files about…**
   - filename/metadata pass, bounded cached skim, semantic shortlist, and cited
     verification;
   - returns evidence rather than merely filenames.
3. **Sort by file type**
   - deterministic destination names;
   - preview, confirm, execute, and undo.
4. **Classify an inbox**
   - choose among user-visible categories such as billing, medical, work,
     personal, and review;
   - semantic decisions may suggest categories; ambiguous items go to review.
5. **Find duplicates**
   - size grouping followed by content hashes;
   - read-only by default; no automatic deletion.
6. **Watch a folder**
   - run a chosen recipe only on added/changed files;
   - scheduled write actions always produce a review batch unless the user has
     explicitly enabled a narrowly defined safe rule.

**Implementation checkpoint (2026-09-22):** Folder report, Find files about…,
and Find duplicates are exposed in Jobs. Find duplicates hashes only bounded
same-size candidates and reports a partial result when work is capped or a
candidate cannot be read. Sort by file type uses the persisted move plan,
explicit Apply, source size/mtime revalidation, and Undo path. Inbox
classification is a bounded filename-only pass: a single category clue yields
a suggestion, while unknown or cross-category clues go to review. It does not
read content or move files. Watch folder saves a complete metadata baseline and
checks every five minutes; the supported read-only inbox or report recipe runs
only on added or changed paths. An incomplete inventory does not advance its
checkpoint. Watch runs are stoppable and remain scheduled until stopped. These
recipes complete the Phase C productized recipe list. The current recovery
slice below adds persistent history replay and restart assertions; broader
dogfood and model qualification are still open. The latest compiled-gateway
fixture covers generated-tree pruning, indexed retrieval, excluded-content
misses, deterministic Jobs recipes, triage review, sort/apply/undo, watch
deltas, and a gateway crash after one durably journaled move.

**Recovery implementation checkpoint (2026-09-22):** `GET /v1/jobs/history`
lists the 100 most recently updated persisted jobs. `GET
/v1/jobs/history/events?job_id=…` replays their durable event JSONL, and the
Jobs screen can reopen that event stream after a browser reload. The compiled
gateway regression verifies a completed report can be listed and replayed
after a gateway restart. `tests/test_chutni_controls.sh` now kills the gateway
during a Chutni build, verifies restart marks it paused, resumes it with the
new UI session token, and waits for ready. `tests/test_chutni_inventory.sh`
creates 10,000-file `.venv` and `node_modules` trees and verifies no descendant
is emitted. The shared inventory enforces a 64 MiB per-file limit and a 2 GiB
aggregate byte limit, includes both in its policy fingerprint, and reports a
partial result when either limit is reached; the inventory regression covers
both limits. `tests/test_chutni_gateway.sh` verifies dependency and hidden
environment decoys are absent from preflight/indexed retrieval, and retrieves
an HTML sentinel with its `fixture.html` citation. That gateway fixture also
checks text, Markdown, and supported PDF evidence, source changes on refresh,
and explicit no-match behavior. Refresh also has a regression for deleted files:
the query route must discard Chutni hits whose source freshness is no longer
`current`, matching the chat evidence path.

The 64 MiB per-file and 2 GiB aggregate ceilings are now sent to both the
metadata inventory and Chutni's reference scanner. The scanner records a
partial result when either ceiling is reached. Users can enter up to 16
additional exact directory names before preflight; names are validated,
normalized case-insensitively, included in the fingerprint, saved with the
scope, and applied by both traversals. The 64 MiB per-file and 2 GiB aggregate
ceilings are retained in the Chutni root policy and used as defaults on later
scans, so a manual/resumed scan cannot silently widen the byte policy. The
gateway test confirms the serialized policy and a differently cased directory
is skipped during preflight and absent from indexed search. Chutni now applies
file and byte budgets in native directory enumeration order, matching the
streaming inventory's order; saved directory hashes remain canonically sorted.
A shared comparison helper verifies exact preflight-selected paths, Chutni's
present file sources, and the limiting reason at aggregate-byte, file-count,
directory-count, per-file-byte, depth, and deadline boundaries. These fixtures
use small top-level and single-child trees. They establish matching behavior
for those unchanged fixtures, but do not prove parity for a large mixed tree,
concurrent changes during a scan, unreadable-entry races, or every filesystem
implementation. Root policy now records inventory policy version 1; preflight
marks an already-authorized index with a missing or unsupported version, as
well as an exclusion mismatch, as requiring a rebuild. The fixture verifies
both legacy and future-version indexes receive the rebuild preview. The
unchanged gateway fixture also confirms
that preflight's five eligible files equal the reference scanner's five
observed files after the custom exclusion. User rules currently accept exact directory names rather
than arbitrary globs. If an already-authorized portable store lacks a newly
requested exclusion in its recorded root policy, preflight marks it as
requiring a rebuild. The UI explains that existing passages will be withdrawn
and offers a separate **Rebuild existing memory** confirmation. Rebuild resets
the authorized root's old sources, artifacts, relations, and index entries in
one store transaction, preserves the portable root, applies the accepted
policy, and starts a durable scan in the existing scope. The gateway regression
verifies an unconfirmed request is rejected, a confirmed rebuild advances the
evidence generation, and a previously searchable passage under the newly
excluded directory is no longer returned. The compiled-gateway fixture now covers a mixed folder through Chutni and
Jobs, including synthetic searchable PDF evidence, report parity against the
shared inventory, unrelated-query no-match behavior, and a complete path/hash
manifest comparison after undo. Separate interruption fixtures kill Folder
report during inventory, the real Chutni scanner during indexing, and Jobs
classification after a durable batch; they verify restart and either explicit
resume or a persisted honest interrupted state. The existing move-batch
interruption fixture verifies journal replay and undo. These are controlled
synthetic acceptance cases, not arbitrary power-loss or concurrent-writer
proofs.

**Evidence run (2026-09-22):** The final `make test` run passed, including
installer and runtime-only release, compiled gateway with the mixed-folder
dogfood and injected move crash, Jobs UI, Chutni inventory/gateway/controls,
detached service, lifecycle, LAN access, Kimi converter, and backend limits.
Then `make jobs-test` passed four filesystem-sidecar unit tests, Jobs UI DOM
fixtures, and the compiled-gateway acceptance flow. `make test-ui-setup`
passed, including 20 Chutni UI contract checks and chooser/conversation setup
fixtures. `make -C vendor/chutni test` passed 42 conformance cases (including
one documented upstream GAP for moved-root remapping), 32 CLI checks, 130 MCP
checks, the compatibility contract, and the Python binding test. The focused
Chutni gateway/control fixtures compare all six traversal-limit boundaries
across preflight and Chutni; the gateway fixture also exercises confirmation,
stale-passage withdrawal, exclusion addition/removal, and legacy/future policy
previews. Five decision-evaluation harness unit tests pass; they validate
metric calculations and strict input validation, not model behavior. Repository
and vendor `git diff --check` pass.
The Chutni gateway fixture exercises uppercase user input against a `Private`
directory, rejects a path-like exclusion, verifies the persisted policy, and
confirms the excluded sentinel is not searchable. A direct reference-scanner
fixture verifies the aggregate byte limit returns a partial result with the
exact limiting reason; other checks confirm existing indexes require and
complete an explicit rebuild when exclusions are added or removed and
legacy/future policy versions are replaced by the supported policy. These are
offline fixture gates. They
do not qualify a real model, all supported file formats, or broader real-world
folder conditions beyond the controlled mixed-folder scenarios above.

### C2. Durable `JobSpec`

Compile every UI submission into a versioned structure containing:

- job and recipe version;
- selected root identity;
- accepted inventory policy fingerprint;
- inputs, categories, schedule, and action mode;
- decision-model identifier/revision and calibrated policy version;
- planned operations and their preconditions;
- approval state, event sequence, checkpoint, result, and undo journal.

The event log is append-only. Summary state is written atomically. Restarting
the gateway reconstructs an accurate state and resumes only resumable work.

### C3. Safety boundary

- The model sees normalized evidence and a closed list of options.
- The model returns a typed label/distribution/abstention, never a path or shell
  command.
- The deterministic compiler validates every source and destination beneath
  the authorized root.
- The executor uses no-follow operations and revalidates file identity
  immediately before mutation.
- No overwrite, deletion, or implicit conflict suffixing.
- Every move is journaled with enough identity information to undo safely.
- If the source changed since preview, that item returns to review.

### C4. Jobs acceptance gates

- Each recipe is launchable from the current browser app and reaches a durable
  terminal result.
- Report and find recipes cause no filesystem writes outside the job journal.
- Sort preview performs no moves; confirmed execution matches the preview;
  undo restores the original tree and hashes.
- Ambiguous classification is visibly held for review.
- Schedules operate on deltas, not a full rescan/reclassification of unchanged
  content.
- The UI history survives browser and gateway restarts and explains failures
  at item level.

## 8. Phase D — a native, catalogue-managed decision runtime

### D1. Stable Samosa decision contract

Create one internal interface independent of either candidate model:

```json
{
  "operation": "choice",
  "evidence": [{"id": "file-17", "text": "..."}],
  "question": "Which inbox category fits this file?",
  "options": [
    {"id": "billing", "description": "Invoices, receipts, and payments"},
    {"id": "medical", "description": "Care, insurance, and health records"},
    {"id": "review", "description": "Insufficient or conflicting evidence"}
  ],
  "max_input_tokens": 4096,
  "policy_id": "inbox-v1"
}
```

The response contains model revision, selected option, all scores, calibrated
confidence, abstention/review reason, evidence IDs, token count, latency, and
runtime memory observations. `relation` and ordered `score` use similarly
closed schemas.

### D2. Development evaluation before catalogue exposure

**Implementation checkpoint (2026-09-22):** Added an initial 36-case English
gold JSONL and a dependency-free prediction validator/reporter at
`tests/decision/`. It covers Jobs intent, five-category inbox triage, 2-, 3-,
7-, and 12-option decisions, support/contradiction/unknown relations,
instruction-bearing evidence, ambiguous/review cases, and decisive evidence at
the beginning, middle, and end. The reporter validates one pinned model
revision and exact token budget, requires per-case calibration and runtime
measurements, and computes accuracy, macro-F1, 10-bin ECE, abstention
precision/recall, p50/p95 latency, peak RSS, swap growth, and temperature.
Macro-F1 is reported over separate task and option-count groups; memory-pressure
levels are recorded per sample and summarized. Five unit tests cover
perfect-run metrics, missing predictions, mixed revisions, over-budget inputs,
and unavailable thermal telemetry. The runner uses locally cached pinned
snapshots and validates measured paired-token input for ModernBERT and
model-reported input length for Laya.

**Measured development run (2026-09-22):** Both candidates ran at their
supported 512/1,024-token budgets on a 16 GiB M3 MacBook Air with PyTorch MPS;
ModernBERT additionally ran at 2,048/4,096/8,192. Each budget used the same 36
hand-authored English cases, with decisive evidence preserved at the beginning,
middle, or end of a near-budget input. Raw rows, reports, package pins, upstream
revisions, and safetensors SHA-256 values are under `docs/evidence/decision/`.

Neither candidate is a release candidate. Laya typed reached 77.8% accuracy,
0.210 ECE, and 0.60 abstention recall at both budgets; it scored 61.5% on
five-option inbox cases and 50% on 12-option cases. Its package warned that
the 12+ option temperature was out of range. ModernBERT scored 58.3% and 61.1%
at 512 and 1,024 tokens, then 55.6% at each longer budget. From 2K through 8K
it missed all three long-position cases; at 8K p95 latency was 52.4 seconds
and peak process RSS was 2.49 GB. Both candidates had zero measured swap
growth and normal sampled memory pressure. Celsius telemetry was unavailable.
The evidence README records the full metrics and the model-free no-go decision.
This small development set is not independent validation and does not qualify
either model.

The no-go result follows the §D4 contingency: keep deterministic Jobs and
review handling, and do not add an unqualified decision checkpoint to the
catalogue. Native runtime parity, larger held-out and multilingual evaluation,
and catalogue download/verify/repair/remove remain conditional on a candidate
passing that evaluation. The deterministic synthetic dogfood gates are recorded
in §9; larger real-folder and broader format qualification remain open.

Use OpenDecision at its pinned commit as a development oracle/harness, not as
the shipped Python server. Build a Samosa-specific gold set covering:

- find/report/organize intent;
- inbox categories and deliberately ambiguous files;
- support/contradiction/unknown evidence;
- file relevance for search;
- adversarial filenames and instructions embedded inside documents;
- class counts of 2, 3–5, 6–10, and more than 10;
- English plus any languages Samosa intends to claim.

Run both candidates at 512 and 1,024 tokens. Run the ModernBERT zero-shot
candidate additionally at 2,048, 4,096, and 8,192 tokens with the decisive
evidence placed at the beginning, middle, and end. Record accuracy, macro-F1,
abstention precision/recall, expected calibration error, p50/p95 latency, peak
RSS, memory pressure, swap growth, and thermal behavior.

For this recovery, a candidate may proceed to native-runtime qualification
only if the development screen reaches at least 90% overall and inbox accuracy,
0.85 macro-F1, 0.10 ECE, and 0.90 abstention recall, with no missed decisive
evidence-position case and p95 below two seconds at its intended input budget.
These are screening thresholds, not proof of production reliability; a passing
candidate still requires a larger held-out evaluation before catalogue
exposure.

An 8K candidate passes only if it improves Samosa decisions enough to justify
its latency and memory cost. Retrieval plus a smaller packed input remains the
default when it is equally accurate.

### D3. Runtime qualification on the 16 GB M3 Air

Prototype the reference PyTorch/MPS path only to establish correctness. Do not
ship Python 3.13, PyTorch, Transformers, or a FastAPI server with Samosa.

Run a native-runtime bakeoff using pinned official weights:

- official ModernBERT safetensors as the parity reference;
- official ModernBERT ONNX FP16/int8/Q4 variants where the chosen runtime
  supports all ModernBERT operators correctly;
- a reproducible Core ML or MLX conversion only after logits and labels match
  the reference within declared tolerances;
- official Laya weights and a reproducible native conversion if Laya wins a
  decision class.

The helper process should be `samosa-decision`, sandboxable and loaded on
demand. It unloads after a bounded idle period and yields to interactive chat.
On 16 GB systems it must not coexist with a memory-heavy chat backend unless a
measured resource gate says that combination is safe.

### D4. Model catalogue work

Decision models are auxiliary components, not selectable chat personalities.
Extend the catalogue and UI with:

- `family: "decision"`, `category: "decision"`, `role: "auxiliary"`;
- `backend_kind: "decision_native"`;
- `routing: "automatic"` and `load_policy: "on_demand_per_job_batch"`;
- exact upstream revision, artifact byte counts, SHA-256 hashes, tokenizer,
  precision, maximum qualified input, languages, and qualification report;
- a **Jobs intelligence** section in Settings with download, verification,
  repair, remove, and active/idle status;
- an in-context Jobs prompt when a recipe needs an uninstalled decision model.

The current catalogue's artifact resolver and dependency allowlist are partly
hardcoded by model ID/backend. Add a validated generic auxiliary-model install
resolution under Samosa home, plus an allowlisted `samosa-decision` dependency,
instead of adding one more scattered special case.

Add a candidate to `assets/models.json` only after the artifact/runtime parity,
accuracy, memory, and licensing gates pass. If neither candidate passes, ship
the deterministic recipes without semantic classification rather than
pretending the model is reliable.

**Current decision (2026-09-22):** Neither candidate met the development
screen in §D2. The two measured checkpoints are not registered in
`assets/models.json`; deterministic inbox suggestions and review handling
remain the supported Jobs behavior. Reopen native conversion and catalogue UI
work only after a new candidate passes a larger held-out Samosa evaluation.

## 9. Phase E — made-up-folder dogfood

**Automated coverage in `tests/test_compiled_gateway.sh` and related recovery
fixtures:** A temporary mixed root with 10,000 decoys in each generated or
environment tree runs Chutni preflight, indexing and evidence retrieval
alongside Jobs find, folder report, inbox triage, sort/apply/undo, and a
scheduled changed-file delta. It checks private content misses, symlink
boundary protection, root-relative skip reasons, deterministic review of the
ambiguous inbox item, unchanged-file delta behavior, and full path/hash
manifest restoration after undo. The anonymous PDF find case invokes the real
extractor and requires the exact extracted vaccination fact in the model
request; Chutni separately retrieves its text sentinel, and an unrelated
question remains an explicit miss. Report counts and skip reasons are compared
with the same shared inventory. Folder report interruption is persisted as
non-resumable and retryable; a real Chutni process killed after scan progress
resumes after gateway restart and indexes all 30 fixture files; classification
restart resumes after its durable batch; move restart replays its journal and
undoes the recorded operation. The RSS sampler observed 6,672 KiB peak across
the gateway and Chutni worker in one synthetic run on the 16 GiB M3 Air. These
are fixture measurements, not a general production memory guarantee.

The shipped Jobs JavaScript submit handler, SSE reader, and event renderers are
exercised with a DOM/HTTP fixture, and compiled-gateway tests exercise the
product API flow. The in-app Browser runtime was unavailable in this session,
so no interactive visual browser run is claimed. Broader real-folder,
multilingual, OCR/image, and format coverage also remains unqualified. The
machine-readable run record and scope notes are in
[`docs/evidence/dogfood/`](evidence/dogfood/README.md).

Create the fixture outside the repository, under a new temporary directory:

```text
Samosa-Dogfood/
  Documents/
    project-plan.md
    anonymous-scan.pdf          # contains a planted Titli vaccination fact
    invoice-september.txt
  Inbox/
    clear-billing.txt
    clear-medical.txt
    clear-work.txt
    mixed-ambiguous.txt
  Software/
    README.md
    src/main.c
    custom-python-env/pyvenv.cfg
    custom-python-env/...       # 10,000+ decoys
    node_modules/...            # 10,000+ decoys
    build/...
    .git/...
  outside-secret.txt
  escape-link -> ../outside-secret.txt
```

Automate these end-to-end scenarios through the same HTTP/UI paths used by the
app:

1. **Chutni preflight:** excluded roots and counts are shown; no excluded
   descendant or outside target is opened.
2. **Chutni build:** the eligible files are indexed and the policy fingerprint
   in the manifest matches the preview.
3. **Retrieval:** planted facts are found with exact file/passages; unrelated
   questions do not fabricate a match.
4. **Folder report:** produces deterministic counts that agree with the shared
   inventory.
5. **Sort by type:** preview, confirm, execute, and undo; the final tree and
   file hashes equal the initial snapshot.
6. **Find Titli:** a meaningless PDF filename is found through bounded content
   evidence, with no dependency-tree scan.
7. **Inbox triage:** clear cases are suggested correctly and the mixed case is
   held for review.
8. **Scheduled delta:** adding one file processes one file; unchanged files are
   not reclassified.
9. **Crash/reload:** interrupt inventory, indexing, classification, and a move
   batch at controlled points; after restart each job is either resumable or in
   an honest review/terminal state.

Keep fixture construction deterministic. Record a machine-readable result and
a short evidence document for every live-model qualification run.

## 10. Delivery order

1. Freeze current failing browser/API scenarios as regression tests.
2. Implement and test the shared safe inventory.
3. Route Chutni preflight and ingestion through it; rebuild unsafe old indexes.
4. Prove Chutni retrieval and chat citations on the dogfood folder.
5. Ship durable deterministic Jobs recipes: report, sort, duplicates, and
   watch-folder delta handling.
6. Add the decision contract and gold evaluation harness.
7. Compare ModernBERT/OpenDecision behavior with Laya; select runtime/model per
   measured gate.
8. Add the qualified auxiliary model to the catalogue and app download flow.
9. Enable semantic find and inbox classification with calibrated review gates.
10. Run the full dogfood, restart, memory, and UI acceptance matrix on the
    16 GB M3 MacBook Air.

Do not block the safe scanner or deterministic Jobs on the decision-model
research. The product becomes useful in increments, and model intelligence is
added only where it beats deterministic behavior.

## 11. Definition of done

The recovery is complete only when all of the following are evidenced:

- Chutni prunes dependency/generated trees before descendant access.
- The visible preflight policy is the executed policy.
- Chutni search and chat retrieve planted evidence with citations.
- The normal Jobs UI completes useful recipes without pasted internal JSON.
- Job state/history is durable and scheduled work is delta-based.
- Filesystem changes are previewed, revalidated, journaled, non-overwriting,
  and undoable.
- Semantic ambiguity is held for review rather than forced into a move.
- The decision-runtime gate has one of two evidenced outcomes: a candidate
  passes Samosa accuracy/calibration, native-runtime parity, pinned-artifact,
  and safe 16 GB M3 gates; or both candidates fail and Jobs stays on its
  deterministic recipes with ambiguous cases held for review. The latter
  outcome must not be presented as semantic classification.
- If a candidate passes, model download/verify/repair/remove works through the
  app. If neither passes, no unqualified decision model is exposed in the
  catalogue.
- Documentation reports the exact tested scope and does not convert a single
  fixture or upstream benchmark into a broad reliability claim.

## 12. Source notes

- OpenDecision repository and API shapes:
  <https://github.com/deepanwadhwa/OpenDecision>
- OpenDecision decision engine at the reviewed revision:
  <https://github.com/deepanwadhwa/OpenDecision/blob/20b2a78f9822130322125c0345976a12a87c76a4/src/opendecision/engine.py>
- OpenDecision document retrieval at the reviewed revision:
  <https://github.com/deepanwadhwa/OpenDecision/blob/20b2a78f9822130322125c0345976a12a87c76a4/src/opendecision/documents.py>
- ModernBERT zero-shot model:
  <https://huggingface.co/MoritzLaurer/ModernBERT-large-zeroshot-v2.0>
- ModernBERT architecture documentation:
  <https://huggingface.co/docs/transformers/model_doc/modernbert>
- Laya model and published evaluation artifacts:
  <https://huggingface.co/convaiinnovations/laya>
