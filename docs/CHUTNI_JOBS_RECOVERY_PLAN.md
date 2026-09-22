# Chutni and Jobs recovery plan

**Status:** implementation plan; no product changes from this document alone

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

## 2. Verified baseline and why the current behavior fails

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

## 9. Phase E — made-up-folder dogfood

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
- The selected decision model has measured Samosa accuracy and calibration,
  native-runtime parity, pinned artifacts, and a safe 16 GB M3 profile.
- Model download/verify/repair/remove works through the app.
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
