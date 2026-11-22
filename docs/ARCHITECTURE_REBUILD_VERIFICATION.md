# Samosa architecture rebuild verification plan

Status: native implementation in progress. Discarded Python implementation removed.

Companion: [architecture rebuild plan](ARCHITECTURE_REBUILD_PLAN.md). Change IDs
match that plan exactly. Existing tests are coverage references, not proof that
the replacement works and not a requirement to preserve old interfaces.
For a bounded first assignment, use the
[start and resume guide](ARCHITECTURE_REBUILD_START.md).

## How to use this file

For every implementation patch, identify its change ID, implement the relevant
behavioral tests, run the focused checks and record evidence. Every C01-C14 gate
must eventually pass. Split a milestone into smaller patches when needed; do not
mark the whole milestone passed because one subcase works.

Test public component contracts and observable behavior. Use fake providers,
reader/process spies and isolated temporary state for deterministic integration
tests. Avoid tests that merely mirror private function structure. Use real engines
and real extraction for the separate quality/native gates.

Do not run tests against the user's normal `~/.samosa` state, active browser
conversations or personal folders. Fixtures must be owned temporary directories.
Record prerequisite failures explicitly. A missing PDFium/OCR/model/platform is
not a passing native gate. Current test command names below are reference commands
that exist today; they may build the gateway and will need new component runners.

### Evidence required for each gate

Record change/patch ID, component versions, host/OS, fixture version, exact test
command, pass/fail/not-run status, assertions and material limits. For load checks
record configured limits, accepted/rejected counts, concurrency, model calls,
extraction calls and resource measurements. For crash checks record the injected
failure point and reopened state/receipts. Keep raw failure artifacts bounded.

Planned test filenames, commands or example scenarios are never execution evidence.

### Test layers

| Layer | Purpose | Required when |
| --- | --- | --- |
| Contract | Valid/invalid inputs, results, versions and ownership. | A public component contract changes. |
| Deterministic integration | Public calls with controlled providers, filesystem and failure injection. | Coordination, state, cancellation or effects change. |
| Native/quality | Actual extractor/engine output and semantic decision quality. | A supported native adapter, extraction path or model strategy changes. |
| Resource/load | Queues, fairness, memory/work bounds, shutdown and restart. | Admission, scheduling, streaming or concurrency changes. |
| Browser/product | Real rendered interaction and assembled workflow. | App behavior changes or a release is qualified. |

## Per-change gates

### C01: use cases and ownership

**Tests/review**

1. Trace ordinary chat, a PDF question, a folder query and approved copy/move/undo
   through named components. Each step has one responsible policy or executor.
2. Assign conversation, exact evidence, checkpoint, plan/approval and presentation
   state to exactly one owner. Identify every cross-owner access explicitly.
3. Review dependency direction. Sources/Inference require no Agent/App private
   types. Read-only inventory requires no action-executor dependency.
4. Label optional capabilities and legacy implementations separately. Confirm
   that the plan does not require a preserved gateway or a fixed repo count.

**Pass:** a small reviewed use-case/ownership matrix; no ambiguous authoritative
state or hidden reverse dependency. This is a design gate, not an executable test.

**Reference:** current gateway call sites and
`docs/FILE_WORKFLOW_IMPLEMENTATION_INVENTORY.md`.

### C02: component contracts

**Tests**

1. Valid and malformed operation, capability, evidence, run and plan payloads;
   unsupported versions and unsupported operations return typed failures.
2. Same operation ID/owner and identical parameters deduplicates; changed
   parameters with that ID fails. Different owners cannot retrieve/cancel it.
3. Out-of-range pages, invalid time/byte selectors, excessive payloads and
   invalid source versions fail before invoking a reader/provider.
4. Full, partial, failed, derived and uninspected evidence remain distinguishable
   after round-trip serialization. Unknown page totals remain unknown.
5. A fake compatible provider with a new arbitrary model ID works through the
   descriptor contract without adding App/Agent model-name branches.

**Pass:** owner-published test vectors validate on both sides of each boundary.
Wire compatibility is demonstrated independently of private storage schemas.

**Coverage references:** `tests/test_samosa_evidence.c`,
`tests/test_ui_chutni_contracts.py`. Use the cases; replace legacy schema coupling.

### C03: providers and model installation

**Tests**

1. Run a text provider through Inference with Agent, Sources and App absent.
   Validate output, streaming, usage and structured error translation.
2. Run plain source reading with all inference provider packages absent.
3. Install a small fake artifact; interrupt, resume and verify it. Wrong checksum
   or truncated download never publishes a ready artifact. Failed activation
   does not falsely report the previous provider as unavailable/changed.
4. Exercise supported/unsupported voice, vision and classification capabilities
   through descriptors. Missing optional adapters do not prevent text operation.
5. Check that the adapter never reads a chat directory or source-cache path.

**Pass:** optional adapters and install state are isolated; no gateway-required
startup; at least one real native smoke test for every adapter shipped as supported.

**Coverage references:** `tests/fake_openai_backend.c`,
`tests/fake_model_download_server.c`, `tests/test_atomic_install.sh`,
`tests/test_runtime_only_release.sh`, `tests/test_voice_gateway.sh`,
`tests/test_molmo2_gateway.sh`, `tests/test_native_summarizer_supervisor.c`.

### C04: inference admission, sessions and cancellation

**Tests**

1. Hold a fake provider on a barrier, fill its configured active/queued capacity,
   and submit excess work. Active/queued counts never exceed the declared limits;
   excess receives a bounded busy result and queue cancellation releases a slot.
2. Queue background work ahead of interactive work while a slot is occupied.
   Release the slot: both complete without FIFO/priority deadlock. Sustained
   interactive load still gives background work the configured fair share.
3. Cancel operation A while B is running/queued. A's child/socket is stopped;
   B's resources, result and provider session remain valid.
4. Bind a run to model/capability version A, change the default selection to B,
   and verify subsequent explicit A calls either run on A or return a typed
   unavailable result. They never silently use B.
5. Simulate constrained memory and a visual operation. Admission either waits,
   rejects, or safely evicts/checkpoints through Inference. Sources does not
   control primary-engine PID state. No overlapping forbidden residency occurs.
6. Test hot follow-up, eviction, cold restore and cancelled generation. The last
   good session stays recoverable; session A does not leak into session B.

**Pass:** bounded fair work, scoped cancellation and coherent session recovery.
Provider-private continuation is preserved where the adapter claims support.

**Coverage references:** `tests/test_samosa_serve.c`,
`tests/test_runtime_settings.c`, `tests/test_settings_compact_proxy.sh`,
`docs/CONVERSATION_CONTEXT.md`.

### C05: source acquisition and bounded reading

**Tests**

1. Register/upload supported text, PDF, HTML, DOCX, image and audio fixtures.
   Enforce byte/type/path limits before expensive processing. Deterministic text
   operations work without Agent or model weights.
2. Read a late page from a generated 100-page PDF. The extractor spy observes
   only the selected bounded batches; absolute page number and total are correct.
3. Exercise digital, blank, scanned and mixed PDF pages with real extraction/OCR.
   Recover a generated sentinel; label failed/incomplete OCR. Unsupported visual
   regions cannot masquerade as fully inspected text evidence.
4. Modify/replace a live file before and during extraction. Refuse stale identity
   or return a changed-source result; never cache it under the earlier version.
5. Exercise symlinks, duplicate basenames, empty files, malformed containers and
   UTF-8 range boundaries. Stay within authorized scope and defined read limits.
6. Stop a slow reader operation. Its process exits and partial results are marked;
   an unrelated reader continues. No implicit full-read fallback starts on Stop.

**Pass:** mechanical reading is independent, bounded and provenance-correct.
All shipped format/platform claims have actual extraction evidence.

**Coverage references:** `make extract-test`, `make doc-read-pdf-paging-test`,
`make test-pdf-ocr-runtime`, `make test-pdf-ocr-routing`,
`tests/test_attachments.sh`, `tests/test_samosa_html.c`,
`tests/test_samosa_docx.c`, `tests/document_reader_spy.py`.

### C06: evidence cache, indexing and enrichment

**Tests**

1. Repeat an identical qualified range read: zero new extraction. Change bytes,
   range, reader contract or extractor fingerprint: cache miss. A partial range
   never satisfies a full-document request.
2. Inject malformed, interrupted, failed and derived cache entries. Consumers
   receive qualified evidence or a miss, never an unjustified complete result.
3. Retrieve a preview from a separate Agent process through Sources' interface.
   Change Sources' private cache layout: the consumer still works unchanged.
4. Crash/restart indexing between items. Finished work is reusable, unfinished
   work resumes, and worker/queue limits hold across several registered folders.
5. Kill the optional enrichment provider. Exact extracted evidence remains
   available; derived artifacts report their failure without corrupting the store.
6. Apply a file-action receipt and modify an indexed source. Locators/index
   freshness reconcile; an earlier immutable citation is not silently rebound.
   Partial inventory/index/content coverage prohibits exhaustive claims.

**Pass:** Sources is the sole cache/index owner; no duplicate reader stack or
consumer-owned cache parser; recovery and coverage survive interruption.

**Coverage references:** `make read-cache-test`,
`tests/test_chutni_inventory.sh`, `tests/test_chutni_crash_resume.sh`,
`tests/test_chutni_controls.sh`, `tests/test_chutni_throughput.py`.

### C07: conversations and durable runs

**Tests**

1. Write a conversation/run, restart Agent, and read it from a different client
   with empty browser storage. Messages, selected scope and evidence refs survive.
2. Concurrent submissions at the same conversation revision are serialized or
   one is rejected as stale. No lost update, duplicate message or mixed turn.
3. Inject process exit before/after a state/event commit. Reopened state and
   event sequence agree; uncommitted progress is never presented as committed.
4. Disconnect/reconnect a progress stream from its last event position. Replay
   has ordered identity and no omitted committed result. Explicit run policy
   determines continuation; socket loss does not arbitrarily kill an action.
5. Retry run submission after a lost response. The original run is returned
   under the same idempotency key; changed parameters fail validation.
6. Grow the event history and inspect storage writes. Appends do not read and
   rewrite the full accumulated history for every new event. Retention is bounded
   and explicit without losing authoritative conversation or action state.

**Pass:** authoritative state is outside the browser; transactions/replay and
deduplication remain correct after restart and concurrent clients.

**Coverage references:** `tests/test_durable_job.c`,
`tests/test_conversation_binding.sh`. Historical migration tests are not required
unless a specific importer is selected for implementation.

### C08: evidence assembly, budgeting and recovery

**Tests**

1. Change prompt wording and remove old textual attachment markers. Existing
   evidence references still resolve and cite the same source/version/range.
2. Ask a follow-up supported by already admitted evidence: no extraction/OCR.
   Ask about an unseen page: a new bounded read is visible. No earlier assistant
   answer is promoted to source evidence.
3. Trigger compaction and cold session recovery. Compaction starts zero reader
   operations; restored context contains qualified admitted evidence once.
4. Use a small context limit, many sources and multimodal evidence. Count the
   final rendered request with the provider's tokenizer/context rules. Stay within
   budget; truncation/omission is explicit and preserves required provenance.
5. Feed injection text inside documents, filenames and fetched pages. It remains
   untrusted source data and cannot authorize tools, select new scope or alter
   the system policy. Literal quotes and derived descriptions are distinguishable.

**Pass:** evidence recovery depends on typed state, not prompt-string parsing;
hot/cold/compacted paths stay grounded and bounded.

**Coverage references:** `make test-document-harness`,
`tests/test_document_context_prefix.sh`, `tests/test_prompt_budget.c`,
`tests/test_prompt_budget.py`, `tests/test_memory_harness.c`.

### C09: task policies and one search pipeline

**Tests**

1. Explicit page requests, inventory counts, duplicate checks and sort-by-type
   choose direct operations without classifier or routing-model calls.
2. Metadata-only, content, compound folder and web requests select appropriate
   policies. Bounded model interpretation cannot introduce unregistered paths,
   invented source IDs or unapproved effects.
3. Invalid/repeated planner decisions, no new evidence, unavailable models and
   exhausted step/time budgets terminate with defined partial/failure behavior.
4. Run one search pipeline against interchangeable scorers. Verify full-name
   boundaries, duplicate basenames, content-only matches, semantic synonyms,
   missing previews, absent matches and partial inventory. Unsupported model
   quotes do not increase supported counts; uncertainty is explicit.
5. Test web search/fetch with controlled servers and injection-bearing content.
   Search failure, fetch failure and partial pages remain distinct. Offline/consent
   policy is respected; an unavailable web adapter leaves local tasks functional.
6. Evaluate the chosen real scorer/planner on frozen positive/negative and held-out
   fixtures. Record accuracy, false positives, uncertain outcomes, call counts and
   latency. Stub correctness does not establish semantic quality.

**Pass:** one implementation per task/pipeline; deterministic paths stay cheap;
model quality meets a recorded, predeclared acceptance threshold for the task.
Fixture-name special cases or selection by one favorable example fail this gate.

**Coverage references:** `tests/test_samosa_decision.py`,
`tests/decision/gold.jsonl`, `tests/decision/memory_cases.jsonl`,
`tests/test_document_harness_live.py`, `tests/test_document_type_search_live.py`,
`tests/test_memory_harness_live.py`, `tests/test_web_search.sh`.

### C10: checked actions and scheduling

**Tests**

1. Approve an immutable copy/move plan, then change its scope, revision, source
   identity or destination. The stale/altered request fails before any effect.
2. Test path escape, symlink substitution, no-clobber collisions, same-content
   names and cross-filesystem copy/move. Verify content, source retention/removal,
   receipts and failure cleanup using owned temporary files.
3. Inject exits before intent, after intent, after filesystem mutation and before
   receipt/state completion. Restart reconciles reality without repeating an
   already completed action or losing knowledge of a completed effect.
4. Retry the same action ID after a lost response; return its qualified outcome.
   Reuse that ID with changed inputs: reject. Cancel a multi-item run: report
   completed/pending items and retain receipts for checked recovery/undo.
5. Undo after a destination was replaced by an unrelated file. Refuse overwrite.
   Successful undo updates source references and preserves accurate receipts.
6. Submit equivalent interactive and scheduled plans. Observe the same executor,
   validation and receipts. A future scheduled action outside its durable approved
   policy fails; a one-time approval never authorizes arbitrary future changes.

**Pass:** one effect implementation, exact approval binding and safe recoverability
at every externally visible interruption point. No claim of a distributed atomic
transaction or automatically reversible side effects.

**Coverage references:** `tests/jobs/`, `tests/test_selected_copy.py`,
`tests/test_file_workflow_live.py`, current apply/undo and scheduled handlers.

### C11: scoped operations and bounded transport

**Tests**

1. Run simultaneous generation, reader, transcription and indexing operations.
   Cancel one by its owner/ID; unrelated children, sockets and runs continue.
   Unauthorized or unknown-operation cancellation does not affect active work.
2. With configured worker capacity 4 and queue capacity 8, hold work on a barrier
   and submit 100 requests. At most 12 are admitted to that worker queue at once;
   excess receives bounded rejection. Also enforce the separate ingress limits.
3. Send oversized bodies, stalled uploads and slow response readers. Authenticate
   before expensive body handling, bound temp bytes/connections, and reclaim owned
   resources on deadline/disconnect. Never delete unrelated temporary files.
4. Stop each component with idle, queued and active operations. Shutdown meets
   its declared deadline, records interrupted work and does not orphan owned
   helpers or kill processes belonging to another instance.
5. Crash a component. Unrelated capabilities remain usable; dependent operations
   fail with typed, correlated results. Retried effects remain deduplicated.

**Pass:** overload and failure are bounded end to end, not merely at the engine;
no global cancellation slot or silent cross-operation interference.

**Coverage references:** `tests/test_document_harness.py`,
`tests/test_voice_gateway.sh`, `tests/test_chutni_controls.sh`,
`tests/test_samosa_detached_service.sh`, `tests/test_v1_fail_closed_default.sh`.

### C12: thin App, authentication and lifecycle

**Tests**

1. Through a real browser: send chat, attach a source, inspect progress, ask a
   follow-up, review/apply an action and cancel a run. Results reflect service
   state; no browser task planner or private cache assumptions are required.
2. Reload/open a second client with empty localStorage. Conversations and runs
   load from Agent. Local drafts/preferences remain local without becoming truth.
3. Filenames, source passages and model text containing markup render as data.
   Display partial coverage, busy/not-ready, stale approval and operation errors.
4. Attempt unauthenticated and wrong-owner calls through public ingress and
   direct service entry points. Protect private source/run state. Local private
   IPC and explicitly configured authenticated headless access work as intended.
5. Start App against already running services, then close it. External services
   stay running. App-owned instances follow configured shutdown/detach policy;
   browser close cannot silently terminate an independently owned task executor.
6. Exercise mic/playback and available TTS/STT adapters. Missing voice, vision or
   a model installation leaves the relevant setup state usable and unrelated
   source-management views functional. Verify keyboard and real rendered layout.

**Pass:** App contains presentation/transport logic, state is owner-backed, and
lifecycle distinguishes owned instances from external/headless services.

**Coverage references:** `tests/test_samosa_app_lifecycle.sh`,
`tests/test_lan_access.sh`, `tests/test_jobs_ui.mjs`,
`tests/test_document_detail_ui.mjs`, `tests/test_voice_ui.mjs`,
`tests/test_composer_ui.mjs`, `tests/test_session_token_ui.mjs`.

### C13: independent builds, contracts and packages

**Tests**

1. Copy each target component folder outside the parent checkout and build/test
   it without sibling source trees or the original gateway. Use public dependency
   artifacts/fakes, not private source inclusion. Verify the minimal model-free
   installation and each owner's source/tests/contracts/build/dependency surface.
2. Change a Sources/App header: unrelated engine builds do not rebuild. Remove
   wildcard dependency coupling and the scheduler's whole-gateway build.
3. Exercise consumer/provider compatibility vectors for independently released
   versions. Unsupported combinations fail explicitly without lockstep upgrades.
4. Install/uninstall an optional provider/task package. Other components retain
   their declared capabilities; package dependency manifests match reality.
5. Verify the declared platform matrix in CI. macOS-only native adapters are
   optional on Linux; supported portable components pass there. A skipped native
   case is reported as unqualified, not silently counted as passed.
6. Run assembled-product acceptance from App using configured component artifacts
   and compatible-version configuration. A parent workspace script only delegates;
   no product coordinator, shared private headers or mandatory sibling source
   builds remain outside the owner folders.

**Pass:** independence is demonstrated by clean builds and mixed compatible
versions, irrespective of how many git repositories are chosen.

**Coverage references:** `tests/test_install_path.sh`,
`tests/test_runtime_only_release.sh`, `Makefile`, `tools/package_hf.py`,
`tools/install_local_dev.sh` and current CI workflows.

### C14: retirement and assembled acceptance

**Tests**

1. Scan the release build, test imports and packaging manifests. No gateway
   fallback, whole-gateway jobs daemon, legacy prompt-marker parser, consumer
   cache parser or duplicate scheduled action executor remains.
2. Run supported product workflows against replacement components with the old
   gateway unavailable. Include ordinary chat, bounded PDF follow-up, folder
   query/search, approved copy/move/undo and each optional capability shipped.
3. Repeat cancellation, component restart, stale identity and crash-after-effect
   cases in the assembled product. Inspect actual files, state and child processes.
4. Run the chosen real-provider/real-source quality corpus. Verify source
   attribution and uncertainty. Record failing cases rather than hiding them
   behind broad aggregate pass counts.
5. Run a fixed load/latency corpus on a declared host with fixed component limits.
   Record p50/p95 queue/service latency, throughput, peak/steady RSS, temporary
   disk, model calls and extraction calls. Compare with a measured reference;
   no invented speedup or mandatory line-count target.
6. Review shipped architecture/API documentation and fixture manifests. One
   current description matches the implementation; obsolete plans are archived.

**Pass:** all required C01-C13 gates have evidence; replaced paths/dependencies
are removed; unresolved required native/quality/resource gates block qualification.

## Execution record

Populate this table only after implementing and running the specified checks.

| Change | Status | Evidence |
| --- | --- | --- |
| C01 | In progress | Native S01 Sources ownership/spec; broader ownership gates open. |
| C02 | In progress | Native evidence/conversation/action/locator/session/operation contracts have qualified subsets; end-to-end parent IDs, deadlines/cancellation and App transport remain open. |
| C03 | In progress | Native fake/real text, owned activation and verified local/HTTPS catalog setup subsets qualified independently; production distribution, optional packaging and wider capabilities remain open. |
| C04 | In progress | Native FIFO79, scoped operation693, controlled HTTP146 and opaque session1106 checks pass on Mac/native ARM64 Linux; actual hot/cold/compact passes. Residency and wider host resource qualification remain open. |
| C05 | In progress | Native bounded immutable UTF-8 and selected PDFium pages/fingerprints qualified; OCR/other readers/live-registration distinctions open. |
| C06 | In progress | Sources-owned evidence cache, resumable literal index593, owner1140 and locator2608 subsets pass; enrichment/semantic retrieval and broader index freshness remain open. |
| C07 | In progress | Native durable revisions/runs/events/idempotency/recovery/quotas and owner-scoped run cancellation qualified subsets; provider propagation and service/streaming gates open. |
| C08 | In progress | Native exact rendered count/pin, literal evidence, hot/cold replay and explicit compaction subsets qualified; full operation/transport gates open. |
| C09 | In progress | Native document/ordinary chat and verified finding110/public220 subsets pass; folder questions/web/semantic scoring/organization and corpus evaluation remain open. |
| C10 | In progress | Checked copy972/undo477/move510 and automatic receipt handoff134/60/public220 qualified; durable scheduled policy/organization integration remain open. |
| C11 | In progress | Native bounded helper groups/parent-death, Inference owner-scoped cancellation and Agent durable run/helper cancellation qualified subsets; correlated parent/provider cancellation, service ingress/auth/shutdown/load remain open. |
| C12 | Planned | Not run. |
| C13 | In progress | Sources/Inference/Agent independently copied Mac/native ARM64 Linux builds/tests/releases and public interactions pass; App, assembled version pins/minimal installation and remaining optional platform declarations open. |
| C14 | Planned | Not run. |

## Resume checkpoint

Current checkpoint, 2026-11-21: preserve/reuse existing working monolith code;
no Python anywhere in project runtime/tooling/tests/CI; native Mac is the required
target and Docker/Linux work is deferred. Ordered work and the deferred post-refactor
Inkling-2-Mini catalogue request are in ARCHITECTURE_REBUILD_PRIORITIES.md.

LIFE-07n checkpoint, 2026-11-21: the original shared text/visual generation gate,
visual selection/reclaim/pause/resume/60-second wait and lower-side/owned-page
retry now belong to Inference turn_control.c/public API. Gateway retains trusted
host/selection/evidence adapters. A reproduced direct-visual close reset race is
fixed by withdrawing only its own generating contribution. Native normal/SAN,
fresh31-file standalone builds, original visual/voice/selection/LAN/App workflows,
all15 ported prompt-budget scenarios, exact packaging/native install and retained
actual-model parity qualify the recorded scope. Original2048 heading quality
remains unqualified; no model algorithm was changed. The stale raw-PID budget
fixture is repaired through backend_process, preserving its domain assertions;
its Python driver/caller are retired for Node. Evidence and limits: Inference
qualification/life07n-turn-control/README.md. No Python/model fetch/Docker/VM/Linux
work. Common controls/Activation/observed identity/capability readiness, stale
rejection/last-good, guardian-death/pre-admission recovery and full area3 remain
open. This preserves exclusion; global reservations/fairness remain area4.
Later Inference areas, Python removal, Agent/App, pinned assembly and qualified
retirement remain. Sources stays closed; Inkling stays after the full refactor.

Historical LIFE-07m checkpoint, 2026-11-21: default VisionPsy/Molmo launch ownership now
reuses the existing EngineProcess guardian. Original model/protocol implementations,
VisionPsy512–2048/64-KiB/120-second and Molmo image/joint/native-video1-MiB/
600-second profiles, inherited diagnostics and grace/retry behavior are preserved.
Actual PID/group remains reserved until cleanup acknowledgement; resistant helpers,
parent death and pending shutdown are qualified. Explicit handoff owns only prepared
PDF pages; durable image/video/PDF/model files stay borrowed. Existing lower-side
retry gives the page back before unload and retakes it after restart. One reproduced
Mac write-pipe HUP timeout is fixed without changing production deadlines.
Final native/SAN276 and independent source276 pass, original supervisor/launch/
protocol assertions remain, and existing guardian525/primary676/Whisper522 pass.
Both actual compiled Gateway PDF SIGKILL checks clean processes and prepared pages,
while preserving durable inputs and an unrelated peer. Original VisionPsy/Molmo
workflows and exact package/native install/upgrade pass. Actual-model parity and
compatibility results are in Inference qualification/life07m-visual-guardian/README.md.
Original VisionPsy2048 heading quality fails this fixture and stays explicitly
unqualified;1536 heading recognition passes. Do not change model algorithms for
an ownership extraction. No Python, model fetch/conversion, Docker/VM or Linux work.
Next complete common controls/activation/observed identity/capability readiness,
stale rejection/last-good and guardian-death policy, then move the existing shared
turn gate/pause/resume/retry coordination into its owner. Pre-admission crash
recovery stays open. Area3/full refactor remain incomplete. Later Inference areas,
Python removal, Agent/App, pinned assembly and qualified retirement remain;
Sources stays closed absent a demonstrated defect. Inkling stays after refactor.

Historical LIFE-07l checkpoint, 2026-11-21: legacy Whisper CLI now reuses existing
EngineProcess group/lifeline/retained-workspace ownership. Original English/-nt,
txt/SRT, long-window inputs,16-MiB UTF-8/result validation and return codes stay.
The command profile reserves actual PID/workspace until output validation and
tracking withdrawal, then finishes reap/cleanup; cancellation handles resistant
helpers and retains busy ownership through a stalled guardian. No new model,
transcription server, guardian or30-second clip restriction was introduced.
An actual killed-Gateway check reproduced a leaked caller-prepared WAV. Explicit
owned handoff now moves only prepared microphone/decoded/PCM windows into the
existing workspace; durable WAV attachments remain borrowed. Readable decoder
output is made0600 during handoff, preserving the original video workflow.
Final native/SAN522 and fresh independent522 pass, with all54 original fixture
assertions retained. Original runner reproduces engine/helper/result leaks on
owner death (survivors explicitly cleaned); fixed native and actual Gateway
SIGKILL checks remove processes/prepared/result workspace. Existing guardian525,
primary676 and activation/Whisper/Vision/summary normal/SAN pass. Full voice and
durable audio including271 seconds/30 windows, video, cache/cancel/restart pass;
actual installed Whisper txt/SRT/expected words and exact package/native install/
upgrade pass. See components/samosa-inference/docs/qualification/
life07l-whisper-guardian/README.md for evidence, source/input hashes and excluded
failures. Entire Gateway is audited against only PID withdrawal and explicit
prepared-input ownership changes; primary owner only moves its self-path helper.
Next integrate default visual helpers using the existing guardian without
shrinking VisionPsy2048/64-KiB/120-second or Molmo image/joint/native-video1-MiB/
600-second profiles, traces/retry/grace. Prove/fix its stubborn-helper path.
Common activation/identity/readiness/stale rejection/last-good, guardian-death and
pre-admission crash recovery/shared coordination remain open; area3/full refactor
are incomplete. Later Inference areas, Python removal, Agent/App, pinned assembly
and qualified retirement remain. Sources stays closed; defer Inkling until after
the full refactor. Native Mac only; no Python/model fetch/Docker/VM/Linux work.

Current qualified Inference revision/pin:dbb680a, committed and backed up on
origin/refactor/extract-working-modules. Earlier pins below are historical.

Previous LIFE-07k checkpoint, 2026-11-21: primary backend_process now reuses existing
EngineProcess guardian, retained ownership lease/lifeline and WNOWAIT cleanup.
Original Qwen/Maple/llama env/log/argv/exec and actual PID/marker are preserved;
server lifetime is unlimited, append/fallback logs unchanged,8-second TERM grace
retained. No second guardian/server or model algorithm was added. Guardian holds
identity until host acknowledgement/death and marker cleanup/reap; pending stop
keeps ownership and refuses replacement. Gateway only dispatches before config
and creates existing run directory before ownership acquisition.
Final primary676 normal/SAN, fresh independent source676 normal/SAN, existing
bounded guardian525 normal/SAN, activation/Whisper/Vision/summary normal/SAN,
actual assembled Gateway SIGTERM/SIGKILL/helper cleanup, original selection/
rollback/settings/LAN/App/llama, exact package/native clean install/upgrade and
retained actual Qwen4-token generation/zero swap growth/native shutdown pass.
Evidence/limits: components/samosa-inference/docs/qualification/
life07k-primary-guardian/README.md. Failed development/harness attempts are kept
and excluded from passes. Pre-existing vendor/chutni edits are preserved.
Next reuse the same guardians for legacy Whisper txt/SRT/long-window/16-MiB,
then default visual wide/native-video profiles; retain INFERENCE_GUARDIAN_COMPATIBILITY.md.
Common activation/readiness/identity/stale rejection, guardian-death policy and
shared coordination/full area3 stay open. Later Inference areas, Python removal,
Agent/App, pinned assembly and qualified legacy retirement remain; defer Inkling
until the full refactor. Native Mac first; no Python/new model/Docker/VM ran.

Previous qualified Inference revision/pin:1194d31, committed and backed up on
origin/refactor/extract-working-modules. Earlier pins below are historical.

Previous LIFE-07j checkpoint, 2026-11-21: existing primary-backend launch/probe/group/
marker/stop and PID state mutations now belong to Inference's backend_process
public owner/archive. Thin Gateway callbacks preserve original mutex/upstream/
shared-specialist coordination and trusted selection/settings/llama inputs.
Audit proves original bodies plus only the reproduced helper-cleanup fix, which
reuses guardian WNOWAIT before finishing the owned group/reaping. Old compiled
Gateway reproduces the leak; fixed Gateway passes the same fixture. Native
normal/SAN398 checks, fresh archive, original selection/rollback/settings/LAN/
app/llama flows, exact package/clean install and actual retained Qwen old/new
generation/native shutdown pass. Affected test helpers are ported off Python
with original assertions retained. Exact evidence/limits: components/
samosa-inference/docs/qualification/life07j-primary-process/README.md.
Next LIFE-07k follows INFERENCE_GUARDIAN_COMPATIBILITY.md to reuse existing
guardians/host adapters without shrinking primary/Whisper/visual contracts.
Common lifecycle readiness/identity, parent-death, shared coordination and full
area3/later/full-goal qualification stay open. No Python/new model/Docker ran.
Qualified Inference revision/pin:20832f5, backed up on the same origin refactor
branch. Earlier pins and checkpoints below remain historical.

LIFE-07i checkpoint, 2026-11-21: Maple's original10 implementation/header files,
packer and20 C++ tests/diagnostics move to Inference. Source bytes and assertions
are preserved after owner include wiring; declared native MLX inputs, frozen
test data, flags/frameworks/output names and original memory guards remain.
18 supported C++ builds,9 native runs,3 sanitizer checks, independent source/data
builds, Gateway quarantine/runtime settings, package and clean install/upgrade
pass.32 parent source/test/header copies (7115 lines) are retired. Two scratch
compile failures reproduce in the original baseline and remain legacy diagnostics.
No real Maple checkpoint or catalogue readiness is claimed. Exact commands,
source/dependency pins, scope and inherited failures are in components/
samosa-inference/docs/qualification/life07i-maple-sources/README.md.
Next LIFE-07j moves existing primary launch/group/reap/probe ownership, then
guarded legacy Whisper and shared coordination. Area3/full goal remain open.
No Python/new model/Docker work ran. Earlier checkpoints below are historical.
Qualified Inference revision/pin:2afb6c5, backed up on the same origin refactor
branch. This is the recorded native source/test boundary, not a full area3 pass.

LIFE-07h checkpoint, 2026-11-21: the original Qwen model/cache/vision/kernels/
support implementation and useful tests move into Inference. Chat delegates
CPU/OpenMP/separate experimental schedule builds with original flags/outputs;
Maple compiles the owner cache. All11 copied source/support files compare
identical; test changes only wire owner/include and explicit caller App assets.
Original native suites normal/SAN, independent source builds, actual old/new
Mac model CLI comparison, gateway runtime settings, package and clean install/
upgrade pass.16 parent copies (10297 lines) are retired. Exact evidence and
remaining limitations: components/samosa-inference/docs/qualification/
life07h-qwen-sources/README.md. Next LIFE-07i moves original Maple sources/builds/
tests, then remaining primary launch/group/reap/probe ownership, guarded legacy
Whisper and shared coordination. Area3/full goal remain open; no Python/Docker ran.
Qualified Inference revision/pin:cbc165f (code7216f8b plus evidence wording),
backed up on the same origin refactor branch. Earlier pins remain historical.

LIFE-07g checkpoint, 2026-11-21: native visual engine builds/tests now delegate
to Inference's existing sources with explicit native MLX inputs. Original flags,
outputs, media frameworks and kernel pairing are preserved. Original VisionPsy
tests and Molmo packer move unchanged;18 identical parent files (3884 lines)
are retired. Fresh standalone builds, original components normal/SAN and real
heading gate, rebuilt actual VisionPsy/Molmo owner normal/SAN, full gateway
visual workflows, package and clean-install/upgrade gates pass. Scope, commands,
source/dependency hashes and excluded attempts are recorded in
components/samosa-inference/docs/qualification/life07g-visual-builds/README.md.
Next: remaining primary-backend source/build/launch ownership, guarded legacy
Whisper and shared coordination per INFERENCE_AREA3_REMAINING_AUDIT.md.
The complete goal, area3 and all later gates remain open. No Python/Docker ran.
Qualified Inference revision/pin:ecf729e, backed up on the same origin refactor
branch. Earlier pins below remain historical checkpoints.

LIFE-07f checkpoint, 2026-11-21: original visual host probes, resource budget,
resident-estimate reserve and bounded reclaim wait now belong to Inference's
vision_budget.c/public archive API. Source audit proves original policy, native
probes and timing/trace bodies preserved after explicit type/sample/control
wiring. Native normal/SAN25 boundary cases and wait recovery/unknown/critical/
stop/deadline cases, actual Mac probes, fresh source build, existing gateway
Molmo/VisionPsy and package/clean-install checks pass. Turn pause/resume and
lower-side retry behavior remain unchanged. No Python or Docker/VM/Linux work ran.
Exact commands, hashes and scope: components/samosa-inference/docs/qualification/
life07f-visual-budget/README.md. INFERENCE_AREA3_REMAINING_AUDIT.md inspects all
four area3 requirements against current code and records why area3 remains open.
Qualified Inference revision/pin:056578e, backed up on the same origin refactor
branch. Earlier pins below remain historical checkpoints.
Next LIFE-07g delegates existing visual engine builds/tests to their existing
owner source, retires proven duplicates, then follows that audit through remaining
primary backend, Whisper guarded execution and shared admission/coordination.
The full goal and all unfinished later-area/Agent/App/assembly gates stay intact.

LIFE-07e checkpoint, 2026-11-21: the original shared Whisper runner now belongs
to Inference's whisper_process.c/public API and owner archive. Gateway retains
explicit original selection/fingerprint and shared busy/PID/cancel callbacks.
Mechanical source audit proves helper/core/coordination and seven return values
preserved; no model algorithm, CLI flags, long-window or cache behavior changed.
Native normal/SAN txt/SRT/271-second/16-MiB/control/cleanup gates, unchanged
verified Whisper508/511 checks and fresh standalone builds pass. Original voice
and full durable-audio workflows pass after porting all embedded Python fixtures
and assertions to Node/shell. Native Mac AAC/MP3/video audio, resume/cancel/restart,
overlap/tail and30-window retrieval assertions remain. Clean owner-source staging
and gateway installation pass. Retained actual Mac Whisper recognizes expected
native-generated words through txt and SRT in normal/SAN C owners; SRT timing
accuracy is not qualified. No Python or Docker/VM/Linux work was invoked.
Commands, source/artifact hashes, excluded attempts and precise limits:
components/samosa-inference/docs/qualification/life07e-whisper-runner/README.md.
Qualified Inference revision/pin: 4ad140d, backed up on
origin/refactor/extract-working-modules. Older pins below remain historical.

Next LIFE-07f extracts the existing Mac visual budget/reclaim policy, then audits
remaining area3 and consolidates existing guardian/shared-admission ownership.
Preserve the original workloads; do not add a new scheduler or narrow the
legacy paths to prepared-clip limits. Full area3/areas4–7, Python/CI migration,
Agent/App/assembly and legacy retirement remain open. Earlier checkpoints below
retain historical evidence and pins.

LIFE-07d checkpoint, 2026-11-21: original gateway visual launch-profile/close
bodies now belong to Inference's existing specialist module/public archive.
Mechanical source audit proves identical starts after explicit input/trace
substitutions. Close has one reproduced defect fix: inactive framing no longer
skips retained worker/lease cleanup. Native normal/SAN launch/protocol/supervisor/
guardian, fresh source subset and gateway summary/runtime/Molmo checks pass.
Full VisionPsy integration passes after porting Python fixtures/assertions to
Node/shell. An obsolete image text-only expectation was reproduced on a binary
from original Chat5d5f5fe; tests now preserve both established image-floor and
document-text-only behaviors. Product routing, resource policy and engines are
unchanged. Clean package/manifest/version and gateway installation gates pass.
Exact commands, excluded development failures and limits:
components/samosa-inference/docs/qualification/life07d-visual-launch/README.md.
Qualified Inference revision/pin: ad3a1e1, backed up on
origin/refactor/extract-working-modules. Earlier pins below are historical.

Next LIFE-07e: extract/reuse the original shared Whisper runner with text/SRT,
microphone/long-window behavior, selected model identity, cancellation and cleanup.
Follow EXTRACTION_GATEWAY_LIFECYCLE_AUDIT.md; do not narrow long audio or invent a
new server. Full area3/areas4–7, remaining Python/CI migration, Agent/App/assembly
and legacy retirement remain open. The following records retain prior evidence.

LIFE-07c component visual residency is qualified on this Mac. The existing
Vision/temporal verification, white-alpha P6 preparation, command encoders and
strict replies now share `visual_assets.c`; both legacy and resident adapters
reuse them. `visual_host.c` adapts existing Activation, guardian workspace/duplex
ownership and SamosaMmSession. No model/backend algorithm or App policy changed.
Optional public registry profiles require bounded native generation before ready
and preserve complete pins/last-good state. Exact commands, source/artifact hashes,
excluded development failures and limitations:
`components/samosa-inference/docs/qualification/life07c-visual-residency/README.md`.
Qualified Inference revision/pin: `4b35a35`, committed and backed up on
`origin/refactor/extract-working-modules`; older preservation pins below are
historical checkpoints.

Native normal/SAN owner and public registry pass changing-input residency, queued
cancellation preserving the active request, deadline invalidation, mutation and
unsafe-directory refusal, failed-ready last-good retention, parent death/unrelated
peer survival/reacquisition and active EOF cleanup. Retained actual VisionPsy
heading and Molmo2 changing-frame cases pass normal/SAN C owners on isolated
private clones. External model engines are not SAN instrumented. Original
one-shot/protocol/activation/registry regressions, fresh owner source-subset builds
and Chat gateway/summary/runtime checks pass. No Python or Docker was invoked.
Gateway lifecycle cutover, Whisper audit, full area3 and independent release stay
open; this is a component checkpoint rather than full Inference completion.

Read-only next-boundary inventory is now in EXTRACTION_GATEWAY_LIFECYCLE_AUDIT.md.
Next LIFE-07d: mechanically extract original visual launch-profile/close glue
through the existing public supervisor/trace boundary, then the shared Whisper
runner. Preserve larger-image, long video/audio and App policy behavior; current
prepared-input profiles are not full gateway substitutes. Areas4–7, remaining
Python tooling/CI migration, Agent/App/assembly and
legacy retirement remain open. The checkpoint below retains earlier evidence.

LIFE-07b is qualified on this Mac: the existing gateway Molmo2 analyze and
VisionPsy inspect bodies now belong to Inference's `specialist_protocol.c` and
public header. Gateway keeps thin calls and explicit trace callbacks. Original
function bodies, JSON fields/defaults, framing, Unicode, limits/error behavior
and engine loops are preserved; the host retains its generic numeric helper for
document selectors/product validation. Inference commit: 3e9b6f1.

Public native protocol normal/SAN passes 25 cases per build. A fresh native
source subset builds the owner archive and passes protocol/control/owned-close
normal/SAN plus summary regression. Chat summary/runtime/Molmo assertions pass.
Shell manifest/version checks and clean installed gateway/jobsd, atomic upgrade/
corruption refusal, all six PATH cases, native HTTP fixture and runtime-only
install/upgrade with unchanged model-request/legacy-identity assertions pass.
The guarded package/HTTP/runtime-only checks refuse/record PATH-based Python calls.
An exploratory fixture run still invoked a second overlooked Python generator;
it is excluded from Python-free evidence, replaced with shell/awk and rerun under
the guard. No Docker/Linux tests ran in this assignment.

Retired tools/package_hf.py and tests/test_package_pdfium.py; their shell ports
preserve existing staging, verification and version behavior. Remaining Python
drivers/default CI are still open and were not broadly run. The real OCR Python
test only received an updated shell-package reference; its migration is pending.
Commands, host, logs, source audit and exact hashes:
`components/samosa-inference/docs/qualification/life07b-specialist-protocol/README.md`
and docs/EXTRACTION_NATIVE_PACKAGING.md. Earlier owned-close evidence is retained
at qualification/life07a-specialist-owned/README.md in Inference.

Local pre-edit recovery snapshot: /private/tmp/samosa-extraction-checkpoint-z6b6bwly
(parent/Inference/modified Chutni base source, working/index patches and verified
changed-file archives). Parent and Inference are on refactor/extract-working-modules
branches. Inference preservation commit: 214fe75 (existing lifecycle/package work
plus the resumed native Mac ownership fixture and evidence); this is a checkpoint,
not full release qualification. Parent/Agent preservation commit: ace72ef. Both
repositories' refactor/extract-working-modules branches are backed up on origin;
the parent pins Inference 214fe75 and Sources 520550d. Agent is now tracked in the
parent while its independent repository remains future work. The four pre-existing
dirty Chutni files were left unchanged/outside these commits and remain covered
by the verified local snapshot. Remote backups preserve source; they do not imply
full current-suite qualification or completed Python removal.

Historical next LIFE-07c: reuse vision.c run/model_verify and temporal.c prepared-frame/
package validation with the existing guardian workspace/duplex owner and summary
host launch/activation pattern. Finish visual residency and qualify actual
generation before public readiness; preserve model loops and App policy. Full area 3,
areas 4–7, Python-helper removal, Agent/App/assembly/retirement remain open.
The entries below retain their historical dates and evidence.

Latest checkpoint, 2026-10-25: LIFE-07a is in progress. Added bounded caller
control to the existing extracted visual supervisor; both original protocols keep
their framing and retain ownership until close/reap. Evidence and remaining work:
[transport checkpoint](../components/samosa-inference/docs/qualification/life07a-specialist-control/README.md).
Next: borrowed guardian endpoints/retryable close, workspace/duplex ownership,
then reuse visual verification/preparation and real generation readiness.
Full Inference/Agent/App/assembly/retirement and the repo-split goal stay active.

### LIFE-06b — changing prepared image decisions, 2026-10-25

The existing NativeImage/preprocessing/typed executor now serves changing frames
on one loaded context through a bounded explicit envelope. Same DecisionHost,
asset verifier, activation snapshot and registry controls; public DTO/provenance/
receipts and original image/text modes are preserved. Independent image/typed1MiB
limits and512-axis bounds remain. Native macOS/Linux normal/SAN codec21/21 and
image/text lifecycle8/8 pass. Actual macOS image20-case and text10-case original
corpora pass resident and legacy modes normal/SAN, within unchanged0.005 bounds.
The old image engine fails new-mode readiness without state/lease leakage.

Evidence/commands/provenance/terminal handles: [LIFE-06b evidence](../components/samosa-inference/docs/qualification/life06b-image-registry/README.md).
New engine3054cf6a1beead8b75afa911ac6467a9c82e0b8f6bc8908931c0b2a3069c9458
is a local candidate; production distribution admission remains open alongside
T5. No model/runtime algorithms were replaced or legacy paths retired.

Next LIFE-07a reuses specialist_supervisor.c/SamosaMmSession and existing
VisionPsy/Molmo2 load/unload loops, with original verification, P6/white-alpha
preparation, reply/usage checks and guardian workspace/Metal-cache behavior.
Require actual native generation before public readiness. VisionPsy/Molmo2/
Whisper lifecycle, full area3/areas4–7, events, Agent/App/assembly/retirement remain
open; the full repo-split goal stays active. No live qualification handles remain.

Earlier continuations below are historical.

### LIFE-06a — public persistent text decisions, 2026-10-25

Qualified: existing persistent decision engine/typed algorithms, DecisionAssets
loader verification and guardian transport now back registry activation/readiness.
The public adapter preserves normalization, result validation/provenance and
receipts. Last-good stores all four pins/profile as a recoverable hash-bound
bundle snapshot, verified before launch. Native macOS/Linux normal/SAN8/8
scenarios and actual macOS5/5 plus all ten original reference cases pass; maximum
error0.000583839146 within unchanged0.005. Existing provider/image/transport
regressions pass. No model/engine implementation was replaced or published.

Authoritative commands, scope, diagnostics, hashes, terminal handles and actual
asset/reference paths: [LIFE-06a evidence](../components/samosa-inference/docs/qualification/life06a-decision-registry/README.md).
No live gates remain. Next LIFE-06b: reuse NativeImage/image_pixels/image_backend
for changing prepared images on one loaded context, preserving existing limits
and legacy image behavior; then registry lifecycle/actual SigLIP qualification.
Remaining image/VisionPsy/Molmo2/Whisper lifecycle, areas4–7, new T5 distribution,
persistent session PCM events and all assembled milestones remain open.

Earlier continuations below are historical.

### Latest continuation — retained Kokoro and remaining-owner audit, 2026-10-25

LIFE-05d Kokoro on existing public speech owner qualified, 2026-10-25.
No production code/model/runtime/installer changed. The existing lifecycle
harness now accepts the original manifest-bound voice quality profile and runs
all6 Kokoro voices through the same loaded worker and activation revision.
The original public real-model harness's fresh-generation assertions were
extracted verbatim into tests/speech_quality.py and reused by both harnesses.
The published profile's1% sample-count tolerance is unchanged; actual generated
PCM is validated against its own exact hash/ranges, and retained replay is exact.
Reference hashes for variable fresh PCM remain diagnostics, as before.

Retained local runtime/model/voice/eSpeak/notice files were matched against every
published catalog manifest entry before copying into an isolated read-only QA
package.375 files including manifest; no conversion, new download or catalog
publication. Package /private/tmp/inference-life05d-kokoro-u75dyiqk;
manifest SHA256:
ebaa30beadfcc040df55621155c8cb56e839ca7ee9ce54f0ce929d7d08afbe76
Original quality profile and all model/library/data pins are unchanged.

Final actual macOS ARM64 normal/SAN public resident gates pass5 scenarios each:
synthesis-qualified start/idempotent readiness, all6 voices with original bounds
and exact range/hash verification on one worker/revision, exact stopped-worker/
missing-package replay and owner refusal, observed-running cancellation/deadline
refusal with explicit reactivation, EOF cleanup. Final profile and executable
pins are checked before/after execution. Earlier local actual passes remain
corroborating but final reports include the explicit profile-pin fence.

The original one-shot public --stream gate also passes583 checks normal and583
SAN across all6 voices, preserving byte-exact provisional stream versus retained
ranges and exact replay. Existing Pocket snapshot/isolation regressions pass265
checks per mode with original Caro/Stuart PCM hashes. The release QA snapshot
helper now includes the shared speech_quality.py with its own recorded hash;
an isolated renamed harness plus pinned helper executes actual Pocket without
repository imports. This is QA tooling reuse, not a new installer or another
published-URL qualification claim.

Native public speech lifecycle remains8/8 scenarios normal/SAN on macOS and
independent Debian ARM64 Linux. Only QA profile/pin/report handling changed after
those native gates; no production bytes changed. Final source/export bytes match.
Actual model scope remains macOS: C host/worker instrumented in SAN, external
Sherpa/ORT release builds. No Linux model execution, intelligibility/human quality
score, full area3 closure or persistent-session PCM-event completion is claimed.
Final actual14985, legacy/original52848, native30757/65616 and snapshot99157 are
terminal0; no live qualification handles remain. Compact logs, derived reports,
voice metrics, source/export/profile pins and original report locations are in
components/samosa-inference/docs/qualification/life05d-kokoro-resident.

Remaining engine audit found existing persistent NativeText decision framing,
VisionPsy load/unload JSON lines and Molmo2 framed command loops. Preserve these
implementations; missing ownership is in registry adapters. Image decisions bind
pixels at startup, and Whisper still invokes its CLI per canonical audio input;
their boundaries need explicit inspection before claiming resident reuse.

Next LIFE-06a: integrate text decisions around its existing persistent
engines/decision/main.cpp serve_requests loop. Reuse DecisionAssets open/recheck/
close and closed native loader graph, existing1MiB BE framing, guardian duplex,
controlled_pipe I/O, typed Rust result validation/normalization, operation
receipts and last-good activation. Require a bounded successful native decision
from the retained reference corpus before readiness. Do not apply the trusted
host dependency allowance to an external ORT engine, replace the engine/model,
or reimplement typed decisions. Preserve the existing image path while its
changing-image resident boundary remains open. Then continue the remaining
modality lifecycle audit/extraction against the full area3 checklist.
Inference area3/areas4–7, new T5 production distribution, all C01–C14/Agent/App/
pinned assembly/legacy retirement remain open. Full goal stays active. The
purpose remains separating existing code/tests into independent repos, with only
necessary ownership and API boundary changes.

Earlier continuations below are historical.

### Latest continuation — public speech lifecycle, 2026-10-25

LIFE-05c public resident speech lifecycle qualified, 2026-10-25.
Existing speech.c now delegates execution to a trusted resident callback while
retaining normalization, original JSON request serialization, AudioStore
publication, provisional PCM callback semantics and durable operation receipts.
SpeechHost reuses existing activation admission/revision/last-good and guardian
ownership, speech package/loader verification and the LIFE-05b resident worker.
The transport borrows one authoritative activation EngineProcess record; no live
process descriptors are copied between owners. Warm request failure reaps through
that record while activation retains its lease/store fence until rollback.

Private speech entries optionally select resident-speech-v1 and declare separately
qualified worker_sha256/worker_size/manifest_size. The actual registry executable
and package manifest are admitted as pinned inputs; the closed speech bundle
retains its existing per-file/loader/model/reference verification. A trusted
model_argument lets the adapter record its actual package-directory argv while
model_path remains the manifest. The explicit pinned-host-adapter-v1 activation
policy requires engine==self plus a qualifying launch callback: it records the
already trusted application's runtime separately from external model-engine
system-runtime-v1. Unsafe loader environments still refuse. External engine
policy is unchanged; a test refuses applying this allowance to a different engine.
The first model-engine policy rejected host curl/sqlite/ASan dependencies; that
failed diagnostic is excluded, not used as acceptance or an external-engine waiver.

Discovery remains configuration-only. Public identity-only start/status/stop use
the existing provider lifetime gate, while operation controls stay concurrent.
Readiness requires complete bundle verification and actual bounded successful
synthesis through the existing PCM/hash validator before activation commit, then
live guardian/activation/artifact/package rechecks. Same healthy start is
idempotent. Failed qualification preserves the exact last-good tuple. Confirmed
cleanup releases owned state/manifest/package/residency; shutdown pending retains
ownership for retry. Existing one-shot entries remain available.

Final macOS and independent Debian ARM64 Linux normal/SAN public native gates
pass8 scenarios each: lazy discovery/strict controls, actual warm qualification,
both voices and original ranges/hash on one loaded worker/revision, exact
stopped-worker and missing-package replay, owner refusal, failed native ABI and
synthesis rollback, active cancellation/deadline with busy lifecycle exclusion,
package mutation and crash/restart, EOF/SIGKILL owner cleanup and unrelated survivor.
Actual macOS retained Pocket normal/SAN passes5 scenarios each, including
original Caro/Stuart PCM hashes on one worker, exact replay/offline retained reads,
observed-running operation cancellation and deadline refusal, explicit restart
and EOF cleanup. Public actual cancellation is not asserted to occur after PCM;
private actual PCM interruption/reap was separately qualified in LIFE-05b.
The harness polls the normal pre-registration unknown_operation race rather than
assuming a reserved control request runs after generation registration.

Actual manifest SHA256 is unchanged:
49a3fc30b37676239559a633d9860f129e6201f6fe3475cfe3a5a7f5bca27173
Package: /private/tmp/inference-life05-pocket-vlslz1c_. C hosts/workers are
instrumented in SAN; external Sherpa/ORT remain release builds. No actual Linux
or Kokoro model execution is added. Existing native activation final113/121
macOS and115/171 Linux, text260/260 (Linux261/261), embedding332/332, T5 lifecycle
11/11, public speech175/175 and nine-provider459/459 pass normal/SAN on both
platforms. Resident worker/old one-shot/PCM integrity/owner cleanup regressions
also pass. Variable activation/worker counts include polling and executable size.

Final native52168/23080, actual65811, text/embedding/T5 regressions55155/26548,
activation26802/61254 and final-default21959 are terminal0. No live gates remain.
Initial host-policy and actual polling failures are excluded in life05c native/
actual logs; no production deadlines/compiler flags/PCM assertions were relaxed.
Final source/export bytes match. Compact logs, source SHA256 and derived reports
are in components/samosa-inference/docs/qualification/life05c-speech-registry.
Reports preserve generation receipts; decoded audio-range hashes/lengths replace
large base64 ranges and original report locations remain recorded.

Next LIFE-05d: qualify the existing published Kokoro package/profile through this
same resident owner and original voice/PCM quality harness, without changing its
runtime, voices, loader or installer. Then audit/extract remaining transcription,
image/video and decision lifecycle ownership from the monolith/component before
closing area3. Persistent registry-session incremental PCM delivery remains an
explicit area5 integration gate; existing one-shot --stream remains qualified and
unchanged, while --session currently returns complete receipts. Do not mistake
correlated replies for incremental generation/events. New T5 production distribution
remains open. Inference area3/areas4–7, all C01–C14/Agent/App/pinned assembly/legacy
retirement are incomplete. Full goal stays active; continue splitting existing
code and tests into independent repos with necessary boundary changes only.

Earlier continuations below are historical.

### Latest continuation — resident speech ownership, 2026-10-25

LIFE-05b private resident speech worker qualified, 2026-10-25.
Reuses the existing speech session, native Pocket/Kokoro runtime, guarded duplex
engine_process ownership, request decoder, provisional/terminal PCM headers,
PCM packet validator and streaming hash checks. New hidden resident dispatch
reads bounded length-prefixed copies of the existing text/voice JSON requests.
The same native session serves sequential requests; each provisional cumulative
header precedes its newly emitted PCM suffix, and the final header adds no PCM.
The host reconstructs the original regular-file PCM packet and runs the existing
progress/result validators. Existing one-shot dispatch remains available.

The nonblocking pipe transfer/SIGPIPE handling is extracted from summary_process
into controlled_pipe.h and shared with speech, preserving summary errors and
control behavior. Resident host requests retain a60s ceiling plus caller control,
16KiB request/16MiB packet/45s audio bounds and bounded chunks. No public paths,
commands, voices or model rules were added. Guardian retains bound package and
residency and handles lifetime, stderr bounds and process-group/owner cleanup.
Active control/sink/protocol failure clears the result and stops/reaps the worker;
shutdown-pending keeps its process/endpoints for retry. Preflight cancellation
sends no request and leaves the worker available. A caller must serialize use.
Startup itself is not readiness; public activation qualification remains open.

Final native macOS worker493/495 and independent Debian ARM64 Linux493/495
normal/SAN pass (poll counts vary). Added tests prove one native load through
three requests, same guardian, original decoded hashes, final-only generation,
preflight versus active cancellation,200ms stalled-read deadline, malformed
native audio, crash/restart, changed package refusal, request EOF and SIGKILL
owner cleanup, released leases and unrelated-process survival.

Actual retained Pocket normal/SAN passes135 checks each on macOS: original
one-shot Caro/Stuart/cancellation plus both voices on one resident guardian,
provisional streaming, original PCM hashes, cancellation after two chunks with
worker invalidation/reap, and successful restarted Stuart generation. Caro78720
samples/22 chunks; Stuart90240/25. Existing package manifest SHA256 remains
49a3fc30b37676239559a633d9860f129e6201f6fe3475cfe3a5a7f5bca27173;
isolated retained package /private/tmp/inference-life05-pocket-vlslz1c_. Native C
host/worker are instrumented; external Sherpa/ORT libraries are release builds.
This adds no actual Linux or Kokoro model execution claim.

Existing normal/SAN summary owned transport passes macOS274/354 and Linux267/272;
original summary-process scalar/batch/reuse/crash/recovery tests and public
speech175/175 pass both platforms. App gateway build, original native summarizer
supervisor, isolated installer and3 package/PDFium tests pass. Existing stage
manifests now include the extracted controlled_pipe.h so owner source builds
remain self-contained. Model, generation, installer and activation algorithms
were reused; no legacy paths were retired or remote catalog assets changed.

Final native19996/52645, regression93114/49614, actual53142, App82266 and
installer70797 are terminal0; no live qualification handles remain. Development
compile errors in life05b-build.log/native-mac-r1.log are excluded; final gates
retain the strict compiler flags/assertions. Linux export bytes match final source
except a subsequent API ownership comment, also copied and recorded as final
bytes. Source/evidence hashes and compact terminal PASS logs are retained in
components/samosa-inference/docs/qualification/life05b-speech-resident.

Next LIFE-05c: integrate this owner into the existing speech.c/public registry
through a trusted lifecycle callback, reusing package verification, admission,
activation revision/last-good semantics and identity-only lifecycle controls.
Require actual bounded successful synthesis before ready, retain exact
owner/provider operation receipts and provisional audio publication semantics,
and qualify failed replacement, mutation/crash/owner death, cancellation/deadline,
stop/restart, exact replay and cleanup. Do not treat a library bundle as an
executable engine or replace its loader/installer; adapt the existing activation
launch/qualification boundary for this verified multi-file native package.
New T5 production distribution gate remains open. Inference area3 and areas4–7,
all C01–C14/Agent/App/pinned assembly/legacy retirement remain incomplete.
The full goal remains active: split existing implementation and tests into
independent repos, adding only necessary API/ownership boundary changes.

Earlier continuations below are historical.

### Latest continuation — speech extraction, 2026-10-25

LIFE-05a speech session extraction qualified, 2026-10-25.
Direct inspection found the working implementation in speech_worker.c and the
previously extracted speech_runtime.c (original gateway native Pocket/Kokoro ABI,
voice mapping and PCM generation). Extracted the worker's existing package
verification, library-graph check, options/voice selection and native open/close
into private speech_session.c/h. The original worker now consumes this helper;
its request parser, guardian ownership, PCM packets, provisional streaming,
terminal publication and public speech adapter are unchanged. The helper retains
one loaded runtime across sequential requests and owns a duplicated bound root
fd plus copied host path/pin. Package checks precede/follow generation; successful
worker finish also rechecks after native destruction as the original worker did.
No second speech engine, installer, model or public lifecycle implementation.

Focused fixture checks run in a separate child process: repeated Caro/Stuart
requests after deliberately changing the native ABI probe prove no reopen;
closing the caller's fd preserves the session's bound root; early cancellation
and package mutation expose no successful result/PCM. Failed generations require
caller cleanup before worker reuse. This private helper alone does not establish
public residency, readiness or process ownership.

Final macOS normal/SAN worker354/358, independent Debian ARM64 Linux355/357;
public speech175/175 on both platforms. Poll-related worker counts vary.
Existing runtime324/324, loader2713/2713 and package compatibility/publication
checks pass both platforms. Final qualified handles69131/62358, regressions
12075/64376 and actual11876 are terminal0; no live qualification handles remain.
Earlier session-mac/final logs are development evidence, superseded by qualified
logs after retaining the post-destruction verification boundary.

Actual public Pocket --stream passes265 checks normal and265 sanitizer on macOS,
using an isolated copy of retained local artifacts, all checked against the
existing catalog manifest before copy. Manifest SHA256:
49a3fc30b37676239559a633d9860f129e6201f6fe3475cfe3a5a7f5bca27173
Package: /private/tmp/inference-life05-pocket-vlslz1c_
Caro78720/Stuart90240 samples retain the original exact PCM hashes, ranged reads,
streaming terminal verification and replay. C host/worker code is instrumented;
external Sherpa/ORT libraries remain release builds. This is existing public
one-shot execution via the extracted helper, not actual resident-worker proof.
No actual Linux or Kokoro claim is added. Model/library/config bytes unchanged.

Next LIFE-05b: reuse this session inside the existing guarded speech worker for
sequential requests, keeping the established bounded/provisional PCM packet
format and final hash checks. Reuse engine_process ownership/duplex controls and
existing package/root/workspace descriptors; preserve cancellation/deadline
invalidation and confirmed-reap lease retention. Qualify repeated native/actual
requests under one loaded worker before wiring identity-only public registry
start/status/stop and successful synthesis readiness. Reuse activation/store
semantics for last-good ownership and existing speech package identity; do not
replace speech.c/artifact stores, the native runtime, installer or gateway paths.
Inference area3 remains active, areas4–7/all C01–C14/Agent/App/pinned assembly/
legacy retirement remain open. The purpose remains splitting existing code into
independent repos, with only necessary ownership/API boundary changes.

Evidence: components/samosa-inference/docs/qualification/life05a-speech-session.

Earlier continuations below are historical.

### Latest continuation — extraction-first, 2026-10-24

LIFE-04c public resident T5 ownership is locally qualified (2026-10-24).
Existing activation admission/revision/last-good store and guardian own the
worker; existing summary_process transport and summary.c normalization, opening
map/reduce, provenance and operation receipts remain authoritative. summary_host
is the lifecycle adapter, not a replacement backend. Shared owned-status checks
are extracted from managed_provider. The same native T5 engine parses a minimal
budget-v1 envelope so requests16/64/128/256 use one resident activation; legacy
frames and the generation algorithm remain intact. Actual bounded calibration
must succeed before activation commit; incompatible old engines fail closed.

Final macOS and independent Debian ARM64 Linux normal/SAN native lifecycle gates
pass11 scenarios each. Actual retained T5 macOS normal/SAN passes5 scenarios each,
including real16-versus256 output differences, unchanged activation revision,
opening reduction, exact stopped-worker/offline replay and owner isolation.
Native failure/cancellation/deadline/mutation/crash/owner-death gates preserve
last-good ownership and release only after confirmed reap. Existing activation,
managed readiness, text/embedding lifecycle, one-shot summary, controlled
transport and nine-provider registry regressions pass both platforms. Original
actual scalar/batch/coalesced corpus passes against the new engine normal/SAN.
Final lifecycle handles67823/68730 and regressions89750/56935/95323 are terminal0.
Actual-model scope is macOS; native C hosts are instrumented in SAN runs, external
model engine remains a release build. Initial build-overlap actual-model evidence
is excluded; final harness pins its CLI before/after execution.

Candidate engine SHA256:
179efc64fa559a4a5ebc5ece2cb2b70b54b0784e264af7d2c84083e0dae606a1
Model SHA256 remains:
30c73e0b2ebd4f65538d87f5157aa7193261e8ff362e441df86c6941f3e9265c
Evidence: components/samosa-inference/docs/qualification/life04c-summary-registry (component-relative).
Published legacy catalog is unchanged. Local resident qualification does not
admit a new production distribution or close area3/areas4–7/C01–C14.

User reaffirmed the purpose: split existing code into separate individual repos.
For each remaining slice, inventory the monolith/component implementation, move
or wire that implementation and its tests, and add only necessary ownership/API
boundary changes. Preserve existing behavior and licenses. Package qualification
uses existing helpers and remains a separate supporting gate; do not let new
packaging work displace the extraction. Next: audit the existing speech worker/
runtime and other modality owners against the remaining area3 checklist, select
the next extraction boundary, and retain the new T5 distribution gate as open.

The previous continuation below is historical.

### Current rebuild continuation — 2026-10-24

Sources is complete. Inference areas1–2 are complete; area3 remains ACTIVE.
LIFE-01 readiness and LIFE-02 managed host extraction were already qualified.
LIFE-03 managed text and embedding registry lifecycle slices are qualified. Read
components/samosa-inference/docs/REGISTRY_LIFECYCLE.md and EXTRACTION_STATUS.md
before the historical Sources checkpoints below.

Changed owner files: managed_host.c/h configuration-only load, existing identity,
activation and reusable deactivation; registry_host.c/h trusted managed entry
binding and start/status/stop, immutable config and per-provider request lifetime
exclusion; registry_main.c persistent --session using the existing managed_stream.
Existing activation/guardian/qualification/operation stores remain authoritative.
The expanded engine-crash gate found an extracted-host defect that retained
attempted ownership after confirmed terminal reap. The fix reports that error
once while releasing residency, permits restart, and frees registry storage on
confirmed terminal shutdown. Unconfirmed shutdown still retains ownership.
New native fixture/tests, existing shared HTTP fixture, optional actual-model
harness, Makefile targets and public registry contract document/qualify the change.
No working engine, supervisor or installer was replaced or legacy path retired.

Final macOS normal/SAN lifecycle260/260 and independent Debian12 ARM64 Linux
261/261 pass, including no-state discovery, owner/identity/profile/context/state
binding refusal, unknown public launch fields, actual fixture qualification,
blocked-generation cancellation and busy replacement, same-port restart,
last-good failure, artifact mutation, engine crash/recovery, EOF/SIGKILL owner
cleanup and unrelated-process survival. Managed host71/71, CLI41/41 and
nine-provider registry459/459 pass on both platforms. Existing readiness and
registry CLI regressions also pass. Exact commands and scope are in the component
record. Linux source export: /private/tmp/inference-life03-linux-siqynlpg;
CC=clang CURL_LIBS=-lcurl, reused existing QA container. Both whitespace checks pass.

Final actual retained Ornith normal/SAN registry lifecycle passes six scenarios:
no-state discovery, actual native generation-qualified startup/health, token
counting/answer4, exact replay with engine stopped, actual restart, EOF cleanup.
Original model/catalog engine pins are unchanged. Actual model scope is
macOS26.5.1 ARM64; native C hosts/guardian are instrumented, external model engine
and Rust tokenizer are release builds. No Linux actual-model claim follows.
Final native collections34631/44775 and actual-model16749 are terminal0. No live
qualification handles remain. Compact terminal logs, both actual receipts and
source/evidence SHA256s: components/samosa-inference/docs/qualification/life03-registry-lifecycle.
Original failures remain excluded in /private/tmp/inference-life03-*.log; no
production deadlines/assertions were relaxed. Post-export header delta was an
ownership comment only; manifest-listed final export/workspace bytes match.

LIFE-03E continuation reuses the same native lifecycle test and HTTP embedding
fixture in an explicit embedding mode. Public startup refuses zero/nonfinite/
usage-mismatched/HTTP-failed embeddings without changing last-good state; exact
stopped-engine replay, active cancellation/deadline and healthy survivors,
mutation/crash/restart and EOF/SIGKILL owner cleanup pass macOS332/332 and native
Linux333/333. Text lifecycle260/260 macOS and261/261 Linux and adapter477/477
regressions pass. No production embedding/registry implementation changed.

The existing actual embedding_real.py corpus now optionally owns its engine
through registry --session. Normal/SAN actual EmbeddingGemma2 qualify768-D
vectors,4/4 paraphrase rankings, context refusal, exact retained replay, original
single/stream vector/profile parity and shutdown, plus no-state discovery, live
readiness, stopped-engine replay, actual restart and EOF key/listener cleanup.
Original external-registry mode also passes. Original catalog engine/model pins
are unchanged; removed temporary model restored at the exact pinned revision,
private outside checkout. No conversion or new profile. Actual model scope
is macOS26.5.1 ARM64; Linux is native control/fixture evidence. Native C host/
guardian sanitizer instrumentation is preserved; external engine/Rust tokenizer
remain release builds. All listed workspace/independent-export bytes match.

Final native1146/58343, actual82367/20128 and regression24331/71030 are terminal0.
No live qualification handles remain. Exact commands and early development
exclusions are in REGISTRY_LIFECYCLE.md; compact logs, rankings, lifecycle,
receipts/refusals/replay/parity artifacts and source/evidence hashes are retained
in components/samosa-inference/docs/qualification/life03e-registry-embedding.
Both whitespace checks pass. Required embedding lifecycle slice is qualified;
remaining modality residency and the original full area3 checklist stay open.

LIFE-04a resident guardian duplex prerequisite is now qualified. Modified
engine_process.c/h reuse the same guardian with borrowed input/output FIFO
validation, descriptor-bound model, retained residency and separately bounded
stderr; the kernel bounds buffering, and the caller retains framing/control.
Expanded test_engine_process.c preserves all old modes and proves repeated
binary frames, EOF without spare writers, explicit backpressure, crash,
log/lifetime bounds, pending shutdown leases and unrelated survivors.

Parent death while a stopped guardian owns a blocked worker exposed an orphan-
group SIGHUP bug: default HUP terminated the guardian before group cleanup,
leaving a descendant holding its lease. A separate debug-only copy caught HUP
before cleanup; it is excluded diagnostic evidence. Guardian now ignores HUP
and cleans its group through the existing lifeline; workers explicitly retain
default HUP. Final tests prove this by requiring a worker to die on HUP while
its guardian survives the orphan-group notification and reaps its descendants.

Final old/new guardian suites pass macOS normal/SAN525/525 and independent
ARM64 Linux527/525 (backpressure polling changes counts). Native C fixture/host/
guardian are instrumented. Minimal exact source export:
/private/tmp/inference-life04a-linux-ohu030fo. Existing activation, readiness,
public summary and protocol normal/SAN regressions also pass on both platforms;
Linux uses the full independent /private/tmp/inference-life03-linux-siqynlpg
export with identical guardian source/header. Commands and counts are in
REGISTRY_LIFECYCLE.md. Native88336/45684 and regressions79377/86226 are terminal0;
no live qualification handles remain. Compact logs, excluded signal diagnostic
and exact source/evidence hashes are retained in
components/samosa-inference/docs/qualification/life04a-resident-duplex. Both
whitespace checks pass. No actual resident T5 readiness or area3 closure is
claimed from this process transport prerequisite.

LIFE-04b controlled summary transport integration now qualifies. Existing
summary_process.c/public header retain original scalar/batch/coalesced framing,
prefix/limits and default App launcher, adding externally owned lifecycle/
borrowed guardian endpoints, retryable typed close and controlled entry points.
Nonblocking writes and reads, monotonic queue/mutex waits and scoped controls
prevent incomplete frames from being reused. Distinct control scopes do not
coalesce. A queued cancellation/deadline removes only its request; active frame
interruption invalidates its worker. A completed queue head hands coordination
to its next waiter, rather than draining unrelated work beyond its deadline.
Pending cleanup retains ownership and refuses new work until explicit retry.
Existing UTF-8 validation is reused; closed pipes become typed failures without
changing the host's global SIGPIPE policy. No T5 engine was rewritten.

Native normal/SAN owned transport280/356 macOS and267/272 independent Linux pass
(counters include timing observations): same-guardian scalar/batch/eight-caller
coalescing, malformed reply/recovery, blocked read/write cancellation/deadline,
queued and mutex isolation, pending shutdown/retry, caller death and unrelated
survivor. Original process/transport fixtures pass normal/SAN on both platforms.
Actual original T5 four-subject/number scalar/batch/coalesced corpus passes
normal/SAN on one stable guardian, with verified cleanup/residency release.
Actual model scope is macOS26.5.1 ARM64; native C host/transport/guardian use
ASan+UBSan, external engine and echo fixtures retain release instrumentation.
Published engine/model pins are unchanged. Test-host binding does not replace
production activation/pin verification or public readiness qualification.

App gateway build, original native summary supervisor, isolated installer and
all three package/PDFium checks pass. The first staging test caught missing
private json.h; packaging and installer manifests now include that dependency.
Test declarations were corrected before passing gates. No assertion/deadline
was relaxed. Final native97811/74849, actual/legacy17757/44803, App/staging77402
and supervisor71189 are terminal0; no live qualification handles remain.
Exact listed source/header/test bytes match independent Linux export
/private/tmp/inference-life04b-linux-wxuyfv9_. Compact logs and exact component/
parent source, model/engine and evidence hashes are retained in
components/samosa-inference/docs/qualification/life04b-summary-owned.
Both whitespace checks pass. Commands/exclusions/scope are in REGISTRY_LIFECYCLE.md.

Exact next action: LIFE-04c pinned public resident T5 activation/registry
lifecycle. Reuse activation.c verified admission/revision/last-good store and
qualification callback with a trusted duplex launch boundary, the new controlled
transport, and summary.c normalization/opening/map-reduce/provenance/receipts.
Public start must qualify an actual bounded warm summary before ready. Preserve
configured provider/owner/operation identities, pins, uncertain coverage, scoped
controls and replay; qualify start/status/stop, failure/replacement, artifact/
owner death and cleanup. Retain working models/installers/guardian/stores and
summary algorithms. All other providers/full area3 and areas4–7, C01–C14, App/
pinned assembly and production retirement remain open. Goal stays active; no
commits/push/retirement.

### Latest Sources work — areas1–3 CLOSED; area4 indexing/Chutni ACTIVE

Area1 media preparation/integration is closed. PQ/HLG conversion, required color provenance and public/
cache validation are implemented. The13-case native corpus and eight-orientation
corpus plus unsupported-transform refusal pass macOS/Linux normal and ASan/UBSan.
Independent HDR-era media source archive builds and actual public HDR/MP3 cache/
offline retry checks pass on both platforms. Native late-video seeking now passes
90s and four-hour source fixtures normally and under ASan/UBSan on both platforms.
All current qualification handles reached terminal exit0; none remain live. See
the area1 execution record at the end of this file.

Final late-audio, full-inspection,14-case visual/nine-case audio, controls and
independent final archive gates also pass on both platforms; see the final area1
record below. A4h upper bound does not qualify every possible4h source against
work/resource limits. Area2's final lifecycle gates now also pass; five areas and
the full Sources release remain open.

Exact next action: durable Sources visual discovery/reindex and SDK publication.
Public analyze_visual capture, source timeline projection and model-bound cache
now pass macOS/Linux normal/sanitizer gates, plus the actual macOS pipeline.
Public publish_visual distinct caption/selected-timestamp publication and retained
SDK reads now also pass129 normal/sanitizer checks on both platforms. The necessary public Inference visual adapter now reuses
the monolith's native VisionPsy interaction; protocol/control gates pass on both
platforms and an actual macOS checkpoint heading gate passes normally and with
the adapter/harness sanitized. This does not close broader area5 qualification.
PDF OCR initial
indexing/reindex/failure/artifact-read gates now reuse public read_pdf_ocr and
monolith page-enrichment behavior; see the latest journal for native qualification.
Derived audio candidate search now passes
both-platform normal/sanitizer and literal regression gates. Retained audio history/read now pass
their normal/sanitizer, ownership/provenance and pagination gates on both platforms.
Compressed audio discovery/reindex
now passes its nine-case normal/sanitizer matrix on both platforms. Native WAV history/quota/crash/control gates now pass
both platforms; see the final entry. Audio freshness/reindex is now integrated and
expanded WAV normal/sanitizer gates pass both platforms. Linked audio identities,
progress and validated completion also pass both-platform gates. Initial owned audio registration, duration measurement and
atomic typed admission are now implemented; see the final entry below.
The helper passes both-platform normal/sanitizer checks; macOS public regression
61771 is terminal0. Retain the remaining area4 modality/enrichment/semantic/Chutni
requirements and reuse working monolith/SDK code. Area3 final archive gates31427(macOS) and
8866(Linux) are terminal exit0, including all six mappings, actual pinned HTTPS,
robots/redirect/provenance/offline proof and normal/sanitizer checks. Current
live Parallel search, exact offline replay and bounded readback pass both platforms.
Do not reopen closed areas under an unspecified
broader-qualification label; fix regressions if actual evidence identifies them.

Current implementation: Sources publish_transcript selects an owner-authorized
cached window, preserves model provenance through the existing Chutni SDK writer,
and records durable parent receipts. Actual WAV/MP3 capture through public
Inference and actual-model publication/readback now pass for qualified WAV/MP3
fixtures on macOS/Linux in normal and ASan/UBSan modes. Native public publication
fixtures cover real registration/preparation, offline retry/recovery, ownership,
receipt corruption and explicit refusal of upload-only file publication.

Final native public publication fixture:188 checks pass in normal and ASan/UBSan
modes on macOS and native ARM64 Linux. All final locator runs reached terminal
exit0; no current test handle remains live. See the Public cached-transcript
publication execution record below for commands, logs and scope limits.

Actual-publication matrix: eight WAV/MP3 normal/sanitizer runs pass; both platform
batch handles reached terminal exit0. The native driver passes2525 checks per macOS
run and2514 per Linux run, including complete retained-capture equality through
public memory_read and original/provider/worker-absent receipt replay. No handles
from this checkpoint remain live. These are acceptance checks, not accuracy data.

The shared private window path now implements bounded preparation/capture working
buffers, existing SDK publication/receipt handling and the monolith's one-second
overlap/ceil-stride/final-window accounting within the30s public bound. Eight WAV
windows publish under a1KiB admission budget with no prepared-cache growth or
eviction. The expanded actual-SDK fixture passes1872 checks and the transcript
regression passes351, both normal and ASan/UBSan on macOS/Linux; all current
qualification handles reached terminal exit0. Long-window tests use a labeled
fixture provider; they are not a long-audio speech quality benchmark.

Durable selected-range audio jobs now expose create/get/step/queue_step/read through
Sources. Owner/root/source/provider/decoder/store identities are bound; capture and
publication checkpoints are separate atomic phases. The existing index.lock lease
is shared with text indexing, and --index-run services audio through the existing
runtime admission, cancellation and pacing, with60s audio attempts. Actual-SDK
native fixtures pass normal and ASan/UBSan on both platforms, including runtime/
TERM, worker failure/backoff/model-absent retry and SIGKILL after SDK commit before
local acknowledgment, recovered with original/model gone. Final status batches
72062/10520 and earlier qualification handles are terminal exit0; none remain live.

The native actual-Whisper CLI fixture now exercises public audio jobs in a fresh
Sources state with no prepared/transcript cache. Actual capture, separate SDK
publication, completed status and offline saved-capture readback pass for the
2672ms English WAV/MP3 fixtures in all eight platform/normal/sanitizer combinations.
macOS2637 and Linux2626 assertions pass per run; these include transport/hash checks,
not independent speech cases. All builds and matrix handles53098/98749/93950/61561/
95051 reached terminal exit0; no current handle remains live. The existing engines,
decoder, SDK and queue runtime are retained.

inspect_audio now measures the usable owned snapshot timeline. WAV uses the existing
validated PCM parser; encoded media scans the existing isolated decoder/resampler
to EOF without retaining PCM. The parent binds and validates the private kind13
timeline DTO. Both-platform normal/sanitizer four-format corpus, public WAV/MP3
endpoint/owner/immutable/durable replay and native active controls pass. All final
handles69199/32187/4989/59692/85522/14493 and prior inspection handles are terminal;
none remain live. See the owned timeline execution record below for scope and the
corrected test assumptions. This is duration inspection, not automatic discovery.

User execution correction: finish one numbered Sources area at a time, in order1–7.
Areas1/2 are now closed by their final execution records; continue area3.
Automatic audio discovery/reindex/freshness is area4 and is
deferred until that turn; current working inspection/queue code is retained.
Identify any necessary public-interface prerequisite explicitly and return to the
active area after completing it. Keep the existing scheduler and engines.
Keep existing public prepared-cache retention semantics. Do not add a replacement
Whisper engine, decoder or catalog. The full Sources/Inference/Agent/App goal and
C01–C14 gates remain active.

The following older checkpoint entries are historical qualification records;
their former live handles and next actions are superseded by this checkpoint.

### Historical Sources work — S18b native media decoder primitive

Visual bounds/reference qualification: Linux99471 installed libswscale-dev and
actual first native color-frame acceptance exited0. Added bounded native stream
probing with single-thread/max pixel/sample options, explicit4h container bound
and H264/HEVC/MPEG4/MJPEG/VP8/VP9 codec admission. Those codec IDs are supported
paths, not all qualified codecs. Expanded visual tests include cancellation/
deadline ingress, malformed input, uncovered source range and optional exact
RGBA comparison with separately generated native CLI reference. Native FFmpeg
reference files /private/tmp/samosa-s18b-frame-reference-{mac,linux}.rgba.
Mac8828/Linux20017 current normal+ASan/UBSan visual/reference batches exited0;
logs /private/tmp/samosa-s18b-video-current-{mac,linux}.log (Mac log captures final
instrumented reference invocation; earlier steps are in tool session output).
Mac six-artifact audio normal/sanitizer regression session23156 exited0;
log /private/tmp/samosa-s18b-six-artifact-audio-mac.log. Linux counterpart
session21000 is live; poll exact handle, log
/private/tmp/samosa-s18b-six-artifact-audio-linux.log. Minimal build still
has no media SDK dependency. Public visual worker/routing/cache, timestamp/color/
orientation/codec corpus and active control remain required; no full media pass.

Timed visual implementation: Linux27646 shared-memory input audio normal/sanitizer
regression exited0. Added optional src/media_video.{c,h}: copied-memory MP4/MOV
demux, single-thread bounded video decode, packet/frame/pixel limits, real source
timestamps, first frame inside selected interval and bounded aspect-preserving
RGBA output at most512px/axis using native bilinear swscale. Reports original
dimensions, selected stream, normalized timestamp/origin and conversion matrix/
range (unspecified/non709/non2020 uses601; broader color/orientation qualification
still needed). Secondary IO refused and cancellation polled. No worker/public
visual routing yet; no visual hard-control/public-cache pass claimed.

Artifact set now includes actual mapped sws_scale library (six images), versioned
media-worker-v2 contract binding audio and visual conversion. Optional SDK builds
include video/input and libswscale; minimal SOURCES stays SDK-free. This changes
fingerprints intentionally; prior caches/releases remain historical evidence.
tests/test_media_video.c decodes native64x64 blue H264 fixture, checks actual RGBA
color/alpha, source-time interval and uncovered selection refusal. Mac12164 exited0,
log /private/tmp/samosa-s18b-video-primitive-mac.log. Linux63988 exited2 because
the optional libswscale development package was absent; no Linux visual pass.
Installing it in owned QA container before replacement build/test in live
session99471; log /private/tmp/samosa-s18b-video-primitive-linux-r2.log. Initial style
was split to avoid GCC misleading-indentation warning before Linux compile.
Remaining: visual probing/codec/color/timestamp/resource corpus, visual worker/
parent/public/cache integration, sanitizers and audio fingerprint regressions,
larger uploads/live freshness/Inference handoff and full Sources scope.

Deadline/shared-input follow-up: Mac57011 public decoder deadline normal+
ASan/UBSan acceptance exited0. Test-only readiness shortens owning deadline after
healthy cache peer completes; requires typed deadline_exceeded, under1s teardown
after expiration, exact reap, no artifact/cache entry and offline exact failed
retry. Distinct IDs preserve separate cancellation/descendant cases. Linux82496
equivalent exited0; log /private/tmp/samosa-s18b-public-deadline-linux.log.
Linux25461 uncovered-time normal/sanitizer acceptance exited0.

Preparing timed visual decoder: moved custom bounded memory read/seek, secondary
IO refusal and interruption callbacks to optional src/media_input.{c,h}, shared
with audio. No algorithm changed and minimal SOURCES still excludes SDK/module.
Optional compile commands now include media_input.c. Mac36019 current audio
normal+sanitizer delayed-video regression exited0, log
/private/tmp/samosa-s18b-shared-input-mac.log. Visual decoding itself is not yet
implemented; it must preserve source frame timestamps, bounded output, mapped
artifact identities and existing owned worker controls. No component closure.

Copied shared-input changes into independent Linux copy only after deadline tests
finished. Linux shared-input normal/sanitizer session27646 is live; poll exact
handle, log /private/tmp/samosa-s18b-shared-input-linux.log. Header dependencies
include media_input.h for optional worker/primitive rebuilds.

Linux caller/timeline follow-up: Linux22748 descendant normal+sanitizer batch
exited0. Copied caller-death test only after terminal completion; Linux44705 now
normal+sanitizer caller SIGKILL checks exited0, log
/private/tmp/samosa-s18b-caller-death-linux.log. Added actual decoder refusal for
60000–60100ms beyond all current fixture durations (at most31s), requiring
invalid_range and no waveform/bytes instead of silently padded output. Mac60423
normal+sanitizer delayed-video timeline run exited0, log
/private/tmp/samosa-s18b-uncovered-timeline-mac.log. Linux primitive change not yet
copied until44705 exited0 to preserve its active snapshot. Linux uncovered-time
normal+sanitizer batch session25461 is live; poll exact handle, log
/private/tmp/samosa-s18b-uncovered-timeline-linux.log. docs/S18b.md reflects implemented
public audio/cache/control and remaining full media/visual/upload/package scope.
Poll exact handles; no broad corpus/component completion is inferred.

Caller-death qualification work: Mac13578 public descendant cancellation sanitizer
acceptance exited0. Linux22748 descendant normal+sanitizer batch remains live;
preserve its inputs until terminal. Added test_media_worker.c caller SIGKILL test:
fork a caller, wait on a private pipe for actual media worker+descendant readiness,
kill/reap caller and require both owned worker and descendant disappear within2s.
Linux subreaper adopts/reaps exact orphan PIDs; macOS observes ESRCH. Healthy
actual decode follows. Mac28031 normal+sanitizer batch exited0; log
/private/tmp/samosa-s18b-caller-death-mac.log. Linux caller-death test not yet copied
pending terminal22748. This tests private owned media execution; public durable
crash recovery and broader codec/timeline/visual/upload/service scope remain.

Descendant-control follow-up: Mac44416 decoder-phase public durable sanitizer
acceptance exited0. Linux26498 subsequently exited0 against its earlier snapshot.
Its inputs were preserved until terminal completion. Added test-only media_audio_descendant_stall
mapping only kind5 so fingerprint completes before worker forks a stalled child.
Public durable test now checks three distinct operation IDs/phases; decoder phase
asserts two started workers and waits for descendant disappearance after scoped
cancellation. Linux test adopts/reaps orphaned descendants with subreaper; macOS
checks eventual ESRCH. Foreign refusal, live status, cached peer, exact main reap,
offline retry, unchanged cache count and healthy survivor remain checked.
Mac78623 normal acceptance exited0, log
/private/tmp/samosa-s18b-descendant-media-mac.log. New Mac descendant sanitizer run
session13578 remains live; log
/private/tmp/samosa-s18b-descendant-media-sanitize-mac.log. Linux descendant inputs
were copied after26498 exited0 for earlier decoder-phase sanitizer acceptance.
Linux descendant normal+sanitizer batch session22748 is live; poll its exact
handle, log /private/tmp/samosa-s18b-descendant-media-linux.log.
Caller death and full remaining media/visual/
upload/live-source/enrichment/acquisition/service scope remain required.

Decoder-phase public-control follow-up: Linux89502 full standard Sources normal+
sanitizer suite exited0, matching macOS47949. Mac68044/Linux77634 earlier public
fingerprint-phase durable cancellation sanitizer tests exited0. Added test-only
media_audio_stall routing so fingerprint queries run normally and only kind5
decode stalls. Public control tests now use distinct operation IDs for both
phases and assert exactly1 vs2 started workers, proving decoder phase is reached.
Both phases retain foreign-owner refusal, live status, healthy cache peer, exact
reap, unchanged cache count/good hit and byte-identical decoder-absent retries.
Mac70646/Linux55005 normal acceptance exited0.
Logs /private/tmp/samosa-s18b-decoder-durable-{mac,linux}.log. Newly launched Mac
instrumented decoder-phase acceptance session44416 remains live; poll it.
Log /private/tmp/samosa-s18b-decoder-durable-sanitize-mac.log.
Linux instrumented counterpart session26498 is live; log
/private/tmp/samosa-s18b-decoder-durable-sanitize-linux.log. Poll exact handles.
Test-only native.c mapping changes postdate full-suite snapshot; production branch is unchanged.
Caller/descendant death, broader codec/timeline corpus, timed visual frames,
uploads/live freshness/Inference handoff and remaining Sources scope stay open.

Public durable-control follow-up: Mac47949 full standard Sources tests/sanitizers
exited0. Linux89502 remains live. Linux17619 concurrent-native-peer/RSS/fault
normal+sanitizer tests exited0. Expanded test_media_cache.c with test-only public
durable cancellation: owned media fingerprint worker stalls at readiness; foreign
owner cancellation refuses, operation_get reports running/not-cancelled, healthy
cached media peer completes while stalled PID is alive, then owner cancellation
causes empty artifact output, exact reap, unchanged good hit and byte-identical
saved retry with media helper absent. Cache count remains1 after cancellation.
Mac34926/Linux63095 normal acceptance exited0, logs
/private/tmp/samosa-s18b-durable-media-{mac,linux}.log. Added distinct instrumented
control-test target; Mac68044/Linux77634 sanitizer runs are live,
using existing instrumented fault workers. Poll exact handles. Logs
/private/tmp/samosa-s18b-durable-media-sanitize-{mac,linux}.log. This test cancels
the fingerprint phase of public preparation; active internal decoder stall is
qualified separately, not inferred from this particular public test. Descendant/
caller death and public decoder-phase durable controls remain required.

Peer/resource qualification follow-up: expanded test_media_worker.c so a healthy
actual media worker decodes while a separately owned fault-ready worker stalls;
the healthy peer completes before scoped cancellation kills/reaps only the stall.
Also exercises resident-memory fault with64MiB parent RSS threshold and exact
reap/healthy survivor. Faults remain test-only. New normal+sanitizer batches
Mac12122 exited0 for normal+sanitizer acceptance. Linux17619 normal acceptance
passed and its sanitizer phase remains live; logs
/private/tmp/samosa-s18b-peer-resource-{mac,linux}.log. Standard full Sources
Mac47949/Linux89502 remain live against the earlier production snapshot; these
test-only changes do not alter that production snapshot. Poll exact handles.
Public durable media cancellation/descendant death, broader malformed/timeline
corpus, visual frames/larger uploads and final platform releases remain required.

Regression follow-up: Mac53710/Linux49270 compressed cache ASan/UBSan tests exited0
against instrumented workers, including fresh-digest payload/decoder tampering
and count/byte refusal/old-hit preservation. Public prepare_audio now refuses
end_ms beyond the4h media bound before artifact queries; CLI acceptance includes
that refusal with worker absent. describe reports media_adapter_configured from
explicit host configuration (configuration presence, not runtime readiness).
Current full standard Sources make test sanitize runs: Mac47949/Linux89502 live,
Linux independent /qa/s18b-media. Logs
/private/tmp/samosa-s18b-full-regression-{mac,linux}.log. Poll exact handles before
claiming passes. Optional actual PDF/OCR/media corpora and refreshed independent
release qualification remain separate required checks. Keep all original Sources
visual/upload/live-source/acquisition/enrichment/service and later rebuild scope.

Current public CLI regression after early range check passed on Mac MP3 and
Linux AAC/M4A (session14476 exited0), including decoder-absent invalid_range,
offline durable retry/conflict and malformed survivor. Linux AAC log
/private/tmp/samosa-s18b-public-aac-linux.log. Full Mac47949 has completed normal
tests and is in sanitizer phase; Linux89502 remains in normal/build phase.
Both full-suite handles remain live; neither complete run is yet a pass.

Compressed cache qualification follow-up: Linux73640 malformed public sanitizer
acceptance exited0. Added native tests/test_media_cache.c against actual production
codec worker and public Sources dispatch: count/byte capacity refuses new windows,
old hits remain; wrong response digest, corrupt WAV payload with freshly computed
digest, and invalid decoder channel metadata with freshly computed digest all
refuse corrupt_cache. Restoring the original response preserves exact hit bytes;
only one artifact remains after failures. Test originals are deleted before cache
checks. Initial macOS compile required _DARWIN_C_SOURCE for mkdtemp; fixed without
warnings suppression. Current Mac52758/Linux23599 run normal cache tests; inspect
terminal results: both exited0. Linux log /private/tmp/samosa-s18b-cache-integrity-linux.log.
Cache sanitizer, active durable public media controls, peers/resources, visual
frames/larger uploads and full independent Sources regressions/releases remain.

Added sanitize-media-cache target requiring explicit instrumented helper/fixture;
both parent and helper are instrumented. Mac53710/Linux49270 sessions are live;
poll exact handles. Logs /private/tmp/samosa-s18b-cache-integrity-sanitize-{mac,linux}.log.

Malformed public media follow-up: Linux6974 delayed-video public sanitizer
acceptance exited0, matching macOS. Expanded media_prepare_cli.sh registers an
ID3 candidate with malformed contents, then requires typed invalid_media refusal,
no artifact/data, byte-identical saved failure retry with decoder absent and
unchanged prior good cache hit. This exposed missing invalid_media mapping in
the private worker error allowlist; added invalid_media and unsupported_audio_format
so actual decode failures retain their intended typed errors. Mac30455/Linux26537
normal CLI acceptance exited0. Linux log
/private/tmp/samosa-s18b-malformed-public-linux.log. Mac11314 public
ASan/UBSan malformed acceptance exited0. Linux equivalent session73640
remains live. Logs /private/tmp/samosa-s18b-malformed-public-sanitize-{mac,linux}.log.
Poll exact handles; no broad malformed codec corpus claim follows from one ID3
fixture. Cache tamper/capacity, active durable public cancellation, actual native
peers/resources and final independent full regression/release remain required.

Mac actual AAC/M4A production CLI also passes expanded preparation/cache/durable
and malformed-survivor acceptance using samosa-s18b-sentinel.m4a.

Public durable/sanitizer follow-up: generated prepared responses now undergo
cached_audio_valid before cache admission/publication, including decoded WAV and
decoder metadata for compressed sources. Expanded production CLI acceptance
checks saved operation retry with original and worker configuration absent and
range-change idempotency conflict, in addition to owner/time/cache checks. Plain
WAV regression session90102 exited0. Current Mac41502 and Linux6974 run normal
MP3 acceptance then independently instrumented minimal CLI plus production codec
worker sanitizer acceptance against delayed-video audio. Mac normal MP3 already
passed; Mac41502 now exited0 including delayed-video sanitizer acceptance.
Linux6974 remains live. Poll its exact handle. Logs
/private/tmp/samosa-s18b-public-sanitize-{mac,linux}.log. No broad media corpus,
cache tamper/capacity/active durable-control qualification or final full Sources
release is inferred. contracts/media-v1.md now documents public preparation.

Public preparation work in progress: prepare_audio now accepts encoded-media
owner claims as well as WAV claims. Copied snapshots are SHA-checked and SQLite
statements finalized before native work. Encoded preparation queries configured
media fingerprint before cache lookup, binds it in existing owner cache identity,
decodes through owned worker only on misses and emits derived audio plus decoder
stream/rate/channels/origin/range metadata. Shared prepared_artifacts budget and
durable operation controls are retained. Cache reuse reconstructs private DTO from
decoder metadata and payload and validates actual canonical base64/WAV. Plain WAV
keeps its own lossless contract. Added media_prepare_cli.sh for actual CLI checks.
Initial compile failed because an automated insertion placed encoded detection
in cache lookup; moved it to snapshot selection. No failing compile is a pass.
Mac corrected run96895 exited0 for public MP3 CLI acceptance. Linux corrected
run74011 exited0 for public MP3 CLI acceptance,
log /private/tmp/samosa-s18b-public-preparation-linux-r2.log. Inspect terminal
results before claims. Expanded fault/cache/durable/public sanitizer and full
regressions remain required; no S18b/component completion is claimed.

Mac delayed-video public preparation CLI also passed. Mac audio regression
session90102 remains live; log /private/tmp/samosa-s18b-public-audio-regression-mac.log.

Public registration follow-up: register_media now uses existing authorized path/
inventory identity, copied immutable snapshots, original SHA and owner claims.
Bounded MPEG header/MP4-QuickTime initial-box candidate checks refuse ordinary
text, with no decoder/model dependency or native work under a store transaction.
Candidate registration does not assert successful decode/timeline/audio coverage;
contracts/media-v1.md records that distinction and remaining full scope. Internal
claim type application/x-samosa-encoded-media cannot grant text/WAV reader rights.
tests/media_registration_cli.sh runs actual production CLI with MP3 and delayed
MP4 fixtures, exact repeated registration, unsupported input and separately
authorized second owner. Mac57794/Linux51498 exited0; Linux log
/private/tmp/samosa-s18b-registration-linux.log. No live sessions remain.
Next public compressed preparation reuses copied snapshots and prepared_artifacts
cache capacity, binding configured worker fingerprint and original time/stream
provenance. Larger uploads, visual frames, Inference handoff and broader Sources
acquisition/index/enrichment/service qualification remain required.

Host configuration follow-up: SourcesConfig.media_helper and CLI --media-helper
select the optional codec executable independently of native_helper. Internal
media queries/decode copy configuration and select only media_helper; missing
configuration returns media_unavailable with empty output. Worker tests deliberately
configure a nonexistent plain helper while media succeeds, and verify absent media
configuration refuses. macOS61405 normal build/test exited0. Linux40646 independent
normal build/test also exited0; log /private/tmp/samosa-s18b-media-config-linux.log.
Prior Linux36146 normal+ASan/UBSan fault/control tests exited0, matching macOS.
No public compressed-media cache route is implemented yet; next reuse immutable
registration/owner claims and prepared_artifacts capacity with configured media
fingerprints, exact source timeline/stream provenance and durable control.

Active worker qualification follow-up: Linux fingerprint sanitizer session41764
exited0, matching the macOS pass. Expanded test_media_worker.c exercises optional
media worker abort, early exit, invalid protocol, oversized output, invalid DTO,
uncooperative stall deadline and active cancellation. For each failure it checks
empty output, under2s response from fault readiness, exact waitpid ECHILD and
kill ESRCH, then a fresh healthy actual MP3 decode. Faults are compiled only into
explicit test executables; production media-worker remains fault-free. Added
test-media-worker-faults; sanitizer target now instruments both fault-enabled
worker and parent. Mac16290 exited0 for normal and sanitizer tests. Linux36146
normal tests passed; sanitizer build/test remains live. Poll that exact handle.
Logs /private/tmp/samosa-s18b-active-worker-{mac,linux}.log.
These tests stall the owned process before decode; they prove parent hard teardown,
not an observed stall inside FFmpeg. Actual concurrent peers, descendants/resource
media checks, malformed codec corpus and public cache integration remain required.

Fingerprint binding follow-up: private kind6 queries actual mapped artifact hashes
inside the optional worker, using a strictly checked one-byte sentinel request.
The minimal executable continues to refuse optional kinds. Parent validates a
64-character lowercase SHA fingerprint; media decoding now requires an expected
fingerprint and discards a mismatched DTO with media_artifact_changed. Actual
worker tests query identity, decode with matching identity and deliberately change
the expected SHA to verify refusal and empty output. Mac39438/Linux89832 exited0
normally, including prior DTO/range/corrupt payload/cancelled ingress tests.
Linux log /private/tmp/samosa-s18b-fingerprint-worker-linux.log. Added optional
sanitize-media-worker target instruments both SDK worker and minimal parent.
Mac36777 sanitizer build/test exited0. Linux41764 remains live; poll its exact
handle before claiming its result. Logs:
/private/tmp/samosa-s18b-worker-fingerprint-sanitize-{mac,linux}.log.
This provides internal fingerprint binding; public cache identity integration,
active teardown/fault qualification and full Sources media scope remain open.

Parent integration follow-up: sources_native_media_audio now owns bounded kind5
requests and validates responses through SDK-free media_validate.c. Validation
checks exact requested times/output length, bounded stream/rate/channels, signed
container origin, lowercase SHA fingerprint, canonical base64 padding and actual
PCM16 mono16k WAV structure/frame count. Cancellation is polled during payload
decoding. Errors clear response buffers; media error codes are explicitly allowed.
tests/test_media_worker.c exercises actual owned MP3 worker decoding, valid DTO,
wrong requested range, corrupt base64 and cancelled ingress. macOS session38619
exited0. Linux independent build/test session92853 also exited0; actual worker
acceptance passed. Log /private/tmp/samosa-s18b-parent-linux.log. No live runs
remain. macOS build session30507 exited0,
log /private/tmp/samosa-s18b-parent-build-mac.log. No public compressed-media
registration/cache route, fingerprint query/expected-fingerprint binding, active
decoder control/fault tests or final sanitizers are qualified by this check.

Worker integration follow-up: added optional make media-worker executable using
SAMOSA_SOURCES_MEDIA_WORKER, private
native request kind5 and media_response.c. The existing fd3 worker lifecycle,
limits, watchdog and scoped callback surround decoder execution. The bounded DTO
reports artifact fingerprint, original selected range, stream/rate/channels,
container origin and base64 WAV; serialization polls cancellation. Minimal build
still excludes media SDK and refuses kind5 at ingress. macOS make all/media-worker
session50560 exited0. Linux session42678 also exited0 in the independent
/qa/s18b-media copy. These are build results, not runtime qualification. Build logs:
/private/tmp/samosa-s18b-worker-build-{mac,linux}.log. This is worker-side routing
only: parent request API/DTO validation, fingerprint query, media error allowlist,
actual worker protocol tests/control qualification and public integration remain.

Previous goal turn made concrete progress: implemented/qualified prepared audio
cache and combined literal/derived capacity, refreshed full Source tests/sanitizers
on both platforms and installed optional Linux SDK. This turn implements optional
src/media_decode.{c,h}; it is NOT in default SOURCES/link dependencies and NOT
connected to public registration/preparation yet. It reads immutable memory via
custom AVIO read/seek, refuses secondary IO, whitelists wav/mp3/mov demuxers,
bounds8 streams/64 metadata entries/50000 packets/1MiB packet/65536 frame samples/
8 channels/8–192kHz/4h container/30s selected output. Codec/probe threads are1,
codec max_samples/max_pixels are set. Source deadlines/cancellation supplement
bounded copying/convert callbacks; uncooperative native process teardown still
requires the owned worker integration. No public compressed-media pass claimed.

Audio decode/resample uses explicit packet time_base, contiguous frame timestamps
(reject missing/discontinuous/changed layout), reported container origin, and
exact original millisecond range. Resampler output is captured at16kHz with an
exact requested frame count; incomplete coverage refuses instead of padding.
Prepared canonical PCM16 mono WAV is separate from source/provenance. EOF flush
is bounded. Contexts/frames/packets/resampler/custom AVIO buffer are cleaned on
all tested success/failure paths. Output metadata retains stream/rate/channels/
container origin. Decoder/header/input checks do not yet prove all corrupt codecs
safe; process ownership and malformed/secondary-ref corpus remain required.

tests/test_media_decode.c and optional test-media-primitive/sanitize-media-primitive
targets build against each SDK. Fixtures: owned C generator test-audio --fixture
creates /private/tmp/samosa-s18b-wave.wav; native FFmpeg fixture encoders create
sentinel.mp3 (32k MP3), sentinel.m4a (32k AAC), stereo48k.wav (2s48kHz stereo),
offset-video.mp4 (64x64 blue H264 video, AAC track delayed0.5s). All paths use
/private/tmp/samosa-s18b-*; /host-tmp inside Sources QA. Native per-platform
FFmpeg selected1.0–1.5s reference PCM validates exact decoded bytes, not only
shape. Linux independently copied /qa/s18b-media excludes build/dist.

Initial Mac19335 WAV normal/sanitizer passed; Linux58088 exited2 for GCC misleading
indentation warnings, corrected without suppressions. Current Wave/MP3/AAC/stereo
Mac54180 and Linux52109 exited0 normal+ASan/UBSan. Reference byte-equality normal
Mac4875/Linux75499 exited0 for MP3/AAC/stereo; delayed video-track equality
Mac16393/Linux42317 exited0. Final current sanitizer batches Mac51188/Linux84391
exited0 for WAV plus all four per-platform reference comparisons. Logs:
/private/tmp/samosa-s18b-primitive-current-{wave-mac,linux}.log,
/private/tmp/samosa-s18b-reference-{mac,linux}.log,
/private/tmp/samosa-s18b-video-{mac,linux}.log,
/private/tmp/samosa-s18b-current-sanitize-{mac,linux}.log.
No live media test sessions remain. The selected output reference equality tests
and cancellation/deadline ingress pass; active native decode interruption and
hard teardown/cache preservation are not yet qualified for this decoder.
Host native CLI reports FFmpeg9.0.2. Default make still targets minimal all;
otool shows production samosa-sources links only system SQLite/libSystem, not
FFmpeg libraries. Optional SDK startup dependency has not leaked into plain reads.

Artifact fingerprint follow-up: src/media_artifacts.{c,h} hashes the executing
decoder image plus loaded av_read_frame, avcodec_receive_frame, swr_convert and
av_malloc images, binding the audio conversion contract. Linux /proc/self/maps
and macOS PROC_PIDREGIONPATHINFO verify mapped inode/device against opened files;
descriptor/path size, timestamps and identity are checked before/after decoding.
Hashing polls scoped cancellation/deadline. A copied executable test makes all
descriptor/path checks agree while the mapped inode differs; it correctly refuses
media_artifact_changed without changing installed libraries or the executable.
This binds these five artifacts, not every transitive library or process shim.
Normal and ASan+UBSan WAV tests Mac54490/Linux34905 exited0; current MP3/AAC/stereo/
delayed-video exact reference comparisons Mac77010/Linux6105 also exited0 in both
build modes. Logs /private/tmp/samosa-s18b-artifact-identity-{mac,linux}.log and
/private/tmp/samosa-s18b-artifact-references-{mac,linux}.log. No live runs remain.

Next: integrate optional
decoder into owned native request/protocol (current native.c kinds1–4 do not route
media), public compressed registration/preparation and cache/provenance validation;
native fault/active control/secondary-input/timeline/resource acceptance; timed
visual frames, larger bounded uploads and public Inference handoff. Keep Sources
acquisition/index/enrichment/Chutni/authenticated service/admission/shutdown and
all later components/full rebuild gates open. Inspect installed primary FFmpeg
headers and kernel mapping identity if hashing loaded SDK artifacts; a pathname
hash after loading does not alone prove mapped bytes when a library was replaced.
Mac proc_info.h exposes proc_regionwithpathinfo and vnode dev/inode metadata;
Linux /proc/self/maps provides mapping inode/device. These checks are now
implemented in the optional primitive; parent/helper fingerprint verification and
public cache integration remain required.

### Current Sources work — S18a media preparation

Latest implementation: prepared_artifacts table stores qualified audio derived
responses. Owner/version/range/reader-bound cache keys, snapshot hash and owner
claim checks precede lookup. Cached digest, artifact/source/version/type, original
and local times, PCM parameters, coverage/provenance and bounded base64 shape
are checked; owning cancellation/deadline polls during large hit validation.
Cache hits skip window copying/encoding; failed/cancelled work commits no entry.
Literal evidence and prepared artifacts now share128 entries/8MiB,2MiB/response,
with host lower limits. Exported internal sources_cache_capacity checks the
combined tables both before preparation and inside transactional admission.
At capacity old hits remain available; duplicate insertion compares prior output.

Initial12979 compile failed from passing a SHA context to the digest-to-hex
routine; fixed with explicit finalization. Mac98216 basic cache normal/sanitizer
passed. Expanded Mac33798 passed, then Mac98751/Linux19625 both exited0 with
cache-hit skip proof, count/byte refusal, shared literal/derived capacity, digest
and fresh-digest semantic corruption refusal, cancelled miss/no cache and prior
good hits. Final added cancellation while validating a large hit and independent
healthy hit while another entry is corrupt are included in full batches:
Mac63605 and Linux46608 terminal exit0 (`make test sanitize`), logs
/private/tmp/samosa-s18a-cache-full-{mac,linux}.log. Counts are per-character
assertions too, not distinct scenarios. Both full suites include public/CLI audio,
literal readers, inventory/index/locators/owners, durable operations and sanitizers.
The S17a archives remain historical, not matching this newer workspace.

Next implementation is owned optional compressed-media decoding. Host already
has FFmpeg native SDK: libavformat63.1.102/libavcodec63.1.102/swresample7.1.102.
Inspected installed primary headers for custom memory AVIO, interrupt callbacks,
bounded streams, codec send/receive and resampling APIs. Minimal Sources retains
no FFmpeg SDK/link dependency. Optional decoder must operate on immutable owned
input, reject secondary/network IO, preserve absolute timestamps, cap output/
streams/frames/native resources and use owned worker teardown. Decoder/contract
fingerprints must bind actual configured executable and loaded native artifacts,
not only a library version string. Inference currently has no public media
contract: add a Sources-side public adapter contract/handoff with explicit
unavailable behavior; defer Inference implementation until its component turn.
Actual assembled neural qualification still remains required later.

Linux optional SDK installation session24987 exited0; log
/private/tmp/samosa-s18b-linux-media-prerequisites.log. This only modifies the
owned Sources QA container; no unrelated image/container cleanup is performed.
Installed FFmpeg5.1.9, avformat59.27.100/avcodec59.37.100/swresample4.7.100.
docs/S18b.md records the concrete decoder caller/boundary, immutable custom memory
IO and refusal of secondary/network sources, timestamp/stream/resource limits,
actual artifact fingerprints, minimal-vs-optional SDK builds, platform acceptance
and public Inference handoff. Decoder code is not implemented yet; it is the next
Sources implementation, not an achieved media gate.

Prior turn made concrete progress: Sources native-peer/corpus implementation,
normal/sanitizer/full regressions and independent macOS package qualification.
Linux independent archive session89046 is now terminal exit0. Its standard,
actual PDF, actual image/PDF OCR and actual/malformed six-format CLI checks pass.
S17a bounded native image/PDF OCR slice is qualified at retained97-file snapshot
(/private/tmp/samosa-s17a-qualified-manifest.json), not whole Sources/C05/C06.
Mac archive867c523225df3f62b948ab3a47284b8f496b90b1f5df80076336b38cad947b02;
Linux archive196e27c43c507ce371736a67f94837ef4264f4a59f4648608a23bae89cf5307e.
Both extracted archives match the captured97-file manifest. New audio work means
the current workspace is newer than that snapshot; do not claim they match it.

Implemented src/audio.{c,h} and public register_audio/prepare_audio. First format
is native RIFF PCM16 mono16000Hz WAV,1MiB original,128 chunks,30s selected window.
Original bytes/version/owner snapshots and absolute millisecond selectors remain
separate from base64 canonical prepared WAV starting at local zero. Artifact
kind=derived/type=prepared_audio, with no transcript/text/evidence_id; lossless
selected samples, reader fingerprint and original locator. Bounded immutable
copy releases SQLite before parser/copy/encoding; cancellation/deadline polls
discard unpublished output. Existing durable operation/canonical retry boundary
now handles prepare_audio selectors; exact saved successful/cancelled retries,
idempotency conflicts and owner refusal are tested. Artifact cache was added and
qualified in the latest work above.

Initial normal/ASan+UBSan Mac81463 and Linux39005 exited0 (21462 checks each).
Initial Mac50751 compile error mkdtemp prototype was corrected with Darwin
feature macro; production build57370 exited0. Expanded fixtures avoid periodic
sample ambiguity and check odd unknown-chunk padding, duplicate fmt refusal,
reordered data/fmt, exact output PCM and changed-range idempotency conflict.
Mac59562 exited0 (21476 normal and21476 sanitizer), log
/private/tmp/samosa-s18a-audio-expanded-mac.log. Linux24651 exited0 at
/private/tmp/samosa-s18a-audio-expanded-linux.log, independent /qa/s18a-audio
copy excluding build/dist (21476 normal/21476 sanitizer checks).
Standard test/sanitize targets now include audio checks.

Separate production CLI registration/preparation, model-free derived output,
absolute/local timeline, immutable saved retries, owner refusal and changed-range
conflict pass Mac73967 and Linux76254 (both exit0), logs
/private/tmp/samosa-s18a-audio-cli-{mac,linux}.log. Explicit default make goal is
now all, keeping fixture-only FreeType/Leptonica out of minimal production builds.
Added active primitive deadline during copying, largest30s/960044-byte prepared
WAV exact samples,1.28MiB-class base64 durable result/retry and corrupt snapshot
refusal. New Mac30971/Linux69093 runs are live, logs
/private/tmp/samosa-s18a-audio-limits-{mac,linux}.log; inspect terminal results
before claiming these additions pass. Update:30971 and69093 both exited0; normal
and ASan+UBSan checks plus separate production CLI passed both platforms. Reported
1301561 assertions include per-character base64 validation of the largest window;
this count is not a count of distinct scenarios. The exact full PCM comparison,
durable large-result equality, active deadline and corruption refusal all pass.
Audio cache and compressed/media neural
work remain pending, as do refreshed full regressions/releases after this work.

Next: full compressed audio/video preparation and public
Inference transcription/analysis. contracts/audio-v1.md and docs/S18a.md preserve
the full media scope; WAV preparation is a first behavior, not a substitute for
MPEG/MP4/QuickTime, conversion, larger bounded uploads or neural extraction.
Keep Sources acquisition/indexing/enrichment/Chutni/service/admission/shutdown
requirements open. No development in Inference/Agent/App until Sources closes.

### Current Sources work — S17a owned native helpers

Latest full macOS97292 exited0: expanded six-format actual/malformed production
corpus normal and ASan+UBSan, all standard Sources tests/sanitizers, real PDF
normal/sanitizer and separate PDF CLI. Independent macOS release15661 exited0:
archive extracted outside checkout and built/tested standard suite, real PDF,
actual image/PDF OCR and public actual/malformed six-format corpus. Root
/private/tmp/samosa-s17a-release-mac.8Od8ha; acceptance.log and archive.tar.gz.
Archive SHA256867c523225df3f62b948ab3a47284b8f496b90b1f5df80076336b38cad947b02.
Compared all97 packaged regular files by SHA256: extracted macOS archive and
current component workspace are identical. Production executable nm inspection
found none of the named QA callback symbols (native_test/helper_event/death_event/
durable_helper_event); this complements source compile guards, not a claim that
the public private-worker entry is absent. No sibling/checkout hardcoded paths
were found in Sources production source or Makefile.
Linux full11355 exited0, including expanded production raster corpus normal/
ASan+UBSan, all standard Sources tests/sanitizers and actual PDF normal/sanitizer/
CLI. Linux independent release/extracted archive acceptance has been launched;
session89046 is live; root /private/tmp/samosa-s17a-release-linux.Y83k9t (Linux
mount /host-tmp), with acceptance.log and archive.tar.gz. Launch log:
/private/tmp/samosa-s17a-release-linux-launch.log. No archive contents-match or
release completion claim until that run and manifest comparison finish.

Update: Linux5643 exited0; simultaneous healthy native peer while stalled worker
remains live passed987 normal/987 ASan+UBSan checks. The six actual raster CLI
formats also passed; fixture include fix was present by compilation, so no Linux
fixture failure occurred. Current full Linux batch11355 remains live at
/private/tmp/samosa-s17a-current-full-linux.log, independent /qa/s17a-helper copy
excluding build/dist. macOS97292 still live. Both batches cover expanded public
corpus, truncated-input typed failures with same-state survivor, production
ASan+UBSan corpus, full standard Sources tests/sanitizers and PDF tests/sanitizers.
macOS corpus/malformed normal+sanitizer already passed; Linux normal corpus and
malformed passed, production sanitizer corpus and broader regressions pending.

LATEST WORK: durable-control Linux90200 exited0,979 normal/979 sanitizer checks.
Added a healthy uncached native OCR peer while the stalled worker PID remains
live before/after; Mac50774 exited0,984 normal/986 sanitizer checks. Linux5643
is live,987 normal checks passed; sanitizer and corpus still pending. Log
/private/tmp/samosa-s17a-helper-native-peer-linux.log. Its copied fixture source
predates the corrected include path (<allheaders.h> for pkg-config lept), so a
fixture compile failure will require recopy/build after the current run ends.

Added native fixture-only Leptonica encoders and public CLI corpus for BMP,
JPEG,PNG,TIFF,GIF,WebP, preserving the known numeric sentinel. GIF signature
registration added to retain existing raster capability; multi-image containers
currently expose only the decoder's first image and coverage stays partial.
Mac31817 corpus exited0 (six formats), log
/private/tmp/samosa-s17a-corpus-mac-r2.log. Initial compile failure99104 was
fixed by using the include path supplied by pkg-config. Added separate truncated
six-format corpus with typed decoder failures/no evidence and same-state valid
PNG survivor, plus production ASan+UBSan corpus target. Full refreshed macOS
run97292 remains live at /private/tmp/samosa-s17a-current-full-mac.log:
test-ocr-corpus sanitize-ocr-corpus test sanitize test-pdf sanitize-pdf. No new
independent release claim yet. Do not restart live runs on observation timeouts.

Resource qualification in progress: parent samples owned worker RSS with a
default 512MiB threshold (host configuration 16–512MiB); exceeding it kills and
reaps the worker before publication. This is a sampled threshold, with possible
inter-poll overshoot. Workers also set descriptor limit64, CPU limit32s and
core limit0; Linux production sets address-space limit2GiB, omitted under ASan
because its shadow mapping requires larger virtual address space. RSS checks
remain enabled under ASan. Descriptor acceptance checks inherited fd100 is
closed in the worker and still open in the caller.

Initial resource runs91583/macOS and37210/Linux exited2: memory fault testing
did not meet its expected result/timing. Changed test-only allocation to volatile
page writes, ensuring optimizing compilers cannot eliminate unobserved memory
touches, and shortened per-chunk pacing. Replacement actual image/PDF normal
and ASan+UBSan runs97933/macOS and26946/Linux remain pending. Logs:
/private/tmp/samosa-s17a-helper-resources-{mac,linux}-r2.log. No resource
qualification pass is claimed until both terminal results are inspected.

Update: replacement macOS run97933 exited0,848 normal and851 sanitizer checks.
Linux run26946 exited0:851 normal and851 ASan+UBSan checks. This qualifies
the tested RSS threshold/descriptor/CPU/core setup and existing worker failure
cases on both platforms; full service/resource and broader decoder gates remain.

Next acceptance added to tests/test_ocr.c for public image and PDF OCR: native
stall readiness triggers owner-scoped operation_get/cancel; foreign cancellation
is refused, cached evidence stays byte-identical, a distinct durable inventory
peer commits before cancellation, the owned helper is reaped, no new evidence is
cached, and the cancelled result retries byte-identically with root/helper absent.
Replacement runs60796/macOS and90200/Linux are pending; logs
/private/tmp/samosa-s17a-helper-durable-{mac,linux}.log. This tests an actual
stalled native helper alongside a cached read and non-native durable peer; it
does not yet prove simultaneous healthy uncached native peers.

Update:60796/macOS exited0:976 normal/979 sanitizer checks. Linux90200 normal
passed979 checks; sanitizer remains live and must be polled. After those builds,
extended the readiness callback with a second actual uncached native OCR worker,
requiring successful numeric sentinel recognition while the original stalled PID
is still alive before/after. Current workspace extension needs a fresh build/run
after90200 reaches terminal state; its results are not covered by60796/90200.

Implemented src/native.{c,h}: POSIX-spawned private Sources worker; bounded duplex
protocol (original1MiB/internal rendered4MiB+64, response2MiB); exact worker PID
and process group; host cancellation/deadline polling; bounded result framing,
exit/status and DTO validation before publication. Worker has no Sources SQLite
handle or sibling state; close-on-exec/closefrom actions discard inherited host
descriptors, OpenMP limit1 is child-local, core dumps disabled. Default CLI uses
its own worker entry; library hosts configure SourcesConfig.native_helper.
Uncached public native image/PDF reads refuse a missing helper; valid existing
cache hits do not require it. Pure low-level adapters retain direct mode for
explicit internal primitive use. PDF text, PDF OCR and image OCR use this boundary.

Worker EOF watchdog prevents orphaned native calls after caller death. Actual
macOS caller-death acceptance exposed events=0 poll failing to detect readable
EOF; fixed with POLLIN and non-consuming recv(MSG_PEEK|MSG_DONTWAIT). Linux also
uses parent-death notification. Latest group teardown handler uses SIGTERM
notification to kill the worker's own group with SIGKILL, including descendants.
All faults and observation hooks are request-local and compiled out of releases.
QA covers abort/early exit, bad framing/oversized output/invalid DTO, uncooperative
stall cancellation/deadline, exact child reaping, no new public image/PDF cache
on failures and byte-identical old good hits. Sources caller SIGKILL acceptance
now includes a live worker descendant; current expanded runs still need results.

Prior cache/worker Mac batch passed769/771 checks normal/ASan+UBSan at
/private/tmp/samosa-s17a-helper-cache-mac.log (session55256 terminal exit0).
Linux cache/PDF/CLI batch passed at /private/tmp/samosa-s17a-helper-cache-linux.log
(session27328 terminal exit0). These predate the descendant extension. Original
Mac death failure /private/tmp/samosa-s17a-helper-death-mac.log is preserved;
corrected watchdog checks passed /private/tmp/samosa-s17a-helper-parent-death-mac.log
(session57108 terminal exit0). Linux timing fixture was changed to expire from
confirmed fault readiness instead of assuming sanitizer exec startup <0.5s.
Old Mac PDF spy deadline assertion failed after helpers began killing during
native calls; spy records call entry before its stall now, preserving the proof
that execution entered the reader without requiring a return after cancellation.

The Docker QA container disappeared externally. An unsafe formatter redirection
then truncated native.c and test_ocr.c when Docker failed. Both were restored
by replaying successful recorded edits and formatting snapshots, in an isolated
recovery tree, with failed patches excluded. Latest descendant block was moved
from its erroneous global insertion into worker_main before fault readiness.
Restored/formatted snapshots persist as
/private/tmp/samosa-s17a-{native,test-ocr}-restored.c; recovery material remains
under /private/tmp/samosa-s17a-recovery-work. Formatter now uses a temporary file,
successful exit/nonempty check, then replacement. Never redirect formatter output
directly onto the target source. Fresh QA container samosa-s17a-linux-qa recreates
native dependencies; no Truly Typed image was changed.

LATEST RESULTS: Mac session31982 and Linux session65839 are terminal exit0 at
/private/tmp/samosa-s17a-helper-recovered-{mac,linux}.log. Expanded image/PDF/helper
checks passed772/775 on Mac normal/sanitizer and775/775 on native ARM64 Linux.
These include actual worker+descendant death after caller SIGKILL, bounded fault
teardown/reaping, and public cache preservation. Linux existing real PDF normal/
sanitizer/CLI and separate OCR CLI also passed. Independent Linux /qa/s17a-helper
copy excludes build/dist. Previous pre-helper clean Linux full
Sources/PDF/OCR/CLI suite session43259 passed exit0, evidence
/private/tmp/samosa-s17a-linux-independent-all.log. Current helper changes still
need full regressions/release qualification after remaining implementation.

S17a and Sources remain incomplete: helper memory/descriptor/peer/durable-control
resource acceptance, broader image formats/malformed decoder corpus, independent
platform releases and matching contents remain. Then finish Sources media,
acquisition, indexing/enrichment/Chutni and bounded authenticated service before
resuming Inference/Agent/App. No C01-C14 milestone closure claimed here.

### Current Sources work — S17a image and explicit PDF OCR

Sources only. Added read_pdf_ocr over original register_pdf versions; bounded
150dpi grayscale renders,4096 axis/4Mi pixels per page,4 selected pages and64KiB
aggregate recognized text. Explicit absolute page IDs, confidence/raster provenance,
uncertain partial coverage and combined PDFium/OCR artifact fingerprints. Existing
owner cache/durable control apply. Native image-only PDF has blank page1 and
sentinel raster page2, with no text objects; oversized variant rejects before
render allocation. See contracts/pdf-ocr-v1.md. Image contract remains in progress.

Actual image adapter acceptance passed236 checks normal/sanitizer on Mac and
native ARM64 Linux: /private/tmp/samosa-s17a-artifacts-mac.log and
/private/tmp/samosa-s17a-native-linux.log, terminal exit0. Expanded image/PDF
acceptance passed346 checks normal/sanitizer on both platforms at
/private/tmp/samosa-s17a-pdf-monitor-{mac,linux}.log. Mac batch terminal exit0;
Linux batch later failed existing PDF CLI because a host build/pdf-fixture Mach-O
binary had been copied into its build directory. This is a QA setup failure,
not a successful platform batch. Original failure preserved at
/private/tmp/samosa-s17a-linux-copied-binary-failure.log. Linux copy now excludes
build/dist and cleans both; full Sources/PDF/OCR/CLI batch is running session43259,
log /private/tmp/samosa-s17a-linux-independent-all.log.
Poll that handle to terminal before claiming full Linux qualification.

Monitor acceptance observes the real native cancel callback through request-local
test-only hooks, then cancels or shortens the deadline and expires it. These hooks
are compiled out of releases. Public cancelled/deadline/artifact-change requests
leave cache counts unchanged; old good hits survive. Private model copy changes
only after native initialization/monitor, preserving the loaded model snapshot.
Symlink/FIFO artifacts and invalid languages are refused without consuming cache.
The original Linux deadline fixture could expire before recognition; replaced
with deterministic monitor-stage expiry. Failure log:
/private/tmp/samosa-s17a-pdf-linux-deadline-fixture-failure.log. Text-layer test now
parses the page text, avoiding accidental sentinel matches in hexadecimal IDs.

Full macOS normal/sanitizer Sources batch before PDF extension passed exit0 at
/private/tmp/samosa-s17a-source-regressions.log; old session43836 is terminal.
Existing real PDF normal/sanitizer/CLI batch passed at
/private/tmp/samosa-s17a-pdf-final-mac.log (terminal exit0). Separate OCR CLI with
real runtime, exact sentinel, immutable/offline retry and owner refusal passed
/private/tmp/samosa-s17a-ocr-cli-mac.log (exit0). A one-byte invalid model invoked
through production CLI returned typed ocr_model_invalid, exit1; native engine
diagnostics and response at /private/tmp/samosa-s17a-corrupt-model-mac.log.

S17a is not closed: native init/decode/render require owned-helper isolation and
hard failure/cancellation/shutdown acceptance, broader raster-format corpus and
independent release/platform matching remain. Do not claim arbitrary corrupt
models or all native aborts safe from the one-byte model check. Sources media,
acquisition, indexing/enrichment/Chutni and bounded authenticated service also
remain open. Continue Sources before touching preserved other-component work.

### Current Sources work — S17a public image reads

Added register_image/read_image through existing immutable snapshots/owner claims,
shared cache and durable operation control; optional OCR runtime now belongs to
the default independent Sources build (dlopen only; no native OCR link/header
requirement). Host CLI supplies explicit engine/decoder/model/language paths.
contracts/image-ocr-v1.md states signature screening, decoded/header limits,
recognized-text selectors and uncertain partial coverage.72 native/public checks
pass normal and ASan/UBSan: /private/tmp/samosa-s17a-public-mac.log, exit0.
Tests prove exact numeric sentinel, provenance, owner refusal, cache equality,
snapshot after original deletion, absent adapter despite existing cache, selected
text, exact durable retry and range refusal. Earlier 19-check checkpoint is historical.
Public HTML/DOCX regression batch after initial integration passed exit0 at
/private/tmp/samosa-s17a-public-build.log. Full normal/sanitizer Sources regression
is currently session43836, log /private/tmp/samosa-s17a-source-regressions.log;
poll to terminal before asserting results or restarting. Active OCR monitor
cancellation/deadline, artifact failure acceptance, PDF rendering/OCR, Linux and
independent releases remain unfinished. Continue Sources only.

### Current Sources work — S17a native OCR primitive

Sources-only execution continues. The FreeType fixture with explicit Arial font
now renders TOKEN 7319; both default and PSM6 actual Tesseract recognition return
that exact sentinel. Optional src/ocr.c adapter passes 19 native macOS checks:
recognition, confidence/dimensions, artifact fingerprint, absent runtime,
pre-work cancellation, input/header dimension limits and successful survivor.
Strict compilation and test-ocr-primitive passed; evidence:
/private/tmp/samosa-s17a-native-mac.log.
The same 19 checks also passed AddressSanitizer/UndefinedBehaviorSanitizer;
evidence: /private/tmp/samosa-s17a-native-mac-sanitize.log (exit0).
Missing standard includes in the first test compilation were fixed before the successful run. Explicit runtime paths
resolve to regular files. Public image/PDF OCR, active recognition cancellation,
Linux qualification and remaining Sources acceptance are still unfinished.
The following fixture-preparation checkpoint is historical.

### Current Sources work — S17a OCR fixture preparation

S16b is qualified; no DOCX platform run remains live. Continue Sources only.
Added component/docs/S17a.md: optional owned native OCR, bounded raster/header/
decoded pixel/text/page coordinates, scoped monitor/deadline, explicit recognition
confidence/partial coverage, engine/decoder/language-data fingerprints, immutable
image and PDF page reads, authority/cache/control and independent acceptance.
Public OCR delivery not implemented yet. Do not copy legacy OCR global cancel/
temporary-path state. Mac Tesseract/Leptonica actual regular artifacts can be
resolved behind /opt/homebrew/lib/libtesseract.dylib and
/opt/homebrew/lib/libleptonica.dylib; English traineddata available.

Initial native bitmap-glyph PPM generator tests/ocr_fixture.c compiles with strict
C11 warnings and creates bounded680×150 RGB image within1MiB. Actual installed
Tesseract CLI returned "TOKEN *s14" for intended "TOKEN 7319" (exit0) at
/private/tmp/samosa-s17a-fixture-text.log, with stderr separate. This is a failed
recognition-quality fixture, NOT OCR qualification. Preserve numeric sentinel
and correct fixture rendering using a declared native font rasterizer such as
FreeType with explicit font, rather than narrowing assertion to TOKEN. No Python
or external image-conversion tooling used. Source C adapter/public read still
needed. Next verify native font availability, generate realistic deterministic
raster and prove actual sentinel recognition before using it in contract tests.
No required process handles currently live; full rebuild goal remains active.

### Latest Sources checkpoint — S16b qualified, OCR next

Both independent full DOCX/Sources normal/sanitizer/PDF/release runs passed:
Mac23572 and Linux11517 terminal exit0. After final XML name-start correction,
Mac87486 and Linux5356 terminal exit0: refreshed DOCX parser/public, HTML and
actual PDF regular/sanitizer checks plus regenerated public source archives.
DOCX public1304, HTML176; parser counts include many ZIP serialization assertions
and are not independent behavior counts. Native separate DOCX CLI processes pass.
Logs /private/tmp/samosa-s16b-{mac,linux}-{independent,final}.log and local
/private/tmp/samosa-s16b-latest-mac.log. Malformed/limited/cancelled/deadline output
does not change the evidence cache. Literal auxiliary text excludes generated
filenames. Final native stored/Deflate fixtures include explicit package entry/
auxiliary/depth/ratio/aggregate expansion and CRC refusal tests.

Extracted archives match each other AND qualified workspace public contents,
verified under /private/tmp/samosa-s16b-compare/{mac,linux}. SHA256:
Mac5a32e4a562f0280042223f0158687034b770e5c1a75b04c41e27f07c7ebc37b3;
Linuxdd9541d8dc940ec5de2005e65b967d5db475f244c18f20082b7d28311e0ed829.
The comparison command's later unrelated ls glob failed because a Leptonica
symlink name was guessed incorrectly; comparison loops and hash outputs had
already completed successfully. No required run is live. S16b reader delivery
is qualified, not whole Sources or C05/C06 closure. Keep existing matched
archives as historical qualified snapshots when adding OCR work.

Next Sources-only OCR: current Mac installed Tesseract5.5.3/Leptonica1.87.0
headers/runtime and /opt/homebrew/share/tessdata/eng.traineddata available.
Legacy native OCR uses Tesseract C API, but its global cancellation/temp paths
must not be copied into the independent component. New reader needs scoped
monitor/deadline control, bounded raster input/decode/output, original version/
pixel/page/text coordinates, explicit recognition coverage/confidence and
engine/data-bound fingerprint/cache. Qualify real native OCR without Python.
Resolve actual regular artifacts behind host symlinks for QA configuration;
do not weaken Sources artifact trust checks. OCR, media, acquisition, indexing/
enrichment, external Chutni and service/resource acceptance still required.
No Inference/Agent/App development until whole Sources closure.

### Current Sources checkpoint — S16b public DOCX implementation

Sources only; full goal and whole-component order remain active. DOCX public
register_docx/read_docx now use existing immutable snapshots/media owner claims,
cache quotas, decoded UTF-8 coordinate space and durable read control. Registration
captures ZIP-shaped bytes within1MiB; actual package/text validation occurs on
read before evidence publication. Shared document_read retains qualified HTML
behavior. DOCX fingerprint binds strict owned contract and literal auxiliary
joining: generated part filenames were removed from extracted literal text.

Native parser now validates bounded XML roots/structure/UTF-8, attributes/entities,
duplicate ZIP names including auxiliaries,8MiB aggregate declared expansion,
1MiB individual/XML,2048 entries,64 auxiliaries,200 ratio above64KiB,64 XML depth/
attributes and127-byte ASCII qualified names. Tests cover stored/Deflate actual
ZIPs, entities/deleted content, literal auxiliary text, metadata/DOCTYPE/root/
duplicate/malformed XML, CRC, expansion/ratio/depth/entry/auxiliary limits. Count
includes many serialized ZIP-byte assertions; do not equate it to distinct
behaviors. Public tests cover identity, owner, UTF-8 selectors, cache equality,
exact durable offline retry, active cancel/deadline, no cache changes on failures,
output limit and corrupt snapshot. Native DOCX CLI generator and separate process
checks pass. Both DOCX targets included default test/sanitize. All fixtures native
C; read-only licensed miniz remains independent, no Python/legacy include path.

Latest local59830 is terminal exit0: corrected XML name-start rule and its native
refusal test pass normal/sanitized DOCX parser/public (1304 public assertions).
Its log is /private/tmp/samosa-s16b-latest-mac.log; no local test remains live.
Mac55391 validated public/parser normal+sanitizer and HTML normal regressions
passed; Mac78097 bounds pass; Mac65071 CLI pass. Later aggregate expansion and
attribute/name-start validation added. Final local focused DOCX handle returned
below; log /private/tmp/samosa-s16b-latest-mac.log. Mac23572 and Linux11517 currently
run independent full normal/sanitizer/PDF/release in roots
/private/tmp/samosa-s16b-{mac,linux}-qa, logs
/private/tmp/samosa-s16b-{mac,linux}-independent.log. These copied BEFORE the latest
XML name-start correction and its test: refresh after terminal completion, rerun
affected parser/public normal/sanitizer/HTML/PDF checks and regenerate releases
before comparing final archives. Do not restart any handle solely for observation
timeout. No S16b closure claimed; stronger public corruption/remaining metadata
acceptance and final platform/package comparison still required.
Next complete S16b, then remaining Sources OCR/media/acquisition/indexing/
enrichment/Chutni/service/resource gates. No Agent/Inference/App development.
Operating correction: Mac23572 is terminal exit0 (full independent Sources
normal/sanitizer/PDF/release). Linux11517 remains confirmed live, currently full
sanitizer phase. Mac final refresh now runs only affected DOCX/HTML/PDF regular/
sanitizer targets and archive regeneration in its independent root; inspect its
new handle and /private/tmp/samosa-s16b-mac-final.log. Linux refresh must wait for
11517 to become terminal. No parallel mutation of its active build copy.

### Latest Sources work — S16b parser begun

After qualified S16a archive comparison, Sources workspace alone gained S16b
DOCX parser preparation. Copy src/samosa_docx.{c,h}, licensed read-only miniz
dependency under component/vendor/miniz, native tests/docx_fixture.h ZIP builder
and tests/test_docx.c. Original input cap remains1MiB for planned public delivery;
parser expansion limits lowered to8MiB aggregate and1MiB per-entry/XML, with64KiB
planned public readable output. Build via make test-docx-parser
sanitize-docx-parser. Mac75870 finished exit0: stored and Deflate fixtures,
entities/deleted text, traversal, duplicate required entry, malformed ZIP,
UTF-8/DOCTYPE and output-limit refusal pass normal/ASan/UBSan. Log
/private/tmp/samosa-s16b-parser-mac.log. The1783 count mostly includes native ZIP
serialization assertions; it does not imply1783 independently tested behaviors.

miniz strict warning failure was fixed by guarding three unused writer-only
array helpers under its existing disabled-writing macro; LICENSE and dependency
manifest preserved. No ZIP-reader algorithm changed. DOCX not wired into public
Sources dispatch yet and not qualified on Linux. Next implement required package
validation (metadata XML/duplicate auxiliary names/ratio limits), public immutable
registration/read, range/provenance/cache/control, stronger refusal fixtures and
independent platform/release gates. No Python fixture or tooling was run.
Sources remains incomplete; keep working Sources exclusively. No current required
live process handles remain. Historical S16a archive hashes refer to the prior
qualified snapshot, not the now-expanded DOCX workspace. Full rebuild goal active.

### Current Sources checkpoint — S16a qualified, DOCX next

Sources only: user-directed whole-component order remains Sources→Inference→
Agent→App→assembly/retirement. S16a HTML public delivery qualified. Mac89268 and
Linux2792 independent full test/sanitizer runs finished exit0. Mac66916 and
Linux72675 finished exit0 with actual PDFium regular/sanitizer regressions and
release gates. Final native HTML assertions Mac96673/Linux36744 finished exit0:
176 public checks each regular/sanitized, parser checks and separate CLI process
checks pass. Covers original-byte immutable identity, decoded UTF-8 selection,
owner refusal, suppressed active markup, retained snapshots, durable exact retry,
cache reuse/corruption, corrupt source refusal, active cancellation/deadline and
no cache changes on interrupted/limited output. No failed partial result admitted.
Logs /private/tmp/samosa-s16a-{mac,linux}-{independent,final,final-assertions}.log.
Local extended/public assertion runs also pass (25041/85451). Native PDF checks
passed221 on Mac and222 on Linux after HTML was compiled into production.

After the final test-only no-cache assertions, independently rebuilt test targets
passed; public source archives were regenerated using the exact release member
list (no compiled input changed since full suites). Extracted contents match
across platforms AND workspace. Comparison /private/tmp/samosa-s16a-compare/.
SHA256 Mac000475a6f126894644234d5489c9a4b01c1353951cc5145d94e54852f93e5df2;
Linux672a4bfc27f0e0aa3db14b2dc932a80d9923bcc05673091b279fedb85add82d6.
This qualifies HTML reader delivery, not all Sources or C05/C06. Current status
is recorded here without mutating the matched historical component archives.
Next implement/qualify DOCX within Sources, using owned native ZIP fixtures,
vendored licensed miniz read dependency and existing bounded extraction primitive.
No Python fixture/tool dependency. Full Sources OCR/media/acquisition/enrichment/
indexing/Chutni/service/resource qualification remains required. No live S16a
required handles remain; all inspected runs are terminal successful.

### Current execution order — Sources first, 2026-10-07

User explicitly rejected interleaved feature development. Finish Sources,
Inference, Agent, App in that order, then assembly/retirement. Full objective and
C01–C14 scope unchanged. Plan/status updated to this instruction. Preserve
unfinished Agent S15a code and diagnostics; do not expand Agent until its turn.
Linux64743 is terminal exit0: independent concurrency/control snapshot passed,
including precommit orphan-lease recovery. Later saved_reply workspace changes
and Mac work.lock creation race remain unqualified/unfinished.

Current Sources S16a: copied native HTML parser/header and native parser tests
into component, with no legacy/sibling source/build dependency. Added public
register_html/read_html, original-byte media claim/identity, decoded UTF-8
coordinate provenance, partial static-text coverage, owner-scoped cache keys
and existing durable read control. tests/test_html_source.c passes91 checks
regular/ASan/UBSan (Mac39737 exit0), parser normal/sanitized also pass (13399
exit0). Sources84517 initial production build succeeded. Public CLI/corruption/
stronger control/index/release gates still open: do not claim S16a complete.
Independent full Sources roots /private/tmp/samosa-s16a-{mac,linux}-qa; handles
Mac89268 and Linux2792 currently running public HTML plus full normal/sanitizers.
Logs /private/tmp/samosa-s16a-{mac,linux}-independent.log. Inspect handles before
calling either live/pass. No optional PDF tests rerun after HTML changes yet.
Next: complete HTML public qualification, DOCX and the full remaining Sources
requirements, rather than returning to Agent concurrency. Whole Sources scope
includes OCR/media/fetch/search/indexing/enrichment/Chutni/service and platform
resource/failure acceptance. No milestone or full component closure yet.

### Current state — 2026-10-07 S14d qualified, S15a implementation

Linux36544 finished exit0: independent Agent full normal/sanitizer/release in
/private/tmp/samosa-s14d-linux-agent-final.log. The delayed synthetic provider
now observes a committed terminal row after accepting cancel, retaining strict
confirmation and unchanged production budgets. Sanitized child6646, cancellation
2881, budget44, transport35, effect-handoff60, reconciliation134, finding110,
Agent811, checked copy972/undo477/move510 pass. Earlier Linux56068 failed only the
timing-sensitive synthetic fixture; its Sources qualification and actual strict
PDF-control2195/3116 plus additional truthful-unconfirmed4175 remain qualified.
Mac61428 normal/sanitized child passed; Mac10710 refreshed independent release.
Mac59555 had independently qualified strict PDF-control2078/2162. Sources actual
PDFium222 regular/sanitizer and full native model-free suites passed on Linux;
the corresponding current Sources macOS suites/PDF/release passed.

Final extracted public package contents match across platforms. Sources also
matches workspace; Agent matched workspace and Linux independent inputs before
the S15a production/test edits. A preparatory S15a documentation-only edit was
copied to Linux before packaging; no compiled input changed during qualification.
Comparison roots /private/tmp/samosa-s14d-compare/{mac,linux}-{sources,agent}.
SHA256:
- Sources Mac:0eccd0e5cff0b11927a089cab2dbceb43ea64484074a298af5d7a6c25d24f82c
- Sources Linux:260eaefca098daa16fca6be06c317a031bd92b957f5eae2dc3e844c1e7bc3b32
- Agent Mac:2143f1c2be41dc2a0bb74694780e074a7fc94b56dc43cad1d42d0e4953bfe2c8
- Agent Linux:893ea3058918b0a584cd4b033f6026439b9a4f5a0861576048c9449fe2836ae3

S14d is qualified; this closes the Sources-control task, not a C01–C14 milestone.
The detached Linux executable transport fix passed deterministic before/after
regression; generic assembled reliability remains a later requirement.

S15a current workspace now implements a short admission gate, eight exact
per-run leases, live-peer-preserving recovery, per-conversation refusal and
completion revision checks,32MiB semantic/64MiB database quotas with live
reservations. Aborted admission removes its exact lease file under the gate.
Native tests/test_concurrent.c verifies actual8 active generations, scoped cancel,
SIGKILL recovery, ninth refusal before history, live compaction refusal, healthy
peers and offline exact retry. Initial Mac regular/sanitizer pass581/1018;
five repeat regular scenarios pass. The first attempt returned state_unavailable
before second admission; cause not reproduced. Test-only state diagnostics and
accurate transient SQLite busy classification added; keep this concern open
until platform qualification. Controls28170 and full normal suite99913 are live
at this checkpoint; inspect handles before treating them as current. Logs
/private/tmp/samosa-s15a-mac-{controls,suite}.log. Linux S15a still to run in a new
independent copy, preserving the S14d snapshot. Full goal remains active: services,
scheduling, supported readers/providers/modalities, App, assembly/retirement and
all broad acceptance gates still required.

Latest S15a correction: Mac28170 and99913 finished exit0 (focused controls and
full normal suite). Precommit SIGKILL test plus bounded orphan-lease recovery
added; pending retry replies now take a committed SQL snapshot. Mac27939 then
failed before a concurrent worker admitted. Diagnostic repeat26171 reproduced:
QA Agent state failure line73 errno2, specifically openat(work.lock,O_CREAT...)
on concurrent first admission. Preserve /private/tmp/samosa-s15a-state-repeat.log;
do not call this solved by SQLite busy classification. Linux64743 currently runs
the independent controls snapshot preceding saved_reply; normal concurrency590
passed including precommit lease cleanup, sanitizer concurrency also passed,
subsequent control tests still running. Linux26233 was terminal compile exit2
due GCC indentation warnings, now fixed. Latest workspace changes need refreshed
Mac/Linux final qualification after resolving the admission-lock creation race.
User asked why zero complete components: explained mostly sequencing choice with
some cross-component dependencies. Finish current concurrency then prioritize
whole-component closure; full C01–C14 scope remains unchanged and active.

### Current state — 2026-10-07 14:12 UTC

Latest checkpoint correction — 2026-10-07:
Linux56068 is terminal exit2, not live: Sources full qualification/release and
strict PDF control plus the separate fully instrumented unconfirmed case passed;
the older synthetic delayed child fixture needed a third guardian launch beyond
the unchanged three-second observation budget. The fixture now observes the
worker's committed terminal row for at most250ms after accepting cancellation.
Mac61428 passed normal/sanitized child tests; Linux36544 is the current full Agent
test/sanitizer/release rerun. Mac59555 is terminal exit0: independent release and
strict PDF controls2078/2162 passed before this latest fixture edit. Final Mac
package refresh and archive/workspace comparisons still required. Full rebuild
goal remains active; no C01–C14 milestone closure is claimed.

Historical checkpoint — 2026-10-07 14:36 UTC:
14:38 correction: Mac38902 finished exit0, final independent strict control
2315/2357 and normal release/transport checks pass. Linux60446 finished exit2:
the additional fully instrumented unconfirmed scenario exceeded the runner's
eight-second worker wait (4s request + ~3.9s controlled teardown + sanitizer
process exit). Strict confirmation had already passed2222/3353. Only the separate
allow-unconfirmed scenario now gets12s worker wait, retaining total25s bound;
strict scenarios keep8s and production budgets are unchanged. Latest Linux full
rerun starts after this fixture correction; inspect its exec handle below.
Next must refresh the final Agent Mac archive for this test-only edit before
cross-platform/workspace comparisons. Do not treat60446 as a full-suite pass.
Historical required live handle: Linux56068. Full goal remains active; latest user
message requests status, not cancellation or pause.
User asked how much is completed/tested: answered three implemented components
with substantial qualified subsets, zero fully closed C01–C14 milestones;
replacement App, assembly and retirement unimplemented. Full goal stays active.
Linux helper failure is fixed with a deterministic before/after regression.
Post-fix strict native control passes regular2222 / sanitized-driver3353 checks,
with actual observed_cancelled and matching exact public terminal; no unconfirmed
accepted by these strict targets. Fully instrumented Linux control exceeds the
1.5s public call/3s observation budget, diagnosed via child.c test-only stderr:
1.915s/2.005s calls time out including owned teardown, correctly report unconfirmed.
New separate sanitize-source-control-unconfirmed tests this specified outcome
against exact parent identity, eventual public terminal and unchanged last-good
state. Strict target retains required observation; budgets are unchanged.
Earlier full-instrumentation strict failure is preserved in
samosa-s14d-linux-post-fix-unconfirmed.log and samosa-s14d-linux-control-budget.log.

LIVE Linux60446: script run.sh includes real PDF regular/sanitized, new transport
regression, strict regular/ASan Agent control, full-instrumentation unconfirmed
scenario, full Sources/Agent tests/sanitizers/releases. Log native.log above.
LIVE Mac38902: refreshed independent Agent release with current code/docs, normal
suite, transport regular/sanitized and actual three-component instrumented Source
control. Log /private/tmp/samosa-s14d-mac-release-final.log. Local focused post-fix
control32509 and final strict integration4312 exit0 (2225/2321 checks).
No further code edits needed unless those runs fail. Collect both, compare final
Sources/Agent tar contents across platforms and against workspace public files,
record hashes. Then implement S15a (spec written at docs/S15a.md): bounded eight
independent conversation runs, one active turn per conversation, exact worker
leases/recovery/cancellation and pre-admission capacity refusal. Do not narrow the
remaining scheduling/services/providers/readers/App/assembly/retirement goal.

14:29 reliability finding: diagnostic conclusively reproduced immediate ENOENT
when readlink(/proc/self/exe) returned the running Linux executable pathname with
` (deleted)` suffix. Preserved log samosa-s14d-linux-deleted-path.log. Agent was
passing that advisory resolved pathname into posix_spawnp. Fixed Linux self_path
in src/command.c to use the kernel `/proc/self/exe` reference itself, selecting the
same loaded executable even after detachment/replacement. macOS mechanism stays
unchanged. Spec docs/S14d-helper-reliability.md written before implementation.
New test_transport.c deterministically copies/executes/unlinks its own binary,
then calls the owned guardian for an exact /bin/cat round trip. Old code reproduces
ENOENT and child_failed; fixed Linux regular25/sanitized35 checks pass,27202 exit0.
Evidence samosa-s14d-linux-transport-{before,after}.log. Mac regular/sanitized4
checks pass,90746 exit0; Linux-only detachment is explicitly not a Mac claim.

Linux initial complete Agent regular/sanitizer94241 now exit0 (pre-helper fix).
Independent Mac complete37779 exit0, including full instrumented control2374.
These packages precede current helper/QA changes and are not final hashes.
LIVE post-fix native Linux integration81262 (run.sh and native log above), Mac
focused cancellation/child/budget32509 (samosa-s14d-agent-control-mac.log).
Collect both, then updated Linux complete Sources/Agent/release and final Mac/
Linux package comparisons. Root cause of the reproduced launch failure is now
established; reliability gate closes only after required post-fix qualification.

14:21 follow-up: extended native integration found a real Sources PDF concurrency
bug: pdf_read retained a SQLite SELECT/read transaction during native extraction,
blocking the peer's cache commit. Fixed src/sources.c to copy the bounded (1MiB)
immutable snapshot/locator, finalize the statement, then extract and hash the copy.
Native peer page97 now completes while the delayed Agent reader remains active;
its completed cancel stays cancel_requested=false. Test additionally preserves
last-good continuation. Fixed GCC misleading-indent in tests/pdf_fixture.h,
tests/pdf_spy.c and the new Agent runner. Local full Sources tests/sanitizers/PDF
passed (2518 exit0; /private/tmp/samosa-s14d-source-final-mac.log). Final local Mac
control2206/2308 checks passed (18964; log samosa-s14d-mac-control-final.log).

Linux real PDFium regular/sanitized222 checks and CLI pass. Native control2185
passes, but sanitized control reproduced child_failed in an external helper
(preserved samosa-s14d-linux-child-failed.log). A later partial diagnostic run
passed sanitized driver3466, then fully instrumented Source/Inference control
reported honest unconfirmed child cancellation; it fails the intended exact
terminal acceptance, so S14d remains open. No ASan report found yet. These Linux
control failures are not waived. New runner compiles test-only helper stderr
diagnostics; latest script forces builds so Makefile flag changes cannot leave
stale binaries. Deadline fixture now4000ms/native four pages each2000ms, requiring
an admitted read child (earlier600ms expired during sanitized startup).

LIVE: Linux Agent full regular/sanitizer exec94241 (regular suites and sanitized
cancel3163/child7729/budget44/actions/handoff/reconcile/find passed; test-agent-
sanitize currently running). Independent Mac full release/sanitizer exec37779,
script /private/tmp/samosa-s14d-mac-qa/run.sh, log samosa-s14d-mac-independent.log.
Latest Linux native diagnostic uses /private/tmp/samosa-s14d-linux-qa/run.sh and
log samosa-s14d-linux-native.log. Source-control binaries compile with
SAMOSA_AGENT_TEST_FAULTS only in QA; stderr and per-process ASan log_path enabled.
Inspect current exec result for its live handle before resuming. Earlier83771,
96784,22262,11433,33833 are terminal failures, not qualification passes.
Exact next: collect the three live runs, inspect helper diagnostic termination
and asan-control files, resolve child_failed/unconfirmed control, then final
independent releases/content comparisons and remaining full rebuild gates.

User explicitly requested continuation of the entire rebuild and creation of an
active goal until complete. Goal created for all C01–C14, App/assembly/retirement
included. User confirms broad authorization; sandbox command approvals remain
platform-enforced. No delegation or Python. Docker restored and native ARM64
qualification resumed. Requested image cleanup removed ten obsolete Samosa/base
entries; all Truly Typed/builder/test images and current QA images retained.

S14d integration spec: components/samosa-agent/docs/S14d-integration.md.
New native runner tests/source_control.c and test-source-control /
sanitize-source-control Makefile targets use actual external Sources/Inference,
native PDFium with page-delay spy, durable Agent conversations, owner refusal,
public Source terminal consistency, scoped cancel, deadline, exact offline retry,
last-good evidence/scope/assistant preservation and healthy follow-up. Mac regular
1079 and ASan/UBSan1229 checks pass (exec65089 exit0). Evidence command and QA roots:
/private/tmp/samosa-s14d-mac-integration-evidence.txt. Fake Inference is used for
control qualification; this is not a model quality claim. Deadline termination
can observe Source cancelled or failed: parent deadline status and observed
exact child status must agree with public Sources state, not a guessed status.

Linux Sources complete test/sanitize passed after fixing GCC misleading-indent
failure in src/control.c; operations723/726, request-control135 both modes,
readers/inventory/index/scope/locator and CLI passed (exec4706). That combined
invocation then hit copied stale macOS Agent binaries, so Agent restarted with
forced native builds (exec94241): regular suites passed, sanitizers running.
This fixture/build failure is separate from the earlier child_failed issue.

LIVE Linux native PDF/integration exec83771. Log:
/private/tmp/samosa-s14d-linux-native.log; independent roots and exact script:
/private/tmp/samosa-s14d-linux-qa/run.sh. PDFium chromium/7961 Linux ARM64 archive
digest verified against GitHub release metadata:
974107999784a438149605024475d42d80dd306799d90e1af5f6fa63f976455f.
Includes PDF regular/sanitized tests, native Agent/Source control and fully
instrumented Agent/Source/Inference integration. Required test failures remain
part of S14d. No final release comparison or Linux reliability closure claimed.

Exact next action: observe exec94241 and exec83771, inspect logs, fix any failing
S14d scenario, then independently package/compare final Mac/Linux contents.
Investigate earlier Linux child_failed with full helper stderr and ASan diagnostics
before moving to concurrent Agent admission/scheduling/services and remaining
App/assembly/retirement gates. Full rebuild goal stays active.

### Previous state — 2026-10-07 13:54 UTC

User requested a status summary and documentation. Current consolidated status:
docs/ARCHITECTURE_REBUILD_STATUS.md. Previous goal turn made progress: completed
S14c2 release contents verification and began S14d after its specification.
Full rebuild goal remains active; no narrowing to the component subset.

S14d Sources now owns tracked read_text/read_pdf/inventory operations with
normalized owner/request/parent/budget identity, offline exact terminal retries,
worker leases,8 active/128 history limits,2MiB result/32MiB reserved+committed
results/64MiB operation DB. Public get/cancel is owner scoped; cancel and terminal
publication serialize. Dead worker recovery reports interrupted, retains intent
and refuses implicit replay. Digest corruption and private symlink refusal tested.
Native PDF now checks cancellation/deadline between pages and every256 characters;
foreign native calls remain cooperative, not hard-interruptible. Agent Source
adapter reuses exact public child cancel/status when required caps are present.

Local Mac results: full existing Source model-free tests/sanitizers57563 exit0;
control30629 passes135 regular/sanitizer; operations78080 passes723/822 checks;
actual native PDF2635 passes221 regular/sanitizer plus separate PDF CLI; Agent
89459 passes44 adapter and1176 child checks. Current source/adapter changes have
NOT yet had independent final macOS/Linux full package qualification or complete
end-to-end Agent reader cancellation. Latest exec handles inspected are terminal.
Logs named in the status document. No new actual model test in this continuation.

At13:54 Docker socket /Users/deepanwadhwa/.docker/run/docker.sock is unavailable.
Restore local daemon before new ARM64 Linux QA; continue useful Mac work meanwhile.
Earlier unexplained Linux untimed child_failed remains an open reliability item,
not proven fixed by later passing diagnostics. No commit/deployment/legacy switch,
permission request, delegation or Python. Tracked legacy diff remains empty.
Next: finish S14d integration/qualification, then all outstanding C01–C14/App/
assembly/retirement requirements listed in the consolidated status.

### Previous checkpoint — 2026-10-07 00:52 UTC


Full rebuild goal remains active. S14c2 implements durable Agent timeout_ms1..60000,
canonical retry binding, receipt-time monotonic deadlines and inherited host
limits. Admission rechecks deadline before commit; finished exact retries stay
offline. Count/generation retries and describe/session/read calls share remaining
time. Deadline failure preserves admitted user history/revision and last-good
assistant/evidence/continuation; cancel_requested remains false unless cancelled.
No delegation, Python, permission request, commit/deployment or legacy switch.

Inference advertises request_budgets and accepts count/generate relative budgets,
binds them into tracked request identity and shares deadline across queue,
properties/template/tokenizer/generation and semantic commit. Agent requires that
capability for timed work. Qualified Sources reads carry exact parent/child refs
and remaining budget; Agent persists read child observations and strips refs
before evidence admission. External Source transport failure stays unconfirmed.
Source budgets now capture receipt before parsing. Its durable get/cancel service
and hard native/syscall bounds remain open. Inference public exact-child cleanup
retains its separate nominal3s observation window, with authoritative terminal
races; expiry is not mislabeled as user cancellation.

Focused Mac tests: Source adapter44 checks regular/sanitized; durable cancellation/
deadline normal262 and sanitized529 before longer Linux-safe fixture startup
budget, then current independent Mac normal262/sanitized574. Expired admission,
invalid budget, offline retry/changed-budget conflict and no new assistant on
expiry pass. Real separate Sources/Agent/Inference timed selected text succeeds,
records completed Source child, strips prompt references and replays offline.
Inference operations832 checks; native controlledHTTP373 checks regular/sanitized
on independent Mac AND ARM64 Linux: properties/template/tokenizer/generation
expiry plus aggregate150ms-per-stage exhaustion, last-good session and survivor.

Actual model fully instrumented Agent/Inference run63480 exits0,1852 checks:
observed active engine, public parent/child cancellation, unrelated queued answer,
2.5s durable deadline, terminal provider child failed/observed_failed, offline
retry, last-good follow-up answer and owned EOF/no credential cleanup.
Log /private/tmp/samosa-budget-actual-all-sanitize.log; QA
/tmp/samosa-child-native.mGHNrk. Same qualified external engine/model as prior
checkpoint; original artifacts read only. No actual Linux model/PDF extension.

Independent Inference full test/sanitize/test-native/sanitize-native/release
Mac49941 and Linux15095 exit0 on production code. Final aggregate fixture/Makefile
focused runs59582/63622 plus release exit0. Logs
/private/tmp/samosa-shared-budget-inference-{mac,linux}.log and
samosa-shared-budget-inference-{mac,linux}-aggregate-final.log.
Root /private/tmp/samosa-shared-budget-independent.20261006-61311-immq6v/inference.
Extracted final Mac/Linux release contents match each other AND workspace.
Hashes Mac e99afd2869fc35fb67da9d97a420806578034f88292085f3f67528ccfe80f154;
Linux fc5774a04ac489c416bc1505bf1cf4fd53268c8116b34291e16ac9acc4a06ddc.

Sources receipt adjustment independently passes control124 regular/sanitized and
release (full normal suite) on Mac49212/Linux50300. Prior native PDF qualification
unchanged. Logs samosa-shared-budget-source-{mac-independent,linux}.log under
/private/tmp. Independent Sources root remains
/private/tmp/samosa-source-budget-independent.20261006-56585-1oaw1b8.
Final extracted Mac/Linux release contents match each other AND workspace.
Hashes Mac3b535975f0603d5164c163a3afeeacf077c9b6ffed08dbc121f2608aa77ccbae;
Linux4cbfa1f926c40390cd66406a8e07a5238ec39edae8833368f74f5c5322f4baf6.

Agent independent Mac full45339 test/sanitize/child/cancel/release exits0; final
fixture/driver/public interaction7794 and changed startup-budget38269 exit0.
Root /private/tmp/samosa-shared-budget-independent.20261006-61311-immq6v/agent.
Linux first73346 regular suites passed, then new test_budget lacked XOPEN realpath
feature macro: fixed.59712 full regular/action/handoff/find/agent sanitizers pass,
then deadline fixture expires before observed hanging-helper marker under Linux
ASan. Increased fixture startup allowance2s->10s while keeping actual helper
lifecycle/deadline15s assertion; real-model2.5s proof is unchanged.74438 passes
regular deadline checks but sanitized UNTImed first operation reproduces earlier
child_failed at test_run_cancel.c54. Cause remains NOT proven; do not claim full
Agent Linux qualification. Failure logs
/private/tmp/samosa-shared-budget-agent-linux-{final,fixture-final}.log.

LIVE exact next: diagnostic48144 with SAMOSA_AGENT_TEST_HELPER_STDERR=1 and
ASAN_OPTIONS=log_path=/component/build/asan-shared-budget. It passes sanitized
run cancellation2701 checks; sanitizer child-operations is still running. Log
/private/tmp/samosa-shared-budget-agent-linux-diagnostic.log. Owned Docker
container d76c8147b189. Source/Inference independently qualified ARM64 binaries
copied into ephemeral /sources/build and /inference/build for final public
interaction (original diagnostic invocation omitted those mounts). Wait on48144;
inspect diagnostics/ASan reports and preserve unexplained transport failure even
if it passes. Finish Linux focused/release/public interaction and compare final
Agent release contents/workspace. No other exec handles remain required live.

00:55 follow-up: diagnostic48144 finishes sanitizer child control successfully,
but its later Source adapter sanitizer failed expected two child observations
because the0.7s fixture deadline elapsed before the intended read stage. No ASan
report was produced. Extended Source fixture deadline5s with blocking read10s,
retaining two durable event/outcome assertions and8s bound. This fixture allowance
is independent of production budgets and actual-model2.5s qualification.
LIVE replacements: Linux69888 (mounted qualified Sources/Inference) and Mac92145
focused adapter normal/sanitizer, optional drivers and release. Logs
/private/tmp/samosa-shared-budget-agent-{linux,mac}-adapter-final.log.
48144 is now terminal; no diagnostic container remains. Wait replacements,
compare final Agent releases/workspace and preserve unexplained UNTImed
child_failed regression; then continue remaining gates. Check current Docker
container IDs rather than reusing the obsolete d76c8147b189.

Final S14c2 follow-up:69888/92145 exit0. Linux current adapter44 normal/sanitized,
optional drivers, full regular release suites and actual public three-component
timed interaction pass. Mac focused adapter/release pass. Agent final extracted
Mac/Linux release contents match each other AND workspace, diff exit0.
Hashes Mac2a1cdbd671f26e992d7755ea09bb9556519dbeb5cf423257b6069b93c1b821dd;
Linux4c3d6b766ff168c2ef738eb354881a58ba09c952966c5e13937ab071e8251865.
Unexplained earlier untimed child_failed remains a reliability qualification
item; diagnostic runs do not establish its cause. No required live exec remains.
Next named work S14d durable Source operations specified before coding at
components/samosa-sources/docs/S14d.md; prior Source package hashes describe the
qualified S14c2 snapshot, not the newly edited S14d worktree.

Then durable Sources operation control, concurrent Agent admission, tasks/
scheduling, authenticated services, actual supported provider distribution/
recovery/resources and modalities/readers, App, assembly/load, transport diagnosis
and C14 retirement remain required. C01-C14/C11 and full rebuild are NOT complete.

### Previous checkpoint — 2026-10-07 00:33 UTC


Prior goal turn made progress. This continuation adds independently qualified
Sources read correlation/cooperative budgets, finishes current Agent independent
build/sanitizer/release checks and preserves the earlier intermittent transport
failure as an unresolved load/reliability item. Full C01-C14 goal remains active.
No permission request, delegation, Python, commit, deployment or legacy switch.
Tracked legacy diff remains empty; components/rebuild documents are untracked.
All owned exec handles below are terminal and docker ps reports no running QA
containers. No actual provider/model/personal service was modified this turn.

Agent test-only diagnostics now identify invalid lifelines, helper/guardian spawn
errors, wait failures and exit status when explicitly enabled by
SAMOSA_AGENT_TEST_HELPER_STDERR in test-compiled binaries. Production stderr stays
suppressed. Earlier full31991 failed the post-cancel status read with child_failed
and no ASan report; focused44611 passed1251/6941. Current full36218 passed ALL
regular and sanitizer component suites, including child1269/6943 and run cancel
189/2922, plus native Agent811. Its overall exit2 was a later GCC warning in the
optional child-native driver (misleading one-line if). Braces fixed it; driver
builds regular/sanitizer and release22595 pass on Linux. Mac full80507 exits0,
child1123/1699 and run cancel189/462; drivers/release82246 exits0. Logs under
/private/tmp/samosa-parent-agent-{mac,linux}-{launch-diagnostic-full,driver-final}.log.
The intermittent child_failed did not recur in the latest full run. Its cause is
NOT proven fixed and must remain covered by later transport/load qualification.
Live /proc inspection confirmed actual LeakSanitizer ptrace scans and changing
owned helper processes during the long Linux suite; no timeout-triggered restart.

Actual-model final proof from prior turn remains973 checks with Agent/Inference
instrumented, observed active work, parent/child cancellation, unrelated queued
answer, offline retry, survivor and credential/EOF cleanup. No new production
transport semantics after that proof; later command.c edits are narrowly test
observability and equivalent explicit spawn-return handling. Native QA driver
only received braces for GCC portability. Inference release contents/hashes
remain the ones recorded at00:06; no Inference source edits in this turn.

Agent current extracted Mac/Linux packages AND workspace distributable contents
compare identical (set-e/diff exit0). Independent root
/private/tmp/samosa-parent-independent.20261006-36881-85l6r1/agent.
Final archive SHA256 Mac5f8e2d8d70115e07964bcba9ed66cb7fd8d3bddd140528dba27c64c04307d923;
Linuxa0008777d048a0f9e5d3782fe528cf5509037278d149790a2afd189749083ca1.

S14c1 Sources adds control.c/h and host-scoped deadline/cancellation fields. Public
read_text/read_pdf/inventory accept optional operation_id/parent_operation_id and
timeout_ms1..60000. Valid references echo on qualified success/failure; parent
requires a different valid ID. Public budgets shorten inherited monotonic budget,
never extend it. Unsupported contract is checked before optional metadata.
Describe advertises request_correlation/request_budgets. References confer no
owner/root authority or durable get/cancel history; index/locator mutation fields
remain strict. Inventory checks between entries; snapshot/cache transactions
check interruption before commit. Expired/cancelled work preserves prior data and
cannot publish a fresh interrupted evidence cache entry. Read cache identities
exclude transient references/budgets. Native extractor/kernel calls still need
owned helper isolation for a hard deadline; no universal syscall bound claimed.
Specs/contracts: Sources/docs/S14c.md, contracts/text-v1.md and README.

Sources controls124 checks normal/sanitizer on Mac and native ARM64 Linux. Real
Mac PDFium196 checks normal/sanitizer include delayed actual native extraction,
deadline no-cache, subsequent fresh extraction and later cache hit. No actual
Linux PDFium qualification was executed in this turn. Full standalone Sources
Mac68952 test/sanitize/test-pdf/sanitize-pdf/release and Linux53717
test/sanitize/release exit0. Final contract-version precedence check added after
those full runs; final focused controls/sanitizer plus release80340/41736 exit0
on current source (release reruns complete normal suite). Logs
/private/tmp/samosa-source-budgets-independent-{mac,linux}.log and
samosa-source-budgets-final-{mac,linux}.log. Source wrapper is preparatory to
Agent's aggregate budget; it does NOT close propagated end-to-end deadlines.

Independent Sources root
/private/tmp/samosa-source-budget-independent.20261006-56585-1oaw1b8.
Extracted Mac/Linux packages AND workspace distributable contents compare
identical, set-e/diff exit0. Final hashes:
Macd3588fe93290f9d0a31768eab58a1cff491d75b584b7c0e160108fbfe6c73950;
Linux2f77363b933a499985a3967f6b9b221fb4d93f4b23cde64af441b876fb6a19d7.

Exact next action S14c2: specify/implement durable submit timeout_ms in Agent's
canonical request, capture monotonic receipt time before admission, preserve
finished offline retry, share remaining budget across Source/Inference calls and
count/generation retries. Use Sources' capability-qualified references/budgets;
propagate Inference budget and qualify typed deadline outcomes, cancellation
observation vs unconfirmed, last-good preservation, completion race and unrelated
survivors. Then durable Sources operation control, concurrent Agent admission,
tasks/scheduling, authenticated services, actual provider distribution/recovery/
resources and supported modalities/readers, App, assembly/load, intermittent
transport diagnosis and C14 retirement remain required. No full rebuild or full
C11 completion is claimed.

### Previous checkpoint — 2026-10-07 00:14 UTC


Full goal remains active; S14b is implemented but independent Agent qualification
is NOT closed. Current production control window3s, up to60 attempts, per-call
allowance1.5s. Most recent fully instrumented actual-model run78845 exited0:
973 checks plus managed EOF/no retained credential. Log
/private/tmp/samosa-child-native-all-sanitize-final.log; root
/tmp/samosa-child-native.59P25z. No actual-provider worker remains from that run.

Final Mac full test/sanitize/release78235 exited0 with children1127/1884 and
run cancellation189/441 regular/sanitizer checks. Optional independent Mac native
QA drivers77385 compile regular/sanitizer. Subsequent fixture status-read-only
changes71505 passed child1107/1807 and release; metadata release74247 passed.
These archives predate the newest TEST-ONLY helper diagnostics below; rebuild and
final content comparison remain required. Existing production code path keeps
helper stderr suppressed.

Linux final67922 failed at the post-cancellation public operation_get transport.
Increasing ONLY follow-up status inspection allowance2s->5s did not fix it:
83760 reported child_failed at test_child_operations.c:79. This is NOT proven to
be a timeout, and earlier speculation is superseded. Parent cancellation and
child outcome assertions before that status read passed in these runs. Focused
sanitizer44263 with ASAN_OPTIONS log_path passed6891 checks, produced no ASan report;
that isolated pass does not erase the intermittent full-suite transport failure.
Logs samosa-parent-agent-linux-{final-pass,qualified-final,asan-diagnostic}.log
retain it. Full assembled cancellation acceptance must not be reported closed.

command.c now has narrowly test-compiled SAMOSA_AGENT_TEST_FAULTS diagnostics:
explicit SAMOSA_AGENT_TEST_HELPER_STDERR preserves helper stderr and reports
failed guardian completion/reap/status. Production binaries do not enable this
host test environment flag. Child fixture builds now define the test macro,
with no fault environment set during ordinary qualification. Independent current
Mac/Linux source copies include these exact changes. Live Linux full run31991
uses ASAN_OPTIONS=log_path=/component/build/asan-child and
SAMOSA_AGENT_TEST_HELPER_STDERR=1, make test sanitize release build/child-native
build/child-native-sanitize. Log samosa-parent-agent-linux-diagnostic-full.log.
Mac matching full diagnostic-source rebuild is live session54050; log
samosa-parent-agent-mac-diagnostic-full.log. Do not restart
a live run on observation timeout. Native Linux image3df5cca1b5a7, --init,
--network none and auto-removal; check only owned diagnostics under independent
root's agent/linux/build/asan-child*. No personal service/model changes.

Independent root /private/tmp/samosa-parent-independent.20261006-36881-85l6r1.
Inference final full tests/sanitizers/native CLI/releases and content comparison
remain passing with hashes recorded in prior checkpoint; no Inference edits since.
Agent packages must be rebuilt, extracted and compared against each other AND
workspace after current runs finish. Most recent Mac archive before test-only
instrumentation is obsolete as final evidence; do not reuse its hash.

Exact next action: poll31991 and current Mac rebuild; inspect Linux guardian/ASan
reports if any failure, explain/fix the actual condition, finish remaining checks
and final archive comparison. Then S14c shared monotonic submit budget and Sources
parent/control boundary as specified below. Full C01-C14, concurrent admission,
tasks/scheduling, authenticated services, production provider recovery/resources,
App, assembly/load and retirement remain open. No permission, delegation, Python,
commit, deployment or legacy switch. Goal remains active, not complete or blocked.

### Previous checkpoint — 2026-10-07 00:06 UTC


Goal remains active; S14b final qualification continues. Subsequent to the prior
checkpoint, fully instrumented Agent and Inference actual-model run passed971
checks plus lifecycle/credential cleanup (samosa-child-native-all-sanitize2.log).
First fully instrumented attempt failed because a named standalone sanitized CLI
target did not exist; optional build/samosa-inference-llama-sanitize now exists
and independently compiles on Mac/Linux. No provider work occurred in that failed
attempt. Current final live run78845/root /tmp/samosa-child-native.59P25z awaits
terminal result after the final control-call budget adjustment.

Linux diagnostics showed honest unconfirmed results when instrumented helper
startup exhausted the per-call0.5s limit. Normal fake provider alone was not enough
because Agent's self-exec guardian remained instrumented. Per-call control limit
now1.5s within UNCHANGED3s total monotonic window; cancellation result fixture
bound remains6s. Linux test-child-operations/sanitize-child-operations now pass
1257/7181 checks (samosa-parent-agent-linux-control-budget.log,35941 exit0).
Sanitizer runs use separately normal-tested fake provider executable while Agent,
its worker, guardian and child control logic remain instrumented; the real native
Inference path is independently instrumented above. The fake provider contains
no production Inference source. Fake provider startup wait20s is separately
bounded. This is not a claim of absolute kernel fsync or process startup timing.

Final independent Mac Agent full test/sanitize/release78235 and Linux67922 remain
running on current source. Earlier corrected Mac full46288 exited0, serialized
fixture/release2549 and qualified99741 exited0. Earlier Linux10980,19294 and71048
failed the control outcome or incomplete continuation; preserved logs are not
passing full qualifications. Continuation71048 also named nonexistent sanitizer
make targets; final whole make invocations replace it and must finish. No running
container is restarted merely because a status poll timed out.

Inference final extracted Mac/Linux archives and workspace distributable contents
compare identical (set-e/diff exit0). Source root remains
/private/tmp/samosa-parent-independent.20261006-36881-85l6r1. Final release targets
73387/23419 exit0, original full qualification96928/77427 exit0.
Archive SHA256 Mac7d048bc0580b810db4a9723c1961e4f7b0aa3e7f6eeeb1eec0fc0a5d71fbc2ca;
Linuxcff49e514cf9101c101206f676d4f3a464b759cba178812ecb6deb8f5e2da9f8.
Agent release content comparison and terminal final suites remain required.
No permission request, delegation, Python, commit, deployment or legacy switch.

Next S14c specification: a bounded relative budget for durable Agent submits;
normalize/bind it for retries, subtract elapsed monotonic time across dependency
calls instead of restarting a fresh timeout for each count/generate retry. Expiry
must preserve admitted user history/last-good state and a typed deadline outcome,
with separately observed/unconfirmed child cancellation. Extend Sources' public
read/operation boundary for parent correlation and owned control; internal Source
reader/index loops inherit the owning operation budget. Public deadlines are
budgets, not trusted wall-clock timestamps. Restart recovers interrupted work,
never silently reissues model or physical effects. Then qualify expired ingress,
slow reader/tokenizer/generation, completion races, unrelated survivors and exact
offline retries. Concurrent Agent admission, task/scheduling, authenticated
services, production provider distribution/recovery/resources, App, assembly/load
and C14 retirement remain required; no full rebuild completion is claimed.

### Previous checkpoint — 2026-10-06 23:59 UTC


Full C01-C14 rebuild goal remains active. S14b implementation is present but
final independent Agent qualification is still running; no milestone closure.
Inference now atomically binds optional parent_operation_id to owner-scoped
count/generate operations, normalized retries, saved results and public status.
Agent records up to16 child operations before transport, validates both references,
uses exact public cancel/get control and distinguishes observed terminal status
from unconfirmed control. Typed provider errors remain typed. Parent death marks
unfinished children unconfirmed; no sibling private state is read.

Controlled current checks: Inference operations769; Agent child fixtures1141
regular/2245 sanitizer after fresh-process worker conversion. Earlier fixtures
598/1645 passed before the control-window correction. Actual owned engine/model
qualification passed825 regular and823 Agent-sanitized checks, including observed
active generation, public Agent cancellation, observed_cancelled child, unrelated
queued answer4, offline exact retry, later Agent answer4, EOF shutdown and no
retained test credential. Logs samosa-child-native-actual3.log and
samosa-child-native-actual-sanitize.log under /private/tmp. Fully instrumented
Agent/Inference actual runner remains live session30671, root
/tmp/samosa-child-native.lg6COe; do not restart based on observation timeout.

Preserved failures: first live runner correctly refused nonprivate original
engine; runner now makes its own mode700 engine copy (model remains read-only).
Next live run returned unconfirmed because12 polling attempts exhausted in~0.6s,
before native curl cancellation observation.60 attempts retain the original3s
monotonic control window; subsequent actual runs passed. One Inference parent
fixture initially exhausted its pre-reserved byte quota; isolated fixture passes.
Independent Mac Agent worker crashed in Apple SQLite system tracing after fork
(reports test-run-cancel-2026-10-06-195622/195704.ips); new cancellation fixtures
exec a fresh worker before database entry. Native actual worker likewise execs.
Linux child sanitizer exceeded its old5s startup marker wait; startup allowance
now20s, separate from unchanged6s cancellation result bound. Failures retained in
samosa-parent-agent-independent-{mac,linux}.log and mac-retry.log.

Independent source root /private/tmp/samosa-parent-independent.20261006-36881-85l6r1
has Mac/Linux Agent and Inference copies, no sibling build dependencies. Inference
make test sanitize test-native sanitize-native release exit0 on both platforms;
logs samosa-parent-inference-independent-{mac,linux}.log. Native Linux image
3df5cca1b5a7, network disabled, --init. Agent full Mac corrected run session46288,
Linux fixture continuation71048 still need final status, remaining sanitizers,
final source/release content comparison and archive hashes. Original Linux Agent
full run26829 failed startup marker; do not report it as passing. Extra Mac
fixture run94106 passed but overlapped full tree builds; final serialized fixture
checks required. Current workspace full Agent suite90884 passed before fixture
launcher changes; current Inference31306 and native85860 passed. No permission,
delegation, Python, commit, deployment or legacy switch. Tracked legacy unchanged.

Next finish S14b independent qualification and package comparison, then S14c
propagated bounded deadlines and Sources correlation/cancellation. Concurrent
Agent admission, tasks/scheduling, authenticated services, provider distribution/
recovery/resources, App, assembly/load and C14 retirement remain unfinished.

### Previous checkpoint — 2026-10-06 23:36 UTC


Previous goal turn was progress; this continuation also changes authoritative
implementation and qualification evidence. Full C01-C14 rebuild goal remains
active. S13b catalog/setup and S14a durable Agent run/helper cancellation now have
independent native qualification. This does not close their broader milestones.
No permission request, delegation, Python, commit, deployment or legacy switch.

Inference adds catalog_host.c and setup_main.c. Native architecture/product OS
(macOS) or kernel version (Linux) is observed without caller overrides. Numeric
minimum versions are validated and enforced before install state creation.
Catalogs can be empty for model-free operation; size pins are <=INT64_MAX.
samosa-inference-setup consumes only operation/provider_id public selection;
singleton host flags refuse duplication, secret files are read only for install/
resume and key buffers are cleared. List hides URLs/paths/secrets and reports
platform/runtime/OS availability. Shared installer lease and aggregate quota
remain authoritative; setup does not activate. SIGINT/SIGTERM propagate to the
transfer callback. New provider-catalog-v1.md/S13b.md and README describe scope.

Catalog950 regular AND ASan/UBSan on macOS/native Debian12 ARM64. Setup40 regular
AND sanitized on both, including real HTTPS/certificate refusal, request/owner
config isolation, quota/foreign refusal, last-good preservation, offline retry,
actual SIGTERM after a retained byte, durable interrupted status, explicit resume
requirement and successful pinned replay. Existing remote167 and credentials42
remain passing on both. Full independently copied make test sanitize test-native
sanitize-native release exit0 in /private/tmp/samosa-catalog-independent.D7ANrl/
{mac,linux}, logs /private/tmp/samosa-catalog-independent-{mac,linux}.log.
Final README/contract/optional QA changes have release exit0 in
/private/tmp/samosa-catalog-complete-release-{mac,linux}.log. Initial focused
logs: samosa-setup-first.log, samosa-setup-cancel.log and
samosa-catalog-boundaries.log, all under /private/tmp, exit0.

New optional tests/remote_native.sh explicitly qualifies only the recorded
20513984-byte macOS static engine pin. It serves actual bytes over controlled
HTTPS, installs by catalog ID, verifies offline exact retry and native executable
qualification, then calls the public managed actual-provider runner against
those downloaded bytes and the existing read-only pinned model. Active engine
generation is observed before in-stream cancellation; cancelled worker, surviving
answer4, EOF shutdown, offline tracked retry and no retained managed key pass.
Regular /private/tmp/samosa-setup-actual-remote.log and independent sanitized
/private/tmp/samosa-setup-actual-remote-sanitize.log exit0. QA roots
/tmp/samosa-remote-native.QuJbYl and .JDK7il; managed roots .K6H2JT/.HaTr9K.
No downloaded/copied model, production published catalog, complete redistribution
license review, Linux actual engine or power-loss qualification is inferred.

Inference Mac/Linux extracted packages AND workspace distributable files compare
identical, set -e/diff -qr exit0. Final archive hashes (metadata differs):
Mac9294d489d350943a65eed1265e7372274a83e37e9b1167a7b41a9b5fc0bc36f6;
Linux3f2e79a9a69f9344a2adf9697c84040d628fbb4d143d53f89735f67158fe9744.

Agent adds owner/operation-keyed private run_controls, cancel_run by exact
operation_id or run_id, and cancel_requested on run responses. Scoped per-run
callback/context flows through command_call_controlled; existing command_call
uses the same implementation. Cancellation closes only that helper lifeline and
waits for guardian group cleanup. Terminal publication and the final cancellation
observation share one SQLite transaction. Cancelled work retains the admitted
user message but commits no assistant or changed last-good context. Committed
success is unchanged by late cancellation. Exclusive work-lease recovery marks
a dead already-cancelled worker cancelled; other dead work stays interrupted.
Exact terminal retries remain offline. Contracts/README/S14a.md record limits.

New native test_run_cancel.c proves two independent same-owner/same-operation
workers, wrong-owner/unknown/ambiguous refusal, run-ID cancellation, helper/
descendant/guardian disappearance, live peer, retained last-good messages/scope,
subsequent success, exact retry and actual reserved-worker SIGKILL after durable
cancel. Counts include clock/poll assertions: independent regular186 on both;
Mac sanitizer420 initially/432 final, Linux sanitizer2406. Final workspace192/
429 passes in /private/tmp/samosa-run-cancel-final-workspace.log.

Independent Agent roots /private/tmp/samosa-agent-cancel-independent.BBgOHI/
{mac,linux}: make test sanitize release exit0, logs
/private/tmp/samosa-agent-cancel-independent-mac.log and
/private/tmp/samosa-agent-cancel-independent-linux-retry.log. Final Mac focused
test-run-cancel sanitize-run-cancel release exit0 in samosa-agent-cancel-final-mac.log;
current docs repackaged by make release exit0 in
samosa-agent-cancel-complete-release-mac.log. Existing Agent811, finding110,
copy972, undo477, move510, reconciliation134 and handoff60 pass regular/sanitized
on both. Linux used native ARM64 Debian12 image3df5cca1b5a7, --network none,
--init. Its process-heavy sanitizer run was polled through the same live handle
until exit0; it was not restarted after an observation timeout. All handles are
terminal and no containers remain. Tracked legacy diff is empty.

Agent extracted Mac/Linux packages AND workspace distributable files compare
identical, set -e/diff -qr exit0. Final archive hashes:
Mac5508c3015f37714ab4fb54883c51029c209e236a216c00d7698a0940a90ab2b0;
Linux96f52e0387a3ca64de14ab19734ed9b6f8891c89ed180ff5979c1abf576f7e18.
Failed Linux runner compile (realpath feature declaration) is preserved in
samosa-agent-cancel-independent-linux.log; _XOPEN_SOURCE700 fixed it before the
successful full retry. One mistaken parent-directory make invocation is preserved
in samosa-agent-cancel-final-command-error.log and was rerun in its owner copy.
Other intermediate patch/copy mismatches were corrected before qualification.

Next exact action S14b: correlate durable Agent parents with Inference child
operation IDs in agent.c/store.c/command.c and the owner-published Inference
operation contract, send operation_cancel while the provider is live, and
qualify actual stopped model work plus unrelated generation survival. S14a proves
owned helper interruption, not long-lived provider control. Multi-run admission,
parent/deadline propagation, Sources/task/scheduling gates, production provider
distribution/recovery/resources, App, assembly/load and C14 retirement remain
unfinished. No full rebuild completion is claimed.

### Previous checkpoint — 2026-10-06 23:11 UTC

Full C01-C14 rebuild is an active goal in this session. S13b trusted provider
catalog work has begun in Inference. Native catalog.c/h validates a version1,
64KiB/16-entry owner-provisioned catalog with revision, provider ID/version,
HTTPS URL, license notice, platform/minimum OS, runtime/recipe and exact size/SHA.
Unknown/duplicate fields, duplicate provider IDs, malformed pins and incomplete
inputs refuse without changing output. Selection requires exact platform and
system-runtime-v1; catalog_artifact maps size/SHA into installer configuration.
No production artifact pins or distribution support were invented.

catalog_file.c loads absolute owner-private single-link regular files with
descriptor/named-inode and nanosecond before/after mutation checks. S13b.md
records scope and acceptance. Makefile includes catalog in test/sanitize.
Workspace make -C components/samosa-inference test-catalog sanitize-catalog
passes721 checks regular and ASan/UBSan on macOS. This is focused local evidence;
independent Linux qualification and release comparison have not yet run for S13b.
One attempted combined patch failed to match Makefile before applying changes;
the corrected patch and focused runs succeeded. No tests failed.

Next exact action: integrate catalog selection and host minimum-OS qualification
into a controlled setup CLI using catalog_file.c, artifact_main.c and the existing
remote acquisition adapter; qualify request isolation and real install/recovery.
Remaining previous gates stay open. No commit, deployment, legacy switch,
delegation or Python. Full rebuild completion is not claimed.

### Previous checkpoint — 2026-10-06 21:13 UTC

Previous goal turn was progress; this continuation also changes implementation
and qualification evidence. Full C01-C14 goal remains active. S13a now has native
byte-range recovery with durable prefix validation and private CLI credentials.
Versioned distribution/setup recovery, broader resource fixtures, other owner
gates, App, assembly and retirement remain unfinished. No permission requests,
delegation, Python, legacy switch, commit or deployment.

remote_artifact.c now stores two alternating144-byte checksummed prefix records
in a fixed288-byte owner-private/single-link checkpoint. Each >=1MiB of accepted
progress, and on an interrupted transfer, fsync(stage) precedes checkpoint fsync.
Recovery chooses the largest valid prefix, hashes it locally in32KiB chunks and
refuses changed bytes/unsafe metadata. A torn new record can use the prior valid
record; no valid record refuses. Stage bytes beyond the checkpoint compare with
the fetched suffix before append. Old stages without metadata replay once;
fully retained stages verify the complete pinned digest without networking.

Range requests start at that verified checkpoint.206 requires exact start/end/
total, optional exact suffix Content-Length, and rejects missing/duplicate/
overflowed/mismatched ranges before body writes.200 fallback compares all retained
bytes. Redirects retain the range and never send bearer credentials outside the
original scheme/host/port. Source identity remains URL hash plus pinned size/SHA;
ETags are not trusted as identity. Final full pinned SHA is mandatory regardless
of the server's claimed object identity; provider activation remains separate.

CLI --bearer-key-file and private_key.h accept only bounded absolute private
single-link owned regular files. Before/after descriptor and named inode checks,
timestamp checks and strict1..128 identifier bytes (optional final LF) precede use.
NUL/CRLF/extra lines/oversize/symlink/FIFO/directory/missing/relative/public/linked
inputs refuse. No key value enters argv, response or source/checkpoint metadata;
CLI clears its key buffer. Actual native server fixture requires Authorization
on the initial request and requires its absence across a different-port redirect.

Independent copies /private/tmp/samosa-range-independent.TBDX96/{mac,linux}:
make test sanitize test-native sanitize-native release both exit0; logs
/private/tmp/samosa-range-independent-{mac,linux}.log. After extra range/tail
boundary fixtures and sanitized CLI target, final focused test-remote-artifact,
sanitize-remote-artifact, sanitize-remote-cli, release both exit0; logs
/private/tmp/samosa-range-final-{mac,linux}.log. Linux: native Debian12 ARM64,
image3df5cca1b5a7, network none. All owned handles terminal; no containers remain.

Final remote167 regular AND ASan/UBSan on each platform; private credential CLI42
regular in the full run AND sanitized in final run; production HTTPS CLI regular
and sanitized certificate refusal/trusted success/offline exact retry pass.
New fixtures prove actual SIGKILL recovery at the production1MiB checkpoint with
32KiB uncheckpointed tail, modified-tail refusal before append then exact repair/
successful resume; changed checkpoint prefix refusal; torn newest record fallback;
private metadata refusal; wrong start/end/total/missing/duplicate/overflowed range;
corrupt suffix refusal; valid no-Content-Length range and full replay fallback.
Tiny death fixture also runs a lowered TEST-only checkpoint interval. No downloaded
model, actual remote-provider activation or power-loss test is inferred.

Final release base suites pass: artifact1024, CLI18, lifecycle97, runtime87,
operations716, FIFO79, fake98, sessions1106; install Mac252/Linux254;
activation Mac92/Linux94; managed stream Mac2738/Linux2922 (poll/branch counts).
Full sanitizer install Mac254/Linux259, activation Mac100/Linux118;
native adapter238, key CLI39, managed CLI25 regular/sanitized pass in full logs.
Workspace logs samosa-range-first.log, samosa-range-checkpoint-first.log,
samosa-range-checkpoint-confirm.log, samosa-range-production-checkpoint.log,
samosa-range-credentials-confirm.log, samosa-range-tail-confirm.log,
samosa-range-boundaries-confirm.log, samosa-range-key-first.log and
samosa-range-cli-sanitizer.log are under /private/tmp; all terminal exit0.
No failed run in this continuation. Previous failed logs remain preserved.

Extracted Mac/Linux packages AND current workspace distributable files compare
identical (set -e/diff -qr, exit0). Archive metadata differs; hashes:
Mac1839757f7f544c89fa922721d6bdd5441c9422407b93946b91f5f429c32abe15;
Linux899d6f90c86d9793b54b4a83ab80d81ee980f788093aecdd8065e94d885b28e4.
Tracked legacy remains unchanged; components and architecture docs untracked.

Next ordered work: versioned trusted provider catalog with source/license/platform/
size/SHA/runtime pins and controlled installer/setup integration; qualify remaining
remote quota/foreign/last-good/resource/recovery fixtures and actual remote provider;
then Agent operation-parent/cancellation/transport, remaining Sources/tasks/
scheduling, App, assembly/load/resources and C14 retirement. Death during initial
checkpoint creation can leave no valid record and requires explicit setup recovery;
automatic repair of that narrow state is not implemented. Metadata/filesystem
overhead and blocked fsync responsiveness remain outside payload quota/time proof.
No App or production default switch exists; complete rebuild is not claimed.

### Previous checkpoint — 2026-10-06 21:02 UTC

Full C01-C14 goal remains active. This continuation implements optional native
remote artifact acquisition; S13a and the full rebuild remain unfinished.
No permissions requested, delegation, Python, legacy switch, commit or deployment.

New remote_artifact.c/h and samosa-inference-download use libcurl HTTPS with
certificate/hostname verification, explicit bounded redirect validation and
original-authority-only bearer credentials in the trusted C adapter. Host flags
select URL/CA/redirect hosts; public requests retain strict artifact operation
schemas. Acquisition writes the existing registered stage under the installer
lease/quota, compares a retained prefix, pins the source URL digest in a private
single-link record, and requires complete size/SHA256 before publication. Fixed
32KiB transfer/compare storage, bounded headers/redirect bodies, connect<=2s and
aggregate transfer<=600s. Verified retry stays offline. Production refuses HTTP;
numeric loopback HTTP exists only in test builds. Installer now refuses a state
root becoming public and unexpected stage/artifact hard links before mutation.

Independent source copies: /private/tmp/samosa-remote-independent.R8fnNd/{mac,linux}.
Initial full test/sanitize/test-native/sanitize-native/release commands both exit0:
/private/tmp/samosa-remote-independent-{mac,linux}.log. Subsequent native/sanitizer
confirmation: /private/tmp/samosa-remote-final-{mac,linux}.log, exit0. Final common
installer changes separately qualified with test-install, sanitize-install,
test-remote-artifact, sanitize-remote-artifact, test-remote-tls, release; both exit0
in /private/tmp/samosa-remote-link-final-{mac,linux}.log. Linux uses native ARM64
Debian12 image3df5cca1b5a7, network none; no containers remain running.

Final checks: install Mac252 regular/254 sanitizer, Linux254/259 (branch/poll
counts); remote58 regular AND sanitizer on both hosts. Controlled fixtures cover
offline reuse, checksum/size/protocol/encoding/redirect/deadline refusal,
interruption, actual downloader SIGKILL, interrupted status and explicit recovery.
Production HTTPS CLI fixtures separately prove untrusted-certificate refusal,
configured trust success and offline exact response retry on both platforms.
Final release reruns existing suites: artifacts1024, artifact CLI18, lifecycle97,
runtime87, operations716, FIFO79, fake98, sessions1106; activation Mac92/Linux94;
managed stream Mac2738/Linux2922 (poll counts). Full earlier native adapters238,
key CLI39 and managed CLI25 also passed. No broad resource gate inferred.

Release extracted trees compare identical (diff -qr exit0), including source,
tests/docs/contracts; archive hashes differ due host tar metadata:
Mac f94646450e0a3134b3952fce0e99e4e460f12aeae26b890ec507cad8d7b67db7;
Linux e2854e8a8d91384a5823b583410d6ac4de3663661982591916856ce5c4946986.
Failed runs preserved: samosa-remote-build.log (Darwin feature macro),
samosa-remote-tls-sanitize.log (LibreSSL lacks -ign_eof),
samosa-remote-death-tests.log (missing sys/stat include),
samosa-remote-root-permissions.log (missing errno include), all under /private/tmp.
Fixes reran strict compilers and affected fixtures; warnings were not disabled.

Next ordered work: implement and qualify byte-range/checkpoint resume (current
HTTP200 replay verifies the whole retained prefix, with unsolicited206 refusal);
private CLI credential files and broader remote failure/resource fixtures;
versioned engine/model catalog and setup/recovery; then remaining Agent/Source/
App/assembly/resource/retirement gates from the preceding checkpoints. Current
resume can exceed its deadline on slow large downloads and is not bandwidth
saving. No App/default runtime switch exists. Actual managed-provider evidence
from20:37 remains valid; this continuation did not run an actual remote model.

### Previous checkpoint — 2026-10-06 20:37 UTC

Previous turn was **progress**; this turn also changed implementation and yielded
new actual-provider evidence. Full C01-C14 goal remains **active**. S13a concurrent
managed foreground ingress/cancellation is now implemented and qualified below.
Remote acquisition/versioned distribution/install recovery and the other owner/
App/assembly/resource/retirement gates remain open. No permissions/delegation/
Python/legacy switch/commit/deployment.

New `src/managed_stream.c/h` replaces the serial loop with correlated private
host envelopes, six work workers plus two reserved describe/operation_get/
operation_cancel workers. Frame contract_version/request_id/request wraps the
unchanged public body; responses correlate by request_id and may complete out
of order. IDs<=64 identifier bytes are unique while outstanding. Invalid frames/
duplicate IDs produce null-ID transport errors; capacity returns stream_busy
for the rejected ID. One bounded emergency response pauses input until delivery.
One main writer serializes complete response lines; workers never share stdout.

Worker requests/ingress<=1MiB, replies<=512KiB through bounded fmemopen, explicit
4MiB stacks, maximumeight worker threads. Atomics publish completed buffers and
chain EOF/signal cancellation through the existing operation callback. Signal
atomics are compile-time required lock-free.20ms nonblocking I/O/collection
observes Darwin FIFO EOF and output closure;2s output stalls cancel work.5s
shutdown deadline: a noncooperative worker causes immediate owner _exit without
freeing live worker references, closes the guardian lifeline and removes all
threads. Durable outcomes after lost delivery require explicit reconciliation,
never silent provider replay. POSIX scheduling/kernel responsiveness limits
remain. Model admission is still one active/four queued, independent of ingress.
The owned engine's authenticated read-only slots endpoint is enabled for
Inference diagnosis/actual QA; no caller chooses engine arguments.

`tests/test_managed_stream.c` proves six simultaneously held workers while a
reserved control request completes; seventh work request refusal, duplicate ID
refusal without replacing existing work, correlated cancellation and all six
terminal responses, oversized response refusal, EOF cancellation, output stall
and forced exit for an eight-second noncooperative dispatch. Fixture builds use
JSON_TOKENS_MAX8192 and a2MiB nested stack load, matching the large native parser
case rather than only the small default parser. Counts include per-byte pipe
assertions and an active-worker observation loop; scheduling changes the count,
not the required cases. contracts/managed-v1.md and README/DEPENDENCIES updated.

Actual macOS public foreground workflow now sends correlated requests, warms
count/generation, observes owned engine is_processing=true, sends cancellation
over the **same foreground input**, receives its independent control response and
cancelled generation response within6s, then a new operation generates4 using
the same surviving owned engine. After FIFO EOF, the owner exits within10s,
exact ordinary-native-CLI offline retry matches the unwrapped saved response,
and no credential file remains. The Inference-owned QA script briefly retains a
private header for its own slot observation, removes it before EOF and on failure,
and never prints credentials; App/Agent do not inspect private state.

Regular actual PASS `/private/tmp/samosa-managed-stream-stack-actual.log`,
session35688 exit0, QA `/tmp/samosa-managed-native.pqzpbV`.
Independent ASan/UBSan actual PASS
`/private/tmp/samosa-managed-stream-independent-actual-sanitizer.log`,
session74090 exit0, QA `/tmp/samosa-managed-native.Jgk1uR`.
Pinned engine/model remain the previously qualified static-system macOS artifacts;
personal engine/model files are read only. Actual Linux engine, multi-owner
distributed ingress, Agent parent-run propagation and full product load/resource
qualification are not established by these checks.

Independent roots `/private/tmp/samosa-managed-stream-qualified.iUDIIS/{mac,linux}`.
Full regular/sanitizer/native/release/actual-driver build Mac31800 exit0,
`/private/tmp/samosa-managed-stream-qualified-mac.log`; Linux60854 exit0,
`/private/tmp/samosa-managed-stream-qualified-linux-confirm.log`, native ARM64
Debian12 image3df5cca1b5a7 with --rm --init --network none.
Stream Mac2922 regular/2922 sanitized; Linux2738/2922. Other counts:
artifacts1024; install Mac236/238 Linux238/242; activation Mac92/100 Linux94/118;
lifecycle97; loader87; operations716; FIFO79; fake98; sessions1106; HTTP238;
managed private-config CLI25 regular/sanitized; key CLI39. Latest cleanup/manifest
refresh native/sanitizer/release passed Mac41363/Linux69677, logs
`/private/tmp/samosa-managed-stream-final-{mac,linux}.log`.
Final recipe/spec package refresh passed78086/85796, logs
`/private/tmp/samosa-managed-stream-package-{mac,linux}.log`; stream counts2738/2922.
Extracted unpacked-{mac,linux} trees under the independent root match via diff-qr.
Compressed package SHA256:
Mac3fbf5d2ed75263bd4be90db048b0cf98382f4d9e5650a30cfbba5689a11c434e;
Linuxa18b9880359d86be57ba84d71c06449a2a60aeeca61019fbf6fa6dbf731016e7.

Retained failures: first actual frontend sessions7098/40166 failed SIGBUS, logs
`/private/tmp/samosa-managed-stream-actual.log` and
`/private/tmp/samosa-managed-stream-active-actual.log`; native nested JSON parsing
exceeded macOS default544KiB pthread stacks. Diagnostic report
`~/Library/Logs/DiagnosticReports/samosa-inference-managed-2026-10-06-162613.ips`
explicitly identifies thread stack size exceeded/stack guard. Fixed with explicit
4MiB stacks, expanded native-size/nested-stack fixtures, and successful real reruns.
The earlier small-parser fixture passing did not qualify the native stack case.
Initial Linux full run `/private/tmp/samosa-managed-stream-qualified-linux.log`
failed strict GCC misleading-indentation in the fixture's for/fputs layout; fixed
statement layout, no warning suppression. Subsequent full/native/package checks
above passed. No failed log removed. No active QA containers, engines/listeners or
unfinished sessions remain. Workspace still untracked components/ and three
architecture documents; legacy tracked code untouched.

Exact next S13a action: implement HTTPS streaming acquisition and explicit
interruption/resume with existing publication/integrity/quotas. Pre-implementation
requirements are now in the owner `docs/S13a-remote.md`; no remote implementation
is claimed yet. Then versioned distribution/install recovery, Agent parent
operation/cancellation and App, remaining Source/task/scheduling policies,
assembled compatibility/resource/load qualification and C14 retirement.

### Current state — 2026-10-06 20:19 UTC

This turn made **concrete progress**: added loader/dependency policy, a managed
text-provider adapter and foreground public executable, qualified actual owned
readiness and active-model cancellation, and completed fresh independent package
checks. Full C01-C14 goal remains **active**. S13a remains **incomplete**: bounded
concurrent managed ingress, remote acquisition/versioned distribution, full
installation/recovery and the remaining owner/App/assembly/retirement gates are
open. No permission request or delegation; no Python or legacy switch.

New `src/engine_runtime.c/h` and `contracts/engine-system-runtime-v1.md` implement
bounded direct-loader inspection of ARM64 little-endian Mach-O/ELF. Tables and
positional reads are file bounded; external/rpath backend libraries, loader
audit/filter/path overrides and DYLD_/LD_/GGML_/LLAMA_ environment overrides
refuse. OS runtime is explicitly trusted outside the engine digest. Generic
inspection does not prove arbitrary executable code never dlopens/opens other
files; the profile additionally asserts trusted static-engine build provenance.
Fixture87 checks pass regular/ASan/UBSan on both platforms (including alternate
format fixtures, malformed offsets/strings/limits and override refusal).
Activation's optional require_system_runtime enforces this policy before spawn
and commits the selected runtime policy in last-good configuration.

Built a new engine outside personal backend state at
`/private/tmp/samosa-s13a-static-engine-20261006`, source HEAD
9fcaed763ccda38ea81068ad9d7f991aaddca451. BUILD_SHARED_LIBS=OFF,
LLAMA_OPENSSL=OFF, GGML_BACKEND_DL=OFF, GGML_METAL_EMBED_LIBRARY=ON confirmed in
the build cache. Direct libraries now comprise only system frameworks/libSystem/
libc++/libobjc; the original external-dylib wrapper correctly refuses inspection.
Pinned executable20513984 bytes SHA256
1634265c6be962401fb2d2419b371b5eedef20957bc603fb3bbde62ed8389ccf.
Actual artifact requires macOS>=26.0 ARM64; tested26.5.1(25F80), SDK26.5.
Commit label alone does not identify private-fork code; binary digest does.
Recipe/provenance/platform limits: `docs/static-engine-package.md` in the owner.
Configure/build logs `/private/tmp/samosa-s13a-static-{configure,build}.log`,
sessions77827/70157 exit0. No personal source/model/engine edits or model copies.

`src/managed_provider.c/h` binds provider identity to profile/platform/runtime/
tokenizer policy, engine/model sizes and digests, context and alias. Startup
requires owned authenticated alias/context, wrong-key refusal and valid native
template/tokenization; bounded120s startup with1s HTTP probes. Managed observations
check guardian terminal channel, held/named artifact mutation metadata and
state/lease anchors. text-v1 provider_status reports ready=true and
owned_system_runtime_v1 only through this qualified adapter. Artifact replacement/
mutation withholds readiness. External native endpoints retain ready=false.
These are checkpoint observations, not protection against a hostile local OS.

New `samosa-inference-managed --config ABSOLUTE_PRIVATE_JSON` is built by optional
`make native`; default model-free builds still have no curl/model requirement.
Private bounded host schema, fixed trusted argv, fd5 model, loopback owned engine,
anchored exclusive random credential file, checked inode cleanup, private memory
key after qualification. Persistent private stdin/stdout serves public newline
requests in order; startup emits provider_started. Signal cancellation now chains
through tracked operation cancellation instead of being overwritten. Operations
suite716 (previous693) proves durable parent cancellation, exact cancelled retry
and subsequent successful operation. CLI25 regular/sanitized checks cover private
config, schema/duplicate/raw-NUL refusal, loader/environment failure and credential
cleanup; key CLI39 and native HTTP238 remain passing. Full config length is parsed,
never a strlen-truncated prefix. Contract `contracts/managed-v1.md` declares scope.
Current stream is serial: second in-stream cancellation during generation is
unfinished. Separate configured native CLI owner-scoped cancellation works.

Actual model remains the read-only5629108704-byte GGUF pinned SHA256
5720d1f671b4996481274fffe01868c3c36e87c135cc8538471cc7bd6087b106.
Actual macOS evidence:
- Static policy activation PASS700 `/private/tmp/samosa-s13a-static-real.log`,
  session52822 exit0, including public count/gen usage equality and offline retry.
- Managed ready status plus private engine mutation refusal PASS687
  `/private/tmp/samosa-s13a-managed-real.log`, session36699 exit0.
- Actual active cancellation PASS825
  `/private/tmp/samosa-s13a-managed-cancel-warm.log`, session71083 exit0.
  The driver enables slots for QA, observes is_processing=true for its only
  generating worker before operation_cancel, requires worker cancelled within5s,
  verifies durable cancelled status, then a new operation generates4 on the same
  surviving engine. This is actual-model evidence, not only controlled HTTP.
- Public managed CLI FIFO EOF qualification PASS
  `/private/tmp/samosa-s13a-managed-public-fifo.log`, session40048 exit0,
  retained QA `/tmp/samosa-managed-native.eAqXG2`: startup/ready/count/gen,
  EOF stop within10s, exact offline native-CLI retry and no retained key file.
- Independent ASan/UBSan managed CLI actual workflow PASS
  `/private/tmp/samosa-s13a-managed-independent-actual-sanitizer.log`,
  session52532 exit0, retained QA `/tmp/samosa-managed-native.r4V9AU`.
  Actual Linux model, Agent parent propagation and complete product resource/load
  qualification remain unproven; do not broaden these claims.

Fresh independent source roots:
`/private/tmp/samosa-s13a-managed-qualified.c8JP7o/{mac,linux}`.
`make test sanitize test-native sanitize-native release build/activation-native`
passed sessions94111/2488 exit0, logs
`/private/tmp/samosa-s13a-managed-qualified-{mac,linux}.log`. Native Debian12 ARM64
container used image3df5cca1b5a7, --rm --init --network none. Counts regular/san:
artifacts1024; install Mac236/238 Linux238/242; activation Mac92/100 Linux94/118;
lifecycle97; runtime87; operations716; FIFO79; fake98; sessions1106; HTTP238;
managed CLI25/25; credential CLI39. Executable-size-dependent streamed assertions
account for installation/activation count differences. Recipe-only package refresh
sessions60257/36450 passed, `/private/tmp/samosa-s13a-managed-package-{mac,linux}.log`.
Extracted packages under this root's unpacked-{mac,linux} are identical via diff-qr.
Compressed SHA256 Mac526ff6c71cdffbc4928fee3d9ba2d7cc956f7850731363e5abc094a42f0862d2;
Linuxba3f7c52c4fa5bad5e42a0fe31ae3f03d851f787f7327b45add202cb53dc0456.

Retained failures/limits:
- `/private/tmp/samosa-s13a-managed-anchored-build.log` missing fcntl declaration
  for AT_SYMLINK_NOFOLLOW; fixed include, confirm log passed.
- `/private/tmp/samosa-s13a-managed-independent-linux.log` GCC rejected misleading
  indentation in actual QA driver; fixed statement layout without weakening flags.
- Initial public QA at `/private/tmp/samosa-s13a-managed-public-20261006` got ready/
  count/gen but macOS FIFO closure did not wake poll. Owning PID10551 was explicitly
  sent SIGTERM; that initial script's printed EOF PASS is **not EOF evidence**.
  Nonblocking input observation after each200ms poll fixed it; separate FIFO and
  independent sanitizer reruns above passed without intervention.
- Cold actual cancellation attempts
  `/private/tmp/samosa-s13a-managed-cancel-{real,diag}.log` failed because a1s slots
  probe timed out during first GPU work (diagnostic curl28/http0). Subsequent test
  warms via canonical generation, permits transient probe timeout within15s,
  and still requires positive active-slot observation before cancelling. PASS825.
- `/private/tmp/samosa-s13a-managed-refresh-linux.log` reused an older binary after
  tar-restored source mtimes; the new raw-NUL config fixture caught it. Fresh roots
  and clean runs supersede this incremental evidence. No failed run is erased.

All QA sessions above are terminal, no active QA containers or owned engines/
listeners remain. Workspace still has untracked components/ and three architecture
documents, no commit/deployment/default production switch/legacy retirement.

Exact next S13a action: implement bounded concurrent managed public ingress with
independent operation/cancellation responses and front-door cancellation/shutdown
qualification; then remote acquisition and versioned installation/recovery.
Continue Agent parent operation/cancellation and App, remaining Source/task/
scheduling policies, assembled version/resource/load gates, then C14 retirement.

### Current state — 2026-10-06 19:53 UTC

Full C01-C14 goal remains **active**. S13a gained verified executable publication,
authenticated native HTTP, and actual owned macOS model qualification. S13a is
still **incomplete**: production managed-provider wiring, dependency closure,
remote acquisition and actual model cancellation are open. App/assembly/retirement
and the other remaining owner gates are not complete.

`artifact_make_executable` verifies the registered published inode and full digest,
requires a native ARM64 executable header (Mach-O MH_EXECUTE or ELF executable/PIE
with nonzero entry), then changes only that owner entry to0500 and fsyncs it.
Source executable permissions stay unchanged. Status/offline retry read executable
entries without reopening them for writing; incomplete installation explicitly
reopens and rechecks its original staged inode. Scripts/text/libraries refuse.

Native HTTP now supports a trusted host API key, validates its alphabet/length,
uses bounded configurable transfer timeouts<=60s, and distinguishes401/403 as
provider_auth_failed. CLI `--api-key-file` accepts an absolute private owner regular
file only; symlinks/FIFOs/directories, relative/missing paths, unsafe permissions,
empty/oversized/CRLF/multiline/embedded-NUL bytes refuse. An optional trailing LF
is accepted. Found and fixed the embedded-NUL acceptance bug caused by strchr's
terminator match. Public JSON never supplies the key. New CLI suite39 checks,
including no secret output; controlled HTTP238 includes good/wrong/injected keys.

Actual QA driver `tests/activation_native.c` owns a new private engine entry,
holds the verified read-only model as fd5, uses a random private engine key,
qualifies authenticated alias/context and wrong-key rejection, compares public
count_tokens against generation input usage, checks2+2=4, stops its own guardian,
and proves exact tracked retry after the endpoint is offline. Durable last-good
read remains live_readiness=unobserved. Engine entry SHA256:
dd5266536619abecaaf3a2676f0ac68070abcf57aeecf7f37a194979e0791830.
Model5629108704 bytes SHA256:
5720d1f671b4996481274fffe01868c3c36e87c135cc8538471cc7bd6087b106.
Personal engine/model were read only. Actual final macOS run PASS212:
`/private/tmp/samosa-s13a-owned-count.log`, QA root
`/private/tmp/samosa-s13a-owned-count-20261006`, session10567 exit0.
Earlier actual PASS86081 `/private/tmp/samosa-s13a-owned-real-absolute.log`
includes per-block fingerprint assertions; final212 uses a supplied expected
model digest but activation still performs full streamed verification.

The small engine entry loads external backend dylibs plus Homebrew OpenSSL;
entry/model hashes do **not** qualify that dependency closure. Current derived
provider identity in the QA driver excludes those dylibs. Do not claim full
package identity, production ready=true, actual cancellation or Linux real-model
qualification. Remaining package/version work must pin and qualify non-system
runtime dependencies and loading paths before declaring managed readiness.

Independent owner copies:
`/private/tmp/samosa-s13a-exec-auth-independent.5jgted/{mac,linux}`.
Full regular/sanitizer/native/release/activation-native builds passed macOS and
native network-disabled Debian12 ARM64 (image3df5cca1b5a7), sessions1056/16710,
logs `/private/tmp/samosa-s13a-exec-auth-final-{mac,linux}.log`.
Counts regular/sanitized: artifacts1024/1024; install Mac236/238 Linux238/242;
activation Mac92/98 Linux94/114; lifecycle97; operations693; FIFO79; fake98;
sessions1106; nativeHTTP238. Install/activation count differences include
executable-size-dependent streamed fixture assertions, not skipped cases.
Latest main/Makefile/key CLI change independently refreshed native tests and
release on both hosts, sessions87732/64152 exit0, logs
`/private/tmp/samosa-s13a-key-independent-{mac,linux}.log`; CLI39 and HTTP238 pass.
Latest packages extracted in `/private/tmp/samosa-s13a-key-unpacked-{mac,linux}`
have identical content via diff-qr (compressed archive hashes may differ).

Retained failed runs: relative argv0 actual launch refused before engine creation
(`/private/tmp/samosa-s13a-owned-real.log`); fixed with an absolute invocation.
CLI build initially failed missing ctype and Darwin feature declarations
(`/private/tmp/samosa-s13a-key-cli-build.log` and `...-confirm.log`); declarations
fixed without weakening warnings. No active QA containers or owned engines remain.
Workspace still has untracked components/ and three architecture documents;
legacy tracked code has not been switched or retired. No commit/deployment.

Ordered next actions: finish S13a dependency-closure/version specification and
managed adapter/foreground lifecycle wiring, then remote acquisition and actual
engine cancellation; continue Agent parent operation/cancellation and App,
remaining Source/task/scheduling policies, assembled installation/resource/load
qualification and finally C14 legacy retirement. No gate is waived.

### Current state — 2026-10-06 19:19 UTC

This turn implemented/qualified the preparatory activation controller and closed
the parent-death residency lease gap. Full C01-C14 goal remains **active**. S13a
is **not complete**: production managed adapter/CLI, qualified actual engine
launch, provider version/readiness wiring and remote acquisition remain open.

New `src/activation.c/h`, `tests/test_activation.c`, `contracts/activation-v1.md`.
Trusted owner config pins private engine/model paths, exact lengths/SHA256, limits,
argv template, expected revision and adapter qualification function. Exactly one
@ENGINE@ in argv0 and one @MODEL@. Engine bytes are held and verified with launch
checkpoint path/mtime/ctime checks; this is not atomic execute-from-fd on macOS.
Model is passed read-only through fd5 and argv /dev/fd/5; original logical path is
retained in committed configuration for explicit recovery. Both hosts' native
fixtures open this inherited descriptor successfully. No actual model was loaded.

One shared host state residency flock protects candidate activation. The process
guardian now retains its duplicate as fd4 until cleanup, including after owning
parent SIGKILL; this avoids releasing the slot merely because the parent died.
New leased/bound lifecycle modes are dispatched by the native Inference main;
model fd5 reaches the engine, while fd4 remains guardian-owned/CLOEXEC for engine.
The controller does not silently overlap engines: active start receives busy,
and explicit stop precedes a new candidate. Pending stop keeps ownership handles.
Independent lifecycle97 checks (previous89) prove lease retained after caller
closes its own copy, across paused guardian shutdown, and through parent death;
owned engine/descendant leases die while the unrelated fixture survives.

Last-good configuration is one owner-private SQLite row, revision checked and
content SHA checked, <=16KiB config/1MiB database/4096-byte pages, zero busy wait,
FULL sync. Nondefault page size refuses before schema creation; private root/DB/
journal ownership, DB single-link/size, lease/path and SQLite HAS_MOVED checks
guard admission/commit. Candidate input mutation/readiness failure/conflict stops
only its own engine and preserves old row. Native fresh-process get proves restart
read without model work, reporting live_readiness=unobserved. Commit/fsync errors
are distinct activation_commit_uncertain and retain live ownership: do not infer
rollback. Actual injected invalid directory fd after successful SQLite COMMIT
tests this uncertain result, saved revision3, retained process and explicit stop.
No request automatically launches the last-good row. No public ready=true result
or production activation command has been added. A trusted qualification callback
is preparatory; production must supply a bounded adapter policy and prove its
endpoint belongs to the owned instance rather than an unrelated compatible server.

Independent copies `/private/tmp/samosa-s13a-activation-independent.6aTN5W/{mac,linux}`.
Full `make test sanitize test-native sanitize-native release`: Mac70262/Linux41004
exit0, `/private/tmp/samosa-s13a-activation-full-{mac,linux}.log`, regular/ASan/UBSan
and native Linux LeakSan. Includes lifecycle97/artifact1024/install216/CLI18/
operation693/FIFO79/fake98/session1106/HTTP219; activation Mac73 regular79 sanitizer,
Linux75 regular95 sanitizer. Later uncertain-commit and page-size hardening passed
focused activation regular/sanitizer plus all release regular suites,
Mac29027/Linux55009 exit0, `/private/tmp/samosa-s13a-activation-limit-{mac,linux}.log`:
activation Mac92/98 and Linux94/114 respectively. Activation counts differ because
the fixture asserts each block while copying its differently sized native regular/
sanitized executable; functional cases are the same. Final engine lifecycle code
was already covered by the full qualification. No failing run in this turn was
reported as a pass. All warning/error/sanitizer flags remain enabled.

Both latest version0.1.0 release payloads include activation and lifecycle contracts,
source and tests. Extracted Mac/Linux trees compare identically. Tar SHA256 Mac
`8ce5c9bc0fc7ddad36b38babdcc4ef72919ebae61005c40359b8d5034e07a839`, Linux
`fffac7cdd4580892d858afcd5e0237a1491d94806682df3f528c2c5e35643292`.
Native ARM64 image3df5cca1b5a7 runs were network-disabled/--init/auto-removing.
Final ps/docker inspections found no activation/lifecycle workers or QA containers.
Small /tmp/samosa-activation-* QA copies/SQLite files remain, alongside previously
recorded install fixtures. Personal model/engine files were not touched.
Workspace remains uncommitted, existing tracked legacy code unchanged.

Next: wire the controller into a managed native provider with private configured
artifact IDs, trusted adapter argv, bounded owned-endpoint qualification, and public
instance/version descriptors binding engine/model digests. Preserve unqualified
external mode. Prove actual owned native load/count/generate from fd5, failed new
candidate with durable last-good recovery, replacement/death races and limits.
Engine executable import needs an explicit safe executable publication policy;
do not silently chmod personal engines or assume imported0600 is executable.
Then finish remote artifact acquisition and S13a remaining gates, followed by the
original Agent/App, source/task/schedule, assembly/quality/resource/retirement work.
No Python, delegation, commits, deployment or legacy retirement occurred.

### Current state — 2026-10-06 19:00 UTC

This turn implemented and independently qualified the Inference owned-process
lifecycle primitive. The full C01-C14 goal remains **active**; S13a activation,
verified engine/model relationship and artifact-qualified readiness remain open.

Added `src/engine_process.c/h`, native `tests/test_engine_process.c` and
`contracts/engine-lifecycle-v1.md`; default/native Inference main dispatches the
private guardian mode. Build/test/sanitizer/release targets include the new code.
No sibling private code/state, Python, delegates, real engine or personal model
artifact changes. Trusted host-configured argv only: absolute executable/self,
at most32 args/16KiB encoded/4KiB per arg. No public model request starts a process.
The native self guardian inherits a private lifeline, creates a separate engine
process group, polls leader exit without reaping and kills that group before
reaping the reserved leader. Parent death/explicit stop, leader failure/exit,
configured<=24h lifetime and <=1MiB processed log limit trigger cleanup. Logs
are drained/discarded in4KiB reads; no retained unbounded log file. Closed log
streams are removed from polling to avoid HUP busy loops.

Start proves exec only, with2s handshake. Stop observes2s terminal response plus
2s reap; a paused/unresponsive guardian returns engine_shutdown_pending while
retaining handles for later observation. Terminal reasons persist through repeated
stop, and an observed response persists if reaping is still pending. Tests exercise
SIGSTOP/CONT and actual later cleanup. Blocking host syscall responsiveness remains
a limitation. Direct guardian SIGKILL cleanup is not qualified. This primitive
does not enforce a host-wide engine residency lease or persist activation truth;
those belong to the next integration. All public provider_status still returns
ready=false/unqualified_external_engine. Do not infer C03/C04/C11 complete.

Independent copies `/private/tmp/samosa-s13a-engine-independent.xFuWAL/{mac,linux}`
passed `make test sanitize test-native sanitize-native release`:
Mac47635/Linux39951 both exit0, regular/ASan/UBSan and native Linux LeakSan.
Logs `/private/tmp/samosa-s13a-engine-final-{mac,linux}.log`:
lifecycle89/artifact1024/install216/CLI18/operation693/FIFO79/fake98/session1106/
HTTP219. Lifecycle checks prove owned stop/idempotent stop, actual parent SIGKILL,
actual engine SIGKILL, descendant kernel lease release, unrelated fixture survival,
exec failure, engine nonzero exit, lifetime/log refusal, closed logs and pending
shutdown recovery. Native Debian12 ARM64 image3df5cca1b5a7 used --init,
network disabled and auto-remove. Final process inspection finds no lifecycle
worker/guardian or QA container. Small install fixture data described below remains.
Version0.1.0 packages include engine-lifecycle contract; extracted Mac/Linux trees
compare identically. Mac tar SHA256
`6cf24504a270bacc64b160267e194720fc8279af4068a5c2db92347babdbe608`;
Linux `a95c09779af6a6f0dc409a6b41d18ea6d147df8163e9413843be67fe09cbcd7b`.

Failures retained: first Mac start failed because same-fd dup2 retained CLOEXEC
on the lifeline; corrected by moving fd3 before spawn actions, without changing
ownership policy (`samosa-s13a-engine-process-mac-initial.log`). Initial GCC build
rejected misleading indentation in fixture loops; braces fixed with strict errors
kept (`samosa-s13a-engine-linux-initial.log`). Initial Linux sanitizer hit a null
strcmp in a test that assumed a failed fixture terminated after a fixed300ms;
that could stop the fixture before its terminal result. Tests now observe the
specific guardian terminal channel and safely check error pointers. Kept
`samosa-s13a-engine-linux-confirm.log` and `samosa-s13a-engine-full-linux.log`;
neither failing run is a pass. Final independent qualification above supersedes
them, with no disabled compiler warnings or sanitizers.

Next: implement S13a activation integration using this process owner, a host-wide
residency lease and durable last-good configuration. Bind verified imported engine
and model inputs to launch, qualify replacement races on both hosts, and preserve
last-good configuration when a candidate fails exec/metadata/readiness. Native Mac
/dev/fd executable spawn failed in the earlier probe; use a separately qualified
launch method, not an assumed descriptor executable path. Qualify an actual owned
provider only after this binding works. Remote acquisition remains open. Then
continue the original Agent/App, source/task/schedule, assembly/quality/resource
and retirement gates. No commits, deployment or legacy removal occurred.

### Current state — 2026-10-06 18:51 UTC

This goal turn made implementation and qualification progress. The full C01-C14
goal stays **active**. S13a installation subset below stands; activation/owned
engine identity and artifact-qualified ready status remain unfinished.

Implemented generic `provider_status` in Inference's public text contract and
descriptor. Requests pin provider_id and reject configuration/path/prompt fields.
Native checks share FIFO admission and call public `/props` only. Extracted
`llama_properties` is also used by normal prompt preparation, avoiding another
alias/context policy. Compatible metadata reports metadata_compatible,
ready=false, artifact_identity=unqualified_external_engine. Fake status remains
fake_contract_only/ready=false/not_applicable. No engine process is adopted or
activated; no session work/tokenization/generation occurs during status. Errors
remain typed: provider_changed for wrong/missing alias/context,
invalid_provider_response for malformed props, bounded HTTP errors and busy.
This does not prove an external engine uses imported model bytes.

Files: `src/inference.c`, `src/llama.c/h`, `tests/test_inference.c`,
`tests/test_llama.c`, public text contract, README and S13a design continuation.
Current fake checks98/native HTTP219 (previous87/146); new checks include
compatible but unqualified status, fake refusal to claim ready, wrong provider,
admission busy and actual controlled changed-context/missing/malformed props.
All existing exact-token, session and scoped-cancellation cases remain.

Initial readiness native test failed due to reuse of a fixture mode's stale port
file, not a provider readiness pass. Retained
`/private/tmp/samosa-s13a-readiness-mac.log`, session30874 exit2. Fixture now
unlinks its own previous port before spawning that mode. Confirmed native and
sanitizer219 pass in `/private/tmp/samosa-s13a-readiness-mac-confirm.log`,
session35999 exit0. Failure left owned fixture PID99089 with exact
`/tmp/samosa-native-llama-44BrHb/http-error` argv; inspected and terminated that
fixture alone. Final process inspection finds no test-llama server or QA container.

Independent copies `/private/tmp/samosa-s13a-readiness-independent.32sVFt/{mac,linux}`
passed `make test sanitize test-native sanitize-native release`, Mac13875 and
native Debian12 ARM64 Linux96566, both exit0. Logs
`/private/tmp/samosa-s13a-readiness-independent-{mac,linux}.log`:
artifact1024/install216/CLI18/operations693/FIFO79/fake98/session1106/HTTP219,
regular and ASan/UBSan with Linux LeakSan. Network-disabled auto-removing --init
container image3df5cca1b5a7. Extracted release trees compare identically and
package public artifact/text/admission/operations contracts. Version0.1.0.
Tar SHA256 Mac `df9d2781468ccca5d7fc5cec0715d79901e81696a576037016c88bdde6c71b85`,
Linux `22a4e8795f86d261cec632e9b9b34349c9d2d0dfd70f1f0e0868f2a683f9e034`.

Engine launch feasibility probe: locally compiled C `/private/tmp/samosa-engine-fd-probe.c`
opened `/usr/bin/true` read-only and attempted posix_spawn on `/dev/fd/<fd>`.
macOS returned13/EACCES; no engine was launched. Do not assume descriptor-based
executable spawn works on macOS. Engine implementation must qualify the verified
private executable/model launch relationship on each host and state checkpoint
limits explicitly. This is a design constraint, not an external blocker.

Next required action: implement the S13a foreground owned-engine supervisor and
durable activation policy described in its specification; test bounded lifecycle,
process-group ownership/parent death, replaced inputs, failed activation with
last-good configuration recovery and actual verified native provider launch.
Remote acquisition remains required subsequent S13a work. Continue the original
Agent/App, reader/task/schedule, assembly/quality/resource and retirement gates
after qualification. No Python/delegation/commits/deployment/legacy removal.

### Current state — 2026-10-06 18:46 UTC

Previous goal turn made implementation/qualification progress. Goal remains
**active for all C01-C14**. S12b descriptor/package closure below stands. S13a
now includes a bounded native local artifact installer; **S13a is not complete**.
Engine identity/activation/readiness and remote acquisition are still open.

Added Inference-owned `src/install.c`, `src/artifact_main.c`, extended
`src/artifact.c/h`, `tests/test_install.c`, `tests/artifacts_cli.sh` and
`contracts/artifacts-v1.md`. Default build now includes the separate libc-only
`samosa-inference-artifacts` manager. Model-free use has no curl/model requirement.
Public requests contain only version/op; host argv pins absolute acquisition/state
paths, SHA256, exact size and byte/disk ceilings. Implemented artifact_status,
artifact_install and explicit artifact_resume. No Agent/Sources/App private state
or personal model artifacts were changed. No Python or delegation.

One private flock installation lease; at most16 digest identities; conservative
aggregate disk reservation twice declared size per identity; no eviction.
Identity records persist exact size/stage device/inode. Partial prefixes must
match the configured current source before resume. Copy uses one fixed64KiB
allocation (two32KiB halves), freed before full64KiB-buffer checksum verification.
Copy and checksum phases each have600-second monotonic deadlines; host storage
syscalls themselves can still block on the host filesystem. Source/state files
open nonblocking/no final symlink and refuse nonregular inputs. Private state
requires effective owner and no group/other mode bits. Metadata records bound128
bytes. State/root/entry/stage scopes and source identity/mtime/ctime are checked.
Source mutation even with restored mtime refuses. Publication links without
clobber, fsyncs and revalidates scope. Unknown/foreign files remain preserved.
Ambiguous incomplete identity creation refuses instead of guessing recovery.
Explicit retries after link/worker death verify the same stage inode/checksum;
final hardening also fsyncs the entry before an offline successful retry returns.

Descriptors distinguish absent/installing/interrupted/verified/invalid and busy
for a different/unknown active target. Every successful result says
activation=not_requested/readiness=unqualified. These checks prove imported
bytes, not checksum-qualified identity of an externally running engine, immutable
future bytes, activation, process ownership or actual provider readiness.

Independent copies `/private/tmp/samosa-s13a-independent.WbHQuJ/{mac,linux}`.
`make test sanitize test-native sanitize-native release` passed on macOS ARM64
session74065 exit0 and native Debian12 ARM64 session83791 exit0 (network disabled,
auto-removing --init image3df5cca1b5a7): artifact1024/install204/CLI18, existing
operation693/FIFO79/fake87/session1106/HTTP146, regular/ASan/UBSan and Linux LeakSan.
Logs `/private/tmp/samosa-s13a-full-{mac,linux}.log`. Later metadata/root/source
hardening passed focused installer216 regular/sanitizer plus CLI18 and all release
regular suites, Mac35273/Linux35609 exit0;
`/private/tmp/samosa-s13a-final-{mac,linux}.log`. Native HTTP implementation was
unchanged by installer hardening. Extracted Mac/Linux final packages compared
identically and include artifact source/tests/task/public contract. Final offline
retry fsync follow-up also passed installer216 regular/sanitizer and release
regular suites on both platforms, sessions76945/79453 exit0,
`/private/tmp/samosa-s13a-durable-{mac,linux}.log`. No QA container remains.
Current workspace changes remain uncommitted.

Tests cover standard SHA vectors including million-a, offline installed retry,
cancel/actual SIGKILL during copy, explicit resume/refusal, prefix conflicts,
wrong digest, truncation, active lease refusal, foreign publication, replaced Root,
source mutation/restored mtime, SIGKILL immediately after link, symlink/FIFO,
unsafe permissions, aggregate disk quota and actual17th identity refusal.
Initial Mac fixture incorrectly renamed its Root on every callback; test failed
before correction to a single replacement (`samosa-s13a-install-initial.log`).
Initial independent GCC build failed misleading-indentation warnings; corrected
formatting with all warnings still errors (`samosa-s13a-install-linux-initial.log`).
These failures remain recorded, not counted as passes. Native fixture directories
under /tmp/samosa-install-* retain small QA inputs; no engine was launched.

Next: implement generic provider
readiness plus owned, verified engine/artifact relationship and activation that
preserves last-good provider on failure; qualify missing/changed alias/context,
external-unqualified status and actual owned engine behavior. Finish S13a's
remaining acquisition/activation acceptance, then continue the original ordered
Agent/App, source/task/schedule, assembly/resource/quality and retirement gates.
Do not mark C03/C04 or the overall goal complete from this installer subset.

### Current state — 2026-10-06 18:37 UTC

The goal remains **active** with its full C01-C14 scope. This entry supersedes
the S12b pending statements in the 16:39 checkpoint below. No whole milestone,
App, production switch, deployment or legacy retirement is declared complete.

**S12b descriptor/package closure:** implemented focused descriptor checks for
unconfigured/default FIFO metadata and configured capacity4/wait1234 metadata,
plus owner-gated sessions/operations/cancellation. Fake contract count is now87
(previous59). Updated text contract's stale zero-queue statement and documented
configuration versus readiness; dependency manifest now accurately records
native Debian12 ARM64 qualification. README/task commands and release payload
were refreshed. S12b bounded assignment is qualified; C04/C11 remain open.

Independent copies: `/private/tmp/samosa-s12b-close.V6jAW7/{mac,linux}` contain
only Inference-owned code/tests/contracts/build/docs, no sibling source.
Mac `make test sanitize` passed operations693/FIFO79/fake87/session1106 regular
and ASan/UBSan. Initial `test-native` failed at test_llama.c:76 because its
fixture could not establish a loopback listener under the old sandbox; this is
retained in `/private/tmp/samosa-s12b-close-mac.log`, exit2. After full access,
`make test-native sanitize-native release` passed HTTP146 regular/ASan/UBSan
and all regular suites, exit0, session96814;
`/private/tmp/samosa-s12b-close-mac-confirm.log`. No predicates were weakened.
Native Linux image3df5cca1b5a7 reports arm64, runtime aarch64. Network-disabled
auto-removing `--init` container `make test sanitize test-native sanitize-native
release` passed the same counts with ASan/UBSan/LeakSan, session49312 exit0;
`/private/tmp/samosa-s12b-close-linux.log`. No QA container remains from this run.

Final documentation refresh release reruns passed, Mac6969/Linux22523 exit0:
`/private/tmp/samosa-s12b-close-release-{mac,linux}.log`. Both tarballs include
contracts/text-v1.md, admission-v1.md and operations-v1.md; extracted trees compare
identically with `diff -qr`. Component version0.1.0, qualified inference.c SHA256
`6aa7df05461c44b3354cda7d0c41bd145354c63d9058310f87e54a2e6c76dcb6`.
Mac tar SHA256 `da61d6b2d7c194050e5193afd1f7c088a72e7aafe9efe6879346c41aec8581e6`;
Linux tar SHA256 `1923a9d25a267cb23e77048c1f0bfac75e146ddf10af276c85fd968593ccea24`.
Archive byte hashes differ due to archive metadata; extracted contents match.
The earlier unexplained actual-model failure remains recorded; no new actual
engine cancellation or real-model qualification is inferred from these runs.

**Next named task S13a is now active:** specification recorded before coding in
`components/samosa-inference/docs/S13a.md`. It requires bounded artifact
acquisition/installation, explicit interrupted resume, checksum-qualified
publication and generic readiness preserving last-good activation. Initial
implementation adds `src/artifact.c/h` and `tests/test_artifact.c`: descriptor-
based regular-file verification using64KiB buffer, pread without cursor mutation,
exact length/host ceiling, full SHA256 and before/after identity/mtime/ctime
observation. No timestamp-only checksum cache. SHA helper is now static inline
to support streamed-only consumers without disabling compiler warnings.
Mac `make test-artifact sanitize-artifact` passes1024 checks each, including
standard empty/abc/multiblock/million-a vectors, wrong checksum, modified content,
size/ceiling, invalid config/descriptor and pipe refusal;
`/private/tmp/samosa-s13a-verify-mac.log`. Independent native Linux regular and
ASan/UBSan/LeakSan verification also passes1024 each, session7491 exit0,
`/private/tmp/samosa-s13a-verify-linux.log`. This is only a verification primitive:
it does not publish/install artifacts, provide durable resume, establish immutable
engine identity or prove readiness. S13a remains unfinished. Its source/Makefile
changes postdate the archived S12b snapshot and need their own full qualification.

Ordered continuation: finish S13a installer/readiness implementation and all its
acceptance cases; then Agent parent operations/cancellation and App ingress;
remaining source/task/scheduling gates; assembly/platform/quality/resource
qualification; only then retire superseded legacy paths. Keep the old checkpoint's
owner limits and broader open gates in force. No Python and no delegation.

### Current state — 2026-10-06 16:39 UTC

**Read this current checkpoint before the chronological entries below.** Those
entries retain earlier evidence/failures and describe older intermediate states.
This checkpoint supersedes their statements about what is currently active.

The goal remains **active**: finish the main architecture plan, including its
C01-C14 acceptance, independence, retirement and completion rules. No C01-C14
milestone is being declared complete here. Three native replacement owners exist;
the fourth target owner, App, does not exist yet. The production default has not
been switched and the legacy gateway has not been retired.

#### Constraints, workspace and process state

- No Python implementation, runtime, build, tests or tooling. Discarded Python
  replacement components were reverted earlier. Current implementation is native
  C11/POSIX/system SQLite, with optional libcurl and configured PDFium. Browser JS
  is available for the future thin App. Mechanical SHA/JSON copies are locally
  owned; there is no fifth shared business package or sibling-private include.
- Host qualification: macOS ARM64 and native ARM64 Debian12 Linux. Linux runs in
  owned auto-removing `samosa-native-qa:debian12-arm64` containers with network
  disabled, `--init` for crash-fixture orphan reaping and no Python packages.
  AMD64 emulation and actual Linux PDFium are **not** qualified.
- `git status --short`: only untracked `components/` and the three architecture
  documents. Existing tracked legacy code has not been changed or deleted.
  No commits/PR/deployment have been made. New work is reviewable in the workspace.
- No agent delegation. No retained QA model process or QA container at the
  checkpoint: owned real-provider test12638 exited0 and its cleanup verified its
  own binary/model/port before terminating the engine. Port18647 has no listener.
  Personal model/engine artifacts were read-only inputs; no personal state edited.
- Exact model used for prior/current real QA: external native llama-server
  `/Users/deepanwadhwa/.samosa/backends/prism-llama.cpp/build/bin/llama-server`,
  `/Users/deepanwadhwa/.samosa/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf`.
  Their presence is a QA prerequisite, not a shipped installation implementation.

#### Implemented and qualified Sources state

Owner folder `components/samosa-sources/`; authoritative snapshots, registration
claims, evidence caches, index jobs and locator observations live in its private
state. Other owners use the public CLI contract and never read Source tables.

- Immutable bounded UTF-8 text captures and exact selected byte evidence;
  Source/version IDs are content SHA. Reads verify retained bytes. Selected PDF
  page ranges use configured PDFium, extractor fingerprinting, absolute one-based
  page citations and explicit partial/text-layer coverage. No OCR fallback.
- Bounded read-only folder inventory distinguishes metadata inspection from
  content coverage. Resumable literal text index has durable per-item progress,
  lease-proven interruption recovery, retained snapshot search and no automatic
  replay of interrupted work. Search previews are candidates, not evidence.
- S11c1 owner/media claims prevent another owner from gaining reads or cached
  provenance by knowing a SHA. Payloads remain deduplicated, while public IDs stay
  immutable and provenance/cache authority remains owner-bound.
- S11c2 `locator_reconcile`/`locator_get` independently check current granted
  paths against owned registered versions and optional historical physical tuples.
  Full hash detects content edits even after mtime restoration; foreign same-byte
  inode replacement is changed and its bytes are not adopted. Missing, changed,
  unsupported and uncertain differ from available. Directory/root/file race
  revalidation precedes atomic observation/result commit.
- Live observation scope binds owner, host, Root device/inode and relative path.
  A changed Root creates a new locator scope. Exact retries/get describe committed
  history; new operation IDs request new checkpoints. These are not continuous
  watcher or atomic whole-filesystem freshness guarantees. Immutable citations
  remain usable after copy/move/inverse/delete of a live path.
- Limits:1MiB/file,128 snapshots/128MiB,256 owner/media claims,128 cached evidence
  entries/8MiB,8 index jobs with256 items/job and512 job operations,256 locators
  and256 locator operation histories, eight observations/op,64KiB observer JSON,
  existing160MiB Source SQLite ceiling. No eviction. Locator lease1 active/0 queued.
- Independent Mac and native Linux full regular/sanitizer/release checks passed:
  text351, inventory743 Mac/757 Linux, index593, owner1140, locator2608. Actual Mac
  PDFium176 passed, including real97th-of100 page sentinel7319 and extraction spy.
  Evidence: `/private/tmp/samosa-s11c2-mac-confirm.log` and
  `/private/tmp/samosa-s11c2-linux-confirm.log` (sessions84400/74506 exited0).
- Still open: stable logical source registrations versus immutable versions,
  broader live/index freshness transitions, HTML/DOCX/OCR/media/web acquisition,
  neural enrichment and evaluated semantic retrieval. Do not infer C05/C06 done.

#### Implemented and qualified Agent state

Owner folder `components/samosa-agent/`; authoritative conversations, ordered
events/runs, admitted evidence, immutable action plans, approvals and checked
effect receipts live in Agent private state.

- Document questions admit validated literal Source evidence, pin provider/context
  and count the exact final prompt before generation. Ordinary chat uses the same
  conversation/context policy with empty evidence/citations and no Sources work.
- Durable revision checks and normalized retries survive process restart.
  Semantic history/evidence survives a lost engine; hot versus cold recovery is
  explicit. Bounded compaction keeps originals and admitted evidence, produces
  qualified lossy excerpts and resets only provider continuation. It extracts no
  new source text. Conversation work lease1 active/0 queued.
- Native finding uses Source candidates -> bounded inspection -> validation ->
  selection. It cannot promote a preview/model quote to verified evidence or infer
  exhaustive counts/absence from a shortlist. Current scorer is literal matching.
- Checked copy, move, undo-copy and inverse move share one approved-plan executor,
  not separate interactive/scheduled effect implementations. Plans bind version,
  metadata identity, host, Root and source/destination parents. Publication and
  capture use no-clobber operations, durable intent/stage/receipts, private inode
  anchors and directory fsync. Unknown quarantines are preserved; recovery never
  blindly republishes/deletes them or overwrites a foreign later file.
- Inverse plans require their own exact revision approval. Historical receipts
  remain visible with reversal markers. Same-filesystem identity anchors are an
  explicit current restriction; cross-device effects refuse before publication
  or source capture. Cross-device move/undo is not implemented.
- S11c3 automatic Source receipt handoff is implemented in `src/reconcile.c/h`
  and `src/actions.c`. Successful effects request at most two batches of eight
  Source observations. Agent validates host/immutable plan Root scope/canonical
  owner locator IDs, paths, versions and physical tuples. Source failures are
  persisted as separate `source_reconciliation` qualifications; committed physical
  effects remain successful. Exact retry returns the saved checkpoint offline.
- Worker SIGKILL during the Source call leaves the committed inode intact;
  operation recovery is interrupted, and an explicit new operation reconciles
  without replaying the copy. Native command guardians terminate only owned
  helper process groups after parent death or deadline.
- Limits:64 conversations,256 runs,512 messages/conversation,512 events,
  8MiB semantic retention/16MiB conversation SQLite,32 plans/128 action operations,
  eight files/plan,1MiB/file and16MiB action SQLite. No eviction. Action lease is
  independent of conversation work,1 active/0 queued. Helper argv32 arguments,
  response1MiB and10-second deadlines are bounded.
- Independent Mac34915/native Linux52060 full suites/sanitizers/releases passed:
  Agent811, finding110, copy972, undo477, move510, handoff validator134 and actual
  handoff crash60. Linux includes LeakSan and real distinct-filesystem refusal.
  Public separately built Source/Agent integration220 passes copy/move/inverse,
  automatic observations, wrong Source Root qualification and offline exact retry.
  Logs `/private/tmp/samosa-s11c3-mac.log` and
  `/private/tmp/samosa-s11c3-linux-corrected.log`.
- Still open: owner-scoped Agent run cancellation/parent-operation propagation,
  disconnect/detach service policy, folder-question/web/organization policies,
  evaluated semantic scoring and durable authorized scheduling. Inference cancel
  alone does not satisfy Agent or end-to-end cancellation.

#### Active Inference assignment: S12b

Owner folder `components/samosa-inference/`. Read `docs/S12b.md` first; S12a FIFO
is its completed preparatory predecessor. Native optional text-provider adaptation
and last-good semantic sessions were qualified before these assignments.

What exists now:

- Native adapter verifies configured model alias/context through public props,
  renders with apply-template/reasoning disabled, counts actual tokenizer IDs and
  generates from exactly those IDs. It reports total versus processed input
  separately and preserves explicit provider/context across hot/cold continuation.
- S12a `src/admission.c/h`: one host admission directory, one active provider
  lease, accepted FIFO tickets, configurable0..8 queued and1..60000ms monotonic
  admission wait (default capacity0/default wait10000ms). Later arrivals cannot
  overtake accepted work. Nine fixed waiter lease files prove worker death without
  PID assumptions; private ticket DB ceiling1MiB. Capacity0 is immediate busy
  through the same scheduler. Accepted count and generation share this policy.
- S12b `src/operations.c/h`: optional `operation_id` for count/generate;
  `operation_get`/`operation_cancel`; configured-owner authority; normalized request
  digest, exact saved responses, running/completed/failed/cancelled/interrupted
  status, per-operation worker lease and cancellation descriptor. Dead workers
  never silently replay model requests. Reordered JSON retries are equivalent;
  changed prompt/session/provider/context/model-alias parameters conflict.
- FIFO polls only the operation's own marker. Native curl progress closes only
  its transfer. A failed/cancelled generation preserves last-good session state.
  Cancellation is a request and can race committed completion; final get reports
  actual status. Untracked compatibility requests remain bounded but are not
  individually cancellable through this operation API. App/Agent must propagate
  tracked operation IDs in the next end-to-end work.
- Limits:128 retained operation histories,8MiB aggregate result bytes including
  512KiB reservation per live operation,16MiB operation SQLite. Dead reservations
  are lease-proven/reaped before admitting new work. No eviction. Sessions retain
  128 handles,128KiB prompt,64KiB answer,8MiB cache/16MiB SQLite. Native HTTP is
  loopback-only,1MiB responses,2-second connect/60-second transfer deadlines.
- Changed implementation files: `src/admission.c/h`, `src/operations.c/h`, local
  `src/sha256.h`, `src/inference.c`, `src/llama.c`, `src/llama.h`, `src/main.c`,
  `Makefile`; native tests `tests/test_admission.c`, `tests/test_operations.c`,
  extended `tests/test_llama.c`; specifications/contracts S12a/S12b and
  admission-v1/operations-v1. Agent `tests/session_native.sh` now additionally
  verifies real tracked generation and exact cached retry while its engine is down.

Exact recent evidence and current qualifications:

| Check | Actual result | Evidence / limits |
| --- | --- | --- |
| `make test-admission` | PASS79 | Actual A/B/C FIFO, queue full, dead head/active, deadline and slot reuse. |
| `make test-operations` | PASS693 | Owner hiding/conflict/reordered retry, actual queued cancel/death, survivor isolation,128-history cap,16 live reservations/byte refusal/dead reservation recovery. |
| `make test-native` | PASS146 | Controlled native HTTP, exact tokens/provider/budgets plus active transfer cancellation, wrong-owner refusal, last-good session and next-request survival. |
| Independent Mac full `make test sanitize test-native sanitize-native release` | PASS41964 | Latest operation693/FIFO79/fake59/session1106/HTTP146, ASan/UBSan. Log `/private/tmp/samosa-s12b-mac-final.log`. |
| Independent native ARM64 Linux same command | PASS20726 | Same counts, ASan/UBSan/LeakSan. Log `/private/tmp/samosa-s12b-linux-final.log`. |
| First actual model/session QA after script extension | FAIL93031 | Exit1 with no phase diagnostic before trap cleanup. Kept `/private/tmp/samosa-s12b-actual.log`; cause was not established. Do not call this initial run a pass. |
| Exact actual-model script rerun with shell trace | PASS12638 | Actual tracked7319/native output, byte-identical cached retry while engine down, document hot/cold/compact and ordinary chat arithmetic4/remembered7319 hot/cold/compact. Log `/private/tmp/samosa-s12b-actual-trace.log`; own engine cleanup completed. |

The independent S12b runs qualify the implemented scheduler/operations/curl/core
behavior. **One later descriptor-only edit is not yet qualified in those copies:**
`src/inference.c` describe now advertises operations/cancellation and explicit FIFO
capacity/wait/configured metadata. Documentation/release payload refresh is also
pending. S12b is therefore **still active**, not marked complete in this checkpoint.
Actual active cancellation is qualified against controlled native HTTP, not yet
against the actual Ornith/llama-server engine; actual engine QA covered tracked
generation, offline retry and session recovery. Do not conflate those results.

Initial failed checks retained in the record: S12a first enqueue preserved the
provider busy error instead of clearing it before enqueue (fixed before FIFO
qualification); S11c3 initial ambiguous patch placement/unused helper compilation
and Linux fixture indentation failed (fixed with strict warnings retained);
S12b Linux initial build lacked POSIX feature visibility for open_memstream (fixed
with `_POSIX_C_SOURCE=200809L`, independent final Linux passes). No required
compiler warning or sanitizer was disabled to obtain a pass.

#### Exact next actions

1. **Finish S12b first.** Qualify the descriptor-only edit using the focused fake
   contract checks and native build in the independently copied Inference owners.
   Refresh Inference README/current task commands and package the new contracts.
   Record the exact latest version/content/evidence. Reconcile the unexplained
   first actual-QA failure if it recurs; do not weaken its acceptance predicates.
   Relevant files: Inference `src/inference.c`, `README.md`, `docs/S12b.md`,
   `contracts/{text,admission,operations}-v1.md`, `Makefile`; Agent
   `tests/session_native.sh`. The main goal remains active afterward.
2. **Record the next named task before coding:** provider artifact checksum and
   readiness/install ownership in Inference (C03/C04), using native bounded
   streaming checks and explicit generic status descriptors. Existing external
   model files are not proof of a shipped installer or checksum-qualified provider.
   Do not make Sources/App require model weights. No next-task implementation has
   begun and no installer/readiness assignment is being claimed completed.
3. **Complete operation propagation at Agent/App boundaries (C07/C11/C12):**
   tracked provider IDs owned by a parent Agent run, owner-scoped run cancellation,
   explicit detach/reconnect policy, bounded authenticated ingress/shutdown. The
   thin App must display committed Agent state, safely render Source text and own
   only the component instances it starts. App currently has no implementation.
4. **Complete remaining supported task and source gates:** folder questions,
   evaluated candidate scoring, organization through the same checked executor,
   bounded HTML/DOCX and separately qualified OCR/media/web paths as supported by
   the plan. Add durable scheduled policy with separate authorization of future
   effects; one-time plan approval must not silently authorize future moves.
   Write individual owner task specifications and focused acceptance first.
5. **Finish C13/C14 assembly and retirement:** independently build/package App;
   pin compatible component versions; prove minimal installation without weights;
   qualify functional/quality/crash/concurrency/resource cases on declared platforms;
   retire each genuinely superseded production path and its tests/dependencies,
   then remove gateway/alternate jobsd once assembled supported capabilities pass.
   Do not ship fake Inference as production quality or claim the current native
   CLI subsets constitute the completed browser product.

No external blocker currently prevents progress. Remaining work is implementation
and qualification, not waiting for user approval. No schedule, App, installer,
whole-milestone completion, production deployment or legacy removal is implied by
the subset passes above.

### Earlier chronological checkpoints

- **Constraint:** no Python for implementation, runtime, builds, tests or tooling.
  Discarded Python components, copied artifacts and their evidence were removed.
  Legacy tracked application code was unchanged during that rollback.
- **Native S01:** independent `components/samosa-sources/`, C11/POSIX plus system
  SQLite; owns immutable text snapshots and literal range evidence. Strict bounded
  JSON, descriptor-relative no-symlink capture, SHA-256 verification, UTF-8 range
  boundaries, explicit 128 snapshots/128 MiB quotas with transactional admission.
  SHA primitive mechanically reused from legacy C; no private include/build link.
- **Evidence:** `make -C components/samosa-sources test sanitize release` PASS:
  271 C checks, concurrent admission 1 accepted/7 refused under count=1, separate
  CLI immutable recovery after live replacement, AddressSanitizer/UBSan and source
  release tarball. Host Darwin25.5 arm64, native component0.1.0. Independent copy
  `/private/tmp/samosa-native-s01-independent-YtIBYZ/sources` rebuilt from clean
  source and repeated `make test sanitize release` PASS without parent/siblings.
  `git diff --check` PASS; no `.py`/`.pyc` under components.
- **Limits:** S01 preparatory only; C01/C02/C05 partial, other C01-C14 gates open.
  No Linux release, service, reader/model/App or legacy retirement claim.
- **Native S02 evidence:** Agent/Inference locally owned native C transport,
  configured public argv execution (no shell/user-command interpolation), one
  qualified read and exact same prompt for count/generate, arbitrary provider ID
  pin, 256 output reserve and immutable citations. `make test sanitize release`
  PASS Agent76/Inference59 checks; owned helper timeout/group teardown and invalid
  evidence before any Inference call covered. Separate native `make test-interaction`
  with explicit external Sources/Inference executables PASS. Independent copies
  `/private/tmp/samosa-native-s02-independent-RbkD9B/{agent,inference,sources}`
  repeated tests/sanitizers/release and public interaction PASS. Final command
  teardown fix keeps child PID reserved until its group is stopped; copied Agent
  rebuilt/retested all including interaction PASS afterward. Fake output only;
  no model quality claim. No sibling private includes or state readers.
- **Native S03:** optional configured PDFium C ABI adapter with owned pinned
  public headers/license. Immutable binary PDF snapshots/format metadata, selected
  inclusive1..4 pages, <=64 KiB text/page, total/absolute citations, always partial
  text_layer_only. Native fixture generator/forwarding spy, no Python generator.
  `make -C components/samosa-sources test sanitize test-pdf release
  PDFIUM_LIBRARY=/Users/deepanwadhwa/Documents/samosa-chat/dist/libpdfium.dylib`
  PASS 287 text/79 PDF checks and separate native PDF CLI page97/100 sentinel7319.
  Spy observes only selected page/text loads, invalid ranges before load, blank
  partial, pinned bytes after live mutation, missing library/malformed refusal.
  Independent `/private/tmp/samosa-native-s03-independent-a894PR/{sources,agent}`
  repeated test/sanitize/release/native PDF and public text interaction PASS.
- **Resolved regression:** macOS system SQLite/logging crashed in a test child
  doing library work after fork without exec (SIGSEGV, local crash stack).
  Concurrent quota now launches fresh native worker processes with posix_spawn.
  Agent public command transport also uses posix_spawn process groups; repeated
  Agent76 tests/sanitizers/release/interaction PASS. No failing gate suppressed.
- **Native S04:** optional system-libcurl C adapter executable; fake/default still
  libc-only. Host-configured endpoint/provider/model alias/context/admission state.
  Public props alias/context check; exact apply-template with thinking disabled,
  engine tokenize IDs, completion on the same array. One active/zero queued kernel
  lock, typed busy; bounded HTTP/body/connect/deadline. Agent uses same policy for
  explicit bytes or PDF pages, validates partial ordered page evidence/citations.
  `make test sanitize native test-native sanitize-native release` Inference PASS
  fake59/controlled HTTP77 checks. Controlled fixture is not a real-quality gate.
  Agent `make test sanitize release test-native` PASS100 checks and actual native
  text/PDF answers7319 with pinned versions/page97 citations; arithmetic native
  output4, actual input26/output2/processed26 tokens. Final alias/context pin added,
  controlled and real native gates repeated PASS. Independent copy
  `/private/tmp/samosa-native-s04-independent-DXrryp/{inference,agent}` repeated all
  builds/sanitizers/native interaction using external S03 Sources copy PASS.
  Engine owns PID35823/localhost18645, metadata/log at
  `/private/tmp/samosa-native-s04-engine-ICB7nX/`; native test launcher avoids shell
  background startup race. Retained only for following required quality gates.
- **Limits:** native S01-S04 are bounded subsets, not completed C01-C14. Model
  alias checks do not replace artifact checksum/version qualification. Streaming,
  installation/multimodal/residency/fair admission/full cancellation/session,
  source cache/OCR/media/web/index, conversations/effects/App and retirement open.
  Docker installed but daemon unavailable; no Linux qualification claimed yet.
- **Native S05:** bounded Agent-owned SQLite conversations/messages/runs/events,
  private host state/owner, random IDs, exact normalized retries before dependencies,
  consistent read transactions, 1active/0queued kernel work lock, recovery only
  when no live peer. Reserve running/user/event before policy; terminal assistant/
  result/admitted evidence/selected scope commit together. Failed new source intent
  stays in run input; last-good pair remains intact. Reuse the one question policy
  with typed semantic history/copied evidence. Persist no duplicate model_request.
  64 conversations/256 runs/512 messages per conversation/512 events/8MiB semantic
  bytes/16MiB database, reserve3MiB before admission, no eviction. Actual successful
  large-evidence turns fill quota; refusal leaves conversation revision0/messages[].
  `make -C components/samosa-agent test sanitize release test-conversation-native`
  with explicit external native dependencies PASS651 C checks and actual7319
  separate CLI restart/empty-client/followup with original file removed and Source
  command unavailable; identical original retry with both dependencies unavailable;
  owner/events checked. C actual reserved-worker SIGKILL restores interrupted even
  through exact submit retry, live peer stays running/readable and new work busy.
  Test barrier compiled only into test executable, production has no fault hook.
  Independent `/private/tmp/samosa-native-s05-independent-EU6S0h/agent` repeated
  clean build/test/sanitizer/release/real conversation using external S03/S04 copies
  PASS. No full service/cancellation/session or retirement claim.
- **Active / next action:** native S06a Inference opaque sessions; read
  `components/samosa-inference/docs/S06a.md` and parent S06 in both owners. Then S06b
  wires Agent public refs/budget/compaction and qualifies
  hot followup token reuse, cold engine replay and explicit compaction without
  Sources; preserve provider-owned last-good opaque sessions. Keep all broad gates
  open until their required independent/native/resource/App checks pass.
- **Native S06a completed subset:** Inference owns opaque owner/provider/context
  scoped handles, successful revision commits, last-good prompt/result, bounded
  SQLite cache and kernel lease recovery. Fake59/session1106/controlled native113
  checks and sanitizers PASS, including actual SIGKILL/reopen and successful
  admissions filling cache quota without eviction. Actual native session answers4;
  independent `/private/tmp/samosa-native-s06a-independent-MOONp6/inference`
  repeated clean test/sanitize/native/test-native/sanitize-native/release PASS.
  Subsequent S06b refinement allows count above capacity (generation remains
  strict) and bounded numeric-array tokenizer decoding; workspace checks PASS,
  final independent recopy still required.
- **Active S06b:** Agent stores public continuation with successful conversation
  commit, refuses provider/context changes, recreates unknown/stale same-provider
  handle once, uses shared bounded typed history/count/omission policy. Explicit
  compact retains original messages/evidence, produces marked derived bounded
  excerpts, resets handle but preserves provider/context pin, starts no Sources.
  Native Agent696 checks PASS; sanitizer696 PASS before final pin-preservation
  adjustment (repeat required). Existing real conversation native restart/retry
  gate PASS with sessions. Actual hot native input365/processed109/output5 and
  answer7319 PASS after bounded engine checkpoint configuration (4 checkpoints,
  min spacing64, ubatch64, cache256MiB). Original engine settings produced full
  reprocessing and are not claimed to pass hot gate. Actual cold/compact check
  currently running; dedicated repeatable owned-engine runner and clean copied
  release qualification remain required before declaring S06b complete.
- **Current owned engine:** previous PID35823 and replacement43072 were stopped
  after recorded-binary verification. Only owned QA PID43376 now runs18645;
  metadata/log `/private/tmp/samosa-native-s06-real-0Nqi8r/owned-engine.json`
  and `engine-cold.log`. Retain for remaining required native qualification;
  stop only this recorded owned engine at completion. No user model process stopped.
- **S06b final focused evidence:** native Agent708 checks incl public session
  recreation/provider pin, explicit budgeting omissions with evidence retained,
  dependency-free compaction/retry/original retention PASS test/sanitize/release.
  Native `make test-session-native` with configured public dependencies/owned
  native launcher/real Ornith model/port18645 PASS; input366/processed109/output5
  hot, cold actual restarted engine full processing, answers7319/pinnedversion,
  zero Sources after initial, original6 messages retained, postcompact handle new.
  Clean `/private/tmp/samosa-native-s06-independent-qnwBIK/{agent,inference}`
  test/sanitize/release (Inference native/control113 and session1106) and copied
  Agent real hot/cold/compact with external copied Sources PASS. Runner stops its
  owned instances; prior manual PID43376 also stopped after binary verification.
  No retained engine currently. One concurrent workspace verification invocation
  failed a quota-completed assertion while another rebuilt its helper executable;
  diagnostic-enabled final sequential workspace/copy checks PASS. Concurrent
  build mutation is not a supported test setup; end-to-end load C11 remains open.
- **Active next:** S07a extractor fingerprint, spec
  `components/samosa-sources/docs/S07a.md`, implementation/test underway. Then
  S07b bounded Sources-owned evidence cache keyed by exact version/range/reader/
  fingerprint; cache spies/partial separation/independence. S06 is a qualified
  bounded subset only. Fair scheduling/cancellation/install/media/OCR/index/
  file actions/service/App/Linux/retirement and other C01-C14 gates still open.
- **S07a native fingerprint checks:** workspace text287/sanitizers and real PDF113
  checks PASS; binary spy versus actual PDFium changes fingerprint/evidence ID
  while retaining source/version/page97/7319; repeat identity stable, FIFO and
  oversized configured artifact refuse. Additional real forwarding spy renames
  its owned copied artifact during page load; typed pdf_adapter_changed publishes
  no evidence (focused rerun underway). Initial independent copy
  `/private/tmp/samosa-native-s07a-independent-3wkmTt` path is recorded in
  `/private/tmp/samosa-native-s07a-independent-path`; passed text/sanitize/PDF113/
  release before mutation fixture addition; final recopy required. Sources/Agent/
  Inference separate CLI interaction PASS after fingerprint change.
- **Native S07a/S07b focused checks:** workspace Sources350/sanitizers/release and
  real PDF141 checks PASS. Identity includes bounded artifact fingerprint; actual
  copied artifact rename during extraction refuses without evidence. Successful
  cache hits skip PDF page loads, partial coverage remains partial, failed read
  isn't admitted, changed artifact/range misses. Count quota3accepted/1refused,
  byte quota1accepted/1refused retain old byte-identical hit. Corrupt cache response
  refuses; pinned source bytes are rehashed before hits. Bounded128entries/8MiB/
  2MiBresponse, no eviction; count preflight before extraction, exact bytes admitted
  transactionally after bounded extraction. Clean Sources copy path recorded at
  `/private/tmp/samosa-native-s07-independent-path` repeats test/sanitize/PDF141/
  release PASS. Separate copied Agent public interaction remains to run.
- **Active next S08a:** native guardian/lifeline for parent-death cleanup of Agent's
  configured public helper groups, spec `components/samosa-agent/docs/S08a.md`.
  Implementation underway; existing tests running, actual mid-helper SIGKILL/
  unrelated helper survival and final sanitizer/independent qualification pending.
  This closes a known S05 ownership gap, not all C11 service/cancel gates.
- **S07 completion subset:** final clean Sources copy
  `/private/tmp/samosa-native-s07-independent-oyuNvi/sources` text350/sanitize/PDF141/
  release PASS, including mutation/no failed cache, zero loads on hit, quotas and
  corruption. Public Agent interaction against this copy and copied Inference PASS.
- **S08a ownership evidence:** Agent736 test/sanitize/release PASS workspace and
  independent copy (path `/private/tmp/samosa-native-s08a-independent-path`).
  Actual Agent mid-provider SIGKILL: owned helper+grandchild gone, unrelated
  helper remains, public recovered run interrupted/revision12/last-good7319.
  Native copied hot/cold/compact repeat after guardian change is running.
- **Active next S08b:** Linux qualification spec
  `components/samosa-agent/docs/S08b.md`; existing Docker application started,
  daemon readiness/native compiler container checks underway. No Linux PASS yet.
  Native goal remains active; all broad gates not proven by these subsets stay open.
- **S08a final native repeat:** copied Agent736 checks/sanitizer/release and actual
  native hot/cold/compact with copied fingerprint/cache Sources PASS, hot input431/
  processed109/output5/answer7319. Test-owned engines stopped. Native helper
  guardian now covers active parent-death, bounded responses/deadlines, descendants;
  full public cancel/auth/service/fairness/resource gates remain open.
- **C01 design matrix:** `components/samosa-agent/docs/USE_CASES.md` records every
  required product path, authoritative state and cross-owner access, provider
  defaults and optional native adapters. Includes ordinary chat, PDF/folder query,
  approved copy/move/undo/schedules, indexing, web, reconnection and App lifecycle.
  This is ownership/design evidence, not execution completion for unbuilt paths.
- **Linux current prerequisite:** Docker daemon ready linux/aarch64 29.0.1. Owned
  Debian12 native QA image build started with GCC/SQLite/libcurl/clang-format and
  explicit no-python check. Base registry metadata currently pending; no Linux
  component build/test evidence until the image actually builds and runs.
- **S08b platform findings:** initial Linux/x86_64 emulated GCC build exposed
  misleading indentation; owner-local native statements clarified with strict
  warnings retained. Emulated LeakSanitizer failed due ptrace/emulation; no passing
  sanitizer gate claimed there. Docker credential-helper stall bypassed with owned
  empty anonymous Docker config; native ARM64 Debian12 pulled successfully.
  Optional clang-format package unexpectedly pulled Python transitively; that
  provisional container was stopped/deleted. It is not a product dependency or
  retained QA image. Replacement native-only package sets explicitly verify
  `command -v python`/`python3` absent. Owned ARM64 QA image
  `samosa-native-qa:debian12-arm64` SHA3df5cca1b5a7de09c6e90878388b1dfbae953f7d0484346021d76a27589b689c,
  GCC12.2/SQLite3.40.1/libcurl7.88.1. Linux Source350 and inventory757 tests pass;
  native ARM64 Source350 sanitizer passes incl quota. Full owner runs underway.
- **S09a bounded inventory:** native Sources inventory requires no state/model/
  executor. Endpoint-scoped host/scope/metadata identities, relative locators,
  explicit inventory completeness and uninspected content.256entries/512names/
  32directories/depth4/4096locator/512KiB output. Mac test743/sanitizer/release PASS
  with actual fresh worker/barrier+parent file creation during directory capture:
  source_changed publishes no entries. Linux757 includes malformed UTF-8 filename
  fixture (APFS rejects creation, so that filesystem-specific fixture runs Linux).
  Escape/symlink/FIFO/duplicates/empty/depth/entry bounds tested. Spec S09a; final
  native PDF regression/clean Source copy still required. No indexing yet.
- **Active next:** finish S08b ARM64 owner tests/sanitizers/releases and public
  interaction; resolve actual failures without weakening checks. Finish S09a native
  independence/PDF regression, then S09b bounded resumable Sources index/retrieval.
  Goal remains active; App/file actions/install/cancel/service/full resource/retirement
  and other broad C01-C14 gates remain open. No retained model engine.
- **Current resume:** S08b native ARM64 Sources350/inventory757 test/sanitize/release
  PASS; Inference fake59/session1106/native controlled113 tests/sanitizers/release
  PASS after strict GCC formatting fixes. Agent736 regular tests PASS; Linux full
  sanitizer still running (many native helper self-execs with LeakSanitizer).
  Linux public interaction and final copied fixture release remain to finish.
  S09a Mac743 test/sanitizer/release and native PDF141 regression PASS; standalone
  inventory CLI with no state/model PASS. Final independent Source recopy pending.
  S09b index spec now recorded; implementation not started yet. Continue S08b
  completion and S09a independence, then native bounded index/retrieval.
- **S08b final bounded platform subset:** native ARM64 Debian12 Linux Sources350/
  inventory757, Inference fake59/session1106/controlled HTTP113, Agent736 tests,
  full ASan/UBSan/LeakSanitizer runs and owner release targets PASS. Strict GCC
  warnings retained; clarified misleading compact statements and fixture buffer
  bounds. Source/Agent/Inference public CLI interaction with read-only separately
  mounted folders PASS. No Linux real PDF/OCR/model/App/product quality claim.
  Copied roots `/private/tmp/samosa-native-linux-mZpcrH/{sources,inference,agent}`.
- **S09a independent native evidence:** Source clean copy path in
  `/private/tmp/samosa-native-s09a-independent-path` repeats Mac text350/inventory743/
  sanitizer/real PDF141/release and no-state/no-model inventory CLI PASS. Linux
  malformed-filename case accounts for757. Later mechanical identity capture
  guard/refactoring for S09b is under new focused verification, not covered by
  the earlier release claim. Index spec recorded before implementation.
- **Active S09b:** bounded Sources-owned resumable text index/retrieval. Add exact
  expected_file_identity guard to registration to refuse stale manifest items
  before capture, preserve qualified immutable versions and copied evidence.
  Index job/item/op owner state, per-item commit,1active/0queued index lease, crash
  checkpoints and lexical retrieval/coverage all still to implement/qualify.
- **S09b implemented/qualified focused checks:** Sources src/index.c/h owns job,
  item and idempotent operation tables, native lease, durable per-item checkpoints,
  literal retrieval and coverage. Registration expected_file_identity prevents
  stale manifest capture. Search validates digest/literal contract/text agreement;
  failure emits one typed error. New tests/test_index.c exercises actual worker
  SIGKILL, live busy/readable progress, offline exact retry, owner isolation,
  changed files, corruption and public quota60 accepted/20 refused. Index589
  checks plus350 text/743 Mac inventory/141 actual PDF all PASS; independent copy
  path `/private/tmp/samosa-native-s09b-independent-path`, make test sanitize
  test-pdf release PASS. Linux ARM64350/757/589 test/sanitize PASS after strict
  GCC formatting fixes. Final strengthened593-check deletion/reuse fixture runs
  in both copies now; do not claim that last added assertion until finishes.
  Contracts index-v1.md/README/describe limits record8 jobs/256 items/512 ops,
  8MiB text+evidence/160MiB page cap. Public consumers never open source database.
- **Active S09c:** component Agent docs/S09c.md specifies bounded candidate ->
  inspect -> verify -> select for explicit literal file finding, with shared
  evidence validation and real public Source interaction. Next exact action:
  implement src/evidence.c/h shared validator and src/find.c/h public policy.
  S09b final593 assertions still running; broad C01-C14 remain in progress.
- **S09b final strengthened qualification PASS:** index593 regular+ASan/UBSan
  on independent Mac copy and native ARM64 Linux copy. Actual finished-file
  deletion before resume still yields3 captured candidates, proving no reread
  of committed item. Public capacity60 accepted/20 refused retains earlier hits.
  No product default switch/retirement implied; broader Sources readers/enrichment
  and Agent folder policies still open.
- **S09c implementation/current checks:** Agent src/evidence.c/h extracts the
  existing strict text/PDF validator unchanged and shares it with src/find.c/h.
  Public find_files policy performs one search/<=3 pinned reads, validates
  actual literal evidence, rejects forged version/identity/coverage/derived text
  and ignores preview claims. Tests/test_find.c110 PASS. Real public
  tests/find_interaction.sh PASS duplicate filenames and pinned evidence after
  live folder deletion with no Inference configured. Existing Agent736 regression
  passed immediately after shared-validator extraction; current final new-policy
  full tests/sanitizers/releases in progress Mac session15211 and Linux75712.
  Linux clean copy path `/private/tmp/samosa-native-s09c-linux-path`. Next exact
  action: inspect these runs, fix failures with strict warnings retained, then
  qualify independent Mac copy + public Linux Source interaction. S09c remains
  in progress until those finish. No Python introduced; goal remains active.
- **S09c independent Mac finalized:** clean Agent copy (path in
  `/private/tmp/samosa-native-s09c-independent-path`) test736/finding110/sanitize/
  release and public find interaction with independently copied Sources PASS.
  Linux native Agent736/finding110 regular and finding sanitizer PASS; existing
  full Agent Linux sanitizer still running24467. Public read-only separate mounts
  Source/Agent finding interaction PASS. Linux native QA source fixes kept warnings.
- **Active S10a ordinary chat:** src/agent.c/context.c/store.c extend the shared
  provider/context/run path with chat/submit_chat and explicit {kind:chat} scope.
  No source command is needed; empty citation/evidence results and mode-bound
  retry normalization. tests/test_agent.c811 regular+Mac sanitizer PASS, including
  prompt/history inspection, offline retry, compaction, doc switch and failed
  switch preserving last-good evidence. Regressed finding110 PASS. Initial document
  invalid-question error changed unintentionally; restored existing invalid_selector
  contract and reran tests successfully. Native tests/session_native.sh real
  Ornith9B PASS documenthot431/processed109/cold/compact plus ordinary arithmetic4,
  remembered7319 hot/cold/compact with no Sources configured. Runner18094 finished
  and stopped its own engines; no retained model process. Final workspace release
  underway68261. Next exact: finalize24467 Linux S09c proof, independently copy
  latest S10a Agent and run811/110 tests/sanitizers/public interactions on Mac and
  Linux, then next bounded assignment. Broad C01-C14/App/actions/service remain open.
- **S09c Linux finalized:** native ARM64 clean Agent736/finding110 full
  ASan/UBSan/LeakSanitizer and release PASS (24467 completed); public finding
  interaction PASS. S09c explicit literal pipeline bounded subset qualified.
- **S10a independent Mac finalized:** clean copy path
  `/private/tmp/samosa-native-s10a-independent-path`,811/110 tests/full sanitizer/
  release/public finding interaction PASS. Native actual chat/document session
  test PASS as recorded above. Linux latest811/110 regular and finding sanitizer
  PASS; full Agent sanitizer still running98427. Workspace release811/110 PASS.
- **Next S11a:** docs/S11a.md specifies approved checked copy, immutable identity
  binding/no-clobber/staged inode receipts/SIGKILL reconciliation. Implementation
  not started; first exact action src/actions.c/h and host-granted action root.
  Move/undo/schedules, wider readers/install/admission/transport/App/retirement
  remain required. Goal remains active, no blocker and no retained model engine.
- **S10a Linux final PASS:** latest independent native ARM64 Agent811/finding110
  full ASan/UBSan/LeakSanitizer and release completed98427. Real Mac native
  ordinary chat and all prior document session checks remain PASS. No retained
  engine/container. S10a bounded ordinary conversation subset qualified.
- **Active S11a first native implementation:** Agent src/actions.c/h adds its own
  owner-local plans/items/operation SQLite and kernel lease. AgentConfig/main
  --action-root/--action-host-id grant host scope. Immutable32-plan/128-operation
  quotas,8 files<=1MiB, metadata/SHA binding, explicit revision1 approval, durable
  staged-inode intent, atomic no-clobber copy and historical public item receipts.
  Native tests/test_actions.c173 PASS actual post-publication SIGKILL recovery,
  live get before crash, approval/no-effects, offline exact retries, inode reuse,
  owner hiding, changed source, foreign destination and escape/symlink refusal.
  This is first focused evidence, not completed C10. Full regressions/sanitizers
  underway Mac76753; fresh Linux
  copy path `/private/tmp/samosa-native-s11a-linux-path` strict GCC build underway.
  Next exact: fix focused failures/strict warnings; strengthen recovery for changed
  destination/root/parent scopes, staging cleanup and public receipt invariants;
  add full quota/concurrency/foreign-replacement fault fixtures; qualify copies.
  Move/undo/schedules/receipt-to-Source reconciliation remain mandatory later work.
- **S11a strengthened focused evidence:** native checked-copy972 tests PASS with
  32-plan capacity (32 accepted/1 refused),128 durable operation capacity via public
  approvals, retained exact hits, changed root/destination-parent refusal and
  second actual publication SIGKILL followed by foreign replacement refusal.
  Running exact retry reports running for a live peer, interrupted after death
  even without root; later get reconciles the known published inode. Public
  unreconciled_items/effect_status and historical receipt freshness prevent false
  completion/absence claims. Source/Agent public native runner58 PASS indexing ->
  finding -> immutable plan -> explicit approval -> checked copy -> Source readback,
  without model or consumer opening private Source state. Stage cleanup verifies
  known inode identities. Strict Linux GCC build passes after native statement
  formatting; no warning flags removed/no formatter package installed.
- **Current S11a verification:** Mac811/finding110/copy972 full sanitizer and
  release PASS33836. Linux811/110/972 regular plus finding/copy ASan/UBSan/LSan
  PASS; full Linux Agent sanitizer still running27644. Public new runner and
  contract/docs were added after that copied Linux release started, so final
  independent Mac copy/public Linux runner/release still required. Next exact:
  finalize27644, copy tests/actions_interaction.c/Makefile/docs to independent
  release roots, run public interaction and independence. Then S11b move/undo
  through the same executor; no broad C10 completion/legacy retirement yet.
- **S11a Linux final bounded subset:** independent811/110/972 full native ARM64
  ASan/UBSan/LeakSanitizer and release PASS27644. Public consumer runner58 PASS
  separately mounted Source/Agent, no model/private state access. Release refresh
  with newly added runner/contract/docs underway (session to record below).
- **Next specified S11b1:** docs/S11b1.md approved inverse plan/check copy undo,
  atomic no-replace quarantine and inode/version verification. Implementation
  not started. First exact action: src/actions.c native rename wrapper, inverse
  plan creation and plan_execute undo branch plus actual crash/race fixtures.
  S11b2 checked move, schedules/organization/Source reconciliation/App/remaining
  C01-C14 gates still required. No goal completion, no blocker, no retained engine.
- **Active S11b1 native implementation:** src/actions.c adds immutable inverse
  plans, separate approval, private inode identity anchors, native no-replace
  quarantine, verified unlink and durable inverse/history reconciliation. Original
  receipts expose reversed=true. State/destination must share a filesystem;
  cross_device_anchor refuses publication otherwise. tests/test_undo.c385 PASS
  actual SIGKILL after capture AND after unlink before receipt, foreign later
  targets preserved, modification refusal and raced restoration. Mac undo385,
  copy972/finding110 sanitizers PASS; full Agent sanitizer/release running96128.
  Public Sources/find/copy/readback/undo/retained snapshot runner77 PASS40438.
  Clean Linux copy path /private/tmp/samosa-native-s11b1-linux-path, strict GCC
  regular811/110/972/385 PASS; full sanitizer/release running5644. Linux source
  statement formatting changed only in its owned copy, must bring that formatting
  back before final qualification. Next exact S11b1: finish these full checks,
  qualify separate public Linux consumer and independent Mac release, then S11b2.
  Broad C01-C14 remains open; no legacy retirement or goal completion claimed.
- **S11b1 latest qualification:** workspace full Mac811/110/972/385 sanitizers and
  release PASS96128. Added prepared-copy anchor verification before publication,
  brought strict-GCC native formatting into workspace; current regular suite
  PASS62097. Independent latest Mac test/sanitize/release/public77 PASS97031.
  Latest Linux strict build/test/release/public77 PASS19093; native real distinct
  /tmp vs /dev/shm filesystem guard PASS10385 (tests/cross_device.c, optional
  make test-cross-device). Latest full Linux sanitizer running35640, prior
  independent full sanitizer still running5644; no completed-pass claim yet.
  docs/S11b2.md now specifies checked move/inverse move before implementation.
  Next exact: finish35640 and5644; update S11b1 completion, then add source-parent
  inode binding and native move checkpoints in src/actions.c/tests/test_move.c.
  No model engines retained. All broad C01-C14 gates remain in progress.
- **S11b1 bounded subset qualified:** native copy undo477 and copy972 current
  Mac full sanitizer/release/public77 PASS35299 and independent PASS77152.
  Independent Linux latest regular811/110/972/477/release/public77/cross-device
  PASS66802; latest changed action paths ASan/UBSan/LeakSan PASS90885. Prior full
  native Linux Agent811/finding110/action972/undo385 sanitizer/release PASS5644;
  second anchor-verification refresh full Agent sanitizer still running35640.
  Recovery now retains unknown quarantine phase/error without blocking unrelated
  plans, exercised by actual capture SIGKILL/name removal/restoration fixture.
  No broad C10/C01-C14 completion claim. Next active S11b2 task docs/S11b2.md:
  first source-parent/source inode binding in actions.c, then checked move and
  inverse through existing executor plus native fault fixtures. No Python.
- **Active S11b2 source scope foundation:** actions.c owner-local prerelease
  schema adds source inode/device and source-parent inode/device; new plans capture
  and verify content/metadata/path again before committing those fields. Existing
  historical plans get zero fields without changing completed retry semantics.
  Native regression suite running46762. No move operation enabled yet; exact next
  action is move_prepare/capture/publication/reconcile and inverse creation using
  these source scopes, with tests/test_move.c. S11b1 latest isolation477 sanitizer
  PASS90885 Linux and full PASS35299/77152 Mac. Linux full refresh35640 pending.
- **Active S11b2 first checked move/inverse implementation:** actions.c now accepts
  immutable move plans, binds source inode/parent, records move_prepared before
  native quarantine capture and move_captured before no-clobber publication;
  moves preserve inode. Recovery observes exact anchored publication without
  replay; separately approved undo_move restores original path and atomically
  marks original receipt reversed. Public moved_items/restored inode history added.
  Initial move212 focused checks PASS92172. Expanded inverse runner exposed test
  operation-ID reuse (idempotency_conflict, full runner89263 FAIL), corrected the
  fixture ID; rerun37667 and full tests/action sanitizers underway. This failed
  acceptance remains active until corrected rerun passes. No S11b2 completion.
  Next exact: validate expanded test_move.c, add foreign inverse/root/parent/race
  fixtures, public Source move/inverse readback and independent Linux checks.
  Linux S11b1 full refreshed Agent811 sanitizer PASS35640. Goal remains active.
- **S11b2 expanded focused evidence:** corrected inverse fixture334 PASS37667;
  current move510 PASS27875 with source-parent replacement retaining SAME file
  inode/bytes, destination-parent replacement, blocked inverse original name,
  same-byte foreign moved-file replacement and native before-capture race
  restoration. All regular811/110/972/477/334 and action sanitizers PASS50823
  before final additional cases. Public Sources/find/copy/undo/move/inverse/
  readback120 PASS82045. Current Mac full test/sanitize/release running54225;
  independent latest Mac copy path /private/tmp/samosa-native-s11b2-independent-path
  test/sanitize/release/public120 underway. Clean native Linux path
  /private/tmp/samosa-native-s11b2-linux-path, strict GCC regular811/110/972/477/510
  plus action sanitizers PASS; full Agent sanitizer/release/public/cross-device
  still running21012. Next exact S11b2: finish these checks, refresh Linux docs
  and qualify public120/cross-device, then record next bounded assignment for
  Source receipt/freshness reconciliation. No goal completion/legacy retirement.
- **S11b2 Mac final PASS:** current workspace and independent native811/110/972/
  undo477/move510 test/full ASan/UBSan/release PASS54225/17595; independent public
  consumer120 PASS17595. Linux exact current actions.c SHA matches its independent
  copy (1fc56566e5dcb37d50b30fb9e8ccd1e5938f172c4fa3c211466b22c0ebc34c3f),
  regular/action sanitizers PASS; full Agent811 sanitizer underway21012. Refreshed
  Linux docs/contracts/README, public consumer/cross-device runner underway.
  Next assignment pre-coding spec components/samosa-sources/docs/S11c.md records
  Sources-owned bounded live locator reconciliation; implementation not started.
  First exact action after S11b2 finishes: locator.c/h public reconciliation,
  immutable content and mechanical locator IDs kept separate, native Source tests.
  Broad C01-C14 remains open; no engine retained/no actual blocker/no goal stop.
- **Next S11c split recorded before coding:** S11c1.md first binds immutable
  snapshot/media claims and evidence cache provenance to host-configured owner.
  Current Source snapshots/cache are globally deduplicated, so live locator work
  must first avoid cross-owner known-SHA read/provenance reuse. S11c2 implements
  bounded live locators from S11c.md afterward. No implementation started while
  S11b2 full Linux qualification21012 runs. Exact next: finish21012, then native
  Source owner claims/read/cache admission and tests/test_scope.c (S11c1).
- **S11b2 bounded checked move/inverse qualified:** native Linux independent
  811/110/972/477/510 regular/full ASan/UBSan/LeakSan/release plus real distinct-
  filesystem refusal and separately mounted Source public120 PASS21012; Mac
  current/independent full suites/public120 PASS54225/17595. No retained QA
  container/engine. Historical move and inverse receipts, same-inode retention,
  unknown-stage qualification and source/destination parent/race guards verified.
  Broad C10 still lacks schedules/organization/Source live reconciliation.
  **Active S11c1** read docs/S11c1.md: Source owner-bound snapshot/media claims,
  owner-salted private evidence cache and native scope tests. First exact edits
  sources.c Source registration/read/cache authority plus tests/test_scope.c;
  preserve public SHA/evidence IDs and qualified semantic history. S11c2 live
  locators follows. Goal active; no Python and no legacy retirement yet.
- **Active S11c1 native owner scope implementation:** sources.c adds max256
  owner/id/media registration claims, requires those claims for text/PDF/cache
  admission, retains each owner's first locator and salts private cache keys while
  preserving public SHA/evidence IDs. Invalid owner refuses before work. Old
  unclaimed snapshots remain stored but are not automatically assigned authority.
  tests/test_scope.c1140 PASS known-SHA/warmed-cache refusal, Alice/Bob independent
  provenance, exact owner offline snapshot reads, actual256 claims/no eviction and
  cross-owner media refusal before PDF adapter access. Real PDF test176 PASS owner
  PDF registration/read/cache isolation; baseline Source351/inventory743/index593
  PASS27485. Original corruption fixture assumed public EV ID == private cache key;
  initial48149 FAIL, changed owner-private fixture to target actual cached response
  and assert exactly one affected row; corrected351 PASS43769/82859/27485.
  Initial full Mac sanitizers1108 scope/351/743/593 +PDF141 release PASS82859.
  Latest owner/media additions independent Mac test/sanitize/release/PDF176 underway;
  clean Linux path /private/tmp/samosa-native-s11c1-linux-path strict test/sanitize/
  release underway35345. Next exact S11c1: finish both, public Agent interaction
  against independently built current Source and package/docs, then S11c2 locator.
  Goal active; no Python/model process and broad C01-C14 still open.
- **S11c1 bounded owner authority qualified:** independent Mac Source351/
  inventory743/index593/scope1140 full ASan/UBSan/release and actualPDF176 PASS72324.
  Independent native ARM64 Linux351/757/593/1140 full ASan/UBSan/LeakSan/release
  PASS35345. Current independent Source with independent Agent public effects120
  and finding pipeline PASS on both Mac/Linux (explicit separate mounted roots,
  no model/private state access). Latest README explains supported subsets/claims.
  Broad owner/service/cancel/migration gates remain open.
  **Active S11c2** docs/S11c2.md specifies owned locator_reconcile/get, live
  observations versus historical retries, version/inode checks and256/256 quotas.
  First exact implementation: Source src/locator.c/h and dispatch/Makefile, then
  native tests/test_locator.c and public effect receipt reconciliation runner.
  Goal active; no Python, no retained engines/containers, no legacy retirement.
- **Active S11c2 initial native mapper:** src/locator.c/h wired into Source
  dispatch/Makefile. Owner/root-scope/path SHA locator IDs, immutable version claims,
  optional expected physical inode, nofollow64KiB streaming <=1MiB/file/8 files,
  held file/parent + root revalidation, observer checkpoint timestamps/revisions,
  max256 locators/256 operation histories, exact normalized retry before root,
  owner-local locators.lock zero queue and atomic private observations/result.
  get/retry verify saved JSON digests and preserve history; no Source snapshot or
  user file mutation. Native strict build52979 underway; no tests yet, no S11c2
  completion. First exact next: finish compile; qualify unavailable/unsupported
  parent serialization (don't present ancestor inode as requested-file inode),
  ensure lease released before stdout (latest dispatch now buffers), then native
  tests/test_locator.c including actual changed file/directory barrier, quota,
  offline/conflict/owner/root transitions and public action receipt reconciliation.
  Source owner claims1140 +realPDF176 and public effects120 already qualified.
  Broad C01-C14/App/install/fair admission/schedules/wider readers still required.
- **S11c2 focused mapper2608 PASS34207:** tests/test_locator.c verifies full hash
  despite restored metadata, same-byte foreign inode refusal, missing/special/
  symlink/escape, offline exact hits/get, owner hiding/conflicts, actual file and
  directory barrier races with unchanged previous revisions, changed root -> new
  scoped ID and retained prior history, actual256 locator/256 operation caps and
  retained original response/revision225 at capacity. Initial Source full Mac
  test/sanitize/PDF176/release PASS1518 with2545 before extra directory/root cases.
  Buffered mapper dispatch releases lease/root before stdout; ancestor inode not
  mislabeled as requested-file identity. Public identity_constraint/expected tuple
  now attests optional physical checks. Consumer actions_interaction.c extended
  to verify Agent publication/removed/restored receipts through Source observer;
  public runner underway after current Source build13609. Next exact: qualify
  latest2608/full regressions/sanitizers/public runner plus independent Linux/Mac,
  then Agent automatic receipt handoff named S11c3. No S11c2 completion yet.
- **S11c2 bounded live observations qualified:** lost terminal output from initial
  47286/67919 was not used as evidence. Repeated independent Mac full test/sanitize/
  release/actualPDF176 PASS84400, retained /private/tmp/samosa-s11c2-mac-confirm.log;
  independent native ARM64 Linux full test/sanitize/LeakSan/release PASS74506,
  /private/tmp/samosa-s11c2-linux-confirm.log. Latest Source351/inventory743 Mac
  (757 Linux)/index593/owner1140/locator2608 all pass. Public separately invoked
  Agent/Sources receipt observer192 PASS55266. No actual Linux PDF qualification.
  **Active S11c3** Agent docs/S11c3.md recorded before coding: automatic checked
  receipt handoff through public Sources observer, immutable plan-root binding,
  separate physical effects versus Source observation qualifications. Native
  src/reconcile.c/h + actions.c/Makefile initial implementation; strict build65096
  underway. Next exact: focused native validator/transport/real public integration
  tests and existing checked effect/recovery suites; Source error never replays an
  effect. Broad C01-C14 still open. No Python, legacy retirement or goal completion.
- **S11c3 native handoff implemented:** Agent src/reconcile.c/h joins committed
  checked receipts to independently verified public Source observations; exact
  plan-root/host/owner locator IDs, paths, versions and physical tuples validated.
  Result persists separate source_reconciliation; failures never change physical
  effect history. Native fake public transport134 PASS, actual SIGKILL during
  Source call60 PASS (durable copied inode/no replay/owned helper killed), public
  integration220 PASS96779 (copy/move/inverse/wrong Source root/offline exact retry).
  Current full811/110/972/477/510 PASS7273 before adding final focused runners.
  Independent Mac full test/sanitize/release/public220 PASS34915; Linux initial
  14263 failed strict GCC misleading indentation in new native fixtures; corrected
  two line breaks without relaxing warnings. Linux rerun52060 full suites underway
  at test-agent-sanitize. Logs /private/tmp/samosa-s11c3-{mac,linux-corrected}.log.
  Source351/743-or757/593/1140/2608 and actualMacPDF176 remain qualified.
  **Next active S12a** Inference docs/S12a.md recorded before coding: bounded FIFO
  admission with kernel waiter leases, max8 queued/one active, bounded deadline.
  src/admission.c/h + config/dispatcher/Makefile initial implementation; initial
  missing stdio.h compile failure corrected, existing native/session/HTTP tests
  underway89156. Next exact: actual native FIFO/capacity/dead-worker/deadline tests,
  then independent Mac/Linux qualification. S12b scoped cancellation follows.
  S11c3 remains awaiting Linux52060; no unresolved external blocker, no Python,
  no legacy retirement or goal completion. Broad App/install/wider readers open.
- **S11c3 qualified bounded automatic handoff:** independent ARM64 Linux52060
  full regular/ASan/UBSan/LeakSan/release, cross-device and public220 PASS; matches
  independent Mac34915. Native134/60 and existing811/110/972/477/510 all qualified.
  Separate physical history and owner/root-bound observer history remain explicit.
  **S12a qualified FIFO subset:** independent Mac93034 and native ARM64 Linux81169
  full regular/sanitizers/controlledHTTP113/release PASS. FIFO79 proves actual
  accepted order, queue full/deadline, dead queued head/active and slot reuse;
  fake59/session1106 regressions pass. Logs /private/tmp/samosa-s12a-{mac,linux}.log.
  Initial64894 found busy status retained while creating first queued ticket;
  reset only the observed busy result before enqueue, corrected79 PASS75575.
  Gate contention waits only inside configured admission deadline; zero-capacity
  remains immediate busy. Database1MiB/8 tickets/nine lease files, no PID assumptions.
  **Active S12b** Inference docs/S12b.md recorded before coding. operations.c/h
  stores owner-bound normalized operation histories/leases/cancel descriptors;
  dispatcher routes tracked count/generate/get/cancel, FIFO polls its own flag and
  curl aborts only its own transfer. Current initial all79/59/1106/HTTP113 PASS62130.
  Next exact: tests/test_operations.c actual queued cancel/death/owner/retry/quota,
  controlledHTTP active cancellation with last-good session/unrelated request;
  then full independent Mac/Linux and actual configured provider regression.
  Broad C04 install/residency/App/readers/C14 remain open; no Python or goal stop.


### 2026-10-07 resumed goal — timed visual worker qualification

The full Sources/Inference/Agent/App rebuild goal remains active; C01–C14 remain
open. Added optional `--visual` mode to the existing native worker test, routing
all actual worker, active fault/control, peer and caller-death checks through
`sources_native_media_frame`. Added valid-JSON mutations for dimensions, source
dimensions, payload byte count, timestamp upper bound, stream, conversion matrix
and range. Existing malformed base64/range, queried fingerprint binding and
cancelled ingress checks now apply to visual responses. Make worker fault and
sanitizer targets accept `MEDIA_MODE=--visual`; audio remains the default.

macOS normal fault/control and ASan+UBSan suites exited0. Actual worker delivered
a timed frame from `/private/tmp/samosa-s18b-offset-video.mp4`; crash, early exit,
bad protocol/output/result, deadline/cancellation stalls, RSS limit, exact reap,
concurrent healthy visual peer and caller/descendant death passed. Logs:
`/private/tmp/samosa-s18b-visual-worker-{faults,sanitize}-mac.log`.
The default audio fault/control/caller-death regression also exited0; log
`/private/tmp/samosa-s18b-visual-change-audio-regression-mac.log`.

Native ARM64 Linux uses the existing `samosa-s17a-linux-qa` container and a fresh
independent copied source tree `/tmp/s18b-visual-worker-check`, with no sibling
component source. Normal and ASan+UBSan visual fault/control suites passed. Combined normal/
sanitizer command session74680 exited0; the retained sanitizer log confirms all
three worker/control, caller-death and actual response acceptance groups. Logs:
`/private/tmp/samosa-s18b-visual-worker-{faults,sanitize}-linux.log`.
Next: implement public `prepare_visual`
with owner-bound immutable snapshots, distinct visual artifact identities,
timestamp/color provenance and the shared prepared-artifact capacity and integrity
rules. Broader codec/color/orientation qualification and component release remain
required. No full media/component/milestone completion is claimed.


### 2026-10-07 public visual preparation/cache — implementation and qualification

Public `prepare_visual` is wired through core dispatch and durable control,
including describe operations and modality limits. Common media preparation now
selects only encoded-media owner claims for visual reads, rechecks immutable
snapshot SHA and queried decoder artifacts, and emits a distinct visual-contract
artifact identity. Derived packed RGBA output carries original interval and decoder
stream/source timestamp/container origin/source and output dimensions/matrix/range.
It has no audio-local timeline and reports partial preparation coverage. Shared
prepared caches validate digest, identity/provenance and reconstructed frame DTO;
audio and visual identities and quota usage are tested together. Parent validation
now also requires the exact bounded aspect-preserving dimension relation.

The public CLI corpus accepts an optional visual modality; native media cache
checks accept `--visual`. Those checks independently decode the entire public
fixture pixel payload and verify exact 64x64 blue RGBA and source second1. Actual
shared audio/visual quota refusal and distinct artifact IDs are checked. New
fault routing isolates frame-phase stalls/descendants so durable fingerprint and
frame-phase cancellation/deadline checks exercise the intended owned child.
Added reproducible Make public/cache targets and media contract documentation.

macOS initial public CLI normal and ASan+UBSan exited0 (logs
`/private/tmp/samosa-s18b-public-visual-cli{-sanitize,}-mac.log`). Latest normal
worker/cache/control and ASan+UBSan batches session62897 exited0; logs
`/private/tmp/samosa-s18b-public-visual-final-{normal,sanitize}-mac.log`.
Default compressed-audio public CLI and control normal session79032 and
instrumented control session9036 exited0; logs
`/private/tmp/samosa-s18b-public-visual-audio-{cli,control}-regression-mac.log` and
`/private/tmp/samosa-s18b-public-visual-audio-control-sanitize-regression-mac.log`.
Full standard Sources normal/sanitizer session61842 is underway against the shared
preparation changes; descriptor-only visual limit additions are newer than the
initial normal binary. Confirm terminal output and rebuild public describe.

Initial isolated native ARM64 Linux tree `/tmp/s18b-public-visual-check` public
CLI/cache/control normal passed. Combined command session39926 is still live
through instrumented public/cache/control; logs
`/private/tmp/samosa-s18b-public-visual-{build,cli,control,sanitize-build,control-sanitize}-linux.log`.
Its copied source predates the final parent dimension relation and updated test
labels. Finish this exact handle, then copy latest source/test/Makefile and qualify
that final snapshot, including full standard regressions and default audio.

Public preparation/cache implementation is complete; broader codec/color/
orientation/timeline corpus, larger uploads, Inference analysis and independent
complete optional release are still required. The full rebuild goal and C01–C14
remain open. No legacy production switch or milestone closure is implied.


Final public visual qualification update: initial Linux session39926 exited0,
including instrumented public CLI/cache/control. Latest copied-production Linux
batch session45590 proves final parent dimension relation and final public
visual normal/ASan+UBSan worker/cache/control (all PASS in
`/private/tmp/samosa-s18b-public-visual-final-{normal,sanitize}-linux.log`). Default
audio regressions have progressed to instrumented control; full standard normal/
sanitizer regressions follow in the same exact live handle. Mac full standard
session61842 exited0; log
`/private/tmp/samosa-s18b-public-visual-full-regression-mac.log`. Latest public CLI
and configured-absent describe session21880 exited0, exposing visual operations
and bounds; logs `/private/tmp/samosa-s18b-public-visual-final-cli-mac.log` and
`/private/tmp/samosa-s18b-public-visual-describe-mac.json`.

Expanded visual corpus: new native `test_media_video_corpus.c` compares complete
RGBA against actual native per-platform FFmpeg reference through both primitive
and owned worker, and checks the selected timestamp and dimensions. Four fixtures
cover 960x540 MPEG-4 scaled512x288, patterned768x432 H.264/BT.709 scaled512x288,
full-range300x600 MJPEG/MOV scaled256x512 and320x180 HEVC/MP4. Both Mac session94620
and isolated Linux session49513 exited0 with eight PASS groups each (four normal,
four ASan+UBSan), using actual configured instrumented helpers. Logs
`/private/tmp/samosa-s18b-visual-corpus-{mac,linux}.log`; native references
`/private/tmp/samosa-s18b-{wide-mpeg4,wide-h264,portrait-mjpeg,hevc}-reference-{mac,linux}.rgba`.
Linux corpus tree `/tmp/s18b-visual-corpus-check` contains no sibling components.

A reproducible owned shell fixture generator now builds originals with native
FFmpeg, derives exact platform references and runs the same native test. Make
`test-media-video-corpus MEDIA_CORPUS_DIR=/explicit/output` and
`sanitize-media-video-corpus MEDIA_CORPUS_DIR=/explicit/output
MEDIA_HELPER=/absolute/instrumented/worker` preserve isolated generated artifacts.
Mac generated normal session36232 exited0; instrumented session90824 is running.
Linux generated normal/instrumented combined session10975 is running. Logs
`/private/tmp/samosa-s18b-visual-corpus-generated{-sanitize,}-{mac,linux}.log`.
Only the shared generation script/test/Make targets are newer than the last copied
Linux public production snapshot; Sources runtime sources have not changed.

Continue polling exact45590/90824/10975 handles. Confirm Linux full standard
regressions and generated corpus runners before updating qualification. Then
extend codec/timeline/orientation/color corpus and full optional packaging; larger
uploads/live freshness, acquisition/search, indexing/enrichment/semantic/Chutni,
public Inference media adapters and authenticated bounded service still remain.
The full four-component rebuild and C01–C14 remain active and incomplete.


Generated corpus confirmation: Mac instrumented session90824 and Linux combined
normal/instrumented session10975 exited0. Each native generated normal or
instrumented log contains four complete primitive/worker reference acceptance
checks. The original per-platform eight-check corpus also passed. Linux main
qualification session45590 is still live in the final standard sanitizer suites;
continue that exact handle, with log
`/private/tmp/samosa-s18b-public-visual-full-regression-linux.log`. The normal/
instrumented default compressed-audio regression within45590 passed before these
full suites; log `/private/tmp/samosa-s18b-public-visual-audio-regression-linux.log`.
No runtime source edits are pending; the new optional corpus test and generation
script are built and qualified on both platforms. Broader media and component
completion remain open.


Final full regression confirmation: exact Linux session45590 exited0. The final
standard Sources normal/ASan+UBSan suites, latest visual worker/cache/control and
normal/instrumented default compressed-audio regressions all pass against the
copied production snapshot. Mac equivalents pass as recorded above. Both normal
minimal executables were checked directly with `otool -L` / `readelf -d`: only
system SQLite/libc are needed; FFmpeg remains optional, with no sibling component
linkage. Shell syntax and whitespace checks pass. No required processes are still
running for this checkpoint. Next is broader media orientation/color/timeline
qualification and optional independent packaging, followed by the remaining full
Sources scope. The four-component/C01–C14 goal stays active.


### 2026-10-07 — orthogonal visual presentation

Previous goal turn was progress: public visual/cache behavior and actual codec
reference qualification changed authoritative state and passed both platforms.
Current workspace/checkpoint confirmed that no prior qualification job remained
live before this turn. The next identified correctness issue was display matrices
being ignored by visual capture.

Implemented frame-priority/stream-fallback display matrix reading through native
FFmpeg APIs: codec parameter side data on current libavformat, stream side data
on the declared FFmpeg5.1 Linux SDK. Exact unit orthogonal matrices map to eight
integer transforms. Packed RGBA conversion/scaling precedes orientation, which
permutes pixels without interpolation, polls cancellation per row and retains
source dimensions/timestamp/origin. Translation is rebased; non-unit affine/
shear/perspective receives `media_orientation_unsupported`. Required DTO
orientation and exact swapped output shape are validated in the parent and cache.
Native error routing accepts the typed refusal. Worker-v3 fingerprint contract
binds the changed method and actual images. No extra runtime library is added.

An initial default-autorotate CLI comparison exposed different chroma edge pixels
(maximum delta169): CLI automatic orientation transforms subsampled YUV before
conversion, while Sources deliberately converts/scales RGBA before its integer
permutation. Kept the explicit Sources method and changed the independently
executed native reference to that declared order; documented the difference.
No claim that default-autorotate pixels equal this method is made.

Owned native fixture writer patches the matrix of a native generated one-track
patterned768x432 H.264/BT.709 MP4, preserving compressed pixels. Normal and
instrumented reference harnesses now verify exact pixels, dimensions, source
timestamp and orientation through primitive, owned worker, public prepare and
cache for every case. All eight transforms plus a shear refusal with no artifact
and exact offline durable public retry pass on macOS. Final normal session95946
and instrumented/corpus session2883 exited0; logs
`/private/tmp/samosa-s18b-orientation-final-{normal,sanitize}-mac.log` and
`/private/tmp/samosa-s18b-orientation-codec-regression-mac.log` (existing four
codecs now also checked through public API/cache normally and under sanitizers).
Mac worker/caller-death/control/cache session48408 exited0; logs
`/private/tmp/samosa-s18b-orientation-{control-build,cache-control,cache-control-sanitize}-mac.log`.
Worker sanitizer session37653 exited0. Default compressed-audio CLI/control
instrumented session51028 exited0; log
`/private/tmp/samosa-s18b-orientation-audio-regression-mac.log`.

Isolated Mac `/private/tmp/s18b-orientation-mac-check` full standard normal and
ASan+UBSan session10824 exited0; log
`/private/tmp/samosa-s18b-orientation-full-regression-mac.log`.
Initial independent Linux `/tmp/s18b-orientation-check` session9858 exited0,
including eight normal/instrumented public/cache orientation cases and worker
controls; its refusal harness predated the extra public durable refusal assertion.
Final copied source/test snapshot matches current workspace (`diff -qr` on actual
source/tests). Linux final optional normal/sanitizer/corpus/control/audio
session34079 and separate full-standard normal/sanitizer session30846 are live.
Logs `/private/tmp/samosa-s18b-orientation-final-{normal,sanitize}-linux.log`,
`/private/tmp/samosa-s18b-orientation-audio-regression-linux.log` and
`/private/tmp/samosa-s18b-orientation-full-regression-linux.log`.
Keep polling exact handles; do not restart solely because observation expires.

Remaining full Sources work still includes broader media color/timeline/format
coverage and optional package qualification, larger uploads/live freshness,
acquisition/search, indexing/enrichment/semantic/Chutni, public Inference media
adapters and authenticated bounded service. Full Inference/Agent/App/assembly/
legacy-retirement/C01–C14 scope remains active and incomplete.


Orientation final qualification progress: independent Linux full standard
normal/ASan+UBSan session30846 exited0. Final optional session34079 has passed
expanded public orientation/shear-refusal normal and instrumented cases, the
existing four-codec instrumented public/cache corpus, visual worker fault/control
and visual cache/control, and has entered its final compressed-audio instrumented
control regression. Retained logs above show exact source/test comparison with
workspace, nine orientation/reference/refusal cases and bounded cache/peer/
durable retry behavior. Continue exact34079 until its terminal result; no other
required job remains live in this checkpoint. Full Sources/four-component rebuild
and milestone closure remain unproven.


Orientation final confirmation: exact Linux session34079 exited0, including its
final default compressed-audio normal public CLI and instrumented control/cache
regression. The retained final-normal log has19 PASS groups; final-sanitizer log
has21 PASS groups, covering orientation/reference/refusal, visual worker/control,
four-codec public/cache corpus and prepared-cache controls. These counts are
acceptance groups, not a measured completion percentage. Source/test snapshots
match current workspace as recorded above. Mac equivalents and isolated full
standard regressions pass both platforms. Shell syntax and whitespace checks
pass; no qualification job remains live for this checkpoint.

This closes the bounded orthogonal orientation work only. Broader color/timeline/
format coverage and independent optional packaging remain next; remaining full
Sources and Inference/Agent/App/assembly/retirement/C01–C14 scope stays active.


### 2026-10-07 — expanded media color/timeline and source archive acceptance

Previous turn was progress: orthogonal orientation implementation and real
normal/sanitizer qualification changed authoritative runtime state. This turn
confirmed its terminal checkpoint before starting new work.

Expanded the native generated corpus from four to ten cases: existing MPEG-4,
H.264/BT.709, full-range MJPEG and HEVC plus VP9/MP4, full-range H.264, 10-bit SDR
BT.2020 HEVC, delayed-video/earlier-audio, video absent from selected interval,
and sparse variable frame rate. Positive cases independently check the expected
source timestamp (delayed1.2s and VFR1.12s), complete native reference pixels,
public metadata and exact cache hits. Negative uncovered selection checks typed
refusal, no artifact and exact offline durable retry.

Initial Linux delayed fixture exposed encoder-default frame duplication: native
ffprobe showed video starting0 despite the requested1.2s input offset. This was
fixture behavior, not a Sources decoder defect. Added explicit `-fps_mode
passthrough` to retain track timing on both CLI generations. Current macOS CLI
has removed `-vsync`; sparse generation now uses supported `-fps_mode vfr`.
Kept strict independent timestamp assertions and diagnostic mismatch reporting.
No Sources runtime source was changed by these corpus updates.

Final Mac session33386 and native ARM64 Linux session97909 exited0. Each normal
and ASan+UBSan log contains ten complete acceptance groups. Logs
`/private/tmp/samosa-s18b-timeline-color-final-{normal,sanitize}-{mac,linux}.log`.
Linux isolated source tree `/tmp/s18b-timeline-color-check` has no sibling sources;
instrumented helper is the already-qualified current production orientation
helper. These matrix/range conversion cases do not claim HDR tone mapping or
broader gamut/transfer conversion.

Source packaging now includes Apache2 LICENSE copied byte-for-byte from the
repository root, alongside existing vendor licenses. Independent Mac release
session81065 and Linux release session22706 exited0 after full normal standard
release tests. Roots `/private/tmp/samosa-s18b-package-mac` and
`/tmp/samosa-s18b-package-linux`; logs
`/private/tmp/samosa-s18b-package-{mac,linux}.log`.

Archives:
- `/private/tmp/samosa-s18b-package-mac.tar.gz`, SHA256
  `48608b2920815f6f53bb9cd6a715d36a0f87733fd147ef3f91cf2c4e8fe219e3`.
- `/private/tmp/samosa-s18b-package-linux.tar.gz`, SHA256
  `037bbb208c680669d03caff77acf75638f20e10fea3c32a59f7ec9d9bceb6f57`.

Extracted archives are under `/private/tmp/samosa-s18b-archive-{mac,linux}` on
host and `/tmp/samosa-s18b-archive-linux` inside Linux. Direct recursive comparison
before builds and a127-file SHA256 manifest prove identical extracted contents
and agreement with the captured workspace. Manifest
`/private/tmp/samosa-s18b-package-manifest.json`. Archive byte hashes differ due
to packaging metadata; content equivalence was checked directly.
Mac extracted acceptance session40192 and Linux14929 exited0: both minimal and
optional executables rebuild, and separate compressed MP3/visual CLI processes
pass preparation, cache reuse, owner hiding, malformed/no-artifact refusal and
exact offline durable retries. Logs
`/private/tmp/samosa-s18b-archive-acceptance-{mac,linux}.log`.
No qualification jobs remain live. Neither archive claims complete Sources or
milestone closure; final whole-component release is still required.

Next available implementation work is larger bounded immutable uploads and then
complete live-file freshness/watch. Initial audit confirms the current original
limit is a shared1MiB macro across store/readers/native protocol, SQLite limits,
registration allocation/read loop and describe metadata. Registration currently
allocates maximum capacity and issues one maximum-length read. Expansion must
coordinate those bounds, chunked interrupted copying, ownership/snapshot checks,
resource accounting, reader budgets and durable cancellation rather than merely
changing describe output. Current indexing/live-locator bounds remain separate.
Keep the full Sources/Inference/Agent/App/assembly/retirement/C01–C14 goal active.

## S19a larger originals checkpoint — 2026-10-07

Sources accepts original snapshots up to64MiB across shared storage/readers/native
worker bounds. Registration allocates observed size+1 and copies in cancellable
64KiB chunks, refuses growth/identity changes, and checks interruption before
transaction commit. All seven registration formats now accept durable controls.
Total snapshots128MiB, database160MiB, cache8MiB and existing document/raster/
packet/selection bounds remain independently enforced. Full Sources remains open.

Normal and ASan/UBSan focused checks passed macOS and isolated ARM64 Linux:
4MiB+ text/cancellation/owner refusal/offline tracked retry/exact EOF evidence;
4,000,044-byte WAV with exact independently decoded samples at60s/cache reuse;
64MiB+1 refusal; actual64MiB text registration and exact immutable EOF evidence.
Logs: `/private/tmp/samosa-s19a-boundary-mac.log` and
`/private/tmp/samosa-s19a-boundary-linux.log`. Completed handles96502/66613 exit0.
Native tests are included in standard test/sanitize targets. Linux isolated copy:
`/tmp/s19a-large-check` in existing `samosa-s17a-linux-qa`.

Full pre-existing normal/sanitizer regressions passed both platforms:
`/private/tmp/samosa-s19a-full-mac.log` and
`/private/tmp/samosa-s19a-full-linux.log` (completed41820/83546 exit0).
Public visual preparation of a4MiB MP4 passed normal and both instrumented parent
and worker on both platforms, including malformed refusal, snapshot delivery,
cache reuse and offline saved retry. Logs:
`/private/tmp/samosa-s19a-large-media-mac.log`,
`/private/tmp/samosa-s19a-large-media-linux.log`,
`/private/tmp/samosa-s19a-large-media-sanitize-mac.log`,
`/private/tmp/samosa-s19a-large-media-sanitize-linux.log`.
Completed79879/61871 exit0; no qualification process remains live.

The4MiB media original expands the prequalified offset-video MP4 with a valid
`free` box; this proves bounded original transport, not dense codec complexity.
Native generator reproduction:
`build/test-media-orientation-fixture /private/tmp/samosa-s18b-offset-video.mp4 /private/tmp/samosa-s19a-large-video-native.mp4 --pad 4194304`.
`cmp` verified exact identity with the original tested padded fixture.
S18b qualified source archives remain unchanged; S19a workspace is newer and has
not received a new independent archive release. Contract/README/current worker
notes now reflect64MiB. Historical scope records retain their original limits.

Next: larger-upload lifecycle and complete freshness/watch semantics, including
failure/race/resource qualification. Acquisition/search, broader indexing and
Chutni, public Inference media analysis, authenticated bounded service/shutdown,
complete Sources release, remaining components and all C01–C14 remain open.

## S19b live observation controls — 2026-10-07

Previous goal turn made concrete S19a implementation/qualification progress.
Current source inspection found locator hashing inherited the64MiB source bound
but omitted host cancellation/deadline checks. Added checks before writer
admission, between observations, each64KiB hashing chunk and immediately before
transaction commit. An interrupted batch cannot replace committed history.
Exact successful retry remains before root access or new observation controls.

Extended native maximum-source test: cancel reconciliation of actual64MiB text,
assert no observations, repeat same operation successfully and require revision1,
available status and67108864 checked bytes, delete original, then require exact
offline saved history. Normal/ASan+UBSan large-source tests pass both platforms.
Logs `/private/tmp/samosa-s19b-locator-mac.log` and
`/private/tmp/samosa-s19b-locator-linux.log` contain successful tests followed by
an initial missing focused Make target; added focused test-locator/sanitize-locator
targets and qualified separately. Initial command exits2 reflect that target
setup error, rather than test failures.

Existing macOS locator regression passes normally and under ASan/UBSan,2608
checks each: `/private/tmp/samosa-s19b-locator-regression-mac.log` (58430 exit0).
Linux qualification: `/private/tmp/samosa-s19b-locator-regression-linux.log`.
Contract corrects per-batch maximum to512MiB (eight64MiB originals), read through
64KiB buffers. This does not claim continuous watch, public locator cancellation,
live-read freshness gating or complete Sources. Those remain required.
Linux normal and ASan/UBSan locator regressions also completed successfully,
2608 checks each (96947 exit0). All S19b qualification handles are terminal.

## S19c live reads checkpoint — 2026-10-07

Previous turn made concrete S19b progress. Current code now exposes locator_read:
validate owner/source/version and nested reader operation, verify current live
content plus optional physical constraints, read the immutable snapshot, and
revalidate held file/parent, directory bindings and root before returning wrapped
evidence. Any freshness race discards evidence, including a warm-cache result.
No observation history is committed or used to bypass a later live verification.
Nested readers cannot choose roots/owners, add operation controls or recurse.

Focused native checks pass macOS/Linux normally and under ASan/UBSan: stable text
evidence, actual mutation after hashing before warm-cache extraction, changed and
missing file refusals, foreign owner refusal, missing request validation, retained
immutable citations after original deletion/offline root. Existing locator suite
also passes2608 checks per mode/platform. Logs:
`/private/tmp/samosa-s19c-live-final-mac.log` (85293 exit0),
`/private/tmp/samosa-s19c-live-linux.log` (94691 exit0).
Prior focused macOS run42529 also passed;94878 locator regression completed.

Wrapper supports existing text/HTML/DOCX/PDF/OCR/audio/visual reader operations;
focused runtime qualification presently covers text, not all modalities. Original
stream bound64MiB, nested evidence2MiB, wrapper4096 bytes. Contracts/S19c scope and
standard test/sanitize targets updated. Descriptor advertises locator_read and
live_read_response_bytes2101248. Descriptor-only change follows focused suites;
no new archive or broad release claim. Watchers/index invalidation, public durable
live-read control and broader freshness/worker failure tests remain required.
All prior C01–C14/full-component release obligations remain open.

## S19d durable live reads — 2026-10-07

Previous goal turn made S19c implementation/qualification progress. Added
locator_read to shared durable request control, including operation_get/cancel,
parent/deadline propagation, normalized nested readers and exact saved retries.
Normalization permits only primitive nested reader fields; recursive requests or
nested controls are refused. Corrected tracked registration normalization so
expected_file_identity is a string as required by core registration.

Extended native live tests cover tracked success, offline retry after live
content changes, reordered nested-key retry, changed-selector idempotency_conflict,
new ordinary request detecting live change, actual operation_cancel during read,
no returned evidence, saved cancelled status/retry, and saved deadline failure.
Initial runs76313/29416 exited2 because the new test expected operation_conflict
instead of existing idempotency_conflict; corrected the test expectation.

Live/control normal+ASan/UBSan pass on both platforms:
`/private/tmp/samosa-s19d-control-final-mac.log` (79411 exit0),
`/private/tmp/samosa-s19d-control-final-linux.log` (80473 exit0).
Existing request control135 checks per mode/platform passed. Subsequently aligned
persisted result cap with live wrapper allowance:2101248 bytes, retaining32MiB
aggregate reservation bounds and2MiB default non-live control response bound.
Live tests and existing durable-operation suites pass normal/sanitizers
on macOS after this change (723 normal/825 sanitizer checks): `/private/tmp/samosa-s19d-result-bound-mac.log`
(29714 exit0). Linux corresponding run27673 remains the exact qualification
handle until terminal; log `/private/tmp/samosa-s19d-result-bound-linux.log`.
Contracts and S19d scope describe saved retries as their original observation,
not a new freshness claim. Broad modality/watch/index/release work remains open.
Final Linux live and durable-operation normal/ASan+UBSan suites completed,
723 normal/726 sanitizer operation checks (counts include polling assertions),
27673 exit0. No S19d qualification process remains
live. This turn made implementation and verified-test progress.

## Live-read binding qualification — 2026-10-07 (S19e)

Previous goal turn made S19d production/test progress. Added native live-read
regressions that perform real rename/replacement of the selected file, parent
directory and configured root during a warmed immutable-cache extraction. State
lives outside the replaced root. Replacement trees contain identical bytes, so
content equality cannot mask broken descriptor/path bindings. Each in-flight read
returns source_changed with no evidence. A new content-only observation accepts
the stable replacement; an expected_device/expected_inode-pinned observation
refuses it. Historical immutable evidence remains readable with the root offline.

Extended live suite passes normal and ASan/UBSan on both platforms:
`/private/tmp/samosa-s19e-bindings-final-mac.log` (29721 exit0),
`/private/tmp/samosa-s19e-bindings-linux.log` (47171 exit0).
Initial macOS binding-only run94808 also passed; final run includes explicit
historical inode constraints. These tests exercise actual filesystem changes
through the host cancellation callback between hashing and nested extraction,
without production test hooks. No production change was needed; stronger evidence
qualifies the previously implemented binding checks. All handles are terminal.

Next remains continuous Sources-owned watching and index freshness integration,
plus modality/worker-death freshness qualification. Upload lifecycle, acquisition,
semantic/Chutni retrieval, public Inference media, bounded authenticated services,
independent full releases, remaining components and C01–C14 remain open.

## Index capture freshness — 2026-10-07 (S19f)

Previous goal turn qualified S19e actual binding races. Inspection found text
index capture performed inventory-identity-bound registration then plain immutable
read, leaving capture-time live freshness unchecked. Index capture now invokes
public locator_read and stores its inner literal evidence, retaining immutable
IDs/selectors/coverage and existing search integrity/offline semantics. Failed
live verification commits a failed item without stale candidate evidence.

Native regression calibrates the final host control checkpoint during a stable
single-item extraction, then mutates the actual file at the same checkpoint in a
new equivalent isolated fixture. It verifies zero indexed items, one failed item,
zero stale candidates and exact offline completed-step retrieval. Stable fixture
still indexes and retrieves literal evidence. Calibration includes no production
fault hooks and runs separately under each platform/instrumentation mode.

macOS focused normal/ASan+UBSan passed (92805 exit0):
`/private/tmp/samosa-s19f-freshness-mac.log`.
Existing index593-check suite passed normally (84787 exit0) and under sanitizers
(94555 exit0): `/private/tmp/samosa-s19f-index-mac.log`,
`/private/tmp/samosa-s19f-index-sanitize-mac.log`.
Initial Linux5542 build exited2 on GCC misleading-indentation warning in new test
helper; split the statements and reran exact qualification. Linux run69626 log:
`/private/tmp/samosa-s19f-freshness-final-linux.log`.
New focused tests join standard test/sanitize; contracts/S19f scope updated.
This closes capture-time freshness only, not watching or invalidating already
indexed snapshots. Sources/full rebuild/C01–C14 remain active and incomplete.
Linux69626 completed exit0: focused freshness and existing593-check index suite
pass both normally and under ASan/UBSan. All S19f qualification handles are
terminal. No archive release or full Sources completion is claimed.

## Observed index freshness — 2026-10-07 (S19g)

Previous turn implemented/qualified S19f capture-time freshness. New index jobs
now persist the configured root scope independently of selected subdirectory,
checked before/after inventory capture. Search candidates include live_observation
from the latest owner/root/path-bound committed locator record. Available hashes
are compared to the candidate version; different actual versions qualify changed.
Changed/missing histories preserve immutable evidence. Legacy jobs lacking root
scope are explicitly unobserved, never inferred from the current configured root.

Shared locator schema initializes before index transactions. Locator history
result/digest and recorded path/scope/status/revision/time are validated before
candidate publication. Candidate/index/history retrieval uses one read transaction.
No new filesystem verification or continuous-current claim is made by search.

Native regression covers subdirectory mapping, unobserved/available/changed/missing
transitions, offline reads, foreign-owner/host observations, and root replacement.
Capture-race regression remains in the same suite. Initial86125/68433 runs exited2
because host-isolation test reused a prior operation identifier under a new host;
existing idempotency_conflict correctly refused it. Gave the independent host
observation a new identifier and reran; no production fix was needed for that test.

macOS normal/ASan+UBSan focused freshness,593-check index and2608-check locator
suites passed: `/private/tmp/samosa-s19g-observation-final-mac.log`
(89689 exit0). Linux corresponding exact handle47511/log
`/private/tmp/samosa-s19g-observation-final-linux.log` remains authoritative until
terminal. Earlier macOS38705 index run passed before final schema-initialization
refinement. Contracts/S19g scope document committed-history semantics and legacy
unobserved behavior. Continuous watch, fresh-only policy and reindex scheduling
remain required; Sources and all C01–C14 remain incomplete and goal active.
Final Linux47511 completed exit0: focused freshness,593-check index and2608-check
locator suites all pass normal/ASan+UBSan. No S19g process remains live. No source
archive release or complete component claim accompanies this checkpoint.

## Durable watch registry — 2026-10-07 (S19h)

Previous goal turn implemented/qualified observed index freshness. Added public
locator_watch_create/get/stop. Registry validates normalized1..8 locators, retained
owner-bound versions and host/root grants; stores registration/digest; supports
same registration offline and owner-isolated status/stop. Exact retry is checked
before root access and again after kernel lease admission, closing duplicate
pre-lease races. Current lifecycle state is registered/stopped and monitoring is
explicitly not_running. Stopped registrations retain history/capacity slots.

Eight registrations globally, shared one-writer/zero-queued locator lease,
64KiB registration result and existing database cap. Creation does not read live
content; missing originals may be registered against retained immutable versions.
No continuous/background freshness claim is made from registry presence.

Native watch and existing2608-check locator suites pass normal/ASan+UBSan on both
platforms: `/private/tmp/samosa-s19h-watch-mac.log` (3307 exit0),
`/private/tmp/samosa-s19h-watch-linux.log` (17285 exit0).
Initial macOS main build27374 completed exit0 before final duplicate-lease check;
final suite rebuilt the updated implementation. New tests join standard checks;
watch contract and S19h scope distinguish registry from continuous monitoring.
Descriptor now lists all three operations and eight-watch bound.

Next: Sources-owned continuous watch runtime and bounded checkpoints, root-change
handling, worker admission/shutdown, removal/replacement and reindex scheduling.
Full upload lifecycle, acquisition/search, broad indexing/enrichment/Chutni,
public Inference media, authenticated service, complete independent release and
remaining components/C01–C14 all remain open. Goal is active, not complete.
Final macOS main/descriptor and index-freshness smoke check41292 completed exit0:
`/private/tmp/samosa-s19h-describe-index-mac.log`,
`/private/tmp/samosa-s19h-describe.json`. Parsed descriptor confirms all watch
operations and locator_watches8. All S19h handles are terminal; no new archive
release is claimed. This turn made production/test progress toward full watching.

## Atomic watch checkpoints — 2026-10-07 (S19i)

Previous goal turn implemented/qualified S19h registry. Added locator_watch_step
with expected_cycle, registered root/host verification, bounded8-row observation,
final binding checks and atomic locator/checkpoint commit. One latest checkpoint
per watch replaces polling history; regular locator_reconcile histories remain
unchanged. Shared writer lease admits one observer, zero queued. Last immediately
preceding-cycle retry returns saved history offline; other cycles conflict.
Watch get/create expose checkpoint and stopped watches refuse stepping.

Native watch test executes261 cycles (beyond256 regular operation-history cap),
available-to-missing transition, offline saved retry, stale-cycle conflict and
stop refusal. Additional regression cancels the missing-file observation at
precommit and verifies prior cycle261 survives. Owner-local corruption fixture
changes checkpoint digest and requires one parseable corrupt_locator response
with no partial checkpoint. Checkpoint watch ID/stored/published cycle agreement
is also validated in production. Registration checkpoint integrity validates
before response output, avoiding partial JSON on a corrupt checkpoint.

Initial watch/locator normal+ASan+UBSan suites passed macOS16321 and Linux15045,
including2608 locator checks; logs `/private/tmp/samosa-s19i-checkpoint-mac.log`
and `/private/tmp/samosa-s19i-checkpoint-linux.log`. Additional macOS75123 tests
passed. Linux93667 build hit GCC format-truncation in new corruption-test database
path; enlarged that fixture buffer and reran final qualification. Final exact
handles/logs:25917 `/private/tmp/samosa-s19i-checkpoint-qualified-mac.log`,
57777 `/private/tmp/samosa-s19i-checkpoint-qualified-linux.log`.
No terminal status is inferred from logs alone; inspect the handles.

Continuous loop/admission/shutdown is not yet implemented; monitoring remains
not_running for one-shot checkpoint work. Removal/replacement, reindex policy,
full Sources/release and all remaining components/C01–C14 remain required.
Final25917/57777 handles completed exit0: latest watch/checkpoint tests pass
normal/ASan+UBSan on both platforms. All these qualification handles are terminal.
No source archive or full watching completion claim is made.
Final main/descriptor and index-freshness smoke66901 completed exit0:
`/private/tmp/samosa-s19i-index-describe.log`, parsed
`/private/tmp/samosa-s19i-describe.json` advertises locator_watch_step.
All S19i handles are terminal. This turn made checkpoint implementation and
verified-test progress; next remains the continuous runtime.

## Continuous watch runtime — 2026-10-07 (S19j)

Previous goal turn implemented/qualified atomic watch checkpoints. Added Sources
CLI --watch-id/--watch-interval-ms runtime with owner-bound admission, private
per-watch kernel lease, duplicate refusal, latest-checkpoint recovery and public
bounded watch steps. Intervals100..60000ms(default1000); each attempt has a five-
second deadline additionally constrained by host control. Temporary contention,
conflict/race/deadline failures retain checkpoints and retry after the interval.
Permanent scope/corruption/state errors stop. Root/state/lease bindings do not
silently rebind. SIGTERM/SIGINT interrupt hashing/precommit and waiting; public
stop ends the loop. SIGKILL lease release permits restart from durable state.

Public monitoring now derives from a checked kernel lease: running/not_running/
unknown. It does not imply observation completeness/currentness. Checkpoint
observed_at_ms and per-locator status remain authoritative. CLI emits terminal
status/error, not an unbounded per-checkpoint stdout stream. The eight retained
registrations bound distinct workers; broad fair scheduling remains required.

Native actual-process tests pass normal and ASan+UBSan on both platforms, including
fully instrumented CLI workers: running/duplicate, content change/missing file,
SIGTERM, SIGKILL restart, public stop, actual root/state replacement refusal and
retained checkpoints. Latest registry/checkpoint suites also pass in both modes.
Logs `/private/tmp/samosa-s19j-runtime-final-mac.log` (40581 exit0),
`/private/tmp/samosa-s19j-runtime-final-linux.log` (47036 exit0).
Earlier20000/24247 suites passed before root/state tests. Initial56078 build lacked
Darwin feature declarations in watch.c; added _DARWIN_C_SOURCE before headers.
Monitor status is sampled once before quoting (avoiding a state-change length race).
All final qualification handles are terminal. New native tests join standard
checks; contract/README/S19j scope updated. No full component/archive release yet.

Remaining: multiwatch fairness/resources, failure health reporting, removal and
replacement, automatic worker lifecycle/service integration and reindex scheduling;
full upload lifecycle, acquisition/search, broader indexing/Chutni, public Inference
media, authenticated bounded service/full Sources release; remaining components,
assembled qualification, legacy retirement and all C01–C14. Full goal stays active.

## Watch health and eight-worker workload — 2026-10-07 (S19k)

Previous goal turn qualified continuous watch runtime. Added component-private
bounded durable health rows for completed attempt/success/failure counts, latest
attempt time and latest error; public status exposes these beside lease/checkpoint.
Successful report clears last_error; transient/permanent step errors record their
code without replacing prior checkpoints. Checkpoint and health commits remain
separate, allowing lag if killed between them. No public counter-writing operation
exists. Counts saturate at INT64_MAX; health types/ranges/count agreement/error
identity validate before any status publication.

Native process tests admit eight simultaneous workers, require successful reports
from every worker, publicly stop one and verify its remaining peers stay running.
Existing content/signal/SIGKILL/root/state tests remain. Actual root replacement
must persist scope_unavailable. Owner-local negative health-counter corruption
must yield a single parseable corrupt_locator response without partial checkpoint.

Initial macOS15785 test encountered the specified busy refusal while registering
additional watches under active observers; added bounded5s retry of busy/store_busy
in the test, matching public zero-queue admission. Six children were observed via
ps in fixture samosa-multiwatch-G9OI8Y; by scoped termination attempt all exact PIDs
were already gone. Subsequent ps confirmed no failed-fixture worker remained.
No unrelated process was killed. Linux58837 initial eight-worker suites passed.
Final bounded-retry macOS44732 runtime/registry/checkpoint normal/sanitizer passed;
logs `/private/tmp/samosa-s19k-multiwatch-final-mac.log`.

Latest health-validation/corruption and eight-worker suites pass normally and
with fully instrumented CLI/test binaries on both platforms:
`/private/tmp/samosa-s19k-health-qualified-mac.log` (96966 exit0),
`/private/tmp/samosa-s19k-health-qualified-linux.log` (78222 exit0).
17786 initial health runtime also passed. All recorded qualification handles are
terminal. Contracts/S19k scope updated. This proves observed workload progress and
peer isolation, not general starvation freedom/FIFO/resource qualification.

Watch removal/replacement, long-running resources/fair scheduling, automatic worker
lifecycle/service integration and reindex scheduling remain required. Full uploads,
acquisition/search, broader indexing/Chutni, public Inference media, bounded service,
complete Sources/release, other components/legacy retirement/C01–C14 remain open.
The full goal stays active; no new archive release or component completion claim.

## Watch removal/replacement — 2026-10-07 (S19l)

Previous goal turn implemented/qualified watch health and eight-worker workload.
Added locator_watch_remove for owned stopped registrations. It acquires the
observer writer lease, excludes runtime leases, atomically deletes registry/
checkpoint/health rows and cleans the held private lease inode. Immutable source,
cache, locator and index history remains. Shared durable controls provide saved
removal retry after the registry entry disappears. Active/foreign/held-lease
refusals precede deletion; successful cleanup frees watch capacity.

New watch lifetimes include32 bytes of OS entropy in their opaque identity, so
same-parameter recreation cannot attach an old worker. Existing create retries
remain stable while a registration exists; removal ends that retained identity.
Stopped runtime admission creates no lease. A late admitted worker discovering
unknown_watch cleans its own held/bound private lease and exits. Cleanup reports
complete/retained; crash-time orphan cleanup remains an explicit qualification gap.

Native watch regression qualifies active/foreign refusal, stopped removal, saved
offline retry, old-ID refusal, capacity recovery, same-parameters fresh identity
and retained immutable text. Additional test holds the actual private runtime lease,
requires watch_busy, releases it and verifies successful removal unlinks the inode.
Final watch/runtime normal+ASan+UBSan passed both platforms:
`/private/tmp/samosa-s19l-removal-final-mac.log` (27475 exit0),
`/private/tmp/samosa-s19l-removal-final-linux.log` (21248 exit0).
Additional held-lease suites passed both modes:
`/private/tmp/samosa-s19l-lease-removal-mac.log` (3579 exit0),
`/private/tmp/samosa-s19l-lease-removal-linux.log` (4322 exit0).
Earlier50143/83424 pre-generation suites also passed.

Inspection found shared durable finalization could replace a completed removal
receipt with late cancellation/deadline failure. Added a component-private committed
result flag, set only after successful removal. Finalization preserves that receipt;
ordinary read results retain late-cancellation checks. New isolated native operation
regression cancels a marked committed operation, expires its deadline, then verifies
completion and exact saved receipt. Final exact qualification handles/logs:
60653 `/private/tmp/samosa-s19l-commit-receipt-mac.log`,
76727 `/private/tmp/samosa-s19l-commit-receipt-linux.log`.
Inspect those handles before claiming terminal qualification.

Remaining: crash-time private artifact cleanup, broader fairness/resources and
service worker lifecycle, reindex scheduling, full uploads/acquisition/search/
indexing/Chutni/Inference media/service/full Sources release; remaining components,
legacy retirement and C01–C14. Full goal active; no archive or component completion.
60653/76727 both completed exit0: final watch removal and durable-operation normal/
ASan+UBSan suites pass on macOS/Linux after the committed-receipt change. All S19l
handles are terminal. No release archive/full-component completion is claimed.

## Interrupted-removal cleanup — 2026-10-07 (S19m)

Previous goal turn qualified S19l removal/replacement. Removal now commits bounded
owner cleanup intent with metadata deletion. Normal checked inode cleanup clears
intent; public locator_watch_cleanup drains at most eight owned intents without a
root grant. Shared observer lease serializes cleanup, runtime leases prevent active
unlink, still-registered identities refuse, and only the checked held inode is
removed. Busy intents remain; foreign-owner calls cannot drain another owner.
Eight pending intents globally refuse further removal before mutation when full.

Native actual-process regression pauses after removal metadata commit, SIGKILLs
that worker and verifies the orphan private lease remains. It tests foreign-owner
non-drain, held-lease retention, recovery after release and repeatable no-op cleanup;
immutable evidence remains readable. Pause is test-fault-only, not compiled into
normal standalone runtime. Normal/ASan+UBSan pass macOS/Linux:
`/private/tmp/samosa-s19m-crash-final-mac.log` (20747 exit0),
`/private/tmp/samosa-s19m-crash-final-linux.log` (1024 exit0).
Continuous runtime normal regressions also pass in those runs. Earlier crash
normal/sanitizer29293/15366 passed before the still-registered guard; initial92602
watch suites passed. All these qualification handles are terminal.

Added public descriptor cleanup operation/pending bound, contract and S19m scope.
This proves recorded post-commit removal recovery, not every orphan scenario:
a worker admitted before deletion could create a lease after intent cleanup and
be killed before detecting unknown_watch. Atomic runtime admission/removal is the
next required fix. Broader resource/fairness/service lifecycle and reindex scheduling
remain open, together with full Sources and full rebuild/C01–C14 requirements.
No source archive/full-component completion claim accompanies this checkpoint.
Final standalone descriptor91332 completed exit0; parsed
`/private/tmp/samosa-s19m-describe.json` confirms locator_watch_cleanup and pending
bound8. All S19m handles are terminal. Next remains atomic late admission.

## S19n: atomic runtime admission and removal ordering

Private runtime admission now holds the shared observer lease while revalidating
owner, registered state and captured root and creating/acquiring its runtime
lease. Stop/removal/cleanup cannot cross that interval. Stale lookup after removal
returns unknown_watch with no recreated private file. Bounded startup retries
busy/store_busy for five seconds; duplicate runtime leases refuse watch_busy.

Native test-fault workers pause before and after lease acquisition while holding
the observer lease. Concurrent stop returns busy; actual SIGKILL releases it, and
subsequent stopped removal/interrupted cleanup succeeds. Post-cleanup admission
refuses without creating a file. Existing registry/checkpoint/cleanup and eight
continuous-worker shutdown/restart/root/state regressions run alongside them.

Final test-watch, sanitize-watch, test-watch-runtime and sanitize-watch-runtime
all pass on macOS (40427 exit0,
/private/tmp/samosa-s19n-admission-final-mac.log) and Linux (47945 exit0,
/private/tmp/samosa-s19n-admission-final-linux.log). Both handles are terminal.
Initial macOS 45974 exit2 exposed startup observer contention in the eight-worker
test; bounded admission retry fixed it. Linux 58439 exit0 preceded the added
before/after SIGKILL admission regression; final runs supersede that subset.

Contract and S19n scope record the ordering guarantee. No general fairness,
resource or complete orphan/lifecycle claim follows from these focused tests.
Automatic reindex scheduling and remaining Sources work continue; full rebuild
goal and C01–C14 acceptance remain open. No full release or legacy switch occurred.

## S19o: observation-driven durable reindex intent

index_pending stores one job/ordinal/latest-revision intent. Reconcile/watch
transactions update it with live_locators, bounded by retained eight jobs times
256 items. Public index_pending reads validated evidence/freshness and detects
revision disagreement. Changed/missing/unsupported/uncertain observations schedule
indexed items; verified bytes matching each indexed version clear pending work.
Content qualification uses the candidate version independently of the watch's
expected version. Identity changes without a verified hash remain changed.

Native freshness tests cover initial empty queue, changed/missing, byte restoration,
four watch checkpoints and exact retries, same-content coalescing, owner/host/root
isolation, historical offline search and offline queue reads. A measured host
cancellation at the final precommit check rolls back both observation and intent.
The watch deliberately expects a different retained version to test per-candidate
hash qualification. Normal/ASan+UBSan freshness and normal index/watch tests all
pass in final macOS61870 and Linux6067 (both terminal exit0):
/private/tmp/samosa-s19o-queue-qualified-mac.log and
/private/tmp/samosa-s19o-queue-qualified-linux.log.

Earlier macOS72074/Linux26470 normal/sanitizer freshness and normal index/watch
passed before added cancellation/version regressions; control96714/21551 passed
before different-version qualification. Initial23190/73453 exit2 exposed a test
signed comparison compile warning, corrected before these successful runs. No
handle remains live. Initial1735 normal freshness also passed without queue tests.

Queue execution, fair draining/retry, missing/new/failed-file handling and old
observation catch-up remain open. Full Sources and full rebuild remain active;
this checkpoint does not claim automatic reindexing or complete release.

## S19p: bounded retained immutable reindex execution

Public index_reindex_step admits one to four pending text items using the existing
index lease/operation records. Registration binds observed metadata identity;
locator_read qualifies extraction; evidence version must equal observed hash.
Commit revalidates root scope and locator revision, stores immutable history and
removes only the corresponding intent atomically. Cancellation before item commit
rolls back; previous item commits remain durable. Latest versions drive future
scheduling; historical search includes all retained original/reindexed evidence.
Combined text/evidence quota8MiB now covers both tables; at most256 added versions
globally, with no eviction. Existing source/cache/database/operation limits apply.

New native test_reindex executes actual SIGKILL after one of two item commits,
competing busy refusal, fresh-operation resume, exact offline retry and selector
conflict, owner isolation, original/history freshness, changed-since-observation
stale_source refusal, missing live_unavailable refusal and later restoration.
A separate worker pauses after extraction; a newer public watch observation
commits before release, so the worker refuses stale_observation and preserves
pending work without publishing the captured version. Public watch/reindex
admissions fill all256 retained versions using two alternating snapshot contents;
the next attempt refuses index_limit, preserves pending intent and old evidence.
Test pauses are absent from normal builds. New targets join standard test/sanitize.

Normal and ASan/UBSan reindex, normal freshness/index/watch regressions pass final
macOS33789 and Linux25736 (terminal exit0):
/private/tmp/samosa-s19p-reindex-qualified-mac.log and
/private/tmp/samosa-s19p-reindex-qualified-linux.log.
macOS76172 exit0 preceded the request-builder cleanup; Linux67851 exit2 exposed
an obsolete null-source formatting branch in that builder. Removed the unused
branch; final runs supersede it. Earlier14955 normal freshness and21717 normal
reindex passed before the added observation-race regression. All these handles
are terminal. Source archives/full release are not updated by this checkpoint.

Fair draining/retry, automatic execution and missing/new/failed-file/legacy policy
remain open. A missing/failed leading pending ordinal currently stops its selected
job step. The full Sources release, rebuild goal and C01–C14 remain incomplete.
Final descriptor-only rebuilds37130/16472 both completed exit0. Parsed standalone
macOS/Linux describe responses confirm index_pending, index_reindex_step and
pending2048/retained-versions256 bounds. Descriptor files are
/private/tmp/samosa-s19p-describe-mac.json and
/private/tmp/samosa-s19p-describe-linux.json. No qualification handle remains live.

## S19q: durable least-attempted reindex ordering

index_attempts retains one row per pending job/item. Attempt admission commits
before content work; pending selection orders by count then ordinal. Repeated
locator/watch observations preserve attempts while intent remains pending.
Successful version commit and observed matching-byte restoration delete counters
atomically with intent. Nonnegative SQLite integer validation precedes selected
work/publication; counters saturate without signed overflow. Public index_pending
exposes attempts, including offline. Bounds inherit2048 pending rows globally.

New native fixture has a missing ordinal0 and changed healthy ordinal1. First
step refuses live_unavailable; another watch checkpoint retains the failed attempt;
the next fresh step processes ordinal1. Offline replies retain count1, another
failed attempt records2, original-byte restoration clears it, and later change
starts at0. Private counter corruption to-1 refuses a single structured error from
pending and reindex before work; restore allows remaining lifecycle checks.
Existing actual SIGKILL/resume, newer-observation race, retry/conflict, historical
evidence and public256-revision capacity tests still execute.

Normal/ASan+UBSan reindex and normal freshness/index/watch regressions pass
macOS74301 and Linux72294 (terminal exit0), logs
/private/tmp/samosa-s19q-attempt-final-mac.log and
/private/tmp/samosa-s19q-attempt-final-linux.log.
Final added corruption fixture passes normal/sanitizer reindex on macOS93912
and Linux58102 (terminal exit0), logs
/private/tmp/samosa-s19q-attempt-integrity-mac.log and
/private/tmp/samosa-s19q-attempt-integrity-linux.log.
Initial11395 normal reindex passed before the dedicated fairness fixture.
All handles are terminal; no full release archive or production switch occurred.

This qualifies selected-job ordering, not global fairness or complete retry policy.
Cross-job scheduling, timed backoff, automatic execution and full missing/new/
failed-file policy remain required. Full Sources and C01–C14 rebuild stay active.

## S19r: bounded cross-job automatic queue step

index_queue_step admits one item under the shared index lease across retained jobs
matching owner and granted root scope. Durable least-recent-dispatch turns order
ready jobs, then durable item attempts order work inside the selected job. Schedule
metadata is bounded to eight retained jobs; corrupt negative/noninteger turns
refuse, and INT64_MAX refuses scheduler_limit before increment. Turns survive
success and recurring changes, failures and restart. Automatic dispatch does not
add client receipts; item checkpoints and retained intent provide recovery without
exact automatic response replay. All existing extraction/root/revision validation
and immutable-history/evidence quotas remain enforced.

Native two-job fixture chooses a missing job first, then a healthy peer after a
repeated checkpoint. Foreign owner, different host and replacement root select
idle without exposing/processing other jobs. Historical evidence remains readable.
520 further failed automatic attempts do not consume receipts: all508 remaining
explicit receipt slots fill (four initial create/step receipts), then the next
explicit operation refuses index_limit. Automatic recovery still commits when
the missing file returns. A hot healthy job changes immediately after success;
the older ready peer executes next despite that recurring input. Negative/max
schedule corruption returns typed errors. Prior actual process death, newer
observation race, retry/conflict, retained256-version cap and integrity tests run.

Final normal/ASan+UBSan reindex and normal freshness/index/watch regressions pass
macOS27001 and Linux90079, both terminal exit0:
/private/tmp/samosa-s19r-turn-qualified-mac.log and
/private/tmp/samosa-s19r-turn-qualified-linux.log.
Earlier88068/41239 passed before added durable job turns/hot-job/overflow tests;
initial43178 normal reindex passed before cross-job fixture. All handles terminal.
S19r contract/scope records automatic recovery semantics and bounds. Continuous
runtime and timed backoff, missing/new/failed-file and legacy catch-up remain open.
Full Sources/rebuild/C01–C14 remain incomplete; no full release or legacy switch.
Final standalone descriptor rebuilds17865/21823 both terminal exit0. Parsed
macOS/Linux describe responses confirm index_queue_step and schedule8/item1 limits
in /private/tmp/samosa-s19r-describe-mac.json and
/private/tmp/samosa-s19r-describe-linux.json. No qualification handle remains live.

## S19s: continuous local index runner

CLI --index-run continuously invokes index_queue_step. Interval100..60000ms,
default1000, paces successful/idle/transient-error attempts. A single regular
owner-only single-link reindex-runtime.lock kernel lease admits one runner/state.
Checks between polls retain startup root scope, state directory and private lease
identity. Attempts have a five-second deadline bounded by the host deadline;
host cancellation remains linked. TERM/INT interrupt content checks and waits;
SIGKILL releases admission. Only a terminal JSON envelope is written to stdout.
No client receipts/per-cycle files accumulate. Permanent quota/integrity/binding
errors terminate; transient missing/stale/busy/deadline errors retry after pacing.

Actual CLI process fixture qualifies duplicate refusal while the original stays
alive, missing-file attempts with healthy peer completion, retained offline state,
TERM/INT, SIGKILL and lease restart, later-file recovery, root/state/private-lease
replacement between polls and historical evidence retained in the original store.
State replacement detects before a new database is created in that fixture.
Both test and runtime binaries are instrumented in ASan/UBSan configuration.
Standard test/sanitize targets now include these native runtime checks.

Final normal/sanitizer queue runtime plus normal reindex/continuous-watch regressions
pass macOS7308 and Linux53063 (terminal exit0):
/private/tmp/samosa-s19s-runtime-final-mac.log and
/private/tmp/samosa-s19s-runtime-final-linux.log.
Initial standalone98584 also built exit0 before the pacing-clock/host-deadline
checks and runtime test were added. All qualification handles above are terminal.

This is the local paced runner; per-item exponential backoff, precheck-to-dispatch/
store-open race closure, long-run resources/fairness, full missing/new/failed-file
and legacy policies remain required. Authenticated bounded service and full Sources
release remain incomplete. Full rebuild/C01–C14 remains active, with no legacy
production switch, publication or updated source archive claimed here.
Final descriptor-only standalone rebuilds23519/91839 both terminal exit0. Parsed
macOS/Linux describe responses confirm worker1, interval100..60000ms and attempt
5000ms bounds in /private/tmp/samosa-s19s-describe-mac.json and
/private/tmp/samosa-s19s-describe-linux.json. No qualification handle remains live.

## S19t: durable per-item automatic backoff

Automatic attempts reserve index_retry before content work, then completed failures
receive a full delay measured at finalization. Level1..7 uses1,2,4,8,16,32,60seconds,
with capped level7. Eligible selection skips cooling items both across jobs and
within a selected job. Repeated same content/identity observations preserve delay;
changed verified target deletes it. Matching indexed bytes and successful version
commit delete retry state with intent. Unavailable observations use a stable target
until verified content/identity becomes available. One row/pending item bounds
state to2048 rows. Pending exposes next_attempt_at_ms/backoff_level, with checked
integer/range metadata. Explicit reindex bypasses automatic cooldown without
advancing its level. Checked wall-clock arithmetic reserves overflow headroom;
far-future delays are treated due to bound waiting after clock rollback.

Native tests cover actual one-second expiry, doubled delay and exact capped-level
deadlines bracketed by public-call times, unchanged checkpoint retention, explicit
override, healthy peer completion,520 cooled polls with no receipt growth,507
remaining explicit receipts after five original receipts, refusal at512, and changed
target automatic recovery despite full receipts. Invalid level8 refuses before
automatic selection and pending publication. Longer-delay cases only accelerate
expiry in owned fixture rows; they verify actual recorded schedules without a
minute wait. Actual automatic worker pauses after reservation longer than its
initial delay; release proves a full one-second delay after failure. Another
reserved worker is SIGKILLed; the retained level2 deadline makes restart polling
idle with pending intent intact. Pauses exist only in test-fault builds.

Final normal/ASan+UBSan reindex and actual queue runtime pass macOS46884 and
Linux84219, terminal exit0, logs:
/private/tmp/samosa-s19t-backoff-process-mac.log and
/private/tmp/samosa-s19t-backoff-process-linux.log.
Prior76297/42308 passed before added delayed-worker/SIGKILL reservation fixtures;
54632/54611 passed prior admission-timer version with stronger timing/corruption
checks, including normal freshness/watch regressions.3647/2579 passed earlier
normal/sanitizer reindex/runtime and normal freshness/watch. Initial25045 exit2
exposed an obsolete test expecting an immediate repeated attempt; revised to
expect cooled idle polling under the new contract. All above handles are terminal.

Contract/S19t scope documents recovery, explicit override and idle semantics.
Broader clock/resource qualification, precheck-to-dispatch/store-open races,
full missing/new/failed-file/legacy policy, authenticated service and complete
Sources release remain open. Full rebuild/C01–C14 remain active, with no full
source archive or legacy production switch claimed for this checkpoint.
Final descriptor-only rebuilds23414/44565 both terminal exit0. Parsed standalone
macOS/Linux responses confirm retry rows2048/min1000ms/max60000ms in
/private/tmp/samosa-s19t-describe-mac.json and
/private/tmp/samosa-s19t-describe-linux.json. No qualification handle remains live.

## S19u: binding pins at core admission and indexing scope selection

SourcesConfig now supports host-only state device/inode pins and an indexing root
scope pin. Watch/index runtime copies retain admitted state identity; the index
runtime also retains its startup root scope. Core store admission checks directory
pins before database creation and does not mkdir an absent pinned state. Root
identity calculations used by queue selection/commit reject a replacement scope
after the outer runtime check. Default zero configuration keeps existing behavior.
Retained immutable reads remain independent of a live root grant.

Native test_binding_pins performs actual state rename/replacement and absence with
retained host pins; requests refuse without creating a new database/directory.
An unpinned host creates/indexes a replacement-root file, then observes its change
to schedule real pending work. The pinned queue request refuses scope_unavailable;
that job remains pending with attempts0. Old immutable text remains readable with
the pin and offline. Standard test/sanitize targets include this test.

Final normal/ASan+UBSan pin tests, normal/sanitizer actual queue runtime and normal
actual watch runtime pass macOS73435 and Linux96467 (terminal exit0):
/private/tmp/samosa-s19u-pins-final-mac.log and
/private/tmp/samosa-s19u-pins-final-linux.log.
Initial87382 normal reindex/queue/watch regressions also passed before the dedicated
pin fixture. All qualification handles are terminal. Contract and S19u scope record
the exact boundary: core admission and live indexing scope identity calculations.

The separate SQLite filename-open race inside store_open, helper/control-store
binding and remaining race/resource qualification are still open. This checkpoint
does not prove an entire filesystem capability/SQLite VFS boundary or full Sources
release. Full architecture rebuild/C01–C14 remains active, with no new archive,
legacy production switch or publication claimed.

Descriptor-bound SQLite prototype investigation (not integrated or qualified):
`src/bound_db.c` and `src/bound_db.h` delegate native Unix locking while routing
opens and metadata lookups through a retained private directory descriptor.
Strict C11 warning-as-error compilation succeeds on macOS and Linux. The Linux
standalone probe opens and commits a table/insert; an actual directory rename and
replacement probe commits through the retained directory without creating a
database in the replacement directory. macOS system SQLite 3.51.0 still returns
SQLITE_CANTOPEN before the intercepted open callback; Linux SQLite 3.40.1 passes
these probes. These are diagnostic probes, not concurrency, crash recovery,
sanitizer, admission-bound or full VFS qualification. Production store paths are
unchanged; the inner filename-open race remains open.

Follow-up macOS diagnostics narrow the failure: an ordinary real-path SQLite open
honors a refusing `open` syscall hook (two callbacks, SQLITE_CANTOPEN), as does
Linux. The synthetic `/samosa-bound-v1/...` name fails on macOS before that hook;
selecting `unix-posix` does not change it. A diagnostic `/dev/fd/<directory>/...`
name reaches open successfully but native SQLite then logs failed descriptor
confinement and journal writes/close return I/O errors. That variant exists only
in `/private/tmp/samosa-bound-devfd.c`, not the component. Thus syscall interception
alone has not established a usable macOS native I/O delegation boundary. Production
integration remains withheld pending a compatible descriptor and locking design;
no state-binding race closure or platform qualification is claimed.

The explicit experimental `test-bound-db`/`sanitize-bound-db` Makefile gates now
exercise two adapter connections, ordinary SQLite lock interoperability, retained
directory writes after replacement, and actual SIGKILL rollback-journal recovery.
Linux normal execution passes. Initial sanitizers detected wrapper alignment
stronger than SQLite's file-storage guarantee; replacing `max_align_t` with
eight-byte alignment fixes it. Final Linux ASan+UBSan execution with
`-fno-sanitize-recover=all` passes without diagnostics (terminal exit0). macOS
`make test-bound-db` compiles but exits2 at the initial SQLITE_CANTOPEN assertion.
These gates are intentionally outside default production tests until the adapter
works on both platforms. A macOS diagnostic that routes `/dev/fd` opens through
the parent's native open callback also fails; that variant is not integrated.

Further diagnostic variant `/private/tmp/samosa-bound-guarddup-dir.c` first opens
the exact leaf with `openat`, then invokes the system SQLite open callback on
`/dev/fd/<already-open-file>` to obtain a descriptor matching its native guarded
I/O. The original macOS native `posixOpen2` implementation was inspected with
LLDB; it uses guarded open/close/write wrappers. This variant passes the full
experimental normal bound-DB fixture on macOS (terminal exit0), including actual
SIGKILL journal recovery, and avoids journal-directory close I/O errors. The
system logger still reports directory confinement EAGAIN. It remains a temporary
diagnostic: closing the temporary unguarded same-inode descriptor could release
other process-local POSIX locks. Opening another adapter while an existing
connection holds a transaction requires dedicated external-process lock testing
before this design can qualify. No macOS production change is made.

The experimental fixture now execs a fresh process to assert native SQLite writer
exclusion before/after opening and closing an adapter connection under a held
transaction. This exposed deferred-descriptor reuse: native Unix may skip the
open hook. The prototype now validates the actual descriptor through its native
fstat callback. Linux normal and ASan+UBSan with nonrecovering diagnostics pass
the expanded fixture. macOS linked explicitly against
`/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib` also passes normal and nonrecovering
ASan+UBSan runs (all terminal exit0); `otool -L` confirms that dependency. This is
additional dependency-specific evidence, not qualification of macOS system
SQLite, portable packaging, or production integration. No guarded-descriptor
diagnostic variant has replaced the component prototype. Dependency selection,
reproducible installation, failure/resource bounds and integration remain open.

Expanded adapter admission fixture reaches 64 simultaneous connections, refuses
the 65th with empty output handles, releases all slots and successfully reopens.
It also refuses unknown database names, symlinks, multiply linked database files
and group/other-readable files; restoring private permissions preserves data.
Final Linux and macOS explicitly linked upstream SQLite ASan+UBSan executions
with nonrecovering diagnostics pass (terminal exit0). `SQLITE_LIBS` now exposes
library selection for reproducible local gate commands. Release dependency
pinning, packaging, macOS system-library compatibility and production integration
remain unqualified.

Opt-in `DESCRIPTOR_DB=1` integration now routes the core Sources and durable
read-operation stores through retained descriptor bindings, including runtime
admission and health writes. It removes separate database pre-open/close and
pathname reopen in the selected build. Operation admission also honors existing
host state pins before directory/database creation. Existing database names and
schema are preserved. Default builds retain the prior path pending dependency
release qualification.

Integrated normal binding-pin, 735-check durable operation and reindex suites
pass macOS with explicit Homebrew SQLite and native Linux (terminal exit0;
macOS60655/Linux54451). Logs: /private/tmp/samosa-bound-integrated-mac.log and
/private/tmp/samosa-bound-integrated-linux.log. macOS sanitizer binding-pin checks
pass; operation sanitizer session54973 also completes with terminal exit0.
Integrated Linux sanitizers, timed post-admission replacement faults, full
regressions, pinned dependency assembly and default-path retirement remain open.

Linux integrated sanitizer binding-pin and durable operation checks complete
with terminal exit0 (session5438; 738 operation checks), log
/private/tmp/samosa-bound-integrated-sanitize-linux.log. The explicit adapter gate
adds a test-only fault callback immediately after retaining/validating the admitted
directory but before SQLite open. It actually renames that directory and creates
a replacement; open succeeds against the retained directory and the replacement
has no database. Final macOS upstream-SQLite and Linux ASan+UBSan executions of
this expanded gate pass with nonrecovering diagnostics (terminal exit0; mac24834).
The callback is absent from non-test builds. This tests the adapter admission/open
boundary; concurrent public runtime namespace replacement, packaging and default
store-path retirement still require qualification. Full Sources and C01–C14 are
not complete.

Integrated continuous watch/queue qualification initially failed on both
platforms during concurrent `locator_watch_get`, returning state_unavailable.
The adapter was setting journal mode before the store installed its busy handler.
It now sets only memory temporary storage during adapter admission; existing
store initialization sets rollback mode after its timeout configuration.
Corrected normal watch and queue runtime fixtures pass both platforms with
terminal exit0 (macOS27451/Linux61919); logs
/private/tmp/samosa-bound-runtime-fixed-mac.log and
/private/tmp/samosa-bound-runtime-fixed-linux.log. Initial failed runs83181/37795
and diagnostic89297 are terminal and superseded for these normal checks by the
corrected runs. Runtime sanitizers and broader release qualification remain open.

Corrected integrated watch and queue ASan+UBSan runs pass both platforms with
terminal exit0 (macOS90241/Linux82020), logs
/private/tmp/samosa-bound-runtime-sanitize-mac.log and
/private/tmp/samosa-bound-runtime-sanitize-linux.log. Qualification still uses
explicit macOS upstream SQLite and DESCRIPTOR_DB=1. Common Sources binary/test
builds now depend on a configuration stamp, recording compiler, flags and library
selection; explicit changed-flag adapter execution rebuilds and passes, unchanged
execution skips compilation and passes (macOS18101). The stamp works with macOS
Make3.81 and separates changed timestamps by one second. Full build/release
dependency packaging and default store-path retirement remain pending.

Bundled dependency qualification begins: unmodified SQLite3.53.4 amalgamation
from https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip is vendored with
public-domain dedication preserved and official archive SHA3-256 verified as
628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e.
Origin and per-file SHA-256 manifest accompany it. `SQLITE_MODE=bundled` builds
a static archive offline, without Homebrew/runtime installation. Normal explicit
adapter fixtures pass macOS31453 and Linux63592 (terminal exit0); macOS adapter
ASan+UBSan and integrated binding-pin checks pass7038 (terminal exit0). Logs:
/private/tmp/samosa-bundled-sqlite-{mac,linux}.log and
/private/tmp/samosa-bundled-integrated-mac.log. SQLite itself is presently compiled
as a normal dependency; owned adapter sanitizer qualification is not a claim of
sanitizing the SQLite amalgamation. Broader bundled integration, clean archive
rebuild, default activation and replaced-path retirement remain pending.

Native `verify-sqlite` now checks pinned source SHA-256 before bundled archive
compilation. The actual vendored files pass; a separate header copy with an
appended newline refuses with digest mismatch and terminal exit1. The gate uses
owned SHA-256 with64KiB streaming buffers and a known empty-input digest check;
no Python/network dependency during builds. Broader `make test` bundled suites
are still live at this checkpoint (macOS56531/Linux60772), logs
/private/tmp/samosa-bundled-full-mac.log and
/private/tmp/samosa-bundled-full-linux.log; partial output does not prove these
suites passed. Default activation and full Sources release remain open.

Bundled macOS `make test` completes with terminal exit0 (56531), including the
standard Sources suite and separate CLI acceptance checks; this does not include
all separately configured optional PDF/OCR/compressed-media qualification gates.
An explicit candidate source archive /private/tmp/samosa-s19v-candidate.tar.gz
contains the current source, vendored dependencies, contracts and build tools;
it is not a qualified Sources release. A fresh macOS extraction builds offline
and passes adapter and integrated binding-pin tests (71694 terminal exit0), log
/private/tmp/samosa-s19v-clean-mac.log. `otool -L` confirms its binding-pin binary
depends only on system libc, with SQLite statically included. Linux full-suite
60772 and independent clean archive build70028 are still live at this checkpoint.
Keep their exact handles; elapsed observation time is not completion or failure.

Clean Linux archive adapter and integrated binding-pin tests subsequently pass
(70028 terminal exit0), log /private/tmp/samosa-s19v-clean-linux.log; `ldd` shows
no dynamic SQLite dependency. Candidate archive SHA-256 is
61257c419a8ff9eb46917d90b4475553250f759f1054e616b2012084d1b5b030.
The Linux standard suite60772 remains live; no final release/default activation
claim follows from the clean subset tests.

Bundled Linux standard `make test`60772 subsequently completes with terminal
exit0, including separate native CLI checks. Together with macOS56531 this
qualifies the standard normal suite under bundled SQLite/descriptor mode, not
separately configured optional gates. Broader bundled sanitizer suites are live
as macOS66937/Linux23500; logs
/private/tmp/samosa-bundled-sanitize-full-{mac,linux}.log. Keep these exact handles.

The direct adapter fixture additionally replaces the database leaf while an open
connection retains the original inode. Its attempted write refuses READONLY;
replacement content and original content both remain unchanged after reopening.
macOS nonrecovering ASan+UBSan fixture66995 passes with bundled SQLite. Linux99713
also completes with terminal exit0. This adds leaf-replacement evidence without
claiming concurrent malicious-owner filesystem capability completeness.

Default-path retirement candidate is staged independently at
/private/tmp/samosa-s19v-default-candidate. It removes the two
SOURCES_DESCRIPTOR_DB pathname-open fallback branches and selects bundled SQLite
by default, while the workspace remains unchanged during its live sanitizer
compilations. macOS candidate adapter and binding-pin fixtures pass; candidate
operation run83582 also completes with terminal exit0. A matching archive
/private/tmp/samosa-s19v-default-ready.tar.gz is extracted into a separate Linux
build for the same default-command gates (live session12139). This is prepared work awaiting
qualification, not evidence of installed default activation or complete release.
Workspace sanitizer66937/23500 still require exact-handle completion checks.

macOS bundled broader sanitizer66937 completes with terminal exit0, including
standard CLI sanitizer acceptance; Linux23500 remains live. Linux default candidate
12139 completes adapter, binding-pin and operation checks with terminal exit0.
The workspace now defaults to bundled SQLite and unconditionally calls the bound
adapter for both stores; both pathname-open branches are removed. A search for
SOURCES_DESCRIPTOR_DB/sqlite3_open_v2 in sources.c/operations.c returns no matches.
Default adapter gates join standard test/sanitize targets. Workspace default
command check30756 passes binding-pin and operation tests with terminal exit0;
log /private/tmp/samosa-default-activated-mac.log.
This is a Sources storage implementation change, not assembled application
activation, service completion or a complete Sources release.

Linux broader bundled sanitizer23500 completes with terminal exit0, including
separate CLI checks. Both platforms now pass standard normal and sanitizer suites
for the bundled/descriptor integration. Linux workspace-default check6882 also
passes with terminal exit0; log /private/tmp/samosa-default-activated-linux.log.

A further dispatch boundary fix pins core execution to the state device/inode
admitted by its durable read operation. Actual test-only replacement during
operation-store admission causes controlled registration to refuse
state_unavailable; the replacement directory remains empty and the original
store contains the failed receipt after restoration. Normal operation suites
pass macOS52407/Linux70967 (750 checks on macOS), logs
/private/tmp/samosa-state-dispatch-pin-{mac,linux}.log. New fix sanitizer checks
are running (macOS1674/Linux67020). Prior
whole-suite results precede this fix and are not its sanitizer qualification.

Dispatch-pin operation sanitizer checks1674/67020 complete with terminal exit0
(843 macOS/753 Linux checks). Default worker regressions after the fix are running
as macOS68965/Linux37028; logs /private/tmp/samosa-default-final-runtime-{mac,linux}.log.
S19v now clearly distinguishes current default implementation from historical
opt-in qualification. Full Sources and architecture release gates remain open.

Final default normal watch and queue runtime regressions68965/37028 complete
with terminal exit0 on macOS/Linux after the dispatch-pin fix. These actual CLI
checks retain duplicate refusal, live changes/missing-file progress, signal and
public stop behavior, SIGKILL restart, root/state/lease replacement refusal and
historical evidence. All S19v qualification handles recorded in this sequence
are terminal. Complete uploads, broader automatic missing/new/failed-file and
legacy catch-up policies, optional media integration, authenticated service and
assembled release qualification remain required.

Next upload work starts by separating owned byte admission from filesystem
acquisition in sources.c. `snapshot_accept` now owns media validation, immutable
digest/deduplication, quotas, ownership/provenance and atomic storage; live
registration retains its existing held-file size/identity checks and buffer
lifetime. SHA-256 admission checks interruption every64KiB, preparing this
shared path for root-independent upload ingestion. Normal Sources, owner-scope
and actual large/64MiB boundary fixtures pass both platforms (63995/4279 terminal
exit0), logs /private/tmp/samosa-upload-admission-refactor-{mac,linux}.log.
This is shared ingestion groundwork, not a completed public upload lifecycle.
Large-source sanitizer checks are running as macOS70359/Linux41366, logs
/private/tmp/samosa-upload-admission-sanitize-{mac,linux}.log.

Shared admission large-source sanitizer70359/41366 completes with terminal exit0
on both platforms. A root-independent host `sources_upload` C entry point now
copies caller bytes in bounded chunks and uses that shared immutable transaction.
Initial normal upload fixture covers owned-copy evidence after caller mutation,
foreign read refusal/independent claim, deduplication, shared quota, invalid format
bytes/UTF8/size/null input and scoped cancellation. Linux57239 passes; macOS2600
was rerun after fixing the fixture's mkdtemp feature declaration. These are initial
checks, not full size/media/lifecycle qualification. See contracts/uploads-v1.md;
streaming/resumability/durable upload controls/service admission remain pending.

macOS2600 remained a compile failure: the native fixture also requires unistd.h
for the platform mkdtemp declaration. That include is added and final rerun3922
passes with terminal exit0 and is the authoritative macOS check; prior compile
attempts are not passing evidence. Both platforms pass initial normal upload
fixtures. Upload API sanitizers and larger/media/lifecycle qualification remain open.

Expanded upload fixtures pass normal and nonrecovering ASan+UBSan on both
platforms (macOS77225/Linux22433 terminal exit0), logs
/private/tmp/samosa-upload-boundary-{mac,linux}.log. Actual64MiB host bytes cancel
at callback1200 during hashing, after bounded copying; independent SQL verifies
zero accepted snapshots, owner claims and formats. Healthy retry accepts the
entire buffer under a one-snapshot64MiB quota, caller input is freed, and the last
five bytes are retrieved through public read_text without a root grant. Existing
small input ownership/deduplication/quota and invalid-input checks pass in the
same fixture. This qualifies the host binary ingestion primitive's text boundary;
streaming/session/durable controls, all-format upload integration and full Sources
release remain required. All handles in this upload checkpoint are terminal.

Host `sources_upload_stream` now reads at most64KiB per callback, enforces exact
declared length with a one-byte excess probe, and samples controls around each
callback. Buffer ingestion delegates to it. Expanded fixtures cover one-byte
fragmented success, short/excess streams, reader errors and invalid reported
chunk sizes, plus existing actual64MiB and hashing-cancellation checks. The
hash cancellation threshold moves to2300 because copy callbacks now have both
pre/post control samples. Normal/sanitizer runs18436/35332 are the authoritative
new pipeline gates and both complete normal/nonrecovering ASan+UBSan checks with
terminal exit0; logs /private/tmp/samosa-upload-stream-{mac,linux}.log.
Durable/resumable staging, bounded service transport admission and complete upload
lifecycle remain open; callbacks currently fill an owned bounded memory buffer.

Upload reader integration fixtures add literal extraction from uploaded HTML and
exact one-millisecond PCM16 WAV preparation from uploaded audio, with no root
grant and after caller-buffer release. The WAV result is decoded independently
to check selected payload size/header and zero PCM samples; repeated preparation
must return the identical cached artifact. Initial32994/3395 normal/sanitizer
reruns supersede compile failures21011/2465 caused by a fixture helper/local name
collision. Logs /private/tmp/samosa-upload-readers-final-{mac,linux}.log. Optional
PDF/DOCX/OCR/encoded-media upload integration and durable staging remain open.

Reader-integration32994/3395 then refused the fixture's bare HTML paragraph,
consistent with the existing declared HTML sniff boundary. The fixture now supplies
a full HTML document with doctype; upload diagnostics include the typed response
on unexpected outcomes. Corrected normal/sanitizer runs74359/65776 both pass with
terminal exit0, logs /private/tmp/samosa-upload-readers-qualified-{mac,linux}.log.
Prior failed fixture runs do not count as reader-integration qualification.

Durable upload session metadata implementation begins in uploads.c/.h: JSON
begin/status/abort, scoped immutable begin identity and parameter conflict,
owner checks, eight open sessions,64MiB aggregate reservations and128 retained
identities. Snapshot admission counts open reservations toward existing quotas.
Normal builds20657/49732 pass both platforms. Initial macOS fixture33641 passes
persisted retry/status, foreign-owner refusal, conflict, reserved snapshot quota,
abort release and active cap. Linux fixture95098 also passes with terminal exit0, log
/private/tmp/samosa-upload-sessions-linux.log. Chunk append/publication, corruption/concurrency/crash/sanitizer
qualification and the complete resumable lifecycle are not implemented/qualified
by this metadata checkpoint. No accepted source is represented by these sessions.

### S19w durable chunk append integrity checkpoint

`make test-upload-sessions sanitize-upload-sessions` passed terminally on macOS
and Linux using the default pinned SQLite build. The fixture verifies owner
isolation, begin identity conflicts, capacity reservations, transactional append,
exact chunk retries, changed-byte conflicts, noncanonical base64 rejection,
forged received-offset rejection, altered stored-digest rejection, and abort
cleanup/reservation release. The session gates are included in standard test and
sanitize targets. Sanitizers instrument component code; the bundled SQLite archive
is not instrumented. Logs: /private/tmp/samosa-upload-append-integrity-mac.log and
/private/tmp/samosa-upload-append-integrity-linux.log. Process handles3171 and62052
both exited0. This does not qualify completed publication, actual restart or
concurrent append, comprehensive staged corruption, or full upload/service release.

### S19w atomic upload publication checkpoint

`upload_commit` now joins shared snapshot admission to its existing immediate
transaction, releases its reservation, deletes staged chunks, and persists a
SHA256-protected commit receipt atomically. `make test-upload-sessions
sanitize-upload-sessions` passed on macOS and Linux (terminal handles3367/56933,
exit0). Tests prove incomplete refusal, publication under exactly one snapshot
and five-byte capacity, identical receipt retry, root-free accepted text read,
completed status, zero remaining chunks, and format rejection restoring the open
session and staged bytes with no snapshot or receipt. Existing direct-upload tests
also passed on both platforms after the transaction refactor. Logs are
/private/tmp/samosa-upload-publication-{mac,linux}.log. Linux initially refused
compilation for missing POSIX feature declarations; adding POSIX200809 resolved
it. Component sanitizer coverage excludes the uninstrumented bundled SQLite.

This is not full upload qualification: actual process restart, concurrent writers,
crash-at-commit recovery, cancellation during publication,64MiB staged publication,
comprehensive completed-receipt validation, and public describe integration remain
open, as do authenticated transport and the remaining Sources work areas.

### S19w fresh-process resume, concurrent retries and receipt integrity

The session fixture now execs fresh native processes for begin, resume status,
append, commit retry and accepted text read. Two gated child processes start the
same append or commit together; both return identical successful envelopes.
Independent SQL checks find exactly one accepted snapshot, one owner claim, one
receipt and zero staged chunks. This covers process-lifetime replacement and
concurrent identical retries; it does not establish every transaction interleaving
or crash recovery.

Completed-session validation now checks zero retained chunks, receipt text bounds
and SHA256, parsed identity/version/format/length, accepted snapshot metadata and
owner/format claims. Tests reject altered receipt bytes, a malformed receipt with
its digest recomputed, and a deleted owner claim; restoring the valid state restores
healthy status. Snapshot payload hashing is qualified by the existing read path,
not by this metadata receipt validation.

`make test-upload-sessions sanitize-upload-sessions` passed terminally on both
platforms, handles98523/21004 exit0, logs
/private/tmp/samosa-upload-receipt-integrity-{mac,linux}.log. Sanitizers include
component/fixture code and exec children; bundled SQLite remains uninstrumented.
Crash-at-commit,64MiB staged publication, scoped cancellation during publication,
broader concurrency, public describe integration and complete upload release stay
open. No full Sources or architecture milestone completion is claimed.

### S19w precommit cancellation, process death and public discovery

The native session fixture cancels at the sixth scoped publication control sample,
which current code places after snapshot/claim/format and receipt writes and before
outer COMMIT. Independent SQL finds zero snapshots/receipts, one staged chunk and
an open five-byte session after rollback. A fresh exec process publishes the same
session and receives SIGKILL at that checkpoint. The parent observes SIGKILL,
then a fresh exec status call recovers the rollback journal. Independent SQL again
finds zero snapshots/receipts and intact staging. Fresh-process publication and
exact commit retry subsequently return identical accepted source envelopes.

`describe` now advertises upload_begin/status/append/abort/commit and limits for
eight sessions,128 retained identities,64MiB reserved bytes,8192-byte chunks and
8200 chunk rows. The session fixture checks public discovery as well as recovery.
`make test-upload-sessions sanitize-upload-sessions` passed both platforms,
terminal handles25464/86531 exit0, logs
/private/tmp/samosa-upload-recovery-describe-{mac,linux}.log. Component/fixture
sanitizers include exec children except the deliberately killed worker; bundled
SQLite is uninstrumented. This is precommit process-death recovery, not machine
power-loss qualification or coverage of all commit phases. Staged64MiB publication,
broader crash/concurrency/control qualification and complete upload release remain
open; all later Sources and architecture work areas remain active.

### S19w actual64MiB staged upload and layout-index optimization

A new native test-upload-large gate stages8192 public8192-byte appends under a
one-snapshot/64MiB quota, computes an independent incremental SHA256, publishes,
checks identical commit retry, reads the final tail! range without a root, and
independently verifies one64MiB snapshot/receipt and zero staging rows. Baseline
normal tests passed both platforms (handles57148/13375 exit0; logs
/private/tmp/samosa-upload-large-{mac,linux}.log). Fixture compilation initially
needed unistd.h on macOS and a SHA256 helper reference under Linux strict warnings;
those were fixed before the passing runs.

Large staging exposed repeated layout scans reading chunk payload pages. An
expression index stores offset/type/length metadata while retaining the same
layout predicates. A local in-memory query-plan probe confirms covering-index
columns for all layout checks. Current optimized macOS normal large-upload and
session regression gates passed (handle90718 exit0), log
/private/tmp/samosa-upload-large-index-mac.log. Optimized Linux normal and both
platform sanitizer gates were launched and remain pending; this checkpoint does
not claim their completion. The full upload/freshness and Sources release areas
remain open.

### S19w optimized staging qualification and request controls

Optimized64MiB public staging/publication gates passed Linux normal/sanitizer and
macOS sanitizer, including the session corruption/process/crash regression gates.
Handles31063/48260 exited0; logs
/private/tmp/samosa-upload-large-index-linux.log and
/private/tmp/samosa-upload-large-index-sanitize-mac.log. Combined with the preceding
macOS normal gate, the metadata-index change has both-platform normal/sanitizer
coverage. The large gates now participate in standard test/sanitize targets.

Append/status/abort/commit now route optional operation_id, parent_operation_id
and timeout_ms through existing durable controls. Mutating successes preserve the
committed result when finishing their operation receipt. The native fixture checks
controlled append/commit success, exact receipt retry, operation_get completed
status, and conflicting reuse of an operation identity. Both-platform normal and
sanitizer gates passed, terminal handles73019/34702 exit0, logs
/private/tmp/samosa-upload-control-{mac,linux}.log. The initial normalization
rejected upload_id/data as non-string fields; adding them to canonical string
fields fixed that. Fixture expectations were corrected to the established
completed/idempotency_conflict contract. Existing non-upload control regressions
have been launched separately and remain pending at this checkpoint.

Begin's operation_id is its upload lifetime identity, not a generic request-control
receipt. Public upload cancellation/deadline worker interleavings, broader crash
phases and aggregate-capacity boundary cases remain unqualified; authenticated
service and complete freshness/upload release remain open. Bundled SQLite is not
sanitizer-instrumented. No complete Sources or C01–C14 completion is claimed.

Follow-up non-upload `make test-control` passed macOS/Linux135 checks per platform
(handles7515/65275 exit0), logs
/private/tmp/samosa-upload-control-regression-{mac,linux}.log. This verifies
existing correlation, owner refusal, budget validation, expired/cancelled ingress,
delayed inventory and survivor behavior after the canonical upload-field change.

### S19w public cancellation and request-deadline rollback

The native session fixture invokes public operation_cancel from its host callback
at the sixth publication sample (after receipt insertion, before COMMIT). The
request returns cancelled, operation_get records cancelled, and exact request retry
retains the cancellation. A different operation identity uses timeout_ms1000 and
delays at the same sample until expiry; it returns deadline_exceeded and saves a
failed operation receipt. Independent SQL verifies zero snapshots/upload receipts,
one staged chunk and an open fully received session. A new operation identity then
publishes successfully. This is a five-byte deterministic host-callback/public-API
boundary test, not coverage of all external-process cancellation races.

Both-platform `make test-upload-sessions sanitize-upload-sessions` passed,
terminal handles91243/17010 exit0; logs
/private/tmp/samosa-upload-public-controls-{mac,linux}.log. Existing large upload,
process/crash and control qualification remains scoped as previously recorded;
bundled SQLite remains uninstrumented. Remaining aggregate-capacity/format and
external-worker cases, complete freshness lifecycle, service admission and final
Sources release are still required. The full architecture goal remains active.

### S19w separate publishing worker cancellation

The session fixture now execs a publishing worker that signals and waits at the
sixth precommit control sample. The parent independently calls operation_get and
verifies running, calls operation_cancel and verifies cancel_requested, then
releases the worker. The worker returns cancelled and exits1. Independent SQL
finds zero snapshots/upload receipts and one retained staged chunk. A fresh exec
process then successfully publishes the upload. This exercises cancellation from
a separate process while the upload transaction is open, without a production
fault hook. It does not qualify arbitrary timing or every commit-phase race.

Both-platform `make test-upload-sessions sanitize-upload-sessions` passed,
terminal handles29850/98915 exit0; logs
/private/tmp/samosa-upload-external-{mac,linux}.log. The final PASS label still
summarizes the prior cases; the added assertions and exec child cover the external
worker case. Bundled SQLite remains uninstrumented. Full uploads/freshness and
Sources release remain open, including aggregate-capacity/format integration,
broader live-watch policies and authenticated service assembly.

### S19w reservation integrity across shared admission

Added a bounded shared reservation validator for new session and new snapshot
admission. It checks all retained session integer/format/status/received-offset
invariants and global128 identities/eight open/64MiB reservation caps before
trusting totals. The native regression forges a negative reservation plus a
positive reservation with aggregate zero; both a new session and a direct new
snapshot now refuse corrupt_upload. Previously aggregate checks could mask that
invalid row. Valid publication continues to release its reservation inside the
same transaction as snapshot admission.

`make test-upload-sessions sanitize-upload-sessions test-upload` passed on macOS
and Linux, terminal handles41160/7482 exit0; logs
/private/tmp/samosa-upload-reservations-{mac,linux}.log. This includes existing
session/process/cancellation/crash cases and direct text/HTML/WAV/64MiB ingestion
regressions. The change does not claim full database integrity, aggregate physical
storage boundary qualification, all-format upload integration, complete freshness
or Sources release. Bundled SQLite remains uninstrumented by component sanitizers.

### S19w uploaded DOCX extraction/cache integration

The default native upload fixture now builds actual stored and raw-Deflate DOCX
ZIP packages using the existing native fixture generator, ingests each through
sources_upload, frees caller bytes, selects the first eight literal uploaded text
bytes through public read_docx with no root grant, and checks exact repeated cached
response equality. `make test-upload sanitize-upload` passed both platforms,
terminal handles82272/88296 exit0, logs /private/tmp/samosa-upload-docx-{mac,linux}.log.
Existing text/HTML/WAV/64MiB and cancellation/quota upload regressions also pass.
The fixture header is an explicit build prerequisite. Component sanitizers exclude
bundled SQLite. This qualifies host DOCX ingestion/read/cache; staged DOCX,
uploaded PDF/OCR/encoded media and complete freshness/service/release remain open.

### S19w uploaded MP3 public preparation and cache integrity

The media cache fixture accepts --upload, ingests actual sentinel MP3 bytes through
sources_upload with rootNULL, releases caller bytes, then exercises public
prepare_audio, count/byte cache limits, cached hit reuse, digest/payload/decoder
tampering and restored survivor behavior. Normal and parent sanitizer gates passed
macOS/Linux, terminal handles13085/69756 exit0; logs
/private/tmp/samosa-upload-media-{mac,linux}.log. Linux's first attempt refused a
missing worker binary; make media-worker produced the explicit helper before the
passing rerun. The worker and FFmpeg libraries were normal builds; this checkpoint
sanitizes parent component/fixture code, not decoder dependencies or the worker.
Bundled SQLite is also uninstrumented. --visual --upload support was added but is
not yet qualified here. Uploaded PDF/OCR, staged media integration and complete
freshness/service/Sources release remain open.

### S19w uploaded visual media exact-frame/cache integration

Uploaded H264 RGB lossless64x64 blue video with AAC audio now passes the media
cache fixture's --visual --upload mode on macOS/Linux normal and parent sanitizer
runs. Before prepare_visual, the host uploads bytes with rootNULL and frees the
caller buffer. Tests validate source second1, exact64x64 RGBA blue pixels, shared
audio/visual cache limits, distinct prepared artifact identities, retained hits,
digest/payload/decoder tampering and restored survivors. The normal fixture's PASS
label includes controls, but SAMOSA_SOURCES_TEST_FAULTS was not enabled here; this
checkpoint does not add durable worker-control qualification.

Terminal handles91070/99459 exit0; logs
/private/tmp/samosa-upload-visual-{mac,linux}.log. No ASan/UBSan diagnostics appear.
Worker/FFmpeg and bundled SQLite were normal uninstrumented builds. Fixture
/private/tmp/samosa-upload-blue-rgb.mp4 SHA256
 e0fea8966bb399249cee441ee2f39363453adf279fe1767eb994ae87e7721b88.
Generated using FFmpeg lavfi blue64x64/r25/d3 and sine440Hz/16000Hz/d3, libx264rgb
crf0, AAC, threads1, shortest. The initial video-only fixture failed the required
shared audio/visual test with media_audio_unavailable; adding the audio track fixed
the fixture. This qualifies the combined uploaded media path, not arbitrary codec
coverage, uploaded PDF/OCR, staged media or full Sources/freshness/service release.

### S19w uploaded PDF text/cache/owner integration

The actual PDFium fixture now creates100 pages, uploads PDF bytes through the host
entry point with rootNULL, frees caller bytes and deletes its fixture file before
public read_pdf(page97). It verifies sentinel7319, absolute page97/total100,
identical cache retry and foreign-owner unknown_source. `make test-pdf sanitize-pdf`
passed macOS/Linux, terminal handles57749/47183 exit0; logs
/private/tmp/samosa-upload-pdf-{mac,linux}.log. Native suites report246/247 checks
and CLI qualification passes. No ASan/UBSan diagnostics appear. The initial fixture
compile collided with an existing bytes variable; renaming upload_bytes resolved
it before passing runs. PDFium and bundled SQLite are not sanitizer-instrumented;
the native self worker uses the test executable's component instrumentation.
This qualifies uploaded PDF text/cache/ownership, not uploaded PDF OCR, staged
format-specific publication or complete freshness/service/Sources release.

### S19w uploaded image/PDF OCR: macOS qualified, Linux pending

The native OCR fixture now uploads owned image bytes with rootNULL, frees the
caller copy, selects TOKEN7319 through read_image, checks uncertain-recognition
provenance, cache equality and foreign-owner refusal. It also uploads the real
image PDF, frees caller bytes and deletes its fixture before read_pdf_ocr(page2),
checking the sentinel, absolute page and cache equality.

macOS `make test-ocr-pdf sanitize-ocr-pdf` passed terminally (handle51062 exit0),
1025/1027 checks, log /private/tmp/samosa-upload-ocr-mac.log. Libraries are explicit
Homebrew Tesseract/Leptonica, eng model, actual PDFium; uploaded cases use the native
self worker. These libraries/model and bundled SQLite are not sanitizer-instrumented.
The first image selection used the older sentinel fixture and failed the existing
primitive sentinel check; the font fixture is required for the passing suite.

Linux normal failed the existing helper-test subsecond ready/termination assertion
at line92 before reaching upload cases, log /private/tmp/samosa-upload-ocr-linux.log.
Its check remains intact. A rerun is confirmed live at handle26231, log
/private/tmp/samosa-upload-ocr-linux-retry.log; Linux normal/sanitizer upload OCR
qualification is pending, not passed. Other format-staging/capacity/freshness,
service and complete Sources release requirements remain open.

Follow-up Linux OCR normal rerun passed1028 checks, including uploaded image/PDF
OCR. The same handle26231 continues its sanitizer run and is confirmed live by
tool polling and /proc/42504 (test-ocr-saniti, running with CPU usage). No sanitizer
diagnostics have appeared, but completion is not proved. The preceding monitor
latency failure is retained as unresolved reliability evidence; a passing rerun
does not erase it. tests/test_ocr.c now prints fault name, readiness and elapsed
seconds on recurrence without relaxing the subsecond assertion. This diagnostics
edit is not present in the currently running copied Linux executable.

Handle26231 subsequently exited0. Linux normal/sanitizer both passed1028 checks,
log /private/tmp/samosa-upload-ocr-linux-retry.log, with no ASan/UBSan diagnostics.
Together with macOS51062 this qualifies the uploaded image/PDF OCR cases on both
platforms. The earlier monitor timing failure remains unresolved broader reliability
work; neither the diagnostics-only edit nor a rerun proves it fixed.

### OCR memory-fault ramp and monitor diagnostics

Inspection found the test-only memory fault touches64KiB then sleeps0.1ms,
repeating4096 times to ramp256MiB. The readiness-to-return assertion includes this
ramp before the64MiB limit crossing, so scheduler overhead can consume its budget.
The test-only ramp now touches1MiB between sleeps while preserving256MiB total,
4096-byte resident page writes, the64MiB sampled limit and the existing subsecond
assertion. Production native behavior is unchanged. Fault-name/elapsed diagnostics
remain enabled for any assertion recurrence. The historical Linux failure lacked
these diagnostics, so this inspection does not prove which fault failed then.

Both normal OCR/PDF/upload suites passed after this change; macOS sanitizers also
passed (handle80784 exit0), log /private/tmp/samosa-ocr-monitor-ramp-mac.log.
Linux handle76945 is still live in the sanitizer phase, log
/private/tmp/samosa-ocr-monitor-ramp-linux.log; sanitizer completion is pending.
The earlier failure remains recorded rather than erased by subsequent passes.
Full freshness/Sources/service/architecture release requirements remain open.

Linux OCR handle76945 subsequently exited0 with1028 normal/sanitizer checks after
the test-only ramp/diagnostics changes. Log
/private/tmp/samosa-ocr-monitor-ramp-linux.log. This completes both-platform normal
and component sanitizer gates for those changes without relaxing monitor limits.
The prior failure's exact fault remains unknown; no production fix is claimed.

### Watched file restoration after retained missing observations

The watch registry fixture now follows260 missing checkpoints with recreation of
the original bytes (available), changed bytes (changed), and original bytes again
(available), retaining the watch and advancing three cycles. Existing cancellation
and offline status checks verify the resulting cycle264. macOS normal/sanitizer
passed (handle63015 exit0), log /private/tmp/samosa-watch-restoration-mac.log.
Linux gates are launched at handle69904 and remain pending. This verifies
registered-file restoration transitions, not directory/new-file discovery or full
freshness/service/release completion.

Linux watch-restoration normal/sanitizer gates also completed, handle69904 exit0,
log /private/tmp/samosa-watch-restoration-linux.log. Both platforms qualify the
missing -> available -> changed -> available registered-file transition sequence
and retained checkpoint/cancellation behavior. Bundled SQLite is uninstrumented.
The unchanged PASS label summarizes registry cases; the new assertions cover the
restoration sequence. Full freshness/new-file/index policy remains unfinished.

### Explicit additional-file discovery in existing index jobs

Added index_discover(job_id,operation_id), advertised by describe. It validates
original root/directory bindings, captures bounded inventory, appends unseen paths
at stable new ordinals, and atomically updates total/operation receipt under the
index writer lease. Existing cursor/items/original inventory and historical versions
remain retained. Final host-control and root/directory rechecks precede commit.
The256-item and512-operation caps remain. Extended content coverage is partial;
original inventory coverage still describes the initial captured manifest.

The native fixture indexes one file, creates a second later, discovers it, verifies
an exact offline receipt retry, indexes the added item and finds its literal text.
A subsequent fresh discovery appends no duplicate. Existing SIGKILL/owner/stale/
corruption/quota regressions pass. `make test-index sanitize-index` passed final
macOS/Linux665 checks each, terminal handles60254/10454 exit0; logs
/private/tmp/samosa-index-discover-final-{mac,linux}.log. Initial fixture name
collision and GCC strict indentation warnings were corrected before final passes.
Bundled SQLite is uninstrumented. Discovery-specific cancellation, quota, root-race
and crash interleavings still need tests. Latest-scan reporting, automatic rescans,
failed-item recovery, broader freshness and complete Sources release remain open.

### Latest discovery scan coverage — macOS/Linux checkpoint

Job replies now expose latest_discovery separately from the original inventory:
null before discovery, otherwise the scan cycle, observed count, inventory coverage
and captured_snapshot freshness. One manifest per job (524288-byte bound), its
SHA-256 and cycle commit with newly appended items and the exact operation receipt.
Retained metadata is validated before rendering a job response, including integer
cycle, text/digest bounds, manifest shape and original directory scope identity.
Overflow is refused. This reports a captured bounded scan, not complete current
filesystem coverage.

The public native fixture verifies the initial null state, first scan with two
files, a later scan with one file while the job retains two indexed items, offline
latest-scan reads, owner refusal, checksum corruption refusal without a partial
job envelope, and exact offline receipt retry after scan metadata corruption.
A new discovery against corrupted retained metadata is refused.

make test-index sanitize-index passed on macOS and Linux: 712 checks per normal
and ASan/UBSan run. Terminal handles53577/21945 exit0; logs
/private/tmp/samosa-index-discovery-coverage-{mac,linux}.log contain no sanitizer
diagnostics. Bundled SQLite remains uninstrumented. Related normal
make test-index-freshness test-reindex passed on both platforms, including existing
extraction-time mutation and actual SIGKILL/reindex recovery checks; handles
86992/10175 exit0, logs /private/tmp/samosa-discovery-integration-{mac,linux}.log.
Those inherited crash tests are not discovery-transaction crash qualification.

Discovery-specific cancellation, quota, root-race and crash interleavings, automatic
rescan scheduling, failed-item recovery and complete freshness policies remain open.
Sources work area #2 and the overall architecture rebuild remain active.

### Discovery transaction interruption and capacity qualification

Discovery now samples host cancellation/deadline and rechecks directory/root
bindings after saving latest-scan metadata and its operation receipt, immediately
before SQLite COMMIT. Previously the last check preceded receipt preparation.
A test-only bounded rendezvous exposes this boundary; production builds omit it.

The new native public fixture pauses a fresh process at that boundary. Cancellation
and deadline expiry return typed failures, and independent SQLite inspection
proves prior total/items/cycle/operation counts are retained. Actual SIGKILL before
the first discovery commit leaves no new item/scan/receipt; the same request then
succeeds after recovery. SIGKILL while replacing an existing successful scan
also preserves its prior counts/cycle. Root replacement and nested-directory
replacement after receipt preparation are refused and rolled back; restoring the
original directory allows the same uncommitted operation to succeed. Filling an
actual job to256 items, then removing one and adding a different filename, refuses
the accumulated item overflow and preserves its previous scan/receipt. These are
commit-boundary checks, not an exhaustive filesystem race proof.

Added make test-index-discovery/sanitize-index-discovery to standard test/sanitize
gates. Final normal and ASan/UBSan runs (with sanitizer recovery disabled) pass
1158 checks each on macOS/Linux. Terminal handles46255/3026 exit0; logs
/private/tmp/samosa-discovery-transactions-qualified-{mac,linux}.log. No sanitizer
diagnostics; bundled SQLite remains uninstrumented. The ordinary index suite
also passes712 checks on both platforms with the new final commit checks,
handles68740/4656 exit0; logs
/private/tmp/samosa-discovery-transactions-final-{mac,linux}.log.

Automatic discovery scheduling, failed-item recovery, broader watch/freshness
policy and full Sources qualification remain open. Sources #2 and the full
Sources/Inference/Agent/App rebuild remain active.

### Automatic discovery and pending initial indexing — platform checkpoint

Added host discovery_interval_ms (zero disables; enabled100..60000ms), CLI
--discovery-interval-ms and public index_refresh_step. The existing owned index
runtime invokes refresh before observation-driven reindexing when enabled.
A refresh attempts at most one scan and one pending initial item. Selection
matches owner and original root scope, with independent fair last-turn counters.
Eight persistent per-job rows reserve attempts before content work; recoverable
directory/limit failures back off1..60s, while healthy jobs remain eligible.
Integer/counter bounds and overflow are validated. Explicit requests retain their
receipts, while automatic scan/capture creates no background client receipts.
The index writer lease spans selection/reservation/work; child operations reuse
the held lease. Scan and initial commits are independent restart checkpoints.

The public native fixture moves one job's directory away and proves a healthy
peer still scans and indexes, with both failed phases receiving cooldowns. It
adds a later file, indexes it, then executes520 further bounded refresh calls and
independently verifies only two client receipts/two scheduler rows remain. It
refuses scheduler overflow, disabled and invalid configuration. A real CLI
worker indexes the initial inventory without explicit index_step, discovers later
files, refuses a duplicate runtime, recovers after actual SIGKILL/restart, discovers
another file, shuts down on TERM and preserves offline search evidence.

make test-index-refresh passed on macOS/Linux (5529/5523 checks, variable polling
counts), terminal handles71924/75247 exit0. Logs /private/tmp/samosa-refresh-{mac,linux}.log.
make sanitize-index-refresh test-queue-runtime test-reindex test-index-discovery
passed on both platforms, terminal handles25832/7030 exit0. ASan/UBSan instrumented
both fixture and actual runtime, sanitizer recovery disabled; refresh5534/5573
checks, normal discovery transaction regressions1158 each, and existing worker/
reindex gates pass. Logs /private/tmp/samosa-refresh-qualified-{mac,linux}.log have
no sanitizer diagnostics. Bundled SQLite remains uninstrumented.
The ordinary manual index/discovery refactor gates also passed712/1158 checks on
macOS, handle37714 exit0, /private/tmp/samosa-refresh-refactor-mac.log.
New refresh gates are part of standard test/sanitize and included by source
release packaging. No full release was assembled or claimed in this checkpoint.

Automatic refresh does not reset failed/skipped items or observe changes to
already indexed locators. Failed-item recovery, complete watch/freshness policies,
clock/resource/race qualification and full Sources release remain open. Sources
work area #2 and the full architecture rebuild remain active.

### Supported failed/skipped text recovery — platform checkpoint

Discovery now queues eligible supported text locators already marked failed or
skipped. index_recovery retains at most2048 job/item targets with metadata
identity, fair turn and exponentially increasing1..60s retry reservations.
Repeated scans of the same target preserve the cooldown; changed identities reset
its next time/failure level. Refresh now attempts one recovery item in addition
to its existing one scan/one initial item, with recovered_items and recovery_error
response fields. The real runtime validates the additional bounded progress field.
No background client receipt is created.

Recovery uses the existing identity-qualified registration/live-text read, original
root/directory validation, evidence quota and host controls. Accepting evidence,
deleting queued intent and adding a bounded recovery marker commit together.
The original cursor stays fixed; already successful indexed items/versions are
not replaced. Job replies expose recovered_items_total and retain partial combined
coverage after repair, rather than reporting repaired evidence as matching the
original inventory.

The expanded native fixture starts with missing, changed/malformed, healthy and
nonregular items. Initial indexing leaves two failures and one skipped item.
A bounded refresh retries malformed UTF-8 and reserves a cooldown; an explicit
scan of the unchanged target leaves that exact next time/level unchanged. Returning
the missing file, correcting malformed text and replacing the skipped directory
with a text file resets three target cooldowns. Subsequent bounded recoveries
accept all three, keep the original cursor4, clear queued rows, report three
recovery markers/partial coverage, and preserve the healthy original evidence
alongside new recovered evidence in offline public search.

make test-index-refresh sanitize-index-refresh test-index test-index-discovery
test-reindex passed on both platforms. Terminal handles67655/42260 exit0; logs
/private/tmp/samosa-recovery-qualified-{mac,linux}.log. Refresh normal7268/7286
checks, ASan/UBSan7231/7306 checks (poll counts vary), manual index712 and discovery
transactions1158 checks each; existing actual SIGKILL/reindex regressions pass.
Sanitizers instrument the fixture and real refresh runtime, with recovery disabled.
No sanitizer diagnostics; bundled SQLite remains uninstrumented.

This does not qualify all recovery transaction crash/control/race/corruption
interleavings or broader readers. Public queue health, remaining watch/freshness
policy, acquisition, enrichment/Inference/Chutni, bounded authenticated service
and complete independent release qualification remain required. Sources work
area #2 and the full architecture rebuild remain active.

### Recovery transaction interruption and offline queue health

Added a test-only bounded rendezvous after recovery's item update, intent deletion
and repair marker insertion, before the existing final host-control/scope checks
and COMMIT. It is omitted from production builds. The native public fixture
creates an actually failed initial item, restores the file, and pauses fresh
processes at this boundary. Cancellation/deadline return typed top-level failures.
Actual SIGKILL, root replacement and nested-directory replacement also preserve
the prior failed item, null index evidence, cursor1, no repair marker and one
reserved queued retry. Root/directory refusals are per-target refresh failures.
Changing the target then permits public retry and offline reading of accepted
recovery state. Immutable source registrations/cache entries from capture are
independent checkpoints; the test asserts index acceptance rollback.

Job replies now include recovery_queue: pending_items, highest_failure_level and
earliest_retry_unix_ms (null when empty). Types, identity format, ordinal bounds
and referenced failed/skipped status are checked before output. The fixture
verifies offline queue health after each interrupted attempt, empty queue after
success, and corruption refusal with no partial job response. Discovery validates
retained queue metadata before a changed-target upsert, so a rescan cannot silently
normalize an invalid retry level; this is directly tested.

make test-index-recovery sanitize-index-recovery test-index-refresh passed final
macOS/Linux runs, handles55563/17912 exit0. Recovery349 checks each normal and
ASan/UBSan run; refresh regression7262/7268 checks (poll counts vary). Logs
/private/tmp/samosa-recovery-health-final-{mac,linux}.log have no sanitizer
diagnostics. Recovery fixture and spawned worker are instrumented, sanitizer
recovery disabled; bundled SQLite remains uninstrumented. The new recovery
gates are included in standard test/sanitize and source release packaging.

The component checkpoint is documented in components/samosa-sources/docs/S19x.md.
Further corruption/resource/race qualification, complete watch/freshness policy,
broader readers, acquisition, enrichment/Inference/Chutni, authenticated bounded
service and the complete independent Sources release remain open. No C01-C14
milestone or overall goal is closed by these commit-boundary tests. Sources #2
and the full architecture rebuild remain active.

### Periodic indexed-locator freshness feeds retained reindexing

Enabled refresh now performs one indexed-locator observation in addition to its
scan/initial/recovery phases. Eight bounded per-job schedule rows retain fair turn,
last ordinal and cooldown. The selected job rotates through indexed items. The
latest literal evidence checksum/version and original root/directory scope are
validated before calling a new private locator observation interface. That path
shares existing full-file hash, physical revalidation, owned-source admission and
transactional observation/index-intent commit, without a client operation receipt.
Changed/missing/restored observations feed the existing reindex runner. Public
refresh reports observed_items0..1 and a nullable freshness_error. Common locator
admission also checks revision integer validity and overflow before incrementing.

The actual CLI fixture starts with two indexed files, discovers two additional
files across SIGKILL/restart, then changes an indexed file's bytes while preserving
its inode, size and exact original modification timestamp. Public search eventually
finds the new literal evidence. Removing another indexed file creates pending
intent without a manual watch; restoring it with changed bytes reindexes it and
clears intent. Independent SQLite inspection proves two retained new versions,
all four original text items, one freshness schedule row and zero client locator
receipts. Offline search returns both historical7319 evidence and new7419 evidence.
The fixture's initial assumption that historical matches should disappear was
corrected after actual state inspection showed new content was already accepted
and old evidence intentionally retained. The corrected test preserves that contract.

Final make test-index-refresh passed macOS/Linux9060/9072 checks, handles4148/13639
exit0; logs /private/tmp/samosa-auto-freshness-final-{mac,linux}.log.
make sanitize-index-refresh test-queue-runtime test-locator test-index-recovery
passed both platforms, handles24013/71830 exit0; ASan/UBSan refresh9007/9226 checks
(variable polling counts), locator2608 and recovery349 each, plus existing owned
worker duplicate/root/state/lease/shutdown/SIGKILL gates. Logs
/private/tmp/samosa-auto-freshness-qualified-{mac,linux}.log have no sanitizer
diagnostics. Both refresh fixture and actual runtime are instrumented; bundled
SQLite remains uninstrumented.

The first failed Linux fixture's orphan worker was identified in /proc and stopped
with TERM before final qualification; it was not confused with a still-running
qualification handle. The macOS failed-fixture worker was already absent when
cleanup was attempted. No production service or legacy path was switched.

This is bounded checkpoint freshness. Complete sweep latency depends on polling
pace, admitted work and job/item count; it does not assert instantaneous current
filesystem completeness. Periodic-specific race/resource/corruption and remaining
watch/freshness/service/release gates remain required. Sources #2 and the overall
Sources/Inference/Agent/App C01-C14 rebuild remain active.

### Staged periodic observation acceptance and shared locator commit

Shared locator commit now performs final host-control, held file/parent and named
root validation after staging observation history, index intent and any receipt/
watch checkpoint, immediately before COMMIT. Previously physical checks preceded
the transaction, leaving a gap after staging. A bounded test-only rendezvous at
that boundary is omitted from production builds.

The new native periodic-observation fixture indexes a real text file and commits
a baseline available observation (revision1). Fresh processes then hash changed
bytes and pause after staging the replacement observation and index intent.
Cancellation/deadline, actual SIGKILL, file mutation/deletion and root/directory
replacement leave revision1, the original indexed text/cursor, no new indexed
version and zero pending intent. Independent SQLite inspection proves rollback
after every case. The next accepted observation reaches revision2, and public
index_queue_step accepts a new immutable version visible through offline search.
A stored signed-integer maximum revision is explicitly refused without overflow.
Reservations/scans from other refresh phases remain independent checkpoints.

make test-index-observation sanitize-index-observation passed macOS/Linux428
checks per normal and ASan/UBSan run, handles23358/35042 exit0. Logs
/private/tmp/samosa-observation-controls-{mac,linux}.log have no sanitizer
diagnostics. Fixture and spawned fresh-process worker are instrumented with
sanitizer recovery disabled; bundled SQLite remains uninstrumented.
The new gates are part of standard test/sanitize and source release packaging.

Related make test-watch test-locator test-index-refresh passed both platforms,
handles43866/57433 exit0; locator2608 checks each, refresh9072/9096 checks
(variable polling counts), and existing watch lifecycle/control/cleanup/lease
regressions pass. Logs /private/tmp/samosa-observation-regressions-{mac,linux}.log.
These commit-boundary cases do not prove every filesystem race/resource
interleaving. Remaining freshness policy/resource/corruption/service and complete
release gates remain open. Sources #2 and the full C01-C14 rebuild stay active.


### Sources #3 — first host HTTP acquisition checkpoint (2026-10-08)

Added sources_fetch host C API, optional libcurl worker, shared exact-origin URL
validation, response header64KiB/body1MiB streaming caps, redirect/proxy refusal,
TLS verification configuration and parent DTO/base64/SHA-256 validation. Only
validated bytes enter existing immutable admission, format checks and owner
quotas; offline reads retain acquisition URL provenance. No sibling runtime
or curl CLI dependency was introduced. Public JSON/CLI fetch dispatch and a
multi-acquisition receipt/provenance ledger remain pending.

Actual native local HTTP fixtures exercise normal and chunked bodies, exact1MiB,
oversize, truncated Content-Length, redirect refusal, excessive headers, stalled
request cancellation/deadline, unauthorized URL variants, malformed/tampered
private responses, offline immutable text and cross-owner refusal. Initial
fixture revisions exposed a body framing error and a cancellation assertion
accidentally shared with the deadline case; both fixture bugs were fixed. Strict
Linux compilation also caught a misleading-indentation warning, now corrected.

make test-fetch sanitize-fetch passed with terminal exit0 on both platforms:
handles81714/13240, logs /private/tmp/samosa-fetch-{mac,linux}.log. Each macOS run
reports213 checks and each Linux run211 (control polling counts vary). Component
and actual worker are ASan/UBSan instrumented with recovery disabled; bundled
SQLite and system libcurl/TLS dependencies are not instrumented. No sanitizer
diagnostics. pkg-config reports libcurl8.7.1 macOS and7.88.1 Linux.

Related make test-upload test-control passed terminal exit0 on both platforms,
handles94819/5850, logs /private/tmp/samosa-fetch-regressions-{mac,linux}.log;
existing immutable upload/format/owner/quota/stream/control gates and135 request
control checks pass. Fetch qualification gates are optional because their worker
requires libcurl; all new source/test/contract files are included by existing
source release packaging.

HTTPS certificate fixtures, public controlled operations/durable receipts,
provider credentials/search adapters, larger remote bodies and service admission
are still open. This checkpoint advances Sources #3; it closes neither Sources
nor C01-C14. See components/samosa-sources/contracts/fetch-v1.md.

Minimal make all builds also pass on macOS/Linux without libcurl linkage,
handles99251/38548 exit0; logs/private/tmp/samosa-fetch-core-{mac,linux}.log.


### Sources #3 — public acquisition and atomic receipt integration (2026-10-08)

Public op=fetch now accepts acquisition_id/url/format, exposes host CLI origin
and worker grants, and joins durable operation correlation/parent/budget/status/
cancellation controls. describe advertises fetch configuration and body/header/
receipt limits. Host configuration remains unavailable to request fields.

New owner-scoped fetch_receipts retain each acquisition URL/format and immutable
response digest. Receipt and snapshot/format/owner claim commit in one Sources
transaction after network completion. Receipt replay independently checks its
shape, request parameters, response digest and actual owning snapshot bytes/hash
and format claim. An execution attempt can be interrupted after acquisition
commit; a new operation ID with the same acquisition ID recovers accepted
content offline. Global128 receipt cap is independent of operation history.

Native fixtures now exercise public dispatch/operation status, normalized exact
retry, new-attempt offline acquisition replay without worker/origin grants,
parameter/owner refusal, receipt corruption and actual CLI subprocess capture
and fresh-process offline replay. Test-only pre/post commit hooks support actual
SIGKILL on either side of atomic publication, precommit public cancellation and
scoped deadline, and postcommit late cancellation. Independent SQLite counts
prove receipt/snapshot/owner claim rollback before commit and persistence after
commit; accepted recovery and operation statuses are checked.

Initial integration test-fetch/sanitize-fetch passed both platforms (handles
80629/11448), followed by broader CLI and commit-boundary qualification. During
the latter Linux sanitizer run (handle93074 exit2), the deadline case failed to
reach its staging marker. Inspection of that exact attempt's stored operation
confirmed failed/deadline_exceeded with no marker: the fixture's1s deadline
expired before staging. The fixture now grants5s to reach staging, still waits
for the real scoped deadline before releasing the staged transaction, and logs
its boundary case. This changes test setup only, not runtime deadline policy.
The completed macOS broader run (handle35414 exit0) was retained as evidence;
current fixture qualification is rerun independently on both platforms.

Related make test-operations test-control test-upload-sessions passed terminal
exit0 on both platforms, handles93031/21322. Durable operations750 checks,
request control135 checks and existing durable upload/transaction/owner/cache/
control regressions pass. Logs
/private/tmp/samosa-fetch-public-regressions-{mac,linux}.log. No Source release
or C01-C14 milestone is closed; TLS/search/provider/concurrency/full component
qualification remain open. Current contract: fetch-v1.md.


Final revised make test-fetch sanitize-fetch passed terminal exit0 on both
platforms, handles80599/21919. Logs
/private/tmp/samosa-fetch-public-qualified-{mac,linux}.log. Normal/sanitizer
counts: macOS834/853, Linux845/1219; counts vary with control/marker polling.
All five logged publication boundary cases pass on both normal and sanitizer
runs. Component, real native worker and real CLI executable are instrumented;
SQLite/libcurl/TLS dependencies remain normal dependency builds. No ASan/UBSan
diagnostics. Test hooks are compiled only into the fixture's component copy;
product/default CLI and actual HTTP worker exclude the commit hooks. The
remaining TLS/provider/search/larger-body/concurrency/release requirements remain
open. Goal stays active.


### Sources #3 — actual HTTPS trust qualification (2026-10-09)

Added optional host-owned fetch_ca_pem (<=8192 bytes), bounded capture through
CLI --fetch-ca-file with regular-file/no-follow/NUL/physical-metadata checks,
and immutable PEM transport through the <=16384-byte private worker request.
The worker uses an immediately unlinked private file's retained descriptor alias
for CURLOPT_CAINFO, retaining peer/hostname verification and disabling CA-path
fallback for supplied trust. No host trust path reaches the worker or public
request. New flat TLS error classes preserve private transport diagnostics.
Only the isolated fetch worker ignores SIGPIPE, consistent with libcurl's
NOSIGNAL contract; caller signal state is unchanged.

Native OpenSSL3 fixture code generates CA/leaf keys and certificates in memory
and runs an actual TLS1.2 server. Product Sources still has no OpenSSL fixture
SDK dependency. Optional Linux QA installed libssl-dev (log
/private/tmp/samosa-fetch-tls-sdk-linux.log); fixture SDK versions macOS3.6.4 and
Linux3.0.22. The tested product curl is reference macOS8.7.1 and Linux7.88.1.

Positive host/API and real CLI custom-CA transfers pass. Unknown trust issuer,
wrong issuer key, hostname mismatch, expired leaf and malformed trust are
refused before immutable admission. Host PEM oversize and CLI symlink/oversize/
embedded-NUL paths are refused. Independent SQLite checks show one accepted
snapshot/owning claim and one CLI receipt across all cases; offline text reads
remain available. The wrong issuer fixture uses identical issuer subject/serial
with a different signing key; curl reports a TLS handshake failure rather than
a verification failure on these builds, and the fixture correctly accepts either
flat TLS refusal class. Initial macOS compile required _DARWIN_C_SOURCE for
O_NOFOLLOW; corrected without weakening file checks.

make test-fetch-tls sanitize-fetch-tls test-fetch sanitize-fetch passed terminal
exit0 both platforms, handles46597/5780. Logs
/private/tmp/samosa-fetch-tls-qualified-{mac,linux}.log. Actual TLS320 checks in
each normal and sanitizer run. Existing HTTP/control/public/receipt/CLI checks
macOS880/908, Linux887/1246 (polling counts vary). Component, real exec worker,
real CLI and fixture are instrumented by ASan/UBSan with recovery disabled;
SQLite/libcurl/TLS SDK dependencies remain normal builds. No sanitizer reports.
Sanitized worker/CLI rules now track source/config dependencies and are shared
between optional HTTP/TLS gates. New fixture is included in source packaging.

Actual public https://curl.se/libcurl/c/CURLOPT_CAINFO.html acquisition succeeded
using default system trust, no PEM/proxy/verification bypass, on both platforms:
8726 identical HTML bytes, source/version
ac492572df4d538b473c0faacebc84051adb2cd5177cbb087ca0538860cb6e3a.
JSON evidence /private/tmp/samosa-fetch-live-tls-{mac,linux}.json, states recorded
in the adjacent -state.txt files. Independent root-free, worker-free offline
read_html(0..256) succeeds on both platforms, explicit partial/static-readable-
text coverage,2802 readable bytes,256 inspected. Evidence ID
 dc7c6dae03b0b1b7ef139ffc08d3e88a1fa96587c032c102c07a801020e2ae52
matches. Read results /private/tmp/samosa-fetch-live-read-{mac,linux}.json.
Successful acquisition replies were independently parsed/asserted; the offline
read invocations returned native CLI exit0. Shell wrapper terminal status alone
was not used as acquisition evidence.

Reference API documentation checked: curl.se/libcurl/c/CURLOPT_CAINFO.html,
CURLOPT_CAINFO_BLOB.html and CURLOPT_NOSIGNAL.html. The latter documents why
NOSIGNAL alone cannot guarantee suppression of socket SIGPIPE. Direct CAINFO
supports the reference backend; CAINFO_BLOB does not support SecureTransport,
so no backend-specific blob fallback was introduced.

Broader certificate/backend/trust-file races, larger remote inputs, concurrent
acquisition/resource/corruption gates, search/provider adapters, service bounds
and complete release acceptance remain open. No Sources/C01-C14 closure.
Search adapter requirements were rechecked in the existing TASKS_WEB_SEARCH.md
and samosa_gateway.c declarative presets, including Parallel keyless MCP and
keyed/generic provider paths. Current official Parallel programmatic-use docs
still declare the anonymous Search MCP endpoint and its optional authentication.
No MCP client configuration/plugin or legacy production paths were changed.


### Sources search adapter integration

The legacy W1 provider declarations now live behind Sources' host configuration
and public op=search. src/search.c normalizes real JSON/SSE replies, applies
result/description limits and credential redaction, admits owned immutable
metadata, and uses existing operation control/replay. The optional fetch worker
now handles validated GET/POST/header inputs without linking networking into the
core. No gateway import or legacy production switch.

Normal and ASan/UBSan search fixtures pass on macOS ARM64 and native ARM64 Debian:
215 API/actual-worker checks per run, plus actual CLI private-file validation,
credential redaction, offline replay/read and host configuration refusal. The
fixture assertion originally compared a JSON escape spelling rather than the
decoded description; it now checks the semantic first/newline/second value.
The CLI also checks an overridden Brave GET declaration, URI query encoding and
its expected credential header against an actual local HTTP server; this is a
preset transport fixture, not a live paid-provider qualification.

Actual keyless Parallel searches for official libcurl CAINFO documentation
returned eight results on each platform through Sources' CLI and worker with
system TLS verification. Each description is at most 400 Unicode characters.
No provider or worker configuration is needed for exact completed-operation
replay or subsequent bounded read_text; both were independently asserted on both
platforms. Search results differ by observation and are not expected to have
cross-platform identical source hashes. Live results:
/private/tmp/samosa-search-live-{mac,linux}.json; saved state locations are in the
adjacent -state.txt files, and offline reads in
/private/tmp/samosa-search-live-offline-{mac,linux}.json.

Search logs: /private/tmp/samosa-search-qualified-{mac,linux}.log.
Mac HTTP/TLS regression log:
/private/tmp/samosa-search-http-regressions-mac.log.
The Linux search log also contains the HTTP/TLS regression execution.
SQLite and SDK libcurl/OpenSSL remain uninstrumented dependencies; owned core,
fixtures and executed CLI/worker paths are instrumented. CLI tests use Python3
only as the external test driver.

Paid provider declarations require live qualification. Search has no separate
acquisition_id receipt and does not promise fetch's post-commit crash recovery.
Broader provider/acquisition policies, service admission/concurrency, enrichment,
semantic/Chutni integration, model-backed preparation and full release remain
open. Sources #3 and C01-C14 are not closed by these checks.

Final terminal status: macOS search/CLI normal+sanitizer and HTTP/TLS
normal+sanitizer commands each exited0. Linux combined search/CLI and HTTP/TLS
normal+sanitizer command exited0; final GET CLI extension also exited0 in both
normal and sanitizer executions. HTTP checks: macOS884/904 and Linux872/1241
(normal/sanitizer, polling counts vary). TLS320 checks per platform per mode.
The Linux QA container was observed stopped with exit0/OOMKilled=false between
completed search checks and regression startup; it was started again after
terminal state inspection, retaining the same candidate and state files.


### Sources existing-store Chutni retrieval

Sources op=memory_search uses the existing external Chutni0.3.0 SDK JSON ABI
(chutni_open/call/free/close) in an isolated native worker. The SDK opens the
explicit host-granted store read-only; Sources never parses its private catalog
or implements another FTS ranking engine. Parent validation checks public store
and artifact/source references, query identity, current catalog freshness, finite
BM25 scores, result shape and bounds. Normalized partial retrieval metadata uses
existing owned immutable admission, operation control and offline replay/read.
There is no Chutni store mutation, auto-discovery or client-selected store path.

Actual SDK integration tests create 12 standalone memory artifacts using its
public API, then verify eight returned results, zero-result queries, 512-byte
query limit, SDK-unavailable refusal, owner isolation, exact offline operation
replay, bounded offline text reading, deadline expiry and unchanged store counts.
Independent C parent-validator tests run40 checks for malformed shape/references,
wrong query/freshness/score type and nonfinite scores. Both integration and parent
checks pass normal/ASan/UBSan on macOS ARM64 and native ARM64 Debian. Owned Sources
CLI/worker/validator code is instrumented; Chutni and SQLite are separate normal
SDK dependencies. Python3 is only the integration test driver.

An initial reference SDK Linux shared-library build lacked POSIX feature macros.
Its build output warned of implicit strdup/realpath declarations; a dedicated
protocol probe and ASan worker stack located the crash in SDK cj_set/strcmp
called by chutni_call(store_info). The faulty SDK was rebuilt, without source
rewrites, with -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 and
-Werror=implicit-function-declaration. The same actual integration then passed.
This requirement is documented in DEPENDENCIES.md; the adapter does not bypass a
broken SDK or substitute its storage implementation.

Reference SDK artifact SHA256:
macOS86bacfa57c0c8422ed3402bec11c0a13c22cfe3ec5a4f2dcb6556b964c889a2d
Linuxcf67f241089788323935e75384f6febd220943b7e3d06c12957c15677429150b
Linux SDK build log /private/tmp/samosa-chutni-sdk-linux.log; actual copied
external dependency /tmp/samosa-chutni-sdk, library build-linux/libchutni.so.
Sources logs /private/tmp/samosa-chutni-qualified-{mac,linux}.log contain the
actual native and sanitizer commands, parent tests and shared control/search
regressions. No private SDK source is incorporated in Sources builds.

This delivers retrieval from existing stores, not publication of Sources index
outputs. Durable cross-store publication/recovery, full referenced artifact and
provenance retrieval, model enrichment, embeddings/semantic retrieval, service
bounds and full independent release remain open. Sources area4 and C01-C14 stay
open; no legacy route was retired or production switched.

Final complete command exited0 on each platform:
make test-chutni sanitize-chutni test-search sanitize-search test-control
test-operations CHUTNI_LIBRARY=<platform SDK library>.
Both logs contain40 parent-validator checks in each mode, actual SDK integration
passes in each mode,215 search-worker checks and CLI passes in each mode,
135 shared control checks and750 durable-operation checks. The final source
includes the Chutni error whitelist and actual bad-SDK refusal test. No test or
build process remains active for this checkpoint.


## Sources indexed-text Chutni publication (2026-10-08)

The existing Chutni SDK engine now receives indexed literal text from Sources.
No replacement Chutni storage/index implementation or vendor source rewrite was
introduced. Public index_publish_step/index_publish_queue_step operations and
an optional --index-run publication phase share durable owner/root/store-bound
intents, bounded retry scheduling, independent worker/parent response validation
and offline publication checkpoint history. Full Source snapshots and literal
byte ranges are verified before publication. The SDK supplies its BLAKE3 identity,
root/catalog registration and refresh, atomic put_file_outputs and retained
artifact lookup. Sources SHA-256 identities remain distinct.

Actual test-publication/sanitize-publication fixtures create an external store
through its public SDK and exercise first registration, changed-version refresh,
retrieval of published text, grant/owner/UUID refusals, completed-operation offline
replay and status. A real SIGKILL after SDK commit but before Sources receipt
commit leaves a pending intent; after a further live-file change, retry recovers
the retained artifact without increasing SDK artifact count. A separate test-only
fixture catalog mutation changes that artifact's inline text while retaining its
provenance. Recovery must refuse with chutni_publication_conflict and retain the
pending intent, then succeed after repair. Production never opens that catalog
through Sources SQL. The actual automatic runtime also publishes a bounded UTF-8
opening range of a file larger than64KiB and shuts down cleanly on TERM.

The earlier macOS Chutni0.3.0 binary did not implement put_file_outputs. An isolated
fresh build of the existing SDK source supplies the operation; the legacy binary
was not overwritten. Feature declarations required by the dependency are recorded
in Sources DEPENDENCIES.md. Current publication SDK SHA256:
macOS bb894e6267a380ead855fff6f07ae0d19cdc204644c9ff3eb3345dfb10472d1a
Linux cf67f241089788323935e75384f6febd220943b7e3d06c12957c15677429150b
Mac library /private/tmp/samosa-chutni-publish-sdk-mac/libchutni.dylib;
Linux library /tmp/samosa-chutni-sdk/build-linux/libchutni.so.

The combined normal/sanitizer publication and Chutni retrieval qualification,
queue-runtime, index-freshness, control135 and durable-operation750 checks exited0
on both platforms. Logs:
/private/tmp/samosa-publication-qualified-mac.log
/private/tmp/samosa-publication-qualified-linux.log
Each includes70 parent-validator checks in both modes and actual SDK tests.
After adding actual artifact-content validation, both final commands exited0:
make test-publication sanitize-publication CHUTNI_LIBRARY=<platform SDK library>.
Normal and ASan/UBSan runs pass the actual content-corruption refusal, restored
artifact recovery, automatic bounded publication and post-SDK SIGKILL fixture
on both macOS and native ARM64 Linux. Final logs:
/private/tmp/samosa-publication-content-mac.log
/private/tmp/samosa-publication-content-linux.log
No build or test process remains active for this checkpoint.

Remaining: full reader-artifact publication/retrieval, broader store/root races,
capacity/concurrency qualification, enrichment/semantic retrieval, public
Inference media extraction, authenticated bounded service, complete independent
releases and the full C01-C14 audit. This checkpoint does not close Sources area4
or the overall rebuild. No legacy production route was retired or switched.


## Sources referenced Chutni artifact reads (2026-10-08)

Public memory_read now consumes store/source/artifact UUIDs and a bounded UTF-8
byte range. It reaches the existing external Chutni SDK through the isolated
worker, preserves artifact status/freshness and full producer/derivation
provenance, validates the normalized response independently in the parent, and
uses existing owned immutable metadata captures and durable operation replay.
No Chutni private schema/header is incorporated in Sources.

The SDK source_context gained an optional artifact_id filter and a bounded
start_byte/end_byte range. This extends its existing text loading/provenance
operation; it adds no replacement catalog, storage engine or ranker. Existing
unfiltered behavior is retained. A range can reach beyond the old262144-byte
prefix limit. Start offsets inside UTF-8 characters fail; a split final character
is omitted. Invalid, fractional, negative or excessive SDK offsets are refused
before integer conversion. Text ranges expose total bytes and actual returned
boundaries. Sources supports text artifacts through64MiB,64KiB windows and256KiB
normalized response bounds, with explicit complete/partial artifact coverage.
Non-text objects and over-limit provenance remain pending/refused.

Actual Sources fixtures use13 SDK memory artifacts (12 search fixtures plus one
360KB UTF-8 artifact), test search-to-read with complete provenance, a later range
starting262144, correct split-tail trimming, subsequent tail read, malformed start,
wrong UUIDs, unchanged store counts, owner claims and offline replay/read.
The publication fixture additionally reads its published literal text and original
Sources evidence through the new public operation. Parent validator cases now
total115 across retrieval, publication and artifact-read DTO mutations.

The SDK call-surface suite includes six new range/invalid-offset checks and now
reports48 passes; conformance reports85 passes/0 failures/1 existing gap,
CLI32 passes, MCP130 checks, Samosa7-operation compatibility and the Python binding
check. Full normal SDK tests exited0 on macOS and native ARM64 Linux. Linux strict
vendor test compilation encountered pre-existing GCC format-truncation warnings;
-Wno-error=format-truncation keeps them visible without blocking SDK regressions.
Sources itself keeps its existing strict -Werror gates. SDK test link targets
were corrected to include the existing platform LDLIBS (-lm on Linux).

SDK normal build artifacts used for the final Sources run:
macOS /private/tmp/samosa-chutni-artifact-qa/libchutni.dylib
SHA25682dec16c06bbcf229ed9f45ca80c60461fbc2157f39eb964bdd2ea1c5b2713f5
Linux /tmp/samosa-chutni-sdk/build-artifact-qa/libchutni.so
SHA256e192239d34085c1bd603bad93539a2a34d895a739b57d8e02f57b0fda1b82607
SDK normal test logs /private/tmp/samosa-artifact-sdk-test-{mac,linux}.log.

Final Sources commands exited0 on both platforms:
make test-chutni sanitize-chutni test-publication sanitize-publication test-control
test-operations CHUTNI_LIBRARY=<SDK artifact above>.
Logs /private/tmp/samosa-artifact-qualified-{mac,linux}.log contain115 parent checks
and actual artifact-read/SDK tests in each mode, actual publication/recovery and
published-artifact-read tests in each mode, shared control135 and operations750.
The SDK's full macOS sanitize command exited0, including instrumented conformance,
call surface, CLI and MCP tests. Linux instrumented conformance passed; its first
call-surface execution found a72-byte leak in the existing fixture's object-hash
selection loop (overwriting strdup without freeing the prior hash). Both fixture
loops now free the prior allocation. This changes test cleanup, not SDK storage
behavior. The affected fully instrumented Linux call-surface binary was rebuilt from the
same SDK/fixture sources with ASan/UBSan; its48 checks and the following CLI32
and MCP130 checks exited0, without sanitizer diagnostics. The rebuilt standalone
call-surface test omitted the release-version define and therefore identifies
itself as0.0.0-unversioned; the qualified normal SDK libraries above and the other
SDK sanitizer executables retain their0.3.0 build identity. This fixture binary
is not a release artifact. Authoritative repaired log:
/private/tmp/samosa-artifact-sdk-sanitize-linux-repaired.log. The original Linux
make sanitize command exited2 at the fixture leak; its completed instrumented
conformance result plus the repaired call-surface and remaining suites establish
coverage of its executed tests, rather than a claim that the original command
exited0. Initial sanitizer logs remain
/private/tmp/samosa-artifact-sdk-sanitize-{mac,linux}.log. Normal SDK suites also
exited0 after the fixture-only cleanup on both platforms. No build/test handle
remains active for this checkpoint.

This does not close area4: other reader outputs, non-text artifact retrieval,
enrichment/semantic retrieval and broader capacity/replacement/concurrency
qualification remain. The full architecture goal, independent releases,
legacy retirement and C01-C14 remain open.


## Sources HTML/DOCX/PDF indexing and Chutni publication (2026-10-08)

Index capture/discovery/recovery/reindex paths now recognize HTML/HTM, DOCX and
PDF text alongside existing plain-text suffixes. They reuse Sources' existing
public registration, locator_read and reader APIs. HTML/DOCX length probes use
empty selected ranges; PDF page1 reports total pages before a bounded first4-page
read. Original reader envelopes and SHA-256 source identities are retained;
HTML/DOCX offsets remain extracted_utf8, PDF ranges remain absolute pages, and
partial static/OOXML/text-layer coverage is preserved. Page text is joined with
mechanical newlines only for index candidate matching/preview; original per-page
evidence and blank-layer statuses remain authoritative. Index candidates now
carry original reader provenance. Aggregate indexed text stays within64KiB.

Publication requires the corresponding owner/media claim and retained snapshot,
checks evidence digest and projection agreement, and reuses the same parsers in
the isolated worker to prove selected text. HTML/DOCX fingerprint and evidence
identities are checked. PDFium reproduces the exact selected-page evidence
including fingerprint, source/version, pages, coverage and provenance before
Chutni is opened for writing. The already isolated publication worker invokes
the existing PDFium reader directly within its own monitored process; it does
not spawn an unsupported nested PDF request through the Chutni helper.
SDK selectors preserve extracted coordinates or pages. The SDK remains the
artifact/catalog writer, with original Sources evidence in its parameters.

The actual fixture captures HTML with script suppression/entity decoding, a
native DOCX package and a100-page PDF (page2 blank), indexes first4 PDF pages,
searches qualified candidates, publishes three new reader artifacts while
reusing earlier plain-text artifacts, and reads complete artifact/provenance
through memory_read. Then actual HTML content, DOCX ZIP comment and PDF trailing
comment mutations change original versions; locator_reconcile and public
index_reindex_step capture three retained versions. Historical candidates remain
searchable and generation1 publication creates exactly three additional SDK
artifacts. No reader implementation was duplicated or vendor SDK changed for
this checkpoint. No private cross-component schema/header access was added.

Actual PDFium dependency SHA256:
macOS c2f68f87106da4f0cb6d6a90a7aad9644b3c1cb92a3463dbdc8f39449aec3869
/Users/deepanwadhwa/Documents/samosa-chat/dist/libpdfium.dylib
Linux38832b476d8a6d2f9862bc8b3ab5a7171cfb5c6d830ef5f73b2918ab96515382
/host-tmp/samosa-s14d-linux-qa/pdfium/lib/libpdfium.so
The Chutni SDK artifacts are the same as the preceding artifact-read checkpoint.

Qualification command:
make test-publication sanitize-publication test-index sanitize-index test-reindex
 test-index-freshness test-index-discovery test-chutni
 CHUTNI_LIBRARY=<external SDK> PDFIUM_LIBRARY=<actual platform PDFium>.
Mac command exited0: reader index/reindex/publication/read fixtures and previous
runtime/crash/content-refusal fixtures pass in normal and ASan/UBSan modes;
index712 checks pass in each mode; real reindex SIGKILL/recovery, freshness,
discovery1158 and Chutni parent115/actual SDK retrieval regressions pass.
Linux final combined command also exited0, with the same reader integration and
prior publication cases passing in normal and ASan/UBSan modes, index712 in both
modes and reindex/freshness/discovery1158/Chutni115 regressions passing. No build
or test handle remains active for this checkpoint. Logs:
/private/tmp/samosa-index-readers-qualified-mac.log
/private/tmp/samosa-index-readers-qualified-linux.log
With PDFIUM_LIBRARY omitted, publication targets run only prior text fixtures,
so such a pass is not evidence for this reader integration.

Remaining Sources scope includes OCR/media index outputs, neural enrichment,
semantic retrieval, broader capacities/provider/store policies, public Inference
media extraction, authenticated bounded service and independent release. Full
Sources and C01-C14 remain open. Legacy production routes remain active.

## Image OCR indexing and Chutni publication checkpoint

Image suffix discovery now reuses register_image/read_image, immutable snapshots
and the existing verified locator capture path. Initial indexing and retained
reindexing preserve the complete original image-ocr-v1 evidence. Publication
reuses the existing Tesseract/Leptonica adapter in the isolated SDK worker;
recognized text, extractor fingerprint, confidence and original raster dimensions
must agree before SDK writes. Selectors remain extracted_utf8; coverage remains
partial/ocr_recognition_uncertain. No OCR engine or catalog was reimplemented.

Recovery now compares complete saved Sources evidence structurally, including
recognition metadata, rather than accepting matching text/identity fields alone.
The actual SDK fixture changes only saved confidence, verifies
chutni_publication_conflict without another artifact, restores the parameters,
and proves successful recovery. This deliberately corrupts private fixture state
using the SDK's bundled SQLite symbols; production still accesses the catalog
solely through the public Chutni SDK. The earlier Python SQLite fixture read
failed with database-disk-image-malformed; using the catalog's own SQLite build
resolved the fixture failure. A strict GCC maybe-uninitialized warning in the
numeric evidence comparator was fixed by initializing both local values.

The shared English PPM fixture contains TOKEN 7319 in a680x150 raster and was
generated with the existing OCR fixture tool and macOS Arial font. Its SHA256 is
f826633e4cee4f5dc5a3d0ed94048d5f5a51a9b5d481bfed32f08076065be51b.
Both platforms use eng.traineddata SHA256
7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2.
Chutni and PDFium dependencies are unchanged from the preceding checkpoint.
The fixture proves actual indexing, candidate search, publication, artifact read,
offline replay and later changed encoded-image retained-version publication.
The changed PPM comment preserves pixels/text while changing the original SHA.
This does not establish OCR accuracy for every format, model or language.

Command: make test-publication sanitize-publication test-index test-reindex
test-chutni CHUTNI_LIBRARY=<SDK> PDFIUM_LIBRARY=<PDFium>
OCR_FIXTURE=<shared PPM> TESSERACT_LIBRARY=<platform Tesseract>
LEPTONICA_LIBRARY=<platform Leptonica> OCR_TRAINEDDATA=<English trained data>.
macOS exited0: both normal and ASan/UBSan publication integrations pass,
including previous reader/runtime/SIGKILL/content-conflict cases; index712,
reindex crash/history/capacity and Chutni parent115/actual retrieval checks pass.
Linux also exited0: the same normal/ASan/UBSan publication integrations, index712,
reindex crash/history/capacity and Chutni parent115/actual retrieval checks pass.
Both execution handles are terminal; no qualification process remains active.
Logs: /private/tmp/samosa-index-ocr-qualified-mac.log and
/private/tmp/samosa-index-ocr-qualified-linux.log.

PDF OCR fallback, audio/video indexing, neural enrichment, semantic retrieval,
authenticated bounded service and independent component release remain open.
Sources area4 and the complete C01-C14 architecture goal remain active.

## Existing Whisper runner through public Inference — preparatory Sources #5

Monolith reuse points: whisper_transcribe_path and voice_stt_fingerprint in
src/samosa_gateway.c around20619–20740, and the audio_srt_timestamp/absolute
normalization path around20935. The optional adapter keeps the existing
whisper.cpp CLI/model/audio/language/SRT behavior and replaces Gateway mutex,
home-path and PID bookkeeping with existing Inference FIFO admission, durable
operations, deadlines/cancellation and owned-engine guardian APIs. It does not
implement another speech engine. Inputs are public prepared audio bytes; no
Sources private file/schema/header is read. Source timeline mapping stays with
the future Sources client, not the engine.

Public transcribe accepts canonical PCM16/mono16kHz WAV base64 through30s/960044
bytes, within a separate2MiB request bound; existing text limits remain1MiB.
It returns derived transcript text/segments, exact input SHA256, explicit
speech_recognition_uncertain coverage and engine/model SHA provenance. Strict
SRT validation refuses malformed/empty/late output. The retained model descriptor
is verified before and after the engine. Exact completed retries replay offline.
The existing guardian now optionally owns a private workspace directory inode,
removing input.wav/result.srt after engine reaping, including active requester
SIGKILL; the parent retains its output descriptor to read bytes after unlink.
The normal/ASan native fixture verifies workspace cleanup and no surviving engine.

Evidence-driven boundary fixes: actual legacy -nt SRT returned a padded30s cue
for a2672ms input, so timestamped adapter calls omit -nt and retain actual timing.
An initial16KiB child file limit killed Metal initialization with SIGXFSZ;
the limit is64MiB per child-created file, with live/final SRT validation16KiB.
This preserves the installed GPU engine. Strict GCC indentation/format-truncation
diagnostics were fixed without weakening warnings. Initial Python fixtures were
removed and replaced with native C tools to obey the plan's implementation
constraint. Only final native fixture runs establish the checkpoint below.

Final command on macOS and native ARM64 Linux:
make test-whisper sanitize-whisper build/test-whisper-real test-engine-process
sanitize-engine-process test-operations test-admission build/test-inference
build/test-sessions.
Both commands exited0: public adapter fixture controls pass normal/ASan/UBSan,
including full30s admission, canonical base64/input bounds, offline retry,
owner isolation, model mutation, malformed/large SRT, exit/log failures,
deadline, cancellation and actual active-requester SIGKILL cleanup. Existing
guardian97 checks pass normal/sanitizer; operation832 and admission79 pass.
Separately executed native fake Inference98 and opaque session1106 checks pass
on both platforms. Helper IO assertions make adapter check counts vary by
platform/instrumentation; these are not independent test-case counts.
Final logs: /private/tmp/samosa-whisper-native-qualified-mac.log,
/private/tmp/samosa-whisper-native-qualified-linux.log and
/private/tmp/samosa-whisper-text-regressions-linux.log.

The actual-model native tool invocation is:
build/test-whisper-real <Inference executable> <Sources executable>
<installed Whisper CLI> <installed model> <English WAV fixture>.
Both normal and ASan/UBSan Inference executables passed on macOS;2416 assertions
include existing public Sources registration/preparation, exact canonical PCM
SHA256 agreement, derived uncertain timed text and offline operation replay
after original deletion. The synthetic macOS say fixture is converted with the
existing FFmpeg to16kHz mono PCM16; original WAV SHA256
927b006bef03f0c5b2430f5ddfa20629ea8fcabf1f0b422403f5143159e8eeb5.
Sources prepared input SHA256:
61224cc2db0e1b3b7f69344a2aa79b7a7b167fdab0b10bdc995e52c22f6abe88.
The input duration is2672ms; recognized text is
" The coriander sample contains 7319." with one0..2660ms segment.
Installed CLI SHA256:
b6aea409400da5f8c2d2dde7e4f31ff1f3ac98a65a4b8fe4ce0408bf3ae629b7.
Installed base.en GGML SHA256:
a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002.
Logs: /private/tmp/samosa-whisper-native-real-mac.log and
/private/tmp/samosa-whisper-native-real-sanitize-mac.log.
All listed execution handles are terminal. This is one English synthetic speech
fixture, not a general speech accuracy benchmark or Linux actual-model pass.

Sources transcript capture/cache, absolute source timeline mapping, Chutni
transcript publication, Linux real-model/broader audio qualification and vision
adapter extraction remain open. Full release also requires converting the three
existing Sources Python fixtures (publication, Chutni retrieval and search CLI)
to native tooling; prior runtime behavior evidence does not close that constraint.
No C01–C14 milestone or complete component release is closed by this checkpoint.

## Sources-owned transcript capture and timeline — Sources #5 partial

The existing Sources prepare_audio/cache/control paths now feed a trusted host
callback consuming public Inference JSON. No engine is duplicated in Sources.
The previously extracted Whisper adapter reuses the monolith's engine and timed
SRT workflow. Sources validates input and provider/model/engine identities,
recognition uncertainty and every cue, maps cues to the original timeline and
commits owner-scoped derived transcript captures. The callback port is implemented;
production transport/CLI wiring is still pending.

Native C commands `make test-transcript sanitize-transcript test-index` passed on
macOS and native ARM64 Linux. Transcript fixtures report326 checks in normal and
ASan/UBSan runs on both platforms; index regressions report712 checks. Coverage
includes128 cues, durable and bare offline reuse, owner isolation, model changes,
invalid provider results, busy/deadline/cancellation, cache quota, digest corruption
and altered model metadata with a recomputed valid response digest. These are
fixture-provider checks, not actual-model capture or a speech quality benchmark.
Existing audio normal/sanitizer, control135 and operation750 checks also passed
on both platforms during this change. Increasing the bounded JSON token capacity
to2048 preserves full cue envelopes while retaining the16KiB request byte limit.

Final logs: /private/tmp/samosa-transcript-final-mac.log and
/private/tmp/samosa-transcript-final-linux.log. Each combined command subsequently
ran the native Chutni parent validator successfully (115 checks), then exited2
because CHUTNI_LIBRARY was not supplied to the integration target. This is not a
new complete Chutni integration pass. The existing Python integration fixture was
not invoked; its native conversion remains required. All execution handles are
terminal. Earlier audio/control/operation results are recorded in
/private/tmp/samosa-transcript-qualified-linux.log and the execution record.

Remaining work includes bounded production Inference transport, full actual-engine
Sources capture/cache qualification, Linux real-model/broader media checks,
transcript indexing/publication, visual analysis and the other Sources release
areas. Neither Sources #5 nor any complete component/C01–C14 milestone is closed.

## Public Inference process transport and actual Sources capture — Sources #5 partial

Sources now provides a native public stdin/stdout Inference process adapter and
host-only CLI configuration. It reuses Sources' existing isolated worker protocol,
watchdog, cancellation/deadline checks and process-group cleanup. It does not
invoke a shell, load a model or copy Inference internals. Public response transport
is bounded262144 bytes; host arguments are bounded64 strings of4095 bytes each.
Public provider error envelopes survive transport for parent semantic validation.

Native process fixtures pass normal and ASan/UBSan checks on macOS and native
ARM64 Linux. They exercise a1280000-byte request, provider busy with exit1,
oversized/NUL output, abnormal exit, deadline, cancellation and requester SIGKILL
with provider termination. Fixture assertion counts vary with polling and platform:
macOS63 normal/76 sanitizer; Linux67 normal/64 sanitizer. The test uses a fixture
provider, not a model. Logs: /private/tmp/samosa-inference-process-death-mac.log
and /private/tmp/samosa-inference-process-death-linux.log. Both commands exited0.

An initial Linux sanitizer run failed because an instrumented fixture provider
inherited the normal worker's2GiB address-space limit, which prevents ASan shadow
allocation. The corrected sanitizer target instruments the Sources worker too,
using the existing sanitizer exemption for that limit. The normal limit remains.
Normal/sanitizer transcript326 checks, control135 and durable-operation750
regressions also pass on both platforms; logs:
/private/tmp/samosa-inference-process-final-mac.log and
/private/tmp/samosa-inference-process-final-linux.log.

The native test-whisper-real now exercises full public Sources transcribe_audio
through the process adapter, in addition to earlier preparation/Inference checks.
Actual installed macOS Whisper recognizes the existing2672ms synthetic English
fixture. Sources validates exact prepared input SHA256
61224cc2db0e1b3b7f69344a2aa79b7a7b167fdab0b10bdc995e52c22f6abe88,
derived uncertain text and absence of literal evidence_id. After removing the
original file/root, a new bare Sources request with provider/model identities but
no transport returns the byte-identical cached transcript. Both normal executables
and instrumented Sources/Inference executables pass2450 assertions. Engine/model
and fixture digests are unchanged from the preceding actual Whisper checkpoint.
Logs: /private/tmp/samosa-sources-transcript-real-mac.log and
/private/tmp/samosa-sources-transcript-real-sanitize-mac.log. Both commands exited0.
All handles listed in this checkpoint are terminal.

These checks establish one English WAV capture/cache pipeline, not broad speech
accuracy or complete audio/video qualification. Linux actual-model qualification,
compressed-media capture, transcript indexing/Chutni publication, vision extraction
and the other Sources release requirements remain open. No complete component
release or C01–C14 milestone is closed.

## Actual native ARM64 Linux transcription and compressed capture — Sources #5 partial

The monolith voice bootstrap's exact pinned Whisper source archive was reused,
not a replacement recognition engine: f049fff95a089aa9969deb009cdd4892b3e74916,
archive SHA256 d8cd961352377b1cc612224016a9ebdfe0ae508dc2b2f9ef514b341d672e3fdc.
Native ARM64 Debian GCC12/CMake3.25 built the unmodified source with static engine
libraries. The default native feature probe produced inconsistent FP16 flags and
failed compilation. Explicit GGML_NATIVE=OFF and
GGML_CPU_ARM_ARCH=armv8.2-a+fp16 resolved it; hardware must support those features.
This is a tested ARMv8.2 FP16 binary, not a universal Linux architecture artifact.
Linux engine SHA256:
3af4995636d99b42593efbbe5579bcf57dec6b9f30da4bdd98ef849db3f02b3f.
The existing base.en model is unchanged:
a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002.
Build logs: /private/tmp/samosa-whisper-pinned-build-linux.log (initial failure)
and /private/tmp/samosa-whisper-pinned-armv82-linux.log (exit0).

Actual Linux recognition exposed a timing defect in the strict adapter: Whisper
ended the2672ms clip at2680ms on its20ms token grid. Pinned source confirms
segment timestamps use20ms token steps. Inference now clips an overrun only when
it equals ceil(input_duration_ms/20)*20, with start still inside the input.
Off-grid and larger overruns refuse. Provenance explicitly records
whisper_20ms_end_clipped_to_input; Sources validates, preserves and revalidates
that optional policy on cache reuse. The previous unrestricted monolith clipping
behavior is not copied. Native fixtures accept a1020ms end for a1001ms input after clipping,
rejection of1019/1040 ends, and invalid policy refusal in Sources. The real test
also checks policy preservation in the Sources capture.

`make test-whisper sanitize-whisper` passed on both platforms (508/511 macOS and
514/526 Linux fixture assertions). `make test-transcript sanitize-transcript`
passed351 checks in both modes/platforms. Logs:
/private/tmp/samosa-whisper-grid-qualified-mac.log,
/private/tmp/samosa-whisper-grid-qualified-linux.log and
/private/tmp/samosa-whisper-grid-sources-mac.log. All commands exited0.

The native test-whisper-real tool accepts the previous five artifact arguments,
plus optional media-worker and encoded-audio arguments. It registers encoded
bytes through public Sources, prepares them through the existing decoder, and
compares the exact canonical PCM digest against an independently decoded WAV.
It then verifies actual public Inference and Sources transcript capture, uncertainty,
source provenance, timing policy and byte-identical bare cache reuse after deleting
the original/root and omitting Inference transport. For MP3, the media helper
remains configured; this does not prove cache reuse with every decoder absent.

Final actual WAV and MP3 runs passed normal and ASan/UBSan modes on macOS and
native ARM64 Linux. macOS reports2451 assertions per run; Linux WAV2440,
MP3 normal2438/sanitizer2440. Counts include helper I/O assertions and vary with
platform. Logs: /private/tmp/samosa-whisper-final-{wav,mp3}-{normal,sanitize}-{mac,linux}.log.
Both orchestration handles exited0; all build/test handles from this checkpoint
are terminal. This uses the existing synthetic2672ms English speech fixture,
not a multi-language or general speech accuracy benchmark. A FLAC trial refused
at current container admission; qualification subsequently used supported MP3.
No FLAC or ALAC support is claimed or added.

MP3 original SHA256:
5fc314dd1618600e664809d9ebaba27017cab96ec7dbd2391473c040d15305e0.
Mac reference WAV SHA256:
3384559b0fdd9a456426c81494ab518749bfb2437fc0c1115fa5f54ffbb91606.
Linux reference WAV SHA256:
0dee5e526b125fa117404b24829888bc05fa4bb63939e59cf7926ea0cb21e056.
Canonical prepared MP3 PCM WAV SHA256:
macOS c81f0a085f069fa09f7487456d615f1bbab93e4bb5ec2dc8f1417e23886f1913;
Linux 0f6122ff2cd7b2c75a852d40a77b97342c7cf7c5997ff4d081daa5cff3b23f97.
Different platform decoder outputs are each compared to their own independent
reference, not asserted bit-identical across platforms. WAV prepared digest remains
61224cc2db0e1b3b7f69344a2aa79b7a7b167fdab0b10bdc995e52c22f6abe88.
All runs recognize " The coriander sample contains 7319.". Mac segment0..2660ms;
Linux segment0..2672ms after bounded end clipping.

Transcript indexing/Chutni publication, broader speech/media quality, visual
Inference extraction and the other Sources release areas remain open. The three
older Python integration fixtures still require native conversion. No complete
component release or C01–C14 milestone is closed.

## Existing Chutni retrieval fixture migrated to native C — release tooling gate

The real-SDK acceptance scenarios from Sources' existing test_chutni.py were
preserved in tests/test_chutni_sdk.c. The driver still loads the independently
built Chutni public SDK and invokes actual Sources CLI/worker executables. No
retrieval engine, store format or production behavior was replaced. Makefile
normal/sanitizer Chutni targets now invoke this native driver; the Python driver
is removed. This closes one of three existing Python-fixture conversion tasks;
publication and search CLI fixtures remain open release gates.

Preserved scenarios: twelve human notes, eight bounded search results, current
freshness/BM25 metadata, no private store path leakage, real artifact reads and
producer provenance, semantic_validation=not_performed, offline exact replay,
owner-authorized retained bytes and foreign-owner refusal, changed store UUID and
missing artifact refusal,360KB UTF-8 later-range/tail reads, split-character and
excessive-range refusals, absent/missing SDK, rejected client-selected store,
empty search,513-byte query refusal, deadline and unchanged SDK store counts.
The SDK's existing split-start refusal is chutni_failed; the old test required
refusal without specifying its code. The native test preserves that measured
behavior, rather than changing production during tooling migration.

`make test-chutni sanitize-chutni CHUTNI_LIBRARY=<external pinned SDK>` passed on
macOS and native ARM64 Linux. Normal and ASan/UBSan parent validator runs report
115 checks. Native real-SDK driver reports98396/98906 assertions on macOS and
98408/102354 on Linux. Counts include individual UTF-8 byte comparisons and
polling assertions, not that many independent scenarios. All commands exited0;
handles are terminal. Logs: /private/tmp/samosa-chutni-native-qualified-mac.log
and /private/tmp/samosa-chutni-native-qualified-linux.log. SDK artifact hashes are
unchanged from the preceding Chutni qualification checkpoints.

The main plan now names concrete monolith reuse entry points for transcript
publication/enrichment, vision, acquisition and freshness. Next action: adapt
chutni_store_model_text/flush_file_outputs construction to publish validated
Sources transcript captures through the public SDK with model provenance and
source-time selectors, retaining the existing durable publication/recovery path.
Transcript publication itself is not completed by this fixture conversion.
No full component release or C01–C14 milestone is closed.

## Transcript model-output publication worker — Sources #4/#5 partial

Adapted the monolith chutni_store_model_text identity/parameters construction to
Sources' existing isolated Chutni publication worker and put_file_outputs SDK
transaction. The existing registration/refresh, original-content hash check,
context lookup, recovery, cancellation and parent validation paths are reused.
No new model or catalog implementation was added. A private captured-transcript
validator reuses the same deep validation as Sources' transcript cache.

The worker writes artifact_kind=transcript, artifact_origin=model_generated and
producer_kind=model; model_id is the configured provider identity and
model_revision is the recorded model SHA256. Complete derived Sources captures
retain uncertainty, cues, engine/model/prepared-input identities and timing policy
in SDK parameters. Selectors use time_range in source_time_ms. Recovery verifies
complete saved capture and model/selector identity. Receipts use artifact_id,
without adding a literal evidence_id to the transcript.

Native real-SDK fixtures pass92 checks on macOS/Linux in normal and ASan/UBSan
modes: publication/readback with model provenance and absolute1000..2000ms selector,
recovery after original removal without a duplicate, refusal of replacement text
under the same publication key, invalid cue refusal even with a recomputed capture
digest, and preserved literal-text publication/recovery. This uses explicitly
labeled fixture model output and synthetic prepared/input identifiers; it is not
actual-model capture-to-publication qualification. Actual public SDK/worker code
runs, not a simulated catalog. Mac initial offline-recovery fixture failed because
/tmp aliases /private/tmp; the corrected fixture uses the canonical source root,
as the existing production publication parent does. No production path change
was needed for that fixture issue.

Commands: make test-transcript-publication sanitize-transcript-publication
CHUTNI_LIBRARY=<the previously qualified platform SDK>. Logs:
/private/tmp/samosa-transcript-publication-final-mac.log and
/private/tmp/samosa-transcript-publication-final-linux.log.
The initial normal transcript regression also passed351 checks. Actual retrieval
and parent validation regressions run through the converted native Chutni driver;
the final execution record below records terminal results.

Next action: expose publication of owner-authorized cached transcript windows
through Sources' public interface with durable parent receipts, then connect audio
indexing/publication queues and qualify actual Whisper capture-to-publication.
Public writer/API and queue integration remain open. No complete component release
or C01–C14 milestone is closed by this worker checkpoint.

Final execution record: all publication, retrieval and capture runs reached
terminal exit0. The final publication fixture passes97 checks in both normal and
ASan/UBSan modes on macOS and Linux; the native transcript capture regression
passes351 checks on each platform. The actual Chutni retrieval regressions also
pass in normal and sanitizer modes on both platforms, including115 parent
validation checks. Logs for the last publication/capture runs are
/private/tmp/samosa-transcript-publication-boundary-mac.log and
/private/tmp/samosa-transcript-publication-boundary-linux.log; retrieval logs are
/private/tmp/samosa-transcript-publication-qualified-mac.log and
/private/tmp/samosa-transcript-publication-final-linux.log.

The final fixture adds the maximum valid128-byte provider identity. This exposed
a concrete bound defect in transcript artifact-ID construction: the512-byte
material buffer could truncate a valid identity while its snprintf return length
was passed to hashing. The buffer is now768 bytes, sufficient for the bounded
inputs, and the maximum-identity capture passes on both platforms with sanitizers.
This is a demonstrated boundary correction; the existing engine and SDK remain
the implementations used by the extracted components.

## Public cached-transcript publication — Sources #4/#5 partial

Sources exposes publish_transcript(source_id,version,start_ms,end_ms), using the
existing owner snapshot/prepared-audio/transcript cache validation and Chutni
publication worker. It selects a cached capture with the transport disabled;
missing capture returns inference_unavailable without a model call. Host write
permission, expected store UUID and a granted root/file locator are required.
The SDK operation key binds owner, root scope, store, source and complete capture.
The existing model output construction, SDK transaction, complete-proof recovery
and parent response validation remain the implementations used by this interface.

Local transcript_publications stores immutable digest-checked receipts in the
existing bound Sources database. Transcript and index receipts share4096 slots;
index reservation now counts both tables. Existing durable operation controls
supply caller IDs, parent correlation, cancellation, deadlines and exact saved
operation replay. A commit whose local receipt is absent recovers through the SDK
operation key. Bare replay validates the retained owner snapshot/prepared payload
and capture before consulting the receipt; a saved operation replays its original
result as existing operation history does. Receipts describe historical commits.
Automatic audio queues are not implemented by this operation.

The native actual-SDK fixture registers a real WAV, prepares its1000..2000ms window,
captures fixture Inference output with its true canonical prepared-input SHA256,
and publishes through the public operation. It checks refusal of an uncaptured
window with zero model calls, model provenance/timing, exact durable retry with
worker absent, bare receipt replay with original/worker absent, foreign owner and
disabled-write refusal, recovery with the local receipt deleted and original gone,
and corrupt local receipt refusal. The existing literal writer and private derived
conflict/cue tests continue to run. Final boundary checks additionally refuse a
receipt with embedded NUL and an otherwise matching digest. No actual Whisper
capture-to-publication claim is made: this fixture's model output is labeled.

Commands: make all test-transcript-publication sanitize-transcript-publication
CHUTNI_LIBRARY=<qualified platform SDK>; make test-transcript test-control
test-operations. The159-check public fixture passes normal and ASan/UBSan modes
on both platforms, and capture351/control135/operations750 regressions pass on
macOS and Linux, with all corresponding runs terminal exit0. The final173-check
boundary fixture is recorded below when both final runs finish.
Logs: /private/tmp/samosa-transcript-public-api-final-mac.log and
/private/tmp/samosa-transcript-public-api-final-linux.log; final boundary logs:
/private/tmp/samosa-transcript-public-api-boundary-mac.log and
/private/tmp/samosa-transcript-public-api-boundary-linux.log.

Next: qualify actual Whisper capture through this public publication operation,
then connect the monolith's existing transcript-window/batch workflow to Sources'
audio indexing/publication queues. Existing vision adapter extraction, enrichment,
semantic retrieval, service lifecycle and full release gates remain open. No whole
Sources release or C01–C14 milestone is closed by this checkpoint.

The173-check embedded-NUL boundary fixture also finished terminal exit0 in normal
and ASan/UBSan modes on both platforms. Inspection then confirmed that binary and
chunk uploads retain the reserved locator upload:, not a registered file path.
The public file-backed writer now explicitly refuses that locator (and acquisition
URLs) rather than interpreting it as a relative filename under the granted root.
The final fixture adds a separate owner with an uploaded WAV, successfully captures
its prepared window, and requires publication_locator_unavailable without another
model call. This preserves the file-backed SDK publication contract; publication
of upload/acquisition sources needs their actual catalog acquisition integration.
Final locator qualification logs:
/private/tmp/samosa-transcript-public-api-locator-mac.log and
/private/tmp/samosa-transcript-public-api-locator-linux.log.

Final public writer execution: the188-check locator fixture passes normal and
ASan/UBSan modes on macOS and native ARM64 Linux. Both final runs reached terminal
exit0. All build/test handles from this checkpoint are terminal. Actual model
publication and audio queues remain the exact next work; the goal remains active.

## Actual Whisper through public transcript publication/readback — Sources #4/#5

Extended the existing native Inference tests/test_whisper_real.c public-CLI
fixture; no production engine, decoder or SDK replacement. Optional host fixture
variables SAMOSA_TEST_CHUTNI_LIBRARY and SAMOSA_TEST_CHUTNI_WORKER select the
existing qualified SDK and Sources worker. The fixture creates an isolated store
through SDK create/call/free/close, then uses public Sources publish_transcript and
memory_read. Default actual-Whisper capture qualification remains available with
those fixture variables absent. Added a native ASan/UBSan driver build target;
Linux's optional fixture uses its platform dl library.

For WAV and MP3 the fixture preserves the previous independent canonical-WAV
input digest, actual installed Whisper result/timing/uncertainty, process transport,
Source-owned capture, provider-absent cache hit and exact Inference durable retry
checks. It additionally checks the public receipt's artifact_id against the capture,
exact text readback and recursively compares the complete SDK-retained Sources
capture with the original capture, ignoring only JSON object field order. Producer
kind/model origin, source_time_ms selector and semantic_validation=not_performed
remain visible. After removing the original file it replays the exact public receipt
with both Inference transport and Chutni worker absent, then reads the retained SDK
artifact with no original/provider and checks the complete capture again.

Eight actual engine/SDK executions pass: WAV/MP3 normal and sanitizer modes on
macOS and native ARM64 Linux. Native driver checks:2525 each macOS run,2514 each
Linux run. These counts include byte/transport assertions, not independent cases
or an accuracy metric. Platform batch handles56299 and15098 both reached terminal
exit0; driver and Sources sanitizer builds also reached terminal exit0. No handles
from this checkpoint remain live. Sanitizer runs instrument the driver, public
Sources parent/private native transport, public Inference and Chutni publication
worker. The existing external engine, model, SDK and media worker are the pinned
qualified artifacts, not freshly replaced engines or instrumented third-party SDKs.

Artifacts, engine/model identities and WAV/MP3 reference digests are the same as
previous actual Whisper qualification. CLI arguments are the existing public
fixture interface: Inference executable, Sources executable, installed engine,
model, reference WAV, optionally Sources media worker and encoded MP3. Host engine
and model paths are unchanged; Linux model is
/tmp/samosa-whisper-pinned/runtime/ggml-base.en.bin. The optional SDK variables
point to /private/tmp/samosa-chutni-artifact-qa/libchutni.dylib on macOS and
/tmp/samosa-chutni-sdk/build-artifact-qa/libchutni.so on Linux, with the matching
normal/sanitizer Sources Chutni worker. Build targets: build/test-whisper-real and
build/test-whisper-real-sanitize in Inference, build/samosa-sources-inference-sanitize
in Sources. Logs: /private/tmp/samosa-whisper-publication-{wav,mp3}-{normal,sanitize}-{mac,linux}.log.

Initial fixture readback tried top-level text; the public memory_read envelope
stores artifact content at memory.result. Correcting the fixture to that existing
public shape required no runtime change. The complete saved derived transcript
remains compared; the surrounding read observation is its own Sources evidence.

Scope: the previously qualified2672ms English WAV and MP3 speech fixtures, exact
prepared input, public process transport and complete artifact preservation/replay.
No full-duration/whole-library audio index, broad recognition benchmark, other
language, acquisition/upload publication or visual inference gate is claimed.
Automatic audio queues remain the next implementation; enrichment, semantic
retrieval, visual adapter extraction, service lifecycle and release gates remain
open. No complete Sources release or C01–C14 milestone closes here.

Monolith reuse inspection for that next implementation: audio_transcript_window_count
uses 1+ceil((total-window)/(window-overlap)) for multi-window audio; its loop advances
by end-overlap and stops immediately when end reaches total. The monolith overlap
is16000 frames (one second); its default600-second window cannot be copied into the
existing30-second public preparation/Inference bound. Reuse the same accounting
with that explicit boundary adaptation. chutni_flush_file_outputs already supplies
source-content identity and outputs to the existing transaction; reuse its workflow
rather than introducing a second catalog writer. Long batches must not retain all
prepared PCM in the persistent8MiB/no-eviction cache; bounded window working state
is necessary to connect the existing preparation, Inference and publication helpers
while preserving current public cache behavior.

## Audio queue window working state — Sources #4/#5 partial

Extracted private per-window functions from existing code, not new decoders,
transports or SDK implementations. audio.c media_prepare now takes a private
persistent flag: public sources_media_prepare remains persistent; the private
sources_media_prepare_transient reuses validated existing entries but adds none.
The same split in transcript.c leaves public sources_transcript_dispatch persistent
and makes sources_transcript_window consume temporary prepared PCM/projected text
without adding new prepared or transcript entries. All owner/snapshot, decoder
fingerprint, payload shape/input digest, control and model result validation is
shared. Temporary buffers remain owned/freed by the same implementation.

publication.c exposes its existing core as source_transcript_publish_capture for a
Sources-owned borrowed capture; public publish_transcript still selects its owner
cache with the transport absent and calls that same core. The core retains host
write/configuration checks, original owner/snapshot SHA, complete capture and
selected range checks, root scope, SDK model provenance/transaction/recovery,
parent receipt validation and durable local receipt recording. Public requests
still cannot supply capture/text/paths or grant writes. Initial extraction build
found a removed local FILE declaration in the publication core; adding that local
fixed strict compilation without changing publication behavior.

sources_audio_window_plan adapts audio_transcript_window_count's ceiling division,
window-overlap stride and final-window stopping to30000ms windows/1000ms overlap.
It admits durations through4h and checks scoped control. Native checks cover an
exact30000ms single window,30001ms overlapped tail, an exact60000ms multiple that
requires three windows, invalid ranges/ordinals and all497 windows of a4h plan,
with no gaps, exact1000ms overlap and the final end exactly at duration.

The existing native real-SDK publication fixture now additionally registers a
233000ms WAV and prepares/captures/publishes all eight planned windows through
these shared private paths. PCM payloads would total over10MiB of base64, exceeding
the persistent8MiB budget. With host cache_limit=1/cache_byte_limit=1024, all eight
still succeed without inserting or evicting any prepared/captured cache row.
The fixture provider derives its input digest/duration from actual prepared WAV
bytes; output remains explicitly labeled fixture model text. Each output is deeply
validated and published through the actual SDK writer. A subsequent public
persistent transcription correctly returns cache_limit without another model
call, preserving previous public admission behavior. It does not make a real
long-audio recognition/accuracy claim.

Commands: make test-transcript-publication test-transcript
sanitize-transcript-publication sanitize-transcript
CHUTNI_LIBRARY=<qualified platform SDK>. Logs:
/private/tmp/samosa-audio-window-working-mac.log and
/private/tmp/samosa-audio-window-working-linux.log. Final execution results follow
when the current Linux sanitizer batch reaches a terminal result.

Scope is the bounded window execution path and monolith accounting. No durable
audio job/controller, fresh discovery, checkpoint retry, automatic runtime queue
or complete audio indexing gate is claimed. Next: connect these functions to
owner/root/provider/decoder-bound durable audio job checkpoints, with separate
capture/publication phases so publication retry never reruns a committed model
capture, then integrate existing queue runtime admission/cancellation/scheduling.
The full Sources/Inference/Agent/App/C01–C14 goal remains active.

Final execution record: macOS12066 and Linux59963 both reached terminal exit0.
The expanded actual-SDK window/publication fixture passes1872 checks in normal and
ASan/UBSan modes on each platform; the existing public transcript capture fixture
passes351 in normal and ASan/UBSan on each platform. All handles from this checkpoint
are terminal. Most additional assertions exercise the497-window plan; counts are
acceptance assertions, not independent model cases. The eight actual SDK writes
use real WAV preparation and explicitly labeled fixture model output. Durable
audio controller/scheduler and compressed queue qualification remain incomplete.

## Durable selected-range audio queue controller — Sources #4/#5 partial

Added audio_index.c/h to the independently built Sources component. Public
operations audio_index_create/get/step/queue_step/read use its existing strict JSON
routing, owner admission, bound SQLite store and durable operation controls.
Jobs bind owner/root scope/source/range/provider-engine-model/decoder recipe and
optional publication UUID. Limits are8 retained jobs,497 windows per job and8MiB
capture bytes; shared existing publication receipts retain their4096 bound. Range
creation validates retained WAV frames; encoded availability is checked when each
window is prepared. This is explicit selected-range admission, not whole-file
probe/discovery qualification.

Capture commits the complete checked derived transcript/digest and cursor in one
transaction. Publication consumes that saved capture, uses the same SDK writer/
recovery, and commits its separately checked receipt/digest. Typed dependency
errors persist bounded backoff and last_error. Saved job identity, integer state,
contiguous windows, publication prefix, complete capture/range and receipt shape/
identity/digests are revalidated before use; corruption is not promoted to text.
Source recipe validation is shared with transcription. The existing index.lock
lease was extracted into source_index_lease and reused; no second scheduler.
Existing --index-run adds one audio phase per cycle when Inference is configured,
with an earlier-budget-preserving60s audio attempt and existing TERM/INT control,
runtime lease and pacing. Text attempts retain their previous budgets.

The actual-SDK native fixture adds repeated public creation without resetting,
eight captured/published phases across database reopen, model call count isolated
to capture, derived final-window readback, foreign-owner/model-change refusal,
worker-absent publication failure with persisted error/backoff, and retry with the
model transport absent. Recomputed-digest cue corruption fails even job status.
An actual existing runtime child completes another job and exits0 on TERM.
A fault-enabled parent is SIGKILLed at the existing post-SDK-commit marker before
local receipt/checkpoint acknowledgment; the original file is removed and retry
with model transport absent recovers the SDK artifact and commits recovered:true
without another model call. Fault code exists only in native test builds.

Final focused commands: make all test-transcript-publication
sanitize-transcript-publication CHUTNI_LIBRARY=<qualified SDK>. Normal/sanitizer
runs reach terminal exit0 on macOS and native ARM64 Linux. Final driver assertion
counts vary with busy polling: macOS2219 normal/2235 sanitizer, Linux2245 normal/
2463 sanitizer. These include planner/poll checks, not independent model cases.
All model output in these queue fixtures is explicitly labeled fixture output;
the SDK, preparation, persistence, public routing and runtime are actual code.
Logs: /private/tmp/samosa-audio-index-status-{mac,linux}.log; prior recovery runs:
/private/tmp/samosa-audio-index-recovery-{mac,linux}.log. Initial Linux strict build
caught misleading-indentation warnings; formatting was corrected without changing
behavior, then normal and sanitizer acceptance passed.

Relevant regressions also pass terminal exit0: public transcript351, native text
index712, durable operations750 and actual CLI queue runtime (duplicate admission,
TERM/INT, SIGKILL/restart, root/state/lease replacement and retained evidence) on
both platforms. Logs /private/tmp/samosa-audio-index-qualified-mac.log and
/private/tmp/samosa-audio-index-recovery-linux.log. Those regressions preceded the
final selected-job error/status/prefix validation addition; final focused normal/
san batches cover that addition and its persisted-error assertion. No current
build/test handle remains live.

Contract: components/samosa-sources/contracts/audio-index-v1.md. Public discovery
of audio jobs, complete duration probing, filesystem reindex/freshness, actual
Whisper/compressed queue qualification and job/history removal remain pending.
Actual-model publication qualification from the prior checkpoint is not used to
claim actual-model queue qualification. Service/authentication/admission lifecycle,
enrichment/semantic retrieval, visual Inference and complete release/C01–C14 gates
remain open. Full Sources/Inference/Agent/App goal stays active.

### Actual Whisper durable audio queue execution (2026-10-09)

Extended the existing native tests/test_whisper_real.c fixture, retaining the
monolith's installed pinned Whisper engine/model and the existing public Sources,
Inference and Chutni interfaces. There is no production engine or decoder change.
The optional actual-SDK path creates a second Sources state and registers the
original WAV/encoded media into it. Its empty prepared/transcript cache ensures
audio_index_step must invoke the real public Inference process for capture.
The fixture checks the exact independently prepared PCM input SHA256, recognized
fixture text and uncertain derived provenance. Separate publication/completion
calls omit model transport. After original removal, read/get calls omit model
transport and configure a missing SDK worker; saved capture bytes remain identical
and status remains completed. Existing public publication/readback checks still
compare the complete retained cached-window capture with SDK provenance.
The queue portion validates committed publication/status and local capture;
it does not independently enumerate/read back its particular SDK artifact.

Build commands: make -C components/samosa-inference build/test-whisper-real
build/test-whisper-real-sanitize; make -C components/samosa-sources
build/samosa-sources-inference-sanitize. The editable Linux candidates use the same
native targets. Existing normal/sanitizer Inference and Chutni worker artifacts,
normal media worker, SDK and engine/model pins match the preceding checkpoints.
Scripts /private/tmp/samosa-whisper-audio-queue-{mac,linux}.sh record arguments;
the first macOS normal WAV run was executed separately with the same artifacts.

All eight runs passed: WAV/MP3 normal/ASan+UBSan on macOS and native ARM64 Linux.
Counts are2637 per macOS run and2626 per Linux run, including file hashing and
transport assertions rather than independent recognition cases. Logs:
/private/tmp/samosa-whisper-audio-queue-{wav,mp3}-{normal,sanitize}-{mac,linux}.log.
Build handles53098/98749, first macOS run93950 and matrix handles61561/95051 all
reached terminal exit0. Sanitizer runs instrument the native driver, Sources parent,
public Inference adapter and SDK worker; external Whisper/model/SDK and the already
qualified normal media worker are configured dependencies, not newly instrumented.

Scope is the2672ms English speech fixture and its MP3 encoding. It does not qualify
actual-model long-window batches, broad recognition quality, other languages,
automatic filesystem discovery/freshness, duration probing or job/history removal.
Existing longer-window/kill/retry tests retain their labeled fixture model output.
The main plan explicitly identifies the old native probe and jobs_watch_delta as
reuse-first starting points for the next integration. No replacement scheduler,
decoder, model engine or catalog was added. Full release and C01–C14 remain open.

### Owned audio timeline inspection execution (2026-10-09)

Reuse-first review inspected attachment_audio_decode_probe_path in the monolith
and the native src/samosa_audio_decode.mm --probe implementation, plus the existing
Sources decoder/resampler and jobs_watch_delta. The old native probe is
macOS/file-path-specific. Sources already has an isolated platform media decoder
over immutable bytes, so it now reuses that same decode/resample/timestamp loop
with a no-PCM-retention scan mode instead of adding a replacement engine. A usable
integer-millisecond endpoint is measured from decoded samples rather than assumed
from container duration. The plain WAV path reuses sources_wave_inspect.

Added public inspect_audio (source_id/version plus existing optional operation
controls), its owned snapshot/byte-digest checks, describe entry and contract.
Private worker request kind13 returns nine fields; the parent validates shape,
duration==floor(frames/16), limits, signed origin, policy and configured decoder
fingerprint before public projection. The scan preserves existing64MiB input,
50000-packet, supported-format/timeline and30s worker limits. New inspection stores
no waveform/prepared capture and has no bare-result cache. Existing persistent
prepared/transcript retention is unchanged; durable operation replay is historical.

Native fixtures now cover full probe, final valid1ms and immediately uncovered
next1ms, cancellation/deadline, owner refusal, worker DTO mutations/fingerprint
binding, cache preservation and actual native process faults. Both audio decode
and probe requests exercise active cancellation/deadline, RSS limits, healthy peer
execution, exact reap and requester-SIGKILL worker/descendant cleanup. Public CLI
tests/audio_inspect_cli.sh checks the independent2672ms WAV/MP3 endpoint,
immutable reread after original removal, model-free/worker-absent durable replay,
owner refusal and extra-field refusal. make test-audio-inspect exposes this with
explicit MEDIA_FIXTURE, MEDIA_FORMAT (audio/media) and MEDIA_EXPECTED_MS.

Final normal/ASan+UBSan four-format corpus passes on macOS and native ARM64 Linux:
samosa-s18b-wave.wav, sentinel.mp3, sentinel.m4a and stereo48k.wav. Logs
/private/tmp/samosa-audio-inspect-corpus-final-{mac,linux}.log. Assertions vary with
configured library sizes/hashing: macOS84/85 normal,162/163 sanitizer;
Linux252/253 normal,502/503 sanitizer. These are acceptance assertions, not
independent speech cases. Existing WAV fixture passes1301740 checks (primarily
byte comparisons), and compressed cache/inspection tests pass on both platforms
normal/sanitizer. Logs /private/tmp/samosa-audio-inspect-normal-{mac,linux}.log,
/private/tmp/samosa-audio-inspect-sanitize-mac.log and
/private/tmp/samosa-audio-inspect-sanitize-final-linux.log. Final worker-control
logs: /private/tmp/samosa-audio-inspect-worker-final-mac.log and the final Linux
sanitizer/CLI logs. Normal transcript351/index712/operations750 regressions pass
on both platforms; macOS log samosa-audio-inspect-regressions-mac.log and Linux
log samosa-audio-inspect-cli-final-linux.log.

All eight public CLI combinations (WAV/MP3, normal/sanitizer, both platforms) pass.
Logs /private/tmp/samosa-audio-inspect-cli{,-wav,-sanitize,-wav-sanitize}-mac.log,
/private/tmp/samosa-audio-inspect-cli-linux.log and
/private/tmp/samosa-audio-inspect-cli-final-linux.log. Sanitizers instrument native
parent/worker/test code, with external codec libraries as configured dependencies.
Final handles69199/32187/4989/59692/85522/14493 reached terminal exit0. Earlier
inspection builds/tests are terminal too; no current handle remains live.

Three fixture assumptions were corrected before final qualification: the CLI
extra-field assertion expected invalid_request although the established core
returns unknown_field; the worker cancellation timer included a synchronous
healthy-peer scan before cancellation could be observed, so it now measures
teardown after that call while retaining the2s limit; and the Linux AAC decoder
returns496640 samples/31040ms from the31s-origin encoded fixture. Its final padding
exceeded the fixture's assumed31s ceiling, so that fixture ceiling became32s;
production bounds were unchanged. Final boundary tests confirm the reported
decoded endpoint is preparable and the next millisecond refuses. The policy
does not promise identical sample counts across decoder versions or an unpadded
recording duration. Initial failed logs remain historical, not final evidence.

Automatic audio discovery/reindex/freshness and durable duration reuse are next,
through the existing index runtime and monolith delta behavior. Long-media/codec
qualification, seek performance, job/history removal, enrichment/semantic
retrieval, visual Inference, complete authenticated service and final release
remain open. No commit, production switch or complete Sources release is claimed.

## Sources area1 — HDR qualification and late-video seeking,2026-10-09

Area1 remains active. No other numbered area was started during this work.
The reuse audit confirms the monolith's attachment admission covers WAV/MP3/
AAC-M4A and MP4/QuickTime (`attachment_blob_extension`,
`attachment_audio_decode_probe_path` in src/samosa_gateway.c). Its video path
uses AVAssetImageGenerator selected times and CoreGraphics DeviceRGB in
src/molmo2/molmo2_media.mm. Those macOS APIs cannot provide the required Linux
worker or immutable memory IO. The extracted worker therefore keeps its existing
portable FFmpeg decoder, uses native libavfilter transfer/tone-map filters and
native backward keyframe seeking, rather than inventing color/seek algorithms.

### Requirement/evidence/gap checklist

| Area1 requirement | Current evidence | Remaining |
| --- | --- | --- |
| Immutable public media preparation, owner identity, provenance and caches | Existing qualified audio/visual controls and cache-integrity corpus; new exact public color metadata and cache reuse | Final refresh after remaining changes |
| Required container/codec/sample-format coverage | WAV/MP3/AAC/MP4/MOV, MPEG4/H264/HEVC/MJPEG/VP9 normal/sanitizer references on both platforms | Audit/qualify admitted VP8 and remaining public-reachable PCM variants; do not claim untested IDs qualified |
| Color and display presentation | Eight orientations/refusal;13-case codec/timeline corpus with PQ/HLG and wrong-HDR-primaries refusal | General photographic gamut rendering is outside preparation's declared policy |
| Supported source time selections | Origin offsets, uncovered intervals and variable-rate cases;90s source at59s and four-hour source's last second | Late encoded-audio preparation and full-scan inspection work-bound audit |
| Worker isolation and parent validation | Both-platform normal/sanitizer DTO fields, crash/protocol/output, active cancellation/deadline, RSS, reaping, peer/caller death | Recheck changes that affect controls |
| Independent optional packaging | Fresh HDR-era archive rebuilds minimal/optional binaries and passes actual public HDR/MP3 on both platforms | Refresh final media archive after seeking/audio/format completion |

### Completed HDR implementation and qualification

PQ16/HLG18 with BT2020 primaries9 and BT2020NCL matrix9 convert to linear BT709,
native Hable tone mapping with fixed peak10/desat0, then sRGB RGBA8. Linux uses
native zscale(npl100); macOS FFmpeg9 uses native scale transfer/primaries support.
Other HDR primaries/matrices refuse without artifacts. These are declared model
preparation policies, not calibrated photometric accuracy or cross-SDK identical
pixels. Parent and cache validate five new required color fields and consistency.
Optional builds add libavfilter; mapped libavfilter and optional libzimg join the
artifact fingerprint. Minimal Sources still has no FFmpeg link/startup dependency.

Final HDR13-case normal and ASan/UBSan corpus passes macOS/native ARM64 Linux,
including exact per-platform reference RGBA, source timestamps, worker/public API,
cache and durable no-artifact refusal/replay. Native eight-orientation/shear
refusal corpus also passes both modes/platforms. Corresponding normal/sanitizer
logs are /private/tmp/samosa-area1-hdr-{normal,san}-{mac,linux}.log and
/private/tmp/samosa-area1-orientation-{normal,san}-{mac,linux}.log.
Actual HDR parent validation and native fault/control sanitizer drivers pass on
both platforms; audio decode/probe/control regressions also pass, recorded in
/private/tmp/samosa-area1-worker-hdr-san-{mac,linux}.log and
/private/tmp/samosa-area1-audio-worker-san-{mac,linux}.log.

Fixture corrections: FFmpeg9 generic output color flags did not preserve the
encoded HEVC HDR tags. Explicit x265 VUI transfer/primaries/matrix parameters
fixed this; tests now assert the expected HDR input transfer so an accidentally
SDR fixture cannot pass. The late-window test's old numeric helper bounded
microsecond timestamps as milliseconds; its limit now covers the existing4h
microsecond domain. No production media bounds were increased.

Independent HDR-era source candidate:
/private/tmp/samosa-area1-source-candidate.tar.gz, SHA256
4e2819220d8fc3f638c3e1b66b4903fea29cbfd220592af21d0fca4840f27511.
Extracted roots /private/tmp/samosa-area1-package-mac and Linux
/tmp/samosa-area1-package-linux rebuild `all media-worker build/test-media-video-corpus`
with strict warnings. Actual PQ public/reference/cache and MP3 CLI owner/cache/
offline durable retry/malformed-survivor checks pass independently; logs
/private/tmp/samosa-area1-package-{build,api}-{mac,linux}.log. This archive precedes
the late-video seek change; it is not the final media archive or a full Sources
release. The final archive must be refreshed after the remaining area1 changes.

### Concrete long-video failure and fix

A native generated40-minute16x16 MPEG4 MP4,800891 bytes, SHA256
44a59fe68b07699408fde0348207b47c47fad5c4765e780791774a47cafb1070,
reproduced public `media_packet_limit` at start_ms2100000/end_ms2100500.
The valid selection exhausted50000 packets by decoding unrelated earlier media.
Artifact: /private/tmp/samosa-area1-long-gap/long.mp4. This confirms a real
integration gap; nominal4h range admission alone did not establish late coverage.

Late visual selections now use existing native backward keyframe seeking at
start_ms>=30000, followed by the same bounded forward decoding/first-in-interval
selection. Original timeline, exact RGBA, orientation and work/resource limits
remain validated. Failed seeking is typed media_timeline_unsupported. Worker-v5
identity binds this method. No audio decoder/inspection behavior changed here.
`test-media-late-window`/`sanitize-media-late-window` generate90s moving video and
a4h compact25fps source (360000 frames), then check the59s selection and the final
allowed second through primitive, worker, public preparation and cache against
native reference pixels and independently expected timestamps. Normal and
ASan/UBSan runs pass both platforms. Final seek build/control handles5988/14157,
normal windows60443/74386 and sanitizer windows7895/79299 reached terminal exit0.
All earlier HDR, orientation, package and regression handles are also terminal;
none remain live.
Logs /private/tmp/samosa-area1-seek-{normal,san}-{mac,linux}.log; strict builds and
refreshed actual-HDR sanitizer worker/control checks are in
/private/tmp/samosa-area1-seek-build-{mac,linux}.log.

Next: address late encoded-audio
selection/inspection using the existing decoder and monolith behavior. Complete
the finite format rows and media archive refresh before area1 closure. Areas2–7,
Sources complete release and C01–C14/full rebuild remain open. No commit, push,
production switch or legacy retirement occurred.

## Sources area1 — final audio, format and control qualification,2026-10-09

Late encoded-audio preparation now reuses native backward seeking with two seconds
of preroll from30s onward. Native resampler padding aligns the source-wide sample
grid; uncovered starts refuse rather than manufacturing samples. Preparation keeps
its50000-packet bound. Full timeline inspection retains no PCM, has a separate
1000000-packet bound and keeps its30s worker deadline. Worker-v6 identifies these
policies. This preserves selected-time monolith behavior using the extracted native
decoder rather than a replacement engine.

The final14-case visual matrix and nine-case late-audio matrix pass normal and
ASan/UBSan checks on macOS/native ARM64 Linux: exact reference pixels/PCM, source
timestamps, worker DTOs, public preparation, cache and offline durable replay.
Additional PCM variants include little/big-endian16-bit,24-bit,32-bit and float
MOV audio. VP8 fixture qualification verifies every encoded packet against its
original WebM source before comparing original-source reference pixels.

Four-hour audio checks measure exactly230385664 native reference frames, prepare
14398000..14398500ms and exercise public inspection plus offline durable retry.
An isolated Linux sanitizer run initially exceeded the existing deadline because
every packet made several synchronous worker/parent/SQLite control round trips.
Only full inspection now refreshes parent cancellation every25ms; local deadline
checks still run at every interruption point. The unchanged30s inspection limit
passes on both platforms. Linux's combined primitive/public/window sanitizer test
takes36s overall; that is several operations, not one worker's deadline.
Active cancellation/deadline, RSS, peer isolation, exact reaping and caller death
checks pass after the change; control failure remains fail-closed.

Final correction records: AAC PNS changes native decoder noise state after seeking,
so its exact reference uses the same native seek/source-grid policy. Other audio
cases retain sequential references. VP8 remux fixture disables a VP9 automatic
bitstream filter and verifies packet hashes. Older Linux FFmpeg stream_loop created
discontinuous repeated AAC timestamps; explicit native concat periods fix the
four-hour fixture. None of these corrections relaxes byte/pixel comparisons,
production deadlines or error expectations. A late audio gap test confirms no
manufactured samples/artifact and exact failed retry without source/helper.

Evidence: /private/tmp/samosa-area1-format-final-mac.log,
/private/tmp/samosa-area1-audio-final-mac.log and
/private/tmp/samosa-area1-final-complete-linux.log contain the completed matrix
gates. The latter ends at the pre-fix inspection failure; final replacement gates
are /private/tmp/samosa-area1-probe-control-full-san-{mac,linux}.log,
/private/tmp/samosa-area1-probe-control-build-{mac,linux}.log and
/private/tmp/samosa-area1-last-linux.log. All replacement handles4420/18990/19829
and macOS12961/9520 reached terminal exit0. Shared cache/control logs are
/private/tmp/samosa-area1-cache-control-{normal,san}-mac.log and
/private/tmp/samosa-area1-cache-control-final-linux.log, also terminal exit0.

Final media source archive: /private/tmp/samosa-area1-media-final-source.tar.gz,
SHA2564595f36a7f05ba42cc6fab164a641b8cab0edab822f6358fa9e1ac1f40831aa4.
Extracted files match the qualified workspace on both platforms. macOS's fresh
minimal/optional build and public four-hour audio/visual cache/owner/malformed/
offline-replay checks pass. Linux independent rebuild/API acceptance also passes,
terminal handles33135/86442 exit0; macOS55145/44013 exit0. Logs are
/private/tmp/samosa-area1-media-final-package-{build,test}-{mac,linux}.log.
Both extracted archives match every packaged workspace file. Area1 is CLOSED
against its finite checklist. This archive is media qualification, not a complete
Sources release. Areas2–7 and the full rebuild remain open.

Area2 audit: all eight reader/preparation operations already route through
locator_read's before/after identity checks, but focused live-read tests exercise
text only. Upload tests already verify root-independent text/HTML/DOCX/WAV and
64MiB lifecycle; native watch tests cover concurrent leases, actual changes,
SIGKILL recovery and root/state refusal. Next is missing modality/race
qualification, adapting existing tests and engines rather than rebuilding them.

## Sources area2 — upload/freshness/watch lifecycle qualification,2026-10-09

Previous goal turn made progress: area1's final independent archive gates passed
and its checkpoint closed. This turn remains within area2. The plan now enumerates
its finite lifecycle gates; authenticated transport/global service admission,
automatic modality reindex execution and final Sources release retain areas6/4/7.
The existing upload, reader, locator and watch implementations are reused.
Monolith capture and jobs_watch_delta provide the behavioral reference; no new
upload engine, decoder, watcher or scheduler was introduced.

New native test_live_modalities plus live_modalities.sh exercise every reader:
text, HTML, DOCX, PDF text, PDF OCR, image OCR, plain WAV, encoded audio and visual
frames. Native reads mutate the actual live file after the real worker starts,
with a cold cache. Every case also mutates during a warmed-cache read, rejects
changed/missing/foreign-owner sources, restores live availability and returns the
exact retained immutable result without a root. Direct, host-streaming and durable
session publication each use fresh stores for every admitted upload format. The
host reader returns short4096-byte chunks; durable append uses canonical8192-byte
chunks, checks every exact append retry and exact commit retry, compares real
selected text/pages/PCM/RGBA and checks cache equality plus durable replay with
helpers absent. Bare compressed cache access still requires extractor identity;
the helper-absent test correctly uses the existing saved operation receipt.

The normal/sanitizer targets explicitly use real PDFium, Tesseract/Leptonica,
English traineddata and native FFmpeg. Sanitizer targets instrument the Sources
parent and actual helper binaries; external decoder/OCR/PDFium and bundled SQLite
libraries retain their existing instrumentation limits. Library symlinks are
resolved before artifact binding. Initial test corrections were canonical artifact
paths, Linux misleading-indentation/unused-function strict warnings and the bare
cache versus durable-replay distinction; none changes production acceptance.

Existing native upload/session and watch suites recheck quota/owner/corruption,
format rollback, fresh-process resume, concurrent identical append/commit,
precommit public cancellation/deadline/SIGKILL rollback, stop/remove/replacement,
interrupted cleanup and stale-admission races, root/state replacement refusal and
SIGKILL restart. The eight-peer watch test now requires16 successful cycles per
peer and verifies exactly eight checkpoint/health/registration rows, one shared
locator row and zero per-cycle locator-operation histories after shutdown.
macOS normal and sanitizer passes are terminal20691 exit0.

A test-only postcommit observation in uploads.c kills a publisher after SQLite
COMMIT but before its response. A fresh process verifies exactly one snapshot,
claim and upload receipt, zero staging, then obtains identical commit retries and
literal evidence. macOS92143 and independently extracted Linux53043 both exited0,
normal and ASan/UBSan. Logs /private/tmp/samosa-area2-postcommit-{mac,linux}.log.
The callback is excluded from production builds. Generic operation receipts remain
separate: death before saving a terminal operation receipt is honestly interrupted;
the accepted upload remains recoverable using its upload receipt.

Existing actual64MiB normal/sanitizer evidence is revalidated in
/private/tmp/samosa-upload-large-index-mac.log,
/private/tmp/samosa-upload-large-index-sanitize-mac.log and
/private/tmp/samosa-upload-large-index-linux.log. The upload store/limits and
production transaction logic have not changed. macOS lifecycle regression log
/private/tmp/samosa-area2-lifecycle-mac.log is terminal64587 exit0. Expanded
watch bounds are in /private/tmp/samosa-area2-watch-bounds-mac.log, terminal20691.

Final all-upload-mode matrices16871(macOS) and78759(Linux) are terminal exit0,
with36 PASS records each: nine admission/reader cases in normal and sanitizer
modes, each proving upload and freshness behavior. Logs
/private/tmp/samosa-area2-all-upload-modes-{mac,linux}.log. Linux lifecycle
pipeline35944 also exited0, log /private/tmp/samosa-area2-final-linux.log,
including strengthened eight-peer bounds and existing crash/control/cleanup gates.
No area2 qualification handles remain live. Area2 is CLOSED against its finite
checklist. Areas3–7, complete Sources release and C01–C14/full rebuild remain open.
Next: audit/finish area3 acquisition/fetch/search adapters using existing code and
working monolith implementations. No commit, push or legacy retirement occurred.

Area3 initial audit: native test_fetch.c/test_fetch_tls.c already cover real HTTP
and TLS; test_search.c covers real worker HTTP, normalization and durable control.
Search CLI qualification still invokes tests/test_search_cli.py from both targets,
contrary to the native-only final architecture. Exact next implementation action:
convert that existing CLI fixture to native C, preserving its provider/escaping/
configuration/ownership/replay scenarios and reusing the existing native loopback
server/process helpers. Audit remaining acquisition/provider gates against
fetch-v1/search-v1 and monolith curl_fetch/fetch_public/web_search_* before closure.
The separate publication Python fixture belongs to area4/7 and stays deferred.

## Sources area3 — native CLI fixture and monolith fetch capacity,2026-10-09

Previous turn made progress and closed area2 against its lifecycle checklist.
Area3 remains active. Native test_search_cli.c replaces the Python CLI fixture,
preserving real POST/query JSON, Brave GET/query percent encoding, exact private
headers, credential redaction, offline operation replay/read and private-file
permission/symlink/oversize/NUL/schema refusal. A native forked loopback server
accepts exactly the expected two requests; extra requests or changed requests fail.
CLI stdout/stderr are drained together with poll and bounded buffers; alarms bound
fixture processes. Normal and ASan/UBSan runs pass macOS/native ARM64 Linux;
initial native conversion handles16646/51421 are terminal exit0. The old Python
fixture is removed after both-platform proof; publication is the only remaining
Python fixture and stays in its later area.

Monolith audit found MAX_PUBLIC_FETCH_BYTES5MiB, PUBLIC_FETCH_MAX_HOPS6,
resolve_public_host/ip_blocked with native DNS address validation and curl pinning,
robots_path_allowed, per-host politeness and an offline choke point. Those useful
public-web behaviors are required extraction work rather than optional improvements
to the narrow initial exact-origin adapter. The current plan explicitly retains
them; no area3 closure is claimed from transport fixtures alone.

Acquisition now preserves the5MiB body limit. Only kind8 HTTP worker envelopes get
the correspondingly bounded base64 capacity; all other native DTO bounds remain
2MiB. The private request carries a parent-derived max_bytes, validated against
5MiB and enforced before body copies/allocation. Search explicitly requests its
unchanged1MiB cap and the parent independently rejects excess output. Worker
oversize fault messages use the limit for their actual request kind. Describe and
fetch-v1 match5MiB acquisition; reader snapshot/cache/work deadlines are unchanged.

Native fetch fixtures now prove exact5MiB acceptance with independent incremental
SHA256/source version and last-five-byte evidence, plus5MiB+1 refusal. Native
search adds actual1MiB+ response refusal to prove its cap stays intact. Existing
receipt pre/postcommit death, cancellation/deadline, ownership, chunked/truncated/
header refusals, actual TLS trust/issuer/hostname/expiry and CLI acceptance remain.
macOS final pipeline67935 exited0; log
/private/tmp/samosa-area3-fetch-body-final-mac.log: normal/sanitizer fetch838/857,
TLS320/320, search228/228 and native CLI passes. An earlier Mac pipeline18665
stopped on a transient missing include while the new search-bound fixture was being
edited; the corrected final gate passes without relaxing checks.

Linux final pipeline76131 exited0, log
/private/tmp/samosa-area3-fetch-body-linux.log. Normal fetch835/TLS320/search228/CLI
and sanitizer fetch1133/TLS320/search228/CLI all pass. No qualification handles
remain live. Continue public-address/DNS policy extraction in area3; do not move
to area4. Full Sources release and C01–C14/full rebuild remain open. No commit,
push, production switch or production legacy retirement.

## Sources area3 — extracted public-address policy, offline replay and IPv6,2026-10-09

The acquisition worker now reuses the monolith's IPv4/IPv6 blocked-address
classification and native DNS resolution in fetch_public.c. Every DNS answer
must pass; malformed or mixed public/private answers refuse the whole result.
The selected address is pinned in libcurl, its address family is fixed, and the
connected peer is compared in binary form. Host-selected public-only policy
requires scheme-standard ports; public request fields cannot override it.
Offline policy rejects new network work before worker admission but preserves
accepted acquisition receipt replay, including replay under a fresh operation ID.

Bracketed IPv6 origins are supported. Zone identifiers, malformed brackets,
invalid ports and origin-prefix escapes refuse. A native IPv6 loopback fixture
proves an explicitly granted HTTP [::1] request through the actual worker and
independent parent validation. Darwin's inet_pton acceptance of zone suffixes is
handled by validating the literal grammar before calling the SDK.

Latest macOS pipeline65336 is terminal exit0; log
/private/tmp/samosa-area3-public-ipv6-final-mac.log. Normal and ASan/UBSan public
policy/actual IPv6 fixtures pass, along with fetch843/869 checks, search228/228
and native search CLI. Latest actual public HTTPS also passes normally and under
ASan/UBSan; logs /private/tmp/samosa-area3-public-ipv6-live-mac.log and
/private/tmp/samosa-area3-public-ipv6-live-san-mac.log. Earlier Linux pipeline80652
is terminal exit0; its code predates the positive IPv6 fixture and final grammar
check, so it is not final qualification for those changes. Latest Linux
pipeline60357 is terminal exit0; log
/private/tmp/samosa-area3-public-ipv6-latest-linux.log. Normal and ASan/UBSan
public-policy/actual IPv6 fixtures pass, along with fetch892/1246 checks,
search228/228 and native search CLI. No IPv6 qualification handles remain live.

The monolith url_join and robots_path_allowed are now extracted as bounded
component-owned helpers. URL joins preserve bracketed IPv6 and explicit standard
ports and cover absolute, root-relative, directory-relative, scheme-relative
and query-only destinations. Destination grammar is validated before returning;
userinfo, malformed escapes, fragments, zone identifiers, nonstandard ports,
controls and oversize joins refuse. DNS validation remains a separate per-hop
requirement. Robots evaluation preserves the monolith's declared subset: specific
agent groups precede wildcard groups, grouped agents are retained, longest
matching prefix wins and Allow wins ties. This is rule extraction, not completed
network integration.

macOS rule pipeline31744 is terminal exit0; log
/private/tmp/samosa-area3-public-web-rules-mac.log. Normal and ASan/UBSan rule,
actual IPv6 and public-policy checks pass. Linux rule pipeline16605 is terminal
exit0; log /private/tmp/samosa-area3-public-web-rules-linux.log. Actual public
HTTPS also passes normally and under ASan/UBSan after rule qualification, with logs
/private/tmp/samosa-area3-public-web-rules-live-linux.log and
/private/tmp/samosa-area3-public-web-rules-live-san-linux.log. No rule qualification
handles remain live.

The next implementation adds a bounded SourcesFetchRoute shared by worker
execution and provenance handling: exactly six permitted request destinations,
immutable original URL, append-only accepted destinations, exact URL loop
refusal, and unchanged route after invalid/overflow attempts. Route initialization
requires an exact initial grant, host public-only policy, standard ports and
online permission. Each later destination still requires DNS/pinning before
network use; route syntax alone does not authorize a connection. Tests cover
cross-origin joins, retained history, loops, malformed destinations, sixth-hop
admission/seventh-hop refusal, offline/private-profile/ungranted-origin refusal.
Mac route pipeline56868 is terminal exit0, log /private/tmp/samosa-area3-public-route-mac.log;
normal and ASan/UBSan route/rule, actual IPv6 and public-policy checks pass.
Linux route pipeline15939 is terminal exit0 from the new archive, log
/private/tmp/samosa-area3-public-route-linux.log. Worker execution and persisted
final-URL provenance were the next integration step; area3 is not closed.

## Sources area3 — bounded redirect worker and retained provenance,2026-10-09

Previous goal turn made progress through extracted route code and qualification.
The public-web redirect workflow is now integrated: host fetch_public_web or CLI
--fetch-public-web selects the public-only acquisition profile. Search never
carries the private public_web grant and continues to refuse redirects. The
worker manually follows only the declared redirect statuses, resolves/validates
all DNS answers and pins/checks the peer at every hop, retaining at most six
destinations. Malformed/duplicate Location headers, loops and excess hops refuse.
No caller/provider credential headers are copied between destinations.

The private response carries URL history. The parent validates its initial grant,
size, destination grammar and uniqueness plus existing exact URL/digest/base64/
length checks. Acquisition stores the original identity and final_url/urls;
immutable admission uses the final URL as captured origin. Receipt validation
accepts historical three-field metadata or validates the new five-field history,
with bounded32768-byte receipt/envelope metadata capacity. Snapshot/body limits
remain unchanged. Early admission failures free capture/provenance allocations.

Native parent tests cover valid cross-origin history and malformed, cyclic,
ungranted and seven-destination histories. macOS integrated regression85619 is
terminal exit0; /private/tmp/samosa-area3-redirect-regression-mac.log includes
normal/sanitizer public-policy/response tests, fetch897/921, search228/228 and
native CLI. Initial build88979 failed on the existing SQL helper name bind
colliding with the newly included native socket declaration and signed count
comparisons; the helper is now bind_text and strict warnings pass. Intermediate
49563 and normal parent test26565 passed compilation/mandatory tests.

The initial live redirect attempt in26565 failed with fetch_http_status. Direct
native curl inspection proved httpbingo returns402 with no User-Agent and302 with
the monolith's fixed SamosaChat/1.0 identity. Public-web hops now preserve that
monolith header; exact-origin provider transport is unchanged. Do not classify
this live case as passed until the corrected tests finish.

Final macOS49737 is live: /private/tmp/samosa-area3-redirect-final-mac.log, then
/private/tmp/samosa-area3-redirect-live-final-mac.log and
/private/tmp/samosa-area3-redirect-live-final-san-mac.log. Final Linux95547 is
live: /private/tmp/samosa-area3-redirect-linux.log, then
/private/tmp/samosa-area3-redirect-live-linux.log and
/private/tmp/samosa-area3-redirect-live-san-linux.log. Both test the fixed native
User-Agent code; live fixtures require three actual destinations, retained
final-URL acquisition metadata and exact offline replay. Poll those handles,
inspect any failure, and do not overwrite/restart their active sources.

Area3 remains ACTIVE. After redirect qualification, implement robots network
gating using the extracted evaluator and shared per-host politeness using the
monolith behavior, then provider-specific qualification/final area acceptance.
Areas1/2 remain CLOSED; areas4–7, full Sources release and C01–C14 remain open.
No commit, push, production switch or production legacy retirement.

Area3 remains ACTIVE. Its finite remaining checklist is bounded public redirects
with final-URL provenance, monolith robots/per-host politeness, provider-specific
request/reply qualification and final area acceptance. Reuse url_join,
robots_path_allowed and public_rate_wait. Do not move to area4, reopen areas1/2
without a demonstrated regression, or claim Sources released. No commit, push,
production switch or production legacy retirement.

## Sources area3 — robots network gate and parent-owned politeness,2026-10-09

Previous goal turn made progress integrating redirect execution/provenance.
Final redirect49737(macOS) and95547(Linux) are terminal exit0. Actual three-hop
redirects, retained original/final URL receipts and exact offline replay pass
normally and under ASan/UBSan on both platforms. Logs
/private/tmp/samosa-area3-redirect-live-final-{mac,san-mac}.log and
/private/tmp/samosa-area3-redirect-live-{linux,san-linux}.log; Linux full regression
is /private/tmp/samosa-area3-redirect-linux.log. Older route15939 is also terminal0.

Public-web hops now probe robots through the existing isolated native transport
with independent per-hop DNS validation/pinning/TLS/peer checks, a256KiB bound,
six destinations and no recursive robots gate. The extracted monolith evaluator
uses samosachat, specific/wildcard groups and longest Allow/Disallow prefixes.
Network/non-text/malformed robots replies fail open as in the monolith; scoped
cancellation/deadline wins. Disallow returns fetch_robots_denied before page work.
Content type is taken from the final HTTP header block and reset between blocks.

The monolith's64-slot, one-second host:port reservation table lives in the Sources
host process and is shared by its workers. Workers request a slot over the
existing private duplex protocol; no worker opens the Sources database or new
state path. Parent admission rejects the new P message unless the exact owned
request kind/profile permits it and bounds each acquisition to42 messages. Rate
keys canonicalize default/explicit ports and IPv6 spelling. Reservations use a
short mutex scope; waits occur unlocked with scoped cancellation/deadline and
worker RSS budget monitoring. New tests prove same-host concurrent spacing,
equivalent default/explicit ports, unrelated-host progress, deadline and
cancellation refusal. Source arrays and histories remain bounded.

Initial macOS23212 is terminal0. Intermediate robots61947 is terminal exit0:
/private/tmp/samosa-area3-robots-final-mac.log and
/private/tmp/samosa-area3-robots-live-{mac,san-mac}.log. Mandatory normal/sanitizer
checks and actual live robots Disallow, pinned/rate-controlled redirects, receipt
provenance and exact offline replay all pass. Final code adds RSS monitoring
during rate waits and header-block content-type reset; do not substitute the
intermediate live evidence for this last code revision.

Final macOS97711 is terminal exit0, log
/private/tmp/samosa-area3-robots-control-final-mac.log, followed by
/private/tmp/samosa-area3-robots-control-live-{mac,san-mac}.log. Mandatory tests,
normal/sanitizer fetch/search/native CLI and final actual live robots/redirect/
receipt/offline proof all pass. Final Linux83626
remains live from the exact new archive, log
/private/tmp/samosa-area3-robots-control-linux.log, followed by
/private/tmp/samosa-area3-robots-control-live-{linux,san-linux}.log. Both run
mandatory normal/sanitizer public tests, full fetch/search/native CLI regressions
and actual live transport/robots/receipt proof. Poll these handles; do not
rewrite their active candidate or restart on observation timeout.

Area3 remains ACTIVE. Provider audit finds six declared presets but test_search.c
currently checks only declaration presence for the full list; qualify each
provider's actual request/reply mapping rather than treating that loop as proof.
After final robots/rate qualification, extend native HTTP fixtures for Parallel,
Brave, Tavily, SerpAPI, Google and SearxNG, preserve credential/query/redaction and
control guarantees, and run final area acceptance. Paid live credentials remain
host-owned; do not invent proof for unavailable live services. Areas1/2 remain
closed. Areas4–7, complete Sources release, C01–C14 and full rebuild stay open.
No commit, push, production switch or production legacy retirement.

## Sources area3 — six provider mappings and final acceptance CLOSED,2026-10-09

Previous goal turn made progress integrating robots and parent-owned politeness.
Final Linux83626 is terminal exit0: mandatory normal/sanitizer public/rate tests,
full fetch/search/native CLI regressions and actual live pinned HTTPS, robots
Disallow, bounded redirects, retained final-URL metadata and exact offline replay.
Logs /private/tmp/samosa-area3-robots-control-linux.log and
/private/tmp/samosa-area3-robots-control-live-{linux,san-linux}.log. macOS97711 was
already terminal0. No robots/rate handles remain live.

New native tests/test_search_presets.c qualifies Parallel, Brave, Tavily, SerpAPI,
Google and SearxNG through real isolated worker HTTP. Only the granted origin is
replaced; original endpoint paths/query templates, headers and POST bodies stay.
The independent server checks exact percent-encoded query/credentials, provider
header placement, Tavily query/key/max_results, Parallel JSON-RPC/tool arguments,
query arrays and32-hex session. Provider-shaped results normalize correctly;
malformed data, provider errors and redirects refuse for each preset. Redaction
and exactly six immutable snapshots across24 attempts are independently asserted.
macOS75683 is terminal0 normally and under ASan/UBSan. These are component mapping
fixtures, not proof of paid vendor availability or authenticated live access.

Final standalone source archive /private/tmp/samosa-area3-accept-source.tar.gz,
SHA2564cce339b50d1b29775898842cf4867988d1cd30fda1b020139293f5008c2bb4a,
was independently extracted/rebuilt in /private/tmp/samosa-area3-accept-mac and
/tmp/samosa-area3-accept-linux. Gates31427(macOS) and8866(Linux) are terminal exit0.
Commands: make all test-fetch test-fetch-tls test-fetch-public test-search
 test-search-presets sanitize-fetch sanitize-fetch-tls sanitize-fetch-public
 sanitize-search sanitize-search-presets; then actual public test --live normally
and under ASan/UBSan, using only helpers built in the copied component tree.
No sibling private source/state or language model is required. Logs
/private/tmp/samosa-area3-accept-{mac,linux}.log and
/private/tmp/samosa-area3-accept-live-{mac,san-mac,linux,san-linux}.log.
Mac fetch841/867 and Linux870/1216 checks pass; TLS320/320 each, search228/228,
native CLI, all six preset cases and public controls pass. Variable fetch counts
include callback sampling; the same assertions are retained. Actual live pinned
HTTPS/robots/three-hop redirects/receipt/offline proof pass on each platform in
both modes. Archive is area3 qualification evidence, not the final Sources release.

Fresh actual keyless Parallel search initially returned search_response_invalid
twice through the workspace CLI; do not erase those observations or claim a
parser fix. Native capture/normalization diagnostics subsequently received valid
139-token structured replies and accepted all item fields. No parser rule was
weakened or product code changed for this observation. Fresh standalone CLI
searches with tracked operation IDs now pass on both platforms with eight items,
descriptions at most400 Unicode characters and immutable source identity. Exact
completed-operation replay passes with --fetch-offline and no provider/helper;
bounded read_text of the same retained snapshot passes offline. Native C checked
the response and generated read requests. Results are observation-dependent;
cross-platform hashes are not expected to match. Current proof:
/private/tmp/samosa-area3-parallel-live-fresh.json and
/private/tmp/samosa-area3-parallel-live-linux.json;
/private/tmp/samosa-area3-parallel-live-offline{,-linux}.json;
/private/tmp/samosa-area3-parallel-read-offline{,-linux}.json.
Initial private-state setup refusal was corrected by making the newly created
fixture directory0700, preserving the existing ownership contract.

Area3 closure checklist is satisfied: bounded5MiB acquisition/separate1MiB
search, exact-origin host grants and private credentials; actual HTTP/TLS and
independent worker DTO checks; extracted public DNS/address pinning and IPv6;
bounded redirects and retained original/final provenance; robots and shared host
politeness; durable owner/idempotency/control/recovery/offline behavior; all six
provider-specific mappings plus current live keyless search; independent
both-platform normal/sanitizer archive acceptance. Service authentication/load
and assembled-product qualification retain their established later scope. No
paid-provider availability, sustained free-tier quota or complete Sources release
is inferred from these checks.

Area3 is CLOSED. Areas1–3 are now closed; area4 indexing/enrichment/semantic
retrieval/Chutni is ACTIVE, and areas5–7 remain open. Next: inspect retained
Source audio filesystem discovery/reindex queue code and tests, compare missing
modality/enrichment/semantic behavior with working gateway and Chutni SDK, and
establish its finite completion checklist. Continue reusing the monolith. Do not
jump to an unrelated later area or mark C01–C14/full rebuild complete. No commit,
push, production switch or production legacy retirement occurred.

Area4 initial authoritative audit: index.c reader_format/text_candidate currently
admit five literal reader categories (text, HTML, DOCX, PDF text and images);
audio_index.c separately has durable snapshot/window/capture/publication jobs.
Automatic audio admission from inventory/discovery/reindex is not connected to
that reader selection. Next exact action: inspect the existing audio index create
contract and its job identity/recipe constraints, then connect inventory-selected
immutable audio versions to that existing queue while preserving observed
identity/freshness, owner claims and durable reindex checkpoints. Reuse the
monolith attachment_audio_decode_probe_path/jobs_watch_delta plus the qualified
native Sources audio inspector; do not build another decoder or transcription
engine. Remaining PDF-OCR fallback, visual/non-text indexing, enrichment and
semantic retrieval stay in area4's checklist. Model capability prerequisites
must use public Inference; area5's broader model qualification remains separate.

## Sources area4 — atomic audio queue admission prerequisite,2026-10-09

Previous goal turn made progress and closed area3 with final independent archive
acceptance. Area4 remains ACTIVE. Index capture currently re-enters public
Sources for reader operations while holding the index lease. Public audio create
also takes that lease, so automatic queue admission cannot simply call that
public operation recursively. Existing create now reuses a caller's active
transaction; public standalone creation still starts/commits its own transaction.
The private sources_audio_index_admit requires a caller-held index lease and
active transaction, invokes existing owner/scope/version/range/recipe/store
checks and queue creation, and returns the same deterministic job identity.
No decoder/model/queue engine is replaced. Typed errors use caller-owned buffers,
so native/recipe failures remain valid after temporary responses are freed.

New native test_audio_index_admit.c proves missing-transaction refusal, rollback
of both schema and job, deterministic retry after rollback, no premature commit,
idempotent admission preserving existing backoff, retained-WAV range refusal and
other-owner refusal. It uses real public registration, SQLite transactions and
index lease with pinned fixture model identity but no model call. macOS74642 and
Linux10013 are terminal exit0, normally and under ASan/UBSan. Logs
/private/tmp/samosa-area4-audio-admit-{mac,linux}.log.

Existing public selected-range audio queue/transcript/Chutni publication gates
are being rerun because their create path changed. Initial macOS51593 failed
with chutni_failed after the command selected the older vendor/build library.
The qualified artifact SDK is /private/tmp/samosa-chutni-artifact-qa/libchutni.dylib;
using that pinned artifact, final macOS61771 is terminal exit0, log
/private/tmp/samosa-area4-audio-admit-regression-final-mac.log. Its normal2205
checks and sanitizer gates pass. Linux46673 is terminal exit0, with2249 normal
and2505 sanitizer checks, log
/private/tmp/samosa-area4-audio-admit-regression-linux.log, using the existing
/tmp/samosa-chutni-sdk/build-artifact-qa/libchutni.so. Models in these regressions are
fixtures; actual Whisper proof remains the earlier declared model qualification.

Next exact integration action: connect inventory-selected owned audio snapshots
and qualified inspect_audio duration to this admission inside initial/reindex
commit, with durable typed audio linkage/status. The current index stores literal
text/evidence and reports indexed counts; do not put an empty string in it and
claim queued audio is indexed. Preserve source observation/version checks and
atomic queue linkage, then let the existing bounded audio runtime capture/publish
windows and expose completed derived artifacts through the correct public
retrieval/provenance path. Initial discovery, refresh/reindex, mutation/refusal,
restart/retry, owner/recipe changes and quota behavior must be qualified before
claiming this workflow complete. Remaining PDF OCR/visual indexing, enrichment,
semantic retrieval and Chutni work stay in area4. Areas1–3 remain closed;
areas4–7, complete Sources release and full C01–C14 rebuild remain open.

## Sources area4 — initial audio discovery integration, 2026-10-09

The preceding status-only goal turn was no progress. This continuation changes
index.c and the native admission fixture. Initial index_step recognizes WAV,
MP3/M4A/AAC/FLAC/OGG/Opus/AIFF paths, registers them through the existing public
reader with the captured file identity, and calls inspect_audio on the owned
snapshot. Non-PCM WAV falls back only on unsupported_audio_format. It admits the
full measured timeline through the existing audio queue, within the same SQLite
transaction as the typed index_audio relation, queued_audio item and cursor.
No model call or new decoder is introduced. Queue admission failures roll back
relation/item/cursor; caller-owned error storage survives return. Queue identity
and existing backoff survive duplicate discovery across index jobs.

Index status now exposes queued_audio_items and reports waiting_audio once
inventory processing ends with queued work. It stores neither fabricated literal
text nor a literal evidence envelope for these items. This is initial admission,
not completed audio indexing. The new native fixture checks public discovery,
queue/linkage consistency, no literal text, unconfigured dependency rollback,
configured retry and exact offline operation replay after original removal.

macOS final code/build/text regression passed all and test-index (712 checks),
log /private/tmp/samosa-area4-discovery-final-mac.log. Expanded discovery fixture
passes normal and ASan/UBSan, terminal31943 exit0, log
/private/tmp/samosa-area4-discovery-expanded-mac.log. Linux final extracted source
qualification84010 is terminal exit0: build,712-check text regression, normal
expanded discovery fixture and ASan/UBSan pass; log
/private/tmp/samosa-area4-discovery-final-linux.log.

Next: expose linked public queue identities and
completion, then implement audio observation/freshness/reindex with version
validation and lifecycle quotas. Finish all remaining PDF OCR/visual indexing,
enrichment and semantic Chutni acceptance before closing4 or moving to5.
Areas1–3 are closed;4–7 and the full rebuild remain open.

## Sources area4 — linked audio progress/completion, 2026-10-09

Previous goal turn made progress: initial discovery was integrated and qualified.
This continuation adds public index audio_jobs entries (ordinal plus public audio
job status), pending/completed derived audio counts, and completion reporting.
The internal report reuses load/window/status from audio_index_get, including
owner, root scope, snapshot digest, model/decoder recipe, all capture envelopes
and required Chutni publication receipts. A linked source mismatch refuses;
corrupt saved captures refuse before index status is emitted. No second queue
lease, original file read, new inference call or literal evidence conversion is
introduced. Capture-only completion requires all captures; publication-backed
completion additionally requires all receipts. Recognition coverage stays partial
and indexed_items remains a literal-only count.

The discovery fixture reuses the existing native publication transport fixture,
then proves linked job identity/progress, completion with transport and original
absent, corruption refusal, configuration rollback/retry and immutable operation
replay. Final build and712-check literal regression plus normal/ASan/UBSan expanded
discovery gates pass macOS and native ARM64 Linux, logs
/private/tmp/samosa-area4-audio-report-qualified-{mac,linux}.log. Those commands
subsequently failed compiling the new SDK test helper because its header was
missing; the initial macOS correction exposed feature-macro ordering, now fixed
by including audio_index.h after all feature macros. Retain the failed logs;
no product acceptance check was relaxed.

The real-SDK publication fixture now checks linked reporting after every capture,
including the last capture before its required publication, and after all eight
committed publications. It verifies reporting does not run another model call.
Final SDK gates48788(macOS) and71855(Linux) are terminal exit0; logs
/private/tmp/samosa-area4-audio-report-sdk-final-{mac,linux}.log. macOS normal2270
and sanitizer2284 checks pass; Linux normal2316 and sanitizer gates pass. Actual model
accuracy is not inferred from these fixture model outputs.

Next exact action: audio observation/freshness and
reindex with atomic revision/version validation, retained history and existing
bounded queue admission/backoff. Derived retrieval, PDF OCR fallback, visual
indexing, enrichment and semantic Chutni retrieval remain in4. Areas1–3 remain
closed;4–7 and all C01–C14 release milestones remain open.

## Sources area4 — audio observation/freshness/reindex, 2026-10-09

Previous goal turn made progress: linked queue reporting was implemented and
qualified with real SDK gates. This continuation connects queued/completed audio
links to the existing owner/root-scoped locator observation and reindex paths.
A shared version extractor validates literal evidence digests or the distinct
stored audio version. Watch observations create/clear matching audio intents;
bounded refresh rotates across literal and audio items without claiming queued
audio is literal text. index_pending reads retained audio intents without a root
or model grant; authorization remains owner-scoped.

Manual/background reindex reuse public expected-identity registration and owned
inspect_audio duration, then the existing shared-transaction queue admission.
Commit rechecks the committed observation revision, mechanical identity, content
hash and root scope. Previous link retention, new/current link, queue admission
and matching intent removal share one SQLite transaction. Old jobs and captures
remain retained. Existing audio eight-job capacity is preserved; linkage history
has a256-transition bound and no eviction. This is integration within those
bounds, not final history/quota/failure or compressed-audio workflow acceptance.

Expanded native WAV fixture covers actual bounded refresh and watch intent,
root/model-absent pending status, missing-Inference rollback, stale-file refusal,
background fairness admission, explicit reindex, exact offline operation replay,
duplicate queue reuse across two index jobs, both previous links retained, old
capture retention and new-version capture after original removal. Existing
transport fixture supplies model output; these checks do not measure recognition
accuracy. Normal/ASan/UBSan pass on macOS66902 and native ARM64 Linux42127, both
terminal exit0; logs /private/tmp/samosa-area4-audio-reindex-final-mac.log and
/private/tmp/samosa-area4-audio-reindex-linux.log. macOS51272 also passed build,
712-check text index and existing actual-SIGKILL reindex regression; Linux42127
passed the same build/text/reindex gates.

macOS78493 is terminal exit0 for existing refresh (9072 checks), observation
(428 checks), extraction-time freshness and sanitized reindex regression, log
/private/tmp/samosa-area4-audio-reindex-regression-mac.log. Linux12918 is terminal
exit0 for the matching refresh,428-check observation, extraction-time freshness
and sanitized reindex gates, log
/private/tmp/samosa-area4-audio-reindex-regression-linux.log. No qualification
handles remain live from this checkpoint.

Next: complete remaining audio history/quota, commit-race/process-death and
compressed-discovery workflow gates, then expose retained derived retrieval
through the correct artifact/provenance path. Complete PDF OCR fallback, visual
indexing, enrichment and semantic Chutni retrieval before closing4. Areas1–3 stay
closed;4–7 and all full rebuild release milestones stay open.

## Sources area4 — audio reindex crash/control/capacity gates, 2026-10-09

Previous goal turn made progress: observation/freshness/reindex was integrated
and existing regression gates passed. This continuation adds native
 tests/test_index_audio_failures.c and a test-only admitted-transaction marker.
Production builds exclude the marker. General test/sanitize targets now include
both audio admission integration and this failure suite.

The new suite exercises real public WAV indexing/watch/reindex transitions. It
SIGKILLs a worker after queue insertion inside the still-active transaction,
then verifies SQLite recovery preserves the old link, zero new history/jobs and
the pending intent. It separately kills after commit before operation receipt,
verifies the durable link/queue/history/intent commit, and resumes without another
job/transition. Before-commit tests supersede the observation revision or replace
the root; both refuse without changing the prior link. Scoped cancellation and
actual monotonic deadline expiry at that boundary also preserve state and permit
retry. Root restoration reuses the original granted scope.

Capacity checks admit eight actual distinct queue jobs, refuse a ninth and verify
old links/history and the pending intent remain. A separate fixture performs256
actual alternating-version public reindexes, retaining256 linkage transitions
while reusing only two queue identities. The257th transition refuses index_limit
without deleting history, changing the current link or clearing the intent. No
synthetic history rows, relaxed capacity, eviction or fake model call is used.
Actual capture preservation/model/SDK behavior remains covered by the prior
admission/publication fixtures; this suite covers its declared PCM WAV failure
and capacity scope, not compressed discovery or recognition accuracy.

Final macOS84327 and native ARM64 Linux15131 are terminal exit0 for normal and
ASan/UBSan gates. Logs:
/private/tmp/samosa-area4-audio-failures-final-mac.log
/private/tmp/samosa-area4-audio-failures-final-linux.log
The earlier7923 macOS suite also passed before root/control cases were added;
only the final logs support the expanded declaration. No qualification handles
remain live from this checkpoint.

Next: qualify actual compressed-audio initial discovery/reindex with the existing
native decoder/capture path, then expose retained derived retrieval with correct
artifact/timeline/provenance semantics. Complete remaining PDF OCR fallback,
visual indexing, enrichment and semantic Chutni retrieval before closing4.
Areas1–3 remain closed;4–7, final Sources release and full C01–C14 remain open.

## Sources area4 — compressed discovery/reindex corpus, 2026-10-09, qualified matrix

Previous goal turn made progress: native WAV crash/control/capacity gates passed.
This continuation adds native tests/test_index_audio_media.c and a fixed FFmpeg
corpus driver. Nine cases cover CBR/VBR MP3, M4A AAC, ADTS AAC, FLAC, Vorbis Ogg,
Opus Ogg, AIFF and stereo48kHz WAV fallback. Each uses31.1s/32.2s distinct versions,
measures the owned timeline, captures both overlapping30s-window/tail jobs after
original removal, reindexes watched changed bytes and validates both new/current
and retained old jobs. Model output reuses the existing native transport fixture;
actual decoding, input SHA, derived timeline/ranges and offline callback-absent
completion are checked. This is integration qualification, not recognition
accuracy or a new waveform-reference benchmark.

The corpus exposed a product gap: index discovery listed those extensions, while
encoded registration and the native decoder admitted only MPEG/QuickTime/WAV.
Sources now also recognizes FLAC/Ogg and RIFF/WAVE/FORM AIFF candidates; the existing
native worker admits ADTS AAC/FLAC/Ogg/AIFF and FLAC/Vorbis/Opus codec IDs through
its same memory-only, secondary-open-refusing, bounded conversion implementation.
No replacement decoder or dependency was added. Existing measured decoded-sample
floor policy counts padding rather than inventing an original speech duration.

Keep failed logs: initial71461(macOS) and66290(Linux) failed because the fixture
read index_step's operation envelope as a direct result; its envelope extraction
was corrected without relaxing acceptance. Subsequent17408(macOS) exposed the
actual ADTS decoder gap. Expanded66976(macOS) then failed on absent optional
libvorbis encoder, fixed by using built-in FFmpeg Vorbis/Opus fixture encoders.
33031 printed all case passes but exited2: the agent edited the corpus driver
while the sanitizer shell was reading it, producing a trailing syntax error.
Do not use that command as terminal qualification. Final stable37323(macOS) is
terminal exit0 with18 normal/sanitizer case passes against the unchanged driver; log
/private/tmp/samosa-area4-audio-media-stable-mac.log. Linux49550 is terminal exit0
with18 normal/sanitizer case passes using
the earlier libvorbis/libopus generator, log
/private/tmp/samosa-area4-audio-media-expanded-linux.log. The stable generator is
copied under a separate index_audio_media_stable.sh filename;74662 is terminal
exit0 with four changed Vorbis/Opus normal/sanitizer case passes against the already-built binaries,
log /private/tmp/samosa-area4-audio-media-stable-generator-linux.log. Its fixtures,
root/state and output directory are independent; no active script/binary is
overwritten. After49550 reached terminal exit0, the stable driver was copied
into the candidate's normal corpus filename for future runs. All qualification
handles from this checkpoint are terminal.

Core regression24829(macOS) and92938(Linux) are terminal exit0 for Sources351
checks and native/plain audio plus separate CLI checks; logs
/private/tmp/samosa-area4-audio-media-core-regression-{mac,linux}.log.

Next: implement
retained derived retrieval through Sources' public index/artifact interfaces.
Areas1–3 remain closed within their declared scopes;4–7 and the complete Sources
release/full rebuild remain open. Expanded format integration is not asserted
from the earlier area1 finite corpus alone.

## Sources area4 — retained derived audio history/read, 2026-10-09

Previous goal turn made progress: compressed discovery/reindex integration and
all declared normal/sanitizer matrix gates passed. This continuation adds public
index_audio_history and index_audio_read, advertises both in describe, and
strictly bounds their request fields. They reuse private queue load/window/status
validation on the same index SQLite read transaction rather than dispatching a
second lease/connection or re-running a model.

index_audio_read authorizes the configured owner and granted index root, checks
membership in current or retained links, binds the expected source version and
returns the exact existing derived transcript capture. It preserves artifact_id,
source/version, source-time range, uncertain coverage and model/input provenance;
no literal evidence_id is fabricated. Old originals and model transport may be
absent, but root/model/decoder configuration must satisfy the queue's existing
validation. Missing/nonmember/foreign links, malformed ranges, changed recipe
and corrupted captures refuse. Publication receipt validation is also reused.

index_audio_history returns at most16 superseded links plus validated public
queue status and next_after/has_more. The cursor orders immutable retained row
insertion, independently of locator observation revision; simultaneous revisions
on different locators cannot skip results. A full fixture checks16 consecutive
pages across256 real public reindex transitions, with increasing cursors and no
lost rows. Retained/source membership is deduplicated for repeated use of the
same queue job. History reads remain owner/root scoped.

Expanded native admission fixture verifies old/current capture reads after
original removal and transport removal, byte-identical replay against the public
queue reader, foreign-owner refusal, changed-model refusal and corruption
refusal. Existing712-check literal indexing, audio discovery/reindex, actual
SIGKILL/control/capacity fixtures and new history pagination all pass normally
and under ASan/UBSan on macOS11688 and native ARM64 Linux75926, terminal exit0.
Logs /private/tmp/samosa-area4-derived-read-qualified-{mac,linux}.log.
These fixtures use model fixture output; no accuracy claim is inferred. Encoded
queue validation is the same existing path, but the earlier compressed matrix
alone is not proof of these newly exposed index selectors on every format.

Next: derived candidate search that retains derived artifact/timeline/coverage
semantics, then complete PDF OCR fallback, visual/non-text indexing, enrichment
and semantic Chutni retrieval. Areas1–3 are closed;4–7, full Sources release and
all assembled C01–C14 remain open. No qualification handles remain live here.

## Area4 retained derived audio search — 2026-10-09

Implemented public `index_audio_search`, strict job/query admission and describe
advertisement. It reuses the existing queue's owner/root/snapshot/model/decoder,
capture digest/range and publication-receipt validation. Current and superseded
links are deduplicated by source/audio-job identity. Results retain derived
artifact identity, source-time range, coverage, provenance, committed freshness
and exact public read selectors; they never manufacture literal evidence.
At most10 candidates and512 preview bytes are returned. Uncaptured windows are
reported separately; all captured windows are validated even beyond the result
limit. The response is buffered until the full scan succeeds, so corruption or
configuration errors return their typed error without leaking partial candidates.
Substring matching is not semantic retrieval or recognition-quality evidence.

Actual qualification, all terminal exit0:

- macOS session94249 and ARM64 Linux session97426:
  `make test-audio-index-admit sanitize-audio-index-admit test-index`.
  Logs: `/private/tmp/samosa-area4-derived-search-qualified-mac.log` and
  `/private/tmp/samosa-area4-derived-search-qualified-linux.log`.
  New assertions exercise case-insensitive matching of both current/superseded
  captures with original files and model transport absent, owner refusal,
  model-configuration refusal, unmatched/empty queries, a real241s WAV reindex,
  uncaptured-window accounting,11 matched captures capped at10 with partial
  candidate coverage, and corruption refusal without any partial candidates.
  Literal-index regression passes712 checks on each platform.
- macOS session79254 and ARM64 Linux session56983:
  `make test-index-audio-failures sanitize-index-audio-failures`.
  Logs: `/private/tmp/samosa-area4-derived-search-dedup-mac.log` and
  `/private/tmp/samosa-area4-derived-search-dedup-linux.log`.
  The256 actual history-transition fixture now also searches its two reused,
  uncaptured audio jobs and proves scanned_captures0/uncaptured_windows2 rather
  than counting every historical linkage. Existing SIGKILL, races, controls and
  capacity assertions continue to pass normally and under ASan/UBSan.

Reuse for the next requirement: `chutni_enrich_source` in
`src/samosa_gateway.c` already distinguishes OCR PDF pages and stores page text
with extraction provenance. Sources already exposes bounded `read_pdf_ocr`,
including locator/cache/control validation. Adapt these existing paths for PDF
OCR indexing fallback rather than introducing another OCR engine. Current
index capture only requests `read_pdf`; its missing fallback is confirmed in
`src/index.c`'s live_request/capture. Preserve selected page identities and
coverage when combining PDF text and OCR.

Next: PDF OCR indexing fallback, then remaining video/non-text, enrichment and
semantic/Chutni retrieval requirements. Area4 remains ACTIVE; areas1–3 remain
closed. Areas5–7 and all C01–C14 milestones remain open. No full component release
or legacy production retirement is claimed. No live test handles remain.

## Area4 mixed PDF text/OCR indexing and SDK publication — 2026-10-09

Implemented PDF indexing fallback using the existing public `read_pdf_ocr`
operation behind locator_read, and the monolith `chutni_enrich_source` behavior
that preserves page text and OCR provenance. Only empty/whitespace text-layer
pages within the existing first-four-page selection receive OCR. Usable text
pages retain their exact extraction. The combined literal capture retains
absolute pages, original source/version, constituent text/OCR captures,
recognition/raster/engine identities and partial coverage. No new OCR engine,
SDK implementation or cross-component private state was introduced.

The existing index capture function handles both discovery and reindex. Configured
OCR failure returns an item error without a partial combined capture. An absent
OCR configuration preserves the existing partial text-layer-only mode. The
four-page and64KiB indexed-text bounds remain. Publication parent recognizes the
combined reader as PDF; its isolated Chutni worker regenerates exactly the same
composition from owned bytes before checking text, capture bytes and page
selectors against the saved evidence. This shares the composition function with
index capture while reusing the existing PDFium/Tesseract extraction functions.
First Chutni source registration retains the existing matching-live-file check;
immutable regeneration does not bypass SDK registration policy.

Qualification command on macOS and native ARM64 Linux:
`make all test-index-pdf-ocr sanitize-index-pdf-ocr test-index`, with actual
PDFium, Tesseract, Leptonica, eng.traineddata, native sentinel PPM and the
qualified external artifact-capable Chutni SDK configured explicitly.

- macOS session64104: terminal0. Normal1080 and ASan/UBSan1082 native/public OCR
  checks; literal index712 checks. Log:
  `/private/tmp/samosa-area4-pdf-fallback-sdk-accept-mac.log`.
- Linux session5221: terminal0. Normal1083 and ASan/UBSan1083 native/public OCR
  checks; literal index712 checks. Log:
  `/private/tmp/samosa-area4-pdf-fallback-sdk-accept-linux.log`.
- Source archive used for Linux:
  `/private/tmp/samosa-area4-pdf-fallback-sdk-accept-source.tar.gz`.
  Candidate rebuilt in `/tmp/samosa-s19v-default-candidate` in the established
  `samosa-s17a-linux-qa` container. This is qualification input, not a final release.
- The new native two-page fixture contains actual PDF text `EXACT TEXT LAYER 2468`
  on page1 and an image-only `TOKEN 7319` raster on page2. Both phrases are
  searchable; only one page receives OCR. Combined candidate preserves page1–2,
  rendered-pixel provenance and qualified coverage. First actual SDK publication
  commits and SDK retrieval finds the OCR token. After original removal and OCR
  configuration removal, retained index search still succeeds; foreign owner
  cannot retrieve the index. Sanitizers instrument the parent, native reader
  worker and Chutni publication worker; external engine/SDK libraries are not
  instrumented. This fixture proves integration, not broad OCR accuracy.

Retained failed attempts are not acceptance evidence. The first test mistakenly
put its state directory under the indexed root, admitting private state files;
state now resides outside the test root. GNU warning-as-error findings in two
new callbacks were fixed. The SDK test initially selected the generic reader
worker, then attempted first registration after deleting the file; it now uses
the dedicated normal/sanitized Chutni worker and publishes before removal.
A failed fixture-edit command caused one redundant failed rerun; only the final
accept logs above establish acceptance.

PDF OCR work remains open for actual changed-version reindex/freshness/failure
qualification, complete artifact-read provenance and publication dependency/error
checks. In particular, regeneration failure currently maps to corrupt_index in
its combined-reader worker branch; qualify/preserve typed dependency/control
errors before considering that failure path complete. Then continue the remaining
area4 video/non-text indexing, enrichment and semantic/Chutni retrieval checklist.
Areas1–3 remain closed; area4 remains ACTIVE; areas5–7, C01–C14 and the complete
architecture goal remain open. No final Sources release or legacy retirement.
All test handles from this patch are terminal; none remains live.

## Area4 PDF OCR reindex, failure and artifact-read gates — 2026-10-09

The combined PDF publication worker now preserves dependency/control failures
through the existing native protocol error catalog. The catalog returns stable
error identities, so the dynamically allocated regeneration error can be freed
without either a dangling pointer or a false corrupt_index result. Unknown error
codes remain native_helper_protocol. No second error list was introduced.

Expanded native PDF OCR acceptance uses actual PDFium/Tesseract, the native
mixed text/raster fixture, and the qualified external artifact-capable Chutni SDK:

- Missing OCR configuration during publication returns ocr_unavailable, records
  that exact pending publication failure and advertised retry time, then succeeds
  with restored configuration after the real retry deadline. The test waits for
  the public timestamp rather than duplicating the backoff formula.
- Actual changed original PDF bytes schedule reindex intent. A configured missing
  OCR model returns ocr_artifact_invalid and preserves the original capture/intent.
- On cold changed versions, the third native extraction is the selected OCR page.
  During that extraction the fixture changes the live file, requests scoped
  cancellation or expires the real monotonic deadline. These return freshness,
  cancellation or deadline errors; the pending item and original searchable
  capture remain, with no extra retained version or partial candidate.
- A healthy observed version then completes reindex. Original and new versions
  remain searchable and publish as distinct SDK artifacts. Public memory_read
  proves exact original/new source identity, mixed text/OCR content, absolute page
  selectors and full original Sources evidence/recognition/raster provenance.
- Exact durable artifact-read retries succeed after original removal with root,
  OCR configuration and SDK worker absent. Foreign index owner is refused.
- A forged OCR text fixture modifies both page/captured recognition text and
  recomputes the evidence digest and matching text projection. Actual isolated
  publication still refuses corrupt_index, with no response/partial artifact,
  because its owned-snapshot regeneration differs.

Final qualification command on both platforms:
`make test-index-pdf-ocr sanitize-index-pdf-ocr test-ocr-pdf sanitize-ocr-pdf test-index`
with actual dependency paths configured as in the preceding journal entry.
The focused SDK target now runs its indexing/publication gates directly; the
complete native/public OCR regression is run separately, including original
worker/raster/control/owner/retry checks. The successful complete commands prove
both scopes without repeating the full OCR regression inside each focused target.

- macOS session24356: terminal0. Focused256 normal and256 ASan/UBSan checks;
  full native/public OCR1184 normal and1186 sanitizer checks; index712 checks.
  `/private/tmp/samosa-area4-pdf-ocr-qualified-mac.log`.
- ARM64 Linux session19084: terminal0. Focused256 normal and256 sanitizer checks;
  full native/public OCR1187 normal and1187 sanitizer checks; index712 checks.
  `/private/tmp/samosa-area4-pdf-ocr-qualified-linux.log`.
- Linux source input:
  `/private/tmp/samosa-area4-pdf-ocr-qualified-source.tar.gz`, rebuilt in the
  established isolated candidate. Parent, reader and publication-worker code are
  instrumented; external engine/SDK libraries remain uninstrumented. No final
  complete Sources release is claimed.

Earlier accepted217-check reindex runs established the initial control/history
cases. The final256-check gates add missing-model and recomputed-digest refusal.
The retained first failed retry fixture waited1.1s despite an advertised later
retry; it now reads/waits that public deadline. Only final qualified logs above
are acceptance evidence. All patch test handles are terminal.

The PDF OCR indexing requirement is qualified within the established four-page,
64KiB text and render/resource limits, reusing the existing reader qualification;
it is no longer an unspecified broader-qualification backlog. Area4 remains ACTIVE
for video/non-text indexing, enrichment and semantic retrieval. The next precise
prerequisite is public Inference visual analysis, absent from its current config
and contracts. Native VisionPsy/Molmo2 engines and gateway inspect/session
interaction already exist and must be reused. The main plan records this limited
owning-component extraction and return to Sources. The existing public Chutni SDK
semantic profile/representation/search API is also recorded for later reuse.
Areas1–3 remain closed; areas5–7, all C01–C14 and the full goal remain open.
# In-progress visual Inference dependency — 2026-10-09

Area 4 remains active. The optional native VisionPsy public adapter is not yet
runtime-qualified. Model identity now rejects malformed pins before formatting;
shared artifact verification checks cancellation/deadlines between 64KiB reads
and before completion. Native artifact tests cover initial, midstream and final
control interruption plus modification during hashing. macOS `make vision
test-artifact sanitize-artifact` passed; both artifact runs report 1032 checks.
The adapter also records model directory/file identity and timestamps before
execution and rejects changes on revalidation; runtime race coverage is pending.
Next: qualify the actual inspect/quit bridge and public controls, then integrate
derived visual captures into Sources indexing. No area or release closes here.

## Public visual-analysis prerequisite qualified — 2026-10-10

The optional Inference adapter reuses native VisionPsy inspect/quit, shared
artifact verification, FIFO admission, durable operations and the existing
process guardian. It accepts bounded packed RGBA pixels and returns an uncertain
derived visual observation with input, PPM, engine and compound model identities.
The public contract is `components/samosa-inference/contracts/visual-v1.md`.
Sources retains source preparation and timeline ownership.

Native tests exercise exact red/transparent-to-white pixel conversion, the full
512x512 request, canonical base64 refusal, dimension limits, pinned model refusal,
offline byte-identical durable replay, changed-request conflicts, owner isolation,
malformed/oversized/empty/wrong-ID/over-budget/NUL/failed-quit responses, model file
mutation, executable and model-directory replacement followed by restoration,
deadline, cancellation and requester SIGKILL. Workspaces and engine processes
are checked absent; a healthy request follows failure recovery.

The first macOS run exposed Darwin denying exec through /dev/fd and model-directory
traversal through /dev/fd/5. The bridge now reuses native path execution on macOS,
resolves its bound model directory via F_GETPATH and checks identity before launch
and retained/named artifact receipts after reaping. Linux keeps descriptor-based
execution and directory access. These are observation checks, not filesystem locks.
The first Linux sanitized malformed-reply fixture leaked its own allocations,
causing LeakSanitizer to exit the fixture before response classification. The
fixture now releases allocations on intentional malformed/big/failure exits;
sanitizer checks remain enabled.

All handles below are terminal exit0:

- macOS final `test-vision sanitize-vision`: 634/644 checks,
  `/private/tmp/samosa-area4-vision-final-mac.log`, handle74956 (also actual sanitized
  gate below). Linux independently extracted candidate: 644/664 checks,
  `/private/tmp/samosa-area4-vision-final-linux.log`, handle82090.
- Shared native text/operation/admission regressions: 98/832/79 checks on each
  platform; macOS handle29731, Linux handle82090. Check counts for process/file
  I/O can vary with executable size and pipe scheduling.
- Guardian normal/sanitized: 97/97 on each platform. Whisper normal/sanitized:
  macOS 508/511, Linux 514/526; qualified logs are
  `/private/tmp/samosa-area4-vision-qualified-{mac,linux}.log`, terminal handles89048
  and19678. Native artifact normal/sanitized remain 1032/1032 on each platform.
- Actual macOS native VisionPsy/installed BF16 checkpoint, public prepared
  395x512 heading input and durable replay: 15881 checks, observed `helloworld`.
  Normal log `/private/tmp/samosa-area4-vision-real-mac.log`, handle8516;
  sanitized adapter/harness log
  `/private/tmp/samosa-area4-vision-real-sanitize-mac.log`, handle74956.
  Native engine/MLX itself is the reused existing build, not a sanitized rebuild.
  The existing hello.pdf fixture was rendered with the native PDFium extractor;
  its packaged dylib was supplied through DYLD_LIBRARY_PATH.

Actual recognition evidence is bounded to that established heading fixture.
Linux fixtures do not claim MLX/Metal checkpoint availability on Linux. Broader
recognition quality, Sources visual capture/cache/index/publication integration,
enrichment and semantic retrieval remain required. Area4 remains ACTIVE; areas1–3
remain CLOSED. Areas5–7, independent final releases, legacy retirement and every
C01–C14 milestone remain open. Next: implement Sources visual capture projection
and model-bound cache, then durable visual discovery/reindex/publication using
the existing media preparation, worker transport and Chutni SDK.


## Sources public visual capture qualified — 2026-10-10

Public analyze_visual now consumes owned prepared frames through public Inference,
retains source timestamps and decoder/preparation/model provenance, and caches
validated derived observations by owner, frame recipe, prompt and model pins.
Coverage explicitly records one selected frame and visual-analysis uncertainty.
Tracked retry replays without the origin, model transport or media worker.
Separate visual provider configuration preserves existing audio configuration.

Terminal exit0 evidence:
- macOS and Linux native visual fixtures: 643 checks normally and 643 under
  ASan/UBSan on each platform. Gates cover independent RGBA/P6 hashes, alpha,
  owner/model/prompt/cache integrity, quota, cancellation, deadline, replay and
  transient buffers. Logs: /private/tmp/samosa-area4-source-visual-final-fixture-mac.log
  and /private/tmp/samosa-area4-source-visual-final-linux.log.
- Actual macOS Sources -> native media worker -> public Inference -> reused
  monolith VisionPsy/installed model: 51 checks normally and 51 with Sources,
  worker, public adapter and harness sanitized. The MLX engine itself was not
  sanitized. Frame timestamp was 1,000,000 us; the heading was recognized but
  repeated, so this is bounded integration evidence, not broad model accuracy.
  Logs: /private/tmp/samosa-area4-source-visual-real-mac.log and
  /private/tmp/samosa-area4-source-visual-real-sanitize-mac.log.
- Both platforms: core Sources 351, control 135 and literal index 712 checks.
  Linux terminal evidence is in the final Linux log; macOS terminal stdout also
  confirmed these counts. All qualification handles are terminal; none remain live.

Area4 remains ACTIVE. Durable visual queue admission, discovery/reindex,
publication, enrichment and semantic Chutni retrieval remain. Areas5–7 and the
full release/C01–C14 goal remain open. Next: reuse the existing media_video.c
bounded duration/stream inspection for silent-video planning, then extend the
existing durable index scheduler and artifact publication path. The monolith's
video enrichment already preserves timestamped sparse-sampling uncertainty;
retain that behavior rather than presenting one frame as full-video coverage.

## Visual timeline inspection implementation — 2026-10-10

Progress: shared bounded video_read now drives both native timed frame decoding
and metadata-only sources_media_video_probe. No second container parser or guessed
duration was added. Public inspect_visual uses a new isolated-worker request,
current engine fingerprint, owned snapshot hash checks and strict parent DTO
validation. It reports container_floor_ms, source dimensions/origin and explicitly
partial/media_inspection_only coverage. Tracked operation replay works offline.
The primitive's actual silent blue two-second video, existing exact timed frame,
uncovered selection, malformed-media, cancellation and deadline checks pass
macOS/Linux normally and under ASan/UBSan. Primitive logs:
/private/tmp/samosa-visual-probe-{build-mac,sanitize-mac,linux}.log.
Initial macOS public fixture gate: 664 checks normal and 664 ASan/UBSan;
control135/index712 regressions also pass. Linux identified an unchecked duration
parse warning; code now checks that parse explicitly before formatting output.
Final changed-source gates were launched as macOS handle35196 and Linux95548,
logs /private/tmp/samosa-visual-inspection-final-{mac,linux}.log. Those handles
must reach terminal success before final qualification is claimed. Durable visual
queue/discovery/reindex/publication remains next; area4 and full goal stay active.

Final macOS handle35196 reached terminal exit0: changed-source public visual
gates passed664 normal/664 ASan/UBSan. Linux95548 has passed its normal664
gate and is still running the sanitizer/regression sequence; do not restart it
on an observation timeout.

Linux final visual normal and ASan/UBSan gates both passed664 checks.
The same95548 handle continues with control/index regressions; no replacement
qualification process was launched.

Final Linux handle95548 reached terminal exit0: visual664 normal/664
ASan/UBSan; control135 and index712 regressions passed. macOS final35196
also terminal0; prior macOS control135/index712 terminal0. All handles from
this inspection slice are terminal. Next: extend existing SDK derived-artifact
validation/publication for visual captures, then durable visual queue admission
and index discovery/reindex/retained-read integration. No area or release closes.

## Sources visual SDK publication implementation — 2026-10-10

Progress: public publish_visual and private source_visual_publish_capture now use
the existing derived-artifact writer, shared4096 receipt quota and isolated public
SDK. No second store or publication transaction engine was introduced. The SDK
worker validates visual captures distinctly from transcripts, writes image_caption
/model_generated with producer samosa-sources-visual-v1, exact selected frame
[timestamp_us,timestamp_us+1) in source_time_us and complete sources_capture
parameters. Source window/decoder/preparation/model identities and uncertain
single-frame coverage remain retained. Public publication reads an owner/model/
prompt-bound cached capture and cannot invoke a model. Tracked success replays
with the file, root, worker and visual provider absent.

Actual native media/public SDK tests passed108 normal/108 ASan/UBSan on both
platforms. Expanded tests add tracked exact/offline publication replay and changed
model cache refusal: final macOS129/129, Linux normal129. Linux's expanded SAN
binary was stale because its earlier compilation completed after the test file
copy; that108 result does not prove expanded checks. Earlier make95536 is now
terminal0, and an explicit newer test timestamp/rebuild is running as62717.
No process was restarted because of an observation timeout.

Shared writer regression: macOS2272 normal/2280 ASan/UBSan; Linux2296 normal/
2574 ASan/UBSan. These native transcript/public SDK/audio queue gates include
literal publication, durable phases, runtime/TERM, SDK commit/SIGKILL recovery,
owner/configuration and corruption refusals. Model is a fixture, not an actual
speech/vision quality claim. Logs:
/private/tmp/samosa-visual-publication-qualified-{mac,linux}.log;
/private/tmp/samosa-visual-publication-final-mac.log;
/private/tmp/samosa-visual-publication-final-linux.log.

Remaining live handles:62717 (Linux expanded SAN + visual/control/index regressions,
/private/tmp/samosa-visual-publication-final-qualified-linux.log) and31714 (macOS
visual/control/index regressions,/private/tmp/samosa-visual-publication-regressions-mac.log).
Area4 stays active: durable visual admission/discovery/reindex/retained history,
enrichment and semantic retrieval remain. Areas5–7/full Sources release/C01–C14
remain open. Next is queue integration using Sources' existing scheduler,
transactional index observations and the now distinct caption writer.

MacOS regression handle31714 is terminal0: visual664/control135/index712.
Expanded Linux SAN publication129 is now confirmed in final-qualified-linux.log;
the same62717 handle is still finishing visual/control/index regressions.

Final Linux62717 reached terminal exit0: expanded publication SAN129,
visual664/control135/index712 regressions. Final expanded publication normal
Linux129 is in final-linux.log; macOS129/129 in final-mac.log. All fixture/model,
SDK writer and regression gates from this slice are terminal successful.
Current checkpoint: public caption publication and retained SDK reads qualify;
area4 remains active. Next durable visual admission/discovery/reindex/history,
then remaining enrichment/semantic retrieval. Broader model quality, complete
service and final assembled releases/milestones remain open.

Final standalone CLI rebuilds36013(macOS)/96360(Linux) are terminal0.
Both actual executables' public describe replies advertise inspect_visual,
analyze_visual and publish_visual. Logs cli-build-{mac,linux}.log and
describe-{mac,linux}.json under/private/tmp/samosa-visual-publication-.
No live qualification handle remains from this slice.

## Shared durable audio/visual queue implementation — 2026-10-10

Progress: audio_index.c and visual_index.c now instantiate media_index_impl.h.
Scheduling, leases, capture/receipt commits, SDK recovery, retained-window proofs,
backoff and admission remain one implementation. Existing audio job identifiers
and namespaces are preserved. Audio/visual share eight retained jobs and8MiB
captures; both schemas initialize in either entry path. The first regression
caught a missing SQL separator between those schemas; corrected before further
qualification. Existing audio native SDK/runtime/death regression then passed2264
checks on macOS (terminal26987).

Sources exposes visual_index_create/get/step/queue_step/read and the existing
runtime executes a bounded visual step per cycle. Admission validates owned video
bytes and duration; recipe binds visual provider/engine/model/default prompt,
128-token/512-axis/30-second sampling and decoder identity. Window planning admits
up to480 selected-frame windows, never continuous visual coverage. Private transient
capture avoids public cache insertion. Model/publication failure preserves prior
captures and durable backoff. SDK commit followed by parent SIGKILL recovers the
same caption without the origin/model callback.

Actual61-second native video/public SDK initial macOS gate1214, expanded runtime/
TERM and mixed audio/visual admission gate1343 (poll counts can vary). These are
fixture model gates, not recognition quality claims. Expanded unknown-op refusal
and final normal/sanitizer gates are running as macOS14931 in
/private/tmp/samosa-visual-queue-final-mac.log. Earlier Linux21664 is still running
its original normal/sanitizer/shared-audio gates in
/private/tmp/samosa-visual-queue-qualified-linux.log; finish it before updating
that candidate to final expanded tests/current strict operation allowlist.

Area4 remains active. Next: transactional visual discovery/reindex association,
retained visual version history and derived candidate retrieval, followed by
remaining enrichment/semantic Chutni retrieval. No area, release or milestone
closes for the queue foundation alone. All C01–C14/full rebuild remain open.

MacOS14931 terminal0: expanded visual queue1357 normal/1459 ASan/UBSan,
shared audio SAN2296; earlier shared audio normal2264. Linux21664 terminal0:
initial visual1214/1214, shared audio2304/2526. Final expanded Linux candidate
copy occurred only after21664 terminated. Final Linux gate and macOS public CLI/
control/index regressions are running; record their terminal evidence next.

Final expanded macOS visual queue1357 normal/1459 ASan/UBSan and shared
audio SAN2296 are terminal0; final macOS standalone CLI/control/index43151
also terminal0 (control135/index712). Final expanded Linux16433 has passed
normal1599 and is still running the same sanitizer/control/index sequence.
Do not restart that handle on observation timeout.

Final expanded Linux16433 did not qualify: its sanitizer runtime child exited1
before a visual capture; parent harness kept polling. Authoritative proc state
showed child7195 zombie/exit256 and runtime job cursor0/total1/last_errorNULL.
The exact failing harness7089 was stopped with TERM;16433 then terminal2. This
was an observed child failure, not an observation timeout/restart. The harness
now records runtime output and checks child exit during polling. Diagnostic
SAN handle41437 is live in /private/tmp/samosa-visual-queue-runtime-diagnostic-linux.log.
Do not claim final cross-platform runtime qualification before resolving it.

Continuation: diagnostic Linux sanitizer handle41437 is terminal2. Its captured runtime output is `store_failed`; the child exited before processing the visual job. Schema initialization in index.c, locator.c and the shared media queue flattened SQLITE_BUSY/SQLITE_LOCKED into a fatal error. Those transient outcomes now report store_busy, preserving the runtime's existing retry path; other SQLite errors still report store_failed. Concurrent status/runtime regression gates are live: Linux14047 and macOS56247. Logs: /private/tmp/samosa-visual-queue-busy-fix-{linux,mac}.log. No area closure claimed.

SQLite contention correction qualified: Linux14047 terminal0, durable visual queue SAN4128 checks, including actual runtime completion under concurrent public status polling, publication retry, SDK commit/SIGKILL recovery and shared audio/visual quota. macOS56247 terminal0, normal1391/SAN1504. macOS41836 terminal0, index712/control135/audio admission gate. Poll-related counts vary with sanitizer/platform speed; they do not represent additional features. Model callback remains a fixture; actual native media worker and public Chutni SDK are exercised. Final Linux normal/regression gate is live in /private/tmp/samosa-visual-queue-busy-regression-linux.log. No area4 closure or complete Sources release claim.

Final Linux normal/regression gate53799 is terminal0: durable visual queue1715 checks, native index712, request control135 and audio discovery/freshness/reindex admission all pass. Together with Linux SAN14047 and macOS56247/41836, the SQLite schema contention fix and expanded durable visual queue/runtime slice are qualified. No live qualification handles remain. Next: automatic visual discovery/reindex, retained visual version history and derived caption retrieval; then enrichment and semantic/Chutni retrieval to close area4. Areas5–7 remain open. Reuse existing audio transaction/queue paths and working monolith media/model paths; preserve temporal Molmo2 analysis for area5. Full architecture goal remains active; no milestone/release closure claimed.

Area4 continuation: index_media_impl.h extracts the existing audio report/read/history/replacement/derived-search implementation and instantiates fixed audio and visual namespaces. Audio IDs and public fields are preserved; visual status fields and strict index_visual_history/read/search added. Visual discovery registers/probes owned media and atomically admits queued_visual; freshness and pending observation join both modalities. Replacement retains old associations under a shared256 history bound. Native test_index_visual uses the actual media worker with a fixture public Inference callback; normal/SAN macOS44154 is terminal0 (210/210), existing index712 and audio admission gate pass. It verifies missing-config rollback, exact offline operation replay, freshness/reindex, owner/corruption refusal, retained caption search after origin removal and SIGKILL after transactional admission preserving the old capture. Linux61169 failed compiler misleading-indentation before runtime; formatting corrected in the isolated candidate, Linux67936 live at /private/tmp/samosa-index-visual-qualified-linux.log. The host receives the same whitespace-only correction after macOS compilation completed. No Source area closure claimed.

Linux67936 is terminal2: normal210 passes, SAN crash-marker harness failed ready after its fixed5s wait; the replacement child9816 remained live and subsequently reached the actual admitted marker. The exact private child was killed after marker verification to release its uncommitted test transaction. Harness now waits up to120s with waitpid early-exit detection and bounded failure cleanup. Added later-file discovery plus automatic initial processing/deduplication assertions. Expanded normal51967/38972 failed because the test expected nonexistent added_items instead of the actual total_items field; test corrected to require total_items2 and committed associations/queue counts. No production weakening. Final expanded gates macOS71722/Linux62808 are live at /private/tmp/samosa-index-visual-final-{mac,linux}.log. Poll counts vary; do not restart live handles on observation timeout.

Expanded normal71722/62808 terminal2 exposed test-only discovery_disabled because the later-file automatic test left discovery_interval_ms at0. Corrected the host test configuration to100ms, restoring0 afterward; this keeps production admission policy unchanged. Current expanded macOS65289 terminal0 (242 normal/249 SAN; variable waitpid poll checks) covers later discovery+automatic admission/deduplication and killed replacement. Linux27978 is confirmed live, normal260 passes and SAN is running. Logs /private/tmp/samosa-index-visual-final-{mac,linux}.log.

Authoritative visual integration checkpoint: final macOS65289 terminal0 (242 normal/249 SAN); final Linux27978 terminal0 (260 normal/400 SAN), native index712 and audio admission/freshness/reindex regressions pass. Variable counters include waitpid polls; all substantive assertions are identical. No live gates remain. Logs /private/tmp/samosa-index-visual-final-{mac,linux}.log. Visual discovery, automatic initial processing of later files, queue/capture deduplication, immutable replacement, retained history/read/lexical caption search, owner/corruption refusal and SIGKILL admission rollback are qualified. Actual native workers are exercised; model callback is a fixture, and no broader model/video-understanding claim is made. index_media_impl.h shares those paths with audio, whose existing regressions pass. Next: finish area4 enrichment and semantic/Chutni retrieval through existing public SDK representations and profile checks, auditing monolith reuse first. Broader temporal visual/speech qualification is area5; complete authenticated service/load/shutdown is6; assembled native qualification/release/legacy retirement is7. The complete architecture objective and C01–C14 remain active/open.

Prepared-vector semantic retrieval continuation: semantic.c/h implements fixed
host-profile validation, bounded finite f32 vectors, nonzero/l2 norm checks and
thread-scoped C numeric formatting. The existing Chutni worker handles public
search_semantic through the same read-only four-function SDK ABI; parent verifies
store/query/profile identity, cosine range/order and distinct score type. Existing
retrieval snapshot/owner/control/operation replay paths are reused. The operation
canonicalizer now admits bounded numeric vectors, including equivalent decimals.
New public operation memory_search_semantic and CLI private semantic-profile file
configuration are advertised, with dimensions1024/results8/profile4096 limits.

Development evidence: initial Mac40964 build terminal0. Native test86949 failed
test feature declaration (mkdtemp needed _DARWIN_C_SOURCE), corrected.19644
exposed actual scalar-only operation canonicalization (invalid_request); vector
canonicalization fixed.72196 then exposed test read_text end4096 beyond the
returned immutable metadata size; corrected to returned bytes.17503 actual-SDK
normal terminal0 (239). Linux43260 normal/SAN terminal0 (239/239). Mac73639 SAN
239 and existing Chutni parent115/actual retrieval98446/control135 terminal0.
Bounds38930 normal/SAN58 each pass; temporary duplicate Make recipes were
corrected before final combined gates, preserving both actual SDK and bounds
checks. Linux19293 bounds50/50, parent115/actual retrieval98412/control135 all
terminal0. Alternative numeric-locale restoration is exercised where that locale
is installed (Mac58 versus Linux50); counts are not feature totals.

Authoritative final combined gates: Mac terminal0 in
/private/tmp/samosa-semantic-final-mac.log (bounds58 + actual SDK239 in each normal
and SAN build); Linux52973 terminal0 in
/private/tmp/samosa-semantic-final-linux.log (bounds50 + SDK239 in each build).
No live gates remain. Tests prove rank ordering, signed vectors, exact model
profile isolation/empty mismatch, read-only SDK counts, wrong store pin refusal,
owner/offline metadata reads/replay, expired/cancelled ingress, malformed parent
scores and max/invalid dimensional/numeric inputs. Representations are fixtures;
no embedding inference or semantic-model quality qualification is claimed.
Native public-SDK store_info helper and actual Sources CLI profile-file queries
pass on both platforms; outputs /private/tmp/samosa-semantic-cli-{mac,linux}.json.
Initial manual CLI fixture selection accidentally matched a tool filename;
restricted to six-character fixture directories before successful CLI runs.

Next reuse audit is concrete: monolith src/samosa_summarizer.cpp and gateway
native_summarize_once/model-text enrichment/publication remain available. Native
OpenSSL SHA256 verifies existing build/samosa-text-summarization-Q8_0.gguf matches
30c73e0b2ebd4f65538d87f5157aa7193261e8ff362e441df86c6941f3e9265c; existing engine
build/samosa-summarizer is514e5c8512d44ecc3da246039d30fcd7a8218e0be6f363364a53e18c212df397.
Next expose this native summarizer through public Inference with artifact/control
qualification, then reuse the bounded summary/batch/provenance pipeline in Sources.
Automatic enrichment, representation publication and public Inference query
embeddings remain required area4 work. No area4/release/milestone closure.

Native summarizer continuation (2026-10-10): added Inference summary_protocol.c/h using the existing monolith scalar framing and summarize-prefix convention. Input is bounded to 60,000 valid UTF-8 bytes; replies require exact framing, nonempty valid UTF-8 and at most 8,192 bytes. Native boundary checks pass macOS/Linux normal and ASan/UBSan through test-summary-protocol and sanitize-summary-protocol. Linux used /tmp/samosa-summary-protocol-qa copied from the current source (the container /workspace mount does not expose components). This is codec qualification only: no public summarize operation, engine execution, automatic enrichment or area4 completion is claimed. Next extend the existing guardian with separately bounded protocol stdin/stdout: it currently discards stdin and merges stdout/stderr into logs, so it cannot run this engine protocol unchanged. Preserve model/executable pins, lease retention, parent-death cleanup, cancellation/deadlines, and derived uncertainty before Sources integration. Areas1–3 remain closed;4 active;5–7 pending. No live checks remain.

Public native summarizer continuation (2026-10-10): implemented optional samosa-inference-summary using the actual unchanged monolith T5 engine. Added summary.c/h/main, public summarize normalization and request controls, model/executable verification and receipts, descriptor-bound native bridge, partial/uncertain provenance with null consumed-token count, and shared guardian regular-file framed stdin/stdout separated from stderr. Guardian retains existing admission lease/group/parent-death ownership and cleans request.frame/result.frame. New summary tests cover pins, unknown overrides, malformed/empty/truncated/oversized replies, owner isolation, offline replay, changed-request conflict, cancellation/deadline, SIGKILL workspace cleanup and subsequent admission. macOS final public normal/SAN pass (see /private/tmp/samosa-summary-final-mac.log); shared guardian normal/SAN124 both platforms pass. Linux normal public361 passes; final SAN is still live handle73515 until polled. Actual macOS T5 public reference normal/SAN1049 each pass (handle14572 terminal0); separately packaged actual engine is not sanitizer-built. Actual normal/SAN JSON receipts /private/tmp/samosa-summary-real-{normal,sanitize}.json. Existing operations/admission/Whisper/vision normal regressions pass macOS/Linux; logs /private/tmp/samosa-summary-regression-{mac,linux}.log. Initial hard-linked model refused as designed; QA uses a private copy. Unbundled native runtime failed; packaged runtime eventually passed actual checks, with initial packaged startup failure retained as development evidence, not a quality claim. Linux source candidate /tmp/samosa-summary-inference-candidate required native rebuilds after a copied Mac build directory; final gates use Linux binaries. Actual Linux T5 model execution remains unqualified. Area4 remains ACTIVE: next wire automatic Sources enrichment/publication using this public transport, then public embeddings and representation/query integration. Areas1–3 stay closed,5–7 remain pending, and the full rebuild/C01–C14 remain open. No commits/push/legacy retirement.

Final summarizer checkpoint: Linux handle73515 terminal0; final public normal361/SAN365 and protocol gates pass. macOS handle6450 terminal0, final public normal360/SAN360; actual macOS model handle14572 terminal0 normal/SAN1049 each. All summarizer/regression handles are terminal; no live gates remain. Counters include native file-hash reads and timing-dependent polls, not feature totals. Next is Sources automatic enrichment/publication through the now-implemented public summary transport; public embedding and representation/query generation remain. Area4 and the full architecture goal stay active.

Sources summary continuation (2026-10-10): implemented summarize_evidence using a separately pinned summary_inference callback/process transport and existing owned literal captures/shared prepared-artifact cache. sources_literal_capture verifies snapshot/cache/selector identity without reopening origin or loading extractor. New native summary module validates exact public Inference input/model/recipe/partial-coverage DTO, stores owner/model/evidence-bound derived artifacts and preserves original selector/coverage. Scalar opening sample is1400 UTF-8 bytes and output128; recipe renamed samosa-native-summary-opening-chunk-v1 because monolith opening-sample-v3 actually uses larger batched input. This is a transport/capture prerequisite, NOT automatic enrichment or full monolith batching. Original native engine is reused unchanged. CLI private summary transport/pins wired and described; describe string split to keep strict ISO literal bounds. Fixed controlled evidence_id canonicalization as a string; empty valid snapshots now reach empty_evidence refusal, not false corrupt_snapshot. Source test fixture development failures (absolute registration path, tracked vs untracked comparison and missing evidence_id type) fixed before passing gates. Mac normal/SAN243 and control135/transcript351/public-process66/index712 pass; Linux normal/SAN243 pass, regression handle28849 still live until polled. Actual end-to-end Mac normal/SAN41 each pass via Source worker -> public Inference -> packaged native T5 checkpoint; native model engine itself not sanitizer-built. Real handle84644 terminal0. Model checkpoint /private/tmp/samosa-summary-real-model.gguf retains known pinned SHA256. Latest recipe/build/test Mac handle80955 still live; do not restart on an observation timeout. Next: reuse full monolith native_summary opening batching through public Inference, then durable automatic enrichment and existing Chutni publication for summary_short, followed by public embeddings and semantic representations/query generation. Area4 and all full-goal milestones remain open; no commits/push/legacy retirement.

Sources summary authoritative follow-up: latest scalar recipe is samosa-native-summary-opening-chunk-v1; full monolith batched opening-sample-v3 is NOT reproduced by this scalar primitive. Mac final normal/SAN243 both pass with that recipe, log /private/tmp/samosa-sources-summary-qualified-mac.log; handle80955 terminal0. Actual final Mac Sources->Inference->native T5 normal/SAN41 both pass; handle88591 terminal0. Shared regressions control135/transcript351/public process66/index712 pass BOTH platforms, logs /private/tmp/samosa-sources-summary-final-{mac,linux}.log; handles8414/28849 terminal0. Native CLI describe is valid JSON and advertises summarize_evidence/summary configuration. Linux final recipe normal243 passes; SAN recompilation/check is still live handle31403 until polled. Pending area4: full legacy opening batching through public Inference, durable automatic enrichment and Chutni summary_short publication, then embeddings/representations/query generation. Areas1–3 closed;4 active;5–7 and full C01–C14 open.

Final Sources scalar-summary gate: Linux handle31403 terminal0, normal243/SAN243 pass with latest opening-chunk-v1 recipe; log /private/tmp/samosa-sources-summary-qualified-linux.log. All Source summary, actual model and regression handles are terminal; no live gates remain. The full goal stays active. Next implement the inherited native batch/opening policy through public Inference and automatic Sources enrichment/publication; do not close area4 for the scalar cache API.

Opening summary continuation (2026-10-10): added public Inference summarize_opening with separate normalized operation identity, existing batch/memory framing C0000001, unchanged native tokenizer chunk partitioning, and ported native_summary_chunk_length/native_summarize_text_impl shrinking reducer (sentence-aware1400-byte chunks, up to4 rounds, UTF-8-safe1800-byte final cap). Recipe samosa-native-summary-opening-map-reduce-v1; provenance records map_output_bytes, reduction_rounds, chunk output budget, partitioning and output_truncated. Scalar DTO remains unchanged. Native failures refuse rather than publishing a digest from a failed reducer. Fixture tests cover same-request scalar/opening idempotency conflict, opening replay, native batch framing, shrink/no-progress/four-round cap; Mac478 normal/SAN478 and Linux478 normal/SAN483 pass, logs /private/tmp/samosa-opening-final-{mac,linux}.log. Inference operation832/admission79 regressions pass Mac, log /private/tmp/samosa-opening-regression-mac.log. Actual Mac varied long-input native model normal1286 passes with positive reduction rounds; SAN still live handle66727 until polled. Earlier repeated corpus mapped below1800 and could not prove real reduction; replaced by varied report corpus rather than weakening that assertion. Development fixture assertion/type-edit mistakes corrected before gates. Corrected scalar documentation: actual engine refuses >512-token prompts, it does not clip them; large scalar actual refusal is tested. Source scalar API remains qualified; next integrate this full public opening path into Sources durable automatic enrichment and existing Chutni summary_short publication, then embeddings/representations/query generation. Area4/full goal remain active and all full C01–C14 open. No commits/push/legacy retirement.

Final public opening-summary checkpoint: real-model handle66727 terminal0, actual Mac normal1286/SAN1286 pass with positive reduction rounds on large varied input, scalar overflow refusal and opening replay. Mac fixture handle92095 terminal0 (478/478), Linux37380 terminal0 (478/483), operation/admission1350 terminal0. All checks are terminal; no live gates remain. Counters include file-hash reads and timing polls, not feature percentages. Next is Sources automatic enrichment/publication using this public opening map/reduce path; scalar cache capture remains separate. Area4 and the full objective remain active.

Sources opening-summary/publication continuation (2026-10-10): public
summarize_evidence_opening uses the owned retained literal capture, up to60000
UTF-8 bytes and public Inference opening map/reduce. Canonical owner-bound caches
retain explicit map/reduction/truncation provenance, original input selector and
coverage, provider/engine/model pins and evidence/input digests. Added pure
capture validation for the isolated SDK worker. Public publish_summary requires
an existing opening capture and disables model calls; it reuses derived receipt
storage, stable SDK operations and public put_file_outputs summary_short, with
full capture parameters and byte/page selectors. No new queue framework added.
Actual SDK text publication/repeated receipt/offline cache/recovery after receipt
loss and origin deletion pass310 checks normal/SAN on both platforms in
/private/tmp/samosa-summary-publication-qualified-{mac,linux}.log. Standalone
summary checks282 each. Shared transcript queue regressions2270 Mac/2324 Linux
and visual publication129 each pass; counters include timing-dependent polls.
Actual Mac Sources->public Inference->inherited T5 scalar and opening integration
normal47/SAN47 pass, /private/tmp/samosa-summary-opening-real-source-final.log;
public adapters/workers are sanitized, native model engine is not. Real test
first used an older Inference executable and refused unavailable; corrected to
current samosa-inference-summary[-sanitize] build. The malformed SDK submission
found during integration was fixed before actual SDK gates. Source pure capture
test initially passed an operation-wrapped response, which correctly refused;
changed to the unwrapped cache artifact. Initial Ruby edit syntax failure and
overlapping make invocation were resolved before qualified rebuilds. PDF-OCR
selector dispatch subsequently corrected to page coordinates; final normal/SAN
recheck logs /private/tmp/samosa-summary-publication-final-{mac,linux}.log,
Mac86223 terminal0, Linux32760 still live until polled. These text publication
gates do not qualify automatic enrichment, all document/media model quality or
Linux actual T5 execution. Next: adapt retained index versions and existing shared
lease/retry/receipt machinery for automatic enrichment, then public embedding and
representation/query generation. Area4 active; areas1–3 closed,5–7 pending; all
full C01–C14 remain open. No commit/push or legacy retirement.

Final Sources opening/publication checkpoint: Linux32760 terminal0; final
normal310/SAN310 pass on both platforms after PDF-OCR coordinate dispatch fix.
Logs /private/tmp/samosa-summary-publication-final-{mac,linux}.log. All gates in
this continuation are terminal; no live checks remain. Automatic enrichment,
embedding/representation publication and text-query embedding integration remain
three substantial area4 workstreams, with their end-to-end qualification still
required. Full goal remains active.

Automatic retained index enrichment continuation (2026-10-10): implemented
index_enrich_step, index_enrich_queue_step and paginated index_enrichment_status.
Work selection reuses retained index_items/index_versions, latest generation for
new work and admitted history for retries. Identities bind owner/root scope/job/
ordinal/generation/full evidence digest/summary recipe/store. The existing index
lease serializes capture/publication; existing public opening summary and derived
SDK publisher/receipt machinery are reused. Durable pending/captured/completed/
empty phases retain captures before writes; retries are bounded to60s.4096 records
and8MiB retained summary bytes, with maximum DTO room checked before model use.
Retained capture validation checks source snapshot ownership/hash and selector
identity, including the existing compound PDF text/OCR identity. Public prepared
cache is bypassed for retained queue evidence and summaries, without changing
public scalar/opening cache behavior. History validates old captures against
captured model pins and SDK receipts against source/summary/publication identity.

Mac normal4174/SAN4202 and Linux normal4175/SAN4251 pass actual SDK/native fixture
queue gates. Logs /private/tmp/samosa-enrichment-qualified-{mac,linux}.log;
Mac45221 terminal0, Linux77333 terminal0. Counters include file-write checks and
timing-dependent polls, not feature percentages. Gates cover model-free publication,
SIGKILL after SDK commit before local receipt, recovery after origin deletion,
local receipt loss, cache quota independence, disabled model, watch/reindex new
generation, old/new model profiles, retained history, owner/deadline and corruption.
Shared summary282/control135/index712 pass both platforms; Mac control56632
terminal2 only because requested nonexistent test-index-native target after its
control pass; correct test-index27444 terminal0. Actual Mac Source index queue ->
public Inference -> packaged inherited T5 normal63/SAN63 pass, handle11990 terminal0,
/private/tmp/samosa-enrichment-real-mac.log. Native T5 engine itself is not sanitizer
built. Initial routing edit left status unused and failed compile, corrected before
passing gates. Initial reindex test configured256 public cache entries exceeding
existing128 bound; corrected fixture. Linux initially reused an old compiled test
whose mtime followed the copied corrected source; removed that build artifact and
rebuilt before qualification. No live gates remain.

This qualifies automatic text enrichment/reindex/publication and the actual Mac
model queue path, not all reader/OCR enrichment, Linux actual T5 execution or new
embeddings. Next finish bounded document/OCR enrichment and control/runtime gates,
then generate/publish embeddings and integrate text-query embeddings through public
Inference. EmbeddingGemma2 remains a user-proposed candidate, not a silently selected
model. Area4 active;1–3 closed,5–7 pending; full goal and all C01–C14 open. No commits,
push, deployment or legacy retirement.

## Retained HTML/DOCX enrichment qualification — 2026-10-10

Extended the native automatic-enrichment fixture through the existing HTML and
raw-deflate DOCX readers. Both documents index without model calls, then capture
and publish through the actual Chutni SDK in separate phases. Retained summaries
preserve reader provenance and extracted UTF-8 coordinates; script and deleted
text sentinels never enter model input. Existing quota, owner/model, reindex,
SDK commit/SIGKILL recovery and corruption checks remain in the same gate.

macOS normal4503 / ASan+UBSan4509 checks pass; session17357 terminal0.
Native ARM64 Linux normal4503 / ASan+UBSan4580 checks pass; session47432 terminal0.
Logs: /private/tmp/samosa-enrichment-documents-qualified-{mac,linux}.log.
Counts vary with recovery polling. Added the DOCX fixture header as an explicit
build dependency and formatted the expanded test. An initial dependency edit
also added the header to compiler inputs; corrected before passing gates.
Initial macOS invocation terminal2; Linux session39534 terminal2. No live gates remain.

This proves HTML/DOCX orchestration with fixture model output and actual SDK writes,
not model quality or PDF/OCR enrichment. Next qualify ordinary PDF, mixed PDF/OCR
and image OCR through retained enrichment, then control/runtime and embeddings.
Area4 remains active; full architecture goal remains open. No deployment or legacy
retirement occurred.

## PDF/OCR retained enrichment extension — 2026-10-10, gates in progress

Extended the same native enrichment fixture with optional explicit OCR artifact
configuration: ENRICHMENT_OCR_PPM, ENRICHMENT_TESSERACT, ENRICHMENT_LEPTONICA,
ENRICHMENT_TESSDATA, ENRICHMENT_PDFIUM and ENRICHMENT_NATIVE_HELPER. It generates
an ordinary text PDF, a mixed text/raster PDF and a sentinel PPM using existing
native fixtures/readers. Each must capture then publish through the actual SDK;
history must retain ordinary/compound reader provenance and extracted coordinates.
The final corruption/no-extra-model-call assertion accounts for all eight captures.

macOS normal passes4567 checks, session65961 terminal0, log
/private/tmp/samosa-enrichment-ocr-mac.log. Initial session8200 terminal2 because
Homebrew library symlinks are rejected by existing artifact fingerprint checks;
resolved library paths before the successful gate. No production relaxation.
macOS sanitizer session44690 and Linux normal/sanitizer session38982 are live at
this checkpoint, logs /private/tmp/samosa-enrichment-ocr-sanitize-mac.log and
/private/tmp/samosa-enrichment-ocr-linux.log. Resume these handles; do not restart
on observation timeout. Linux's selected native helper is normal in this initial
combined invocation; a sanitized helper run remains required for full worker
qualification. Area4 remains active, and the full goal remains unfulfilled.

Follow-up observation: macOS sanitizer4575 checks, session44690 terminal0.
Linux normal4568 checks passed; session38982 remains live compiling sanitizer.

## PDF/OCR gates completed; runtime integration started — 2026-10-10

Previous Linux combined session38982 is terminal0: normal4568 and sanitizer4640.
Separate sanitized native-worker session31560 is terminal0,4639 checks, log
/private/tmp/samosa-enrichment-ocr-worker-sanitize-linux.log. Together with Mac
normal4567 and sanitizer4575, this qualifies ordinary PDF, mixed text/raster PDF
and image-OCR retained enrichment/capture/publication using fixture model outputs
and actual native readers/Chutni SDK. Counts vary with recovery polling. It does
not establish actual model summary quality for those documents.

Audit found sources_queue_run advances reindex/literal publication/audio/visual
but omitted the new enrichment queue. Added one index_enrich_queue_step per cycle
when summary_inference is configured, sharing root/state pins, TERM cancellation,
60-second deadline, bounded processed_items response validation and dependency
retry handling. Background-runtime regression session8965 was started, log
/private/tmp/samosa-enrichment-runtime-mac.log. Dedicated actual SDK enrichment
runtime progression/TERM and both-platform sanitizer gates remain required before
claiming that integration qualified. No area4 or full-goal closure.
Follow-up: runtime regression session8965 terminal0. This is the existing reindex
runtime regression, not the dedicated enrichment progression acceptance gate.

## Dedicated enrichment runtime progression gate — 2026-10-10

Extended test_enrichment with a child running the existing sources_queue_run at
100ms pacing and a new pinned summary model profile. Parent observes a completed
retained capture/publication through the actual SDK without manual enrichment
steps, then sends TERM and requires clean bounded exit. Child-local transport
calls do not alter the parent's counter; existing corruption/no-extra-call and
prepared-cache independence assertions follow. Runtime uses its existing pinned
scope/state and cancellable deadline, not a separate scheduling framework.

Initial sessions98681(macOS)/79652(Linux) terminal2: fixture omitted watch.h's
runtime declaration; added header before final reruns. macOS normal4581 and Linux
normal4564 pass. Sanitizer gates remain live: session55914 macOS, session32932
Linux. Logs /private/tmp/samosa-enrichment-runtime-qualified-{mac,linux}.log.
Resume those exact sessions; do not infer completion from normal passes.
Further cancellation while model work is active, backoff/dependency failure and
runtime crash/restart enrichment gates remain open. Area4 and full goal active.
Follow-up: session55914 macOS terminal0; sanitizer gate passed (count in log).
Linux session32932 remains live; final sanitizer acceptance still pending.

## Active-model TERM and checkpoint recovery extension — 2026-10-10

Linux runtime sanitizer session32932 terminal2: native fixture's independent
SQLite observer hit lock contention at prepare. Added bounded5000ms SQLite busy
waiting in fixture database helpers, without changing production locking.
Orphaned fixture runtime PID16807 was observed then sent TERM. macOS repeated
baseline session87187 terminal0; an attempted Ruby insertion failed before writing
new cancellation code, so that run is not cancellation evidence.

Added blocking fixture transport that marks entry and observes propagated
cancellation. Runtime TERM must exit cleanly, preserve pending/cancelled checkpoint,
then normal transport captures/publishes it after clearing only test retry time.
No original evidence or identity is replaced. Existing runtime progression, SDK
recovery, corruption and cache independence remain in the same gate.
New qualification sessions82980(macOS) and62215(Linux) live, logs
/private/tmp/samosa-enrichment-cancel-{mac,linux}.log. Resume these exact handles;
results are not yet acceptance evidence. Full goal and area4 remain active.

## Active-model cancellation results — 2026-10-10

macOS session82980 terminal0: normal4604 and ASan/UBSan4671 checks. Linux
normal4611 passed; session62215 remains live compiling sanitizer at this checkpoint.
Logs /private/tmp/samosa-enrichment-cancel-{mac,linux}.log. Tests prove propagated
TERM cancellation inside model transport, clean runtime exit, pending cancelled
checkpoint and retained capture/publication retry with the actual SDK. They use
fixture model transport, not actual native T5 termination. Linux observer's5000ms
busy waiting is test-only. Reader qualification remains the prior separate matrix.
Remaining runtime failure/restart gates and public embedding generation/publication
are still required. Area4/full goal active; no commits/push/legacy retirement.
Follow-up: Linux session62215 terminal0, final sanitizer passed (count in log).
Both-platform normal/sanitizer cancellation/checkpoint gates complete; no live
gates remain. Runtime restart/dependency-failure and embeddings remain next.

## Runtime active-model SIGKILL/restart extension — 2026-10-10

Extended the same fixture to reserve a fresh pinned model profile, observe entry
into blocking model transport, SIGKILL the runtime and verify the retained pending
record has no fabricated capture. Restarts the actual shared runtime without
clearing retry time or manually stepping enrichment; waits for completed actual
SDK publication, then requires clean TERM exit. Parent model-call counter remains
unchanged because runtime work occurs in children. Existing TERM/cancellation,
SDK-commit recovery, corruption and quota tests remain.

macOS normal4669 passes. Current sessions8441(macOS normal/SAN combined) and
26550(Linux normal/SAN combined) remain live at this checkpoint, logs
/private/tmp/samosa-enrichment-restart-{mac,linux}.log. Resume exact handles.
This is fixture transport with real queue/SDK, not actual native-model death.

Reuse audit: vendor/chutni/src/chutni.c implements chutni_search_semantic and its
JSON operation; Sources semantic.c already supplies the fixed-profile prepared
vector adapter. No public embedding/pooling implementation was found in current
Inference src/contracts. Preserve the existing SDK/Source retrieval machinery;
add the missing generation/representation boundary instead of replacing search.
Model candidate remains unselected. Area4 and full architecture goal active.
Follow-up: macOS session8441 terminal0; sanitizer passed (count in log).
Linux session26550 remains live, acceptance not yet established.

## Runtime restart completed; dependency retry gate — 2026-10-10

Linux restart session26550 terminal0: normal4675 and sanitizer5738 checks.
Mac normal4669/SAN4754, session8441 terminal0. Logs
/private/tmp/samosa-enrichment-restart-{mac,linux}.log. Runtime SIGKILL during
fixture-model work and restart without manually resetting retry time now qualify
through real SDK completion on both platforms normal/SAN.

Added missing-transport runtime fixture: new model profile with call=NULL must
retain pending/inference_unavailable, positive failures/retry time, remain alive
across further paced cycles, terminate cleanly on TERM and make no parent model
calls. Existing recovery/corruption/quota checks remain. Current qualification
sessions82806(macOS) and82627(Linux) live, logs
/private/tmp/samosa-enrichment-dependency-{mac,linux}.log. Poll exact handles.
No full-goal/area4 closure; public embeddings/representations remain required.

## Dependency gates and embedding boundary audit — 2026-10-10

Mac dependency session82806 terminal0: normal4694/SAN4768. Linux normal4700
passes; session82627 remains live compiling sanitizer. Logs
/private/tmp/samosa-enrichment-dependency-{mac,linux}.log. Existing runtime records
missing transport as pending/inference_unavailable with retry, stays alive across
paced cycles and exits cleanly on TERM without model calls.

SDK reuse audit located jcall_op_put_representation in vendor/chutni/src/chutni.c,
which binds a vector to artifact payload hash through chutni_representation_put.
Added proposed embedding-v1 contract under Inference, explicitly unimplemented:
pinned generation/roles/profile identity, parent validation, existing admission
and receipts, exact Sources chunk selectors and SDK representation recovery.
This records the integration destination; it does not establish embedding support.
Full goal/area4 remain active. Next finish live Linux gate, implement the public
embedding execution adapter and Source publication/query linkage using these
existing primitives. No model selection/legacy retirement/deployment occurred.
Follow-up: Linux session82627 terminal0; sanitizer passed (count in log).
Both-platform dependency runtime gates complete, no live gates remain.

## Native embedding transport started — 2026-10-10

Added internal llama_embed to the existing loopback provider adapter. Reuses owned
status/alias/context checks, bounded HTTP, cancellation and deadlines. Tokenizes
already-prefixed text directly (no chat template), refuses over-context input,
then submits exact token IDs to /v1/embeddings. Validates single index0 vector,
configured1–1024 dimensions, f32 finite values, L2 norm and exact prompt-token
usage before returning. On failure vector is freed and never returned.

Primary protocol reference checked:
https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/README.md
Initial strict compile refused signed dimension comparison; corrected cast.
Existing native-provider regression session71179 terminal0. Extended its native
HTTP fixture with embedding reply/token-input validation, successful2d capture
and mismatched3d refusal. Extended regression session73792 started, log
/private/tmp/samosa-embedding-transport-regression-mac.log; compile log
/private/tmp/samosa-embedding-transport-build-mac.log. This internal transport is
not yet public embed_text, configured profile/pin generation, durable replay,
Sources linkage, actual model quality or Linux/sanitizer embedding qualification.
Those remain implementation work; full goal/area4 active. No model selected.
Initial extended session73792 terminal1: fixture tokenizer only admitted chat
RENDERED prefix/parse_special=true. Added separate explicit task-query prefix/
parse_special=false fixture branch, preserving existing generation assertions.
Rerun session13024; observe its terminal status before claiming acceptance.

## Embedding transport numerical/refusal qualification — 2026-10-10

Confirmed prior extended regression session13024 terminal0,376 checks. Added
thread-scoped C numeric locale for f32 conversion/restoration and formatted native
adapter. Mac session56333 and Linux session20891 terminal0, normal376 checks.
Linux candidate /tmp/samosa-embedding-inference-candidate is freshly native-built
from copied src/tests/Makefile, with no Mac build artifacts.

Expanded controlled HTTP fixture refuses zero norm, f32 overflow and mismatched
consumed-token count, retaining dimension mismatch and direct prefixed token IDs.
Mac normal391 and Linux normal391 checks pass. Final normal/SAN sessions42505
(macOS) and53605(Linux) live at checkpoint; logs
/private/tmp/samosa-embedding-transport-final-{mac,linux}.log. Resume exact handles.
This qualifies low-level fixture transport only; public embed_text/profile binding,
pinned model/runtime generation, operation replay and Source representation/query
linkage remain unimplemented. No model-quality/actual-embedding claim. Area4 and
full architecture goal remain active; no legacy production switch.
Follow-up: sessions42505 and53605 terminal0; normal391 and sanitizer391 on each
platform. No live gates remain. Next expose public configured embedding operation
through existing admission/operations, then Sources/SDK representation linkage.

## Public configured embedding dispatch started — 2026-10-10

Added embedding.c/h and optional EmbeddingConfig to Inference. Host-only settings
supply model/revision, model/runtime/tokenizer SHA256, dimensions, task prefixes
and recipe. Public embed_text accepts text, query/document role, optional document
title and operation controls; unknown overrides refuse. Existing inference_schedule,
budgets and operations handle admission/cancellation/deadlines/owner receipts.
Canonical request binds profile, recipe/prefixes, context and configured digests.
SDK model_revision is computed from model/runtime/tokenizer/recipe/prefixes/output
settings so representations with different preprocessing cannot share a profile.
Document formatting preserves title; vector response binds input digest/role,
profile and declared provenance, with thread-scoped numeric locale restored.

Public controlled-HTTP fixture captures a query, replays with endpoint absent,
and refuses same operation ID after recipe change. Initial session85323 terminal1
expected wrong error label; existing operation machinery uses idempotency_conflict,
corrected assertion. Normal406 checks pass on both platforms. Final sessions43311
macOS and70008 Linux remain live at checkpoint for SAN, logs
/private/tmp/samosa-embedding-public-{mac,linux}.log. Default non-native build
session15980 started, log /private/tmp/samosa-embedding-default-build.log.

This is public C configuration/dispatch, not CLI setup or verified runtime/model
artifact binding. Configured digest declarations do not prove actual model identity
on an unqualified external endpoint. Owned embedding runtime, CLI configuration,
actual-model/pooling checks, document/malformed/owner/control gates and Source SDK
representation/query publication remain required. Candidate not selected. Full
goal/area4 active; no production switch, commit or deployment.
Follow-up: sessions43311/70008 terminal0, normal406/SAN406 each platform;
default build15980 terminal0. No live gates remain. Public fixture checks establish
query orchestration/replay only, not complete embedding-provider qualification.

## Public embedding document/request identity gates — 2026-10-10

Replaced substring field allowlisting in embedding request parsing with explicit
field names and duplicate-bit tracking; crafted op|text key now refuses. Bound
provider model_alias into normalized receipt identity. Responses now retain
actual optional title and prepared-input bytes/SHA256, in addition to text digest,
role/profile/provenance, so document prefixes/title formatting are auditable.

Expanded HTTP fixture admits separately asserted document formatting and tests
titled public document embedding, query title refusal, crafted unknown key
refusal and owner-isolated embedding operation_get. Existing query capture/replay,
recipe idempotency conflict, vector/token validation and provider controls remain.
macOS session9382 and Linux session9718 terminal0: normal426 and sanitizer426
checks each. Logs /private/tmp/samosa-embedding-public-extended-{mac,linux}.log.
No live gates remain. These fixtures still do not qualify actual owned runtime/
model identity, CLI setup, actual model vectors, Source publication or query
integration. Continue those requirements; area4/full goal active.

## Host embedding CLI configuration added — 2026-10-10

Added --embedding-config-file to the existing native-provider CLI with matching
public loader/free functions. Requires configured endpoint/admission and absolute
owner-private regular config file, size1–8192, no symlink/hardlink, complete strict
nine-field schema, valid configured digests/dimensions/prefixes. Existing native
endpoint/model-alias/context/owner/admission flags retain their validation.
Configuration strings are copied and released after request dispatch.

Mac native CLI/test build session24564 terminal0. Extended fixture covers valid
config and world-readable, symlink and hardlink refusal. Current normal/SAN gates
sessions33397(macOS) and55419(Linux) live, logs
/private/tmp/samosa-embedding-cli-qualified-{mac,linux}.log; Mac normal439 passes.
No actual CLI embed request or owned-runtime/model pin verification is claimed.
Config-file stability currently compares device/inode/size and second-resolution
mtime/ctime; tighten native nanosecond stability before qualifying mutation races.
CLI allows only the existing loopback transport, still unqualified until owned
runtime binding/actual-model tests. Source representation/query linkage remains.
Area4/full goal active; no deployment/model default selected.
Follow-up: sessions33397/55419 terminal0, normal439/SAN439 each platform.
Linux native CLI build also passes. No live gates remain. Loader/dispatch fixture
qualification does not establish actual CLI model execution or file mutation races.

## Embedding CLI execution and config stability — 2026-10-10

Tightened config loader stability to compare device/inode/size/mode/owner/link
count plus native nanosecond mtime/ctime against both opened and named files.
Removed checking a freed key pointer in schema parsing. Existing private file,
symlink/hardlink and strict schema validation remain. No injected mutation-race
fixture has yet established every race boundary.

Extended native fixture with TEST_EMBEDDING_CLI: spawns the actual CLI using a
private request/config, validates vector/profile, then repeats the same operation
against unreachable loopback port1 and requires byte-identical durable replay.
Normal fixture uses normal CLI; sanitizer fixture uses sanitizer CLI.
Initial Ruby patch had syntax error before any edits; sessions65366/70519 were
baseline-only terminal0 and do not prove new CLI execution. Corrected patch before
current sessions89851(macOS) and17454(Linux), live at checkpoint. Logs
/private/tmp/samosa-embedding-cli-execution-{mac,linux}.log. Resume these handles.
These are controlled HTTP vectors; actual owned model/runtime binding and model
quality remain required, followed by Sources representation/query integration.
Area4/full goal active, no model default selected or deployment.
Follow-up: macOS session89851 terminal0 normal459/SAN459. Linux normal459 passes;
session17454 remains live for sanitizer at this observation. No actual model claim.
Final follow-up: Linux session17454 terminal0 normal459/SAN459. Both-platform
actual CLI controlled-HTTP generation/offline receipt replay passes. No live gates.

## Owned actual embedding model execution — 2026-10-10

Added optional managed profile llama-embedding-static-system-v1 with private
embedding_config_file, model/runtime pin equality, explicit mean pooling,
context-sized batch/ubatch, authenticated owned readiness vector, and existing
activation/admission/operation/guardian lifecycle. Added --single public request
mode for the existing Sources isolated process transport; stream remains default.
Replaced managed schema substring matching with exact fields and duplicate bits.
Separate managed sanitizer build target now tracks source/header dependencies.

Candidate remains an evaluation choice, not a production default. Model repository
revision bfcd298762cc34d0357ece5ebdd31791a3a374d8, Q8_0 text GGUF309855456 bytes,
SHA2562188ac1deca4b77dffefd603c2776a9d76d9d74ec01841392982ebb840b09135.
Runtime source commit4fbc76dec51d0add466f0210855c0596589b60d4; independently built
native CPU-only static provider libraries, no backend plugins, OpenSSL or RPATH.
Mac binary SHA256958601dbd22ab2f5857e09860247f30c74153cb0f3914b8d62f2bacbba9aab97;
Linux SHA2569fca39e8db7c859a0c427599350c37f0a615d5caa6ef7be458d1145a2aad2ca4.
Direct loader evidence: macOS libSystem/libc++ only; Linux libc/libm/libstdc++/
libgcc_s only. Exact evaluation recipe is tests/embeddinggemma2-candidate.json.
No modifications to upstream runtime source; isolated /private/tmp source git
status is clean. Linux independent source copy and compilation use Debian12 ARM64.

Actual public owned Inference gates pass on both platforms with normal and
ASan/UBSan adapters. Each requires actual model vectors768, finite f32/L2 norm,
input/role/title/provenance/profile binding, four intended paraphrase rankings
against unrelated documents, context refusal instead of truncation, durable
same-operation replay, offline replay against unreachable port1 after runtime
shutdown, and --single/stream exact vector/profile parity. Mac positive-minus-next
margins0.0384/0.2140/0.1233/0.1510; Linux0.0368/0.2143/0.1227/0.1516.
This is a small actual-model text retrieval corpus, not a broad retrieval-quality
acceptance or Sources/Chutni end-to-end claim. External llama.cpp runtime itself
is not sanitizer-built; owned C adapters and their executed process binaries are.

Final actual receipts/rankings/refusal/replay/single responses:
/private/tmp/samosa-embedding-real/mac-normal-r3,
/private/tmp/samosa-embedding-real/mac-sanitize-r2,
/private/tmp/samosa-embedding-real/linux-normal-r2,
/private/tmp/samosa-embedding-real/linux-sanitize.
Linux originals /tmp/samosa-embedding-real-linux-{normal-r2,sanitize} in container
samosa-s17a-linux-qa; final actual sessions4718/70161 terminal0. Mac final
sessions72651/12483 terminal0. Linux actual logs
/private/tmp/samosa-embedding-real-linux-{normal-r2,sanitize}.log.

Native transport normal439/SAN439 and managed CLI normal41/SAN41 each platform
pass. Final managed mac session91577 and Linux10839 terminal0; Linux final log
/private/tmp/samosa-embedding-owned-linux-final.log. Broader native regressions
sessions55001(mac)/47045(Linux) terminal0, logs
/private/tmp/samosa-embedding-owned-{mac,linux}.log. Runtime build/rebuild handles
38169/66201(mac),16131/87194(Linux) terminal0. No live gates remain.

Development refusals preserved: initial Mac runtime linked default OpenSSL,
correctly refused unqualified_engine_dependencies; rebuilt LLAMA_OPENSSL=OFF and
CMAKE_SKIP_RPATH=ON. Initial Linux copied model retained UID501 whereas QA owner
is root; unsafe_activation_input correctly refused, fixed QA model ownership.
Initial harness masked startup error with shutdown assertion; fixed to retain the
original failure. No earlier failed gate is counted as acceptance.

User reaffirmed no shipped Python runtime. Actual inference is native C/C++.
The Python3.11+ real-model script is an opt-in developer/CI harness only, never
called by installed executables. Dependency docs and plan record this constraint.

Area4/full goal remain active. Next implement Sources retained-evidence embeddings,
automatic Chutni representation publication/readback/recovery and natural-language
query generation using this owned public transport. Require exact artifact/profile
binding, changed-artifact refusal, SDK commit-before-ack/SIGKILL recovery, owner/
cancellation/deadline controls and actual-model public Sources/SDK rankings on
both platforms before closing area4. Areas1–3 remain closed,5–7 remain open.
No commit, push, default-model switch or production deployment.

## Native Sources text-query checkpoint — 2026-10-10

Sources `memory_search_text` now calls owned public Inference `embed_text`,
independently binds model/runtime/provider/profile/recipe and original/prepared
input, validates finite normalized f32 output, and delegates ranking to the
existing isolated Chutni SDK worker. Retained metadata includes query and model
receipt. Operation identity binds owner, host embedding configuration and store;
exact offline replay requires the binding but not the callable runtime or SDK.
Public model/profile overrides and unknown/duplicate/crafted field names refuse.
The path and real-model QA executable are native C/C++; no shipped Python runtime.

Fixture orchestration gates pass359 checks each in normal and ASan/UBSan on
macOS/Linux. Semantic regression passes239 each (boundary checks58 macOS,50 Linux).
Control135 and durable operations750 pass on both platforms. Logs:
/private/tmp/samosa-text-query-final-fixture-mac.log,
/private/tmp/samosa-text-query-fixture-linux-r2.log,
/private/tmp/samosa-text-query-regression-{mac,linux}.log.

The native actual-model test reads verified document vectors from the preceding
Inference gate and seeds them through the public SDK. Four public natural-language
queries obtain actual owned model vectors and retrieve the expected SDK artifact;
an additional query exercises the Sources CLI host configuration/transport.
It checks retained provenance and exact Sources offline replay. Mac normal and
ASan/UBSan each pass303 checks, with receipts:
/private/tmp/samosa-sources-embedding-real-mac-normal-r4,
/private/tmp/samosa-sources-embedding-real-mac-sanitize.
Linux normal and sanitizer each pass303 (sessions26143/52906 terminal0).
Receipts copied to /private/tmp/samosa-sources-embedding-real-linux-{normal,sanitize};
originals are /tmp/samosa-sources-embedding-real-linux-{normal,sanitize} in
container samosa-s17a-linux-qa. Logs use the same host paths with .log suffix.
The pinned external llama.cpp runtime remains uninstrumented in sanitizer gates.

This is prepared document publication, not automatic Sources vector publication
or broad retrieval-quality acceptance. Area4 remains active. Next durable capture
and automatic publication must bind the exact pre-embedding artifact payload and
recover SDK commit-before-ack without duplicate representations or another model
call. The existing SDK put_representation binds the payload at write time but has
no expected-payload input or retry deduplication; resolve that public-SDK gap before
claiming changed-artifact or SIGKILL publication acceptance. No private SDK SQL or
alternate vector store/ranker. Areas1–3 closed,5–7 open; no completion percentage.

## Payload-checked SDK vector publication — 2026-10-10

The public JSON SDK now exposes `get_artifact.payload_hash` and the distinct
`put_representation_bound` operation. It requires the expected artifact payload
hash and validates finite f32 values, dimensions and nonzero/L2 norm. A write
transaction compares the payload and recovers an identical artifact/payload/
profile/encoded-vector representation before inserting. Recovery verifies the
content-addressed vector object. Older SDKs refuse this operation; consumers must
not fall back to the legacy unchecked write. Legacy behavior remains available.
No Sources access to private SDK SQL or additional vector store was introduced.

Native SDK call-surface tests cover payload readback, mismatched payload refusal,
exact retries, reopening, distinct model profiles, required preconditions,
invalid numeric/dimension/norm inputs, and an actual publisher SIGKILL after SDK
commit before any representation ID or receipt reaches its parent. The fresh
retry recovers and exact SDK counts prove no duplicate row. The final macOS
normal and ASan/UBSan call-surface runs pass65 checks. Final macOS full `make test`
also passes (conformance85 with one existing gap, CLI32, MCP130, compatibility7).
Existing Sources embedding/search regression passes359 against the updated SDK
on both platforms.
Logs /private/tmp/samosa-bound-representation-mac-final-normal.log,
/private/tmp/samosa-bound-representation-final-{call-mac-r3,sanitize-mac}.log,
/private/tmp/samosa-bound-sdk-source-regression-mac.log.

Linux fresh BUILD=build-bound-linux normal `make test` passes the same counts.
GCC QA uses `CC='cc -D_DEFAULT_SOURCE'` for existing POSIX helpers, and retains
existing conformance-test format-truncation warnings with
`-Wno-error=format-truncation`. The production path_join now uses checked bounded
copies and the implementation declares X/Open700 for realpath. Initial default
build attempted copied macOS objects; it was discarded for a fresh build path.
Linux final ASan/UBSan call-surface passes65, including post-commit publisher
SIGKILL and retry counts. Its binary timestamp is newer than the final source
and test files. Linux full sanitizer verification passes (session20354 terminal0):
conformance85 with one existing gap, call-surface65, CLI32 and MCP130. Mac broader
sanitizer session65203 also terminal0 (conformance85, earlier call-surface57,
CLI32 and MCP130), supplemented by the final65-check native call-surface gate.
Logs /private/tmp/samosa-bound-representation-linux-final-normal.log and
/private/tmp/samosa-bound-representation-linux-r6.log. Earlier failed build logs
are not acceptance evidence.
Additional final focused logs:
/private/tmp/samosa-bound-representation-linux-final-sanitize-call.log and
/private/tmp/samosa-bound-sdk-source-regression-linux.log.

Area4 remains active. This resolves the public-SDK prerequisite; Sources must
still persist document embedding captures, publish automatically through this
operation and qualify its own controls/chunks/model/publication crash phases.
There is no shipped Python runtime, default model switch, commit or deployment.

## Native document vector generation/publication — 2026-10-10

Sources adds a separate private native Chutni worker mode for document bindings
and vector publication. The parent pins library/store/UUID and requires an
explicit write grant and exact host semantic profile for publication. The worker
uses only the four public SDK ABI functions. It verifies the selected artifact's
complete text through source_context and obtains its payload_hash through
get_artifact before generation. Publication rechecks text and saved payload
binding and calls only put_representation_bound. Responses bind store/artifact,
text SHA-256/bytes, payload hash, exact canonical profile/vector arguments and
representation ID/recovery flag. Existing deadlines, cancellation, supervision
and parent-death cleanup apply. No private SDK SQL or Python runtime.

Sources document capture validation now reuses the full generation validator
without calling Inference: input and prepared digests/bytes, role/title, operation
identity, provider/model/runtime/recipe, tokens, complete profile and finite
normalized f32 vector all remain checked. Changed text/title/recipe refuses.

Final fixture gates pass402 each normal/ASan/UBSan on macOS26.5.1 arm64 and
Debian12.15 aarch64 in samosa-s17a-linux-qa. They include native complete-text
binding, payload mismatch refusal, write grant/profile refusal, stable SDK retry,
malformed receipt refusal, controls and saved capture validation without a model.
Logs /private/tmp/samosa-native-vector-worker-{mac,linux}-final.log.

The native actual-model gate no longer seeds document vectors. It seeds only
four document texts, verifies their SDK bindings, generates fresh owned public
Inference document embeddings and publishes through the Sources native worker.
Each SDK retry returns the same representation and does not call Inference;
saved-capture validation succeeds with the callable removed. Four natural-language
queries retrieve the expected artifacts, followed by Sources CLI query and exact
offline replay. Each platform normal/SAN passes451 checks. This is a small
four-topic actual-model corpus gate, not broad retrieval-quality or release
acceptance. The external pinned llama.cpp runtime remains uninstrumented in SAN.
Final mac session59921 and Linux sessions84231/24989 terminal0. Artifacts/logs:
/private/tmp/samosa-sources-vector-real-mac-normal-r2,
/private/tmp/samosa-sources-vector-real-mac-sanitize,
/private/tmp/samosa-sources-vector-real-linux-normal,
/private/tmp/samosa-sources-vector-real-linux-sanitize (each with .log transcript).
Linux originals /tmp/samosa-sources-vector-real-linux-{normal,sanitize} in the
same container, copied to the host paths above. Earlier mac normal gate451 also
passed but the r2 run includes the final parent profile check.

Semantic regressions239 normal/SAN and boundaries58 mac/50 Linux pass. Public
Inference process transport normal66 on both platforms, SAN76 mac/65 Linux pass
(platform/runtime-dependent requester-death probes). Logs
/private/tmp/samosa-native-vector-semantic-regression-mac.log,
/private/tmp/samosa-native-vector-inference-regression-mac.log,
/private/tmp/samosa-native-vector-regression-linux.log. Build/fixture sessions
75077(mac)/22616(Linux), regression33868/14751(mac)/55452(Linux) terminal0.

Area4 remains active. Next discover owned index generations and model profiles,
persist immutable document capture before SDK publication, then automatically
publish/recover through these helpers. Require Source phase SIGKILL and controls,
reindex/watch retained history, bounds/chunk or explicit refusal coverage and
broader actual-model acceptance before area4 closure. Areas1–3 closed,5–7 open.
No default-model switch, commit, push or deployment.

## Automatic retained-generation embedding queue — 2026-10-10

Sources now implements index_embed_step, index_embed_queue_step and bounded,
owner/root-scoped index_embedding_status. The existing native queue runtime
automatically runs embedding after literal SDK publication when the host grants
publication and configures an embedding provider. New captures use the latest
indexed generation; admitted earlier-generation captures remain recoverable.
The durable phases are pending, captured and published. Complete validated model
receipts and SDK text/payload bindings commit locally before vector publication.
Publication uses the saved capture with the model callable removed, rechecks the
artifact payload, and uses only public put_representation_bound. Reindex retains
both generations. Binding identities include the owned model/runtime/profile and
host preprocessing configuration. Failures keep captures and bounded retry
state; admission reserves storage before invoking the model, with 4096 rows and
8 MiB of capture/receipt storage. Whole-document text over 60000 bytes is refused
explicitly rather than silently truncated. The shared index lease, pinned scope,
deadline/cancellation and native worker supervision continue to apply.

The native fixture adds actual SDK automatic publication, operation replay with
no callable, owner isolation, reindex history, SIGKILL after local capture and
after SDK commit before local receipt, plus unattended queue execution. Final
macOS normal/SAN gates pass559 each. Initial 3-second runtime fixture pacing was
insufficient under concurrent Linux sanitizer compilation; the final fixture
uses a bounded 20-second window, with the same publication assertions. The
callback no longer counts timing polls as assertions. Logs:
/private/tmp/samosa-index-embedding-mac-recovery-r3.log;
Linux final /tmp/samosa-index-embedding-linux-recovery-r2.log in
samosa-s17a-linux-qa (final terminal status recorded below).

The actual-model gate now starts with four source files and an empty SDK store.
It indexes and publishes literal evidence, discovers due generations, generates
fresh vectors through owned native Inference, persists captures, then publishes
with the callable removed. All four paraphrase queries rank the expected artifact
first; CLI query and exact offline replay also pass. macOS normal/SAN and Linux
normal pass454 each. The old executable's printed footer still says scheduler
pending; this checkpoint supersedes that label. The native C QA host updates the
managed activation revision between process calls. This proves the hosted queue
path; it does not qualify an unchanging CLI managed configuration for repeated
activations. The external pinned llama.cpp runtime and the loaded SDK in this
gate are not sanitizer-built; their separate qualifications are recorded above.

Artifacts/logs:
/private/tmp/samosa-sources-index-embedding-real-mac-normal;
/private/tmp/samosa-sources-index-embedding-real-mac-sanitize-r2;
Linux /tmp/samosa-sources-index-embedding-real-linux-normal and
/tmp/samosa-sources-index-embedding-real-linux-sanitize in the same container.
Each has a .log transcript. Final Linux sanitizer status recorded below.

macOS semantic regressions239 and boundary58 pass normal/SAN. Existing CLI queue
runtime normal/SAN passes duplicate refusal, missing/healthy progress, TERM/INT,
SIGKILL/restart, root/state/lease replacement and retained evidence. Log:
/private/tmp/samosa-index-embedding-mac-regression.log. Linux equivalent log:
/tmp/samosa-index-embedding-linux-regression.log (final status below).

Area4 remains active. Before closure, finish broader actual-model retrieval
acceptance, automatic active-model controls/watch integration, capacity and
archived ledger cross-binding audits, and repeated hosted/CLI configuration
lifecycle qualification. The small four-topic corpus is not broad retrieval or
release acceptance. Areas1–3 remain closed;5–7 remain open. No completion
percentage, shipped Python runtime, default-model switch, commit or deployment.

Final terminal results for this checkpoint: fixture559 normal/SAN on both
platforms (mac session53374 and Linux68728 terminal0); actual-model454 normal/SAN
on both platforms (mac normal9791/SAN82984, Linux normal83295/SAN92520 terminal0).
Semantic239 and bounds58 mac/50 Linux pass normal/SAN; CLI queue runtime passes
normal/SAN (mac71268/Linux43045 terminal0). Linux logs and both actual-model
artifact directories have been copied to /private/tmp on the host, retaining
their filenames. The printed pending-scheduler footer is corrected in source for
future runs; the recorded454 transcripts retain their original footer.

## Embedding ledger, fixed-config lifecycle and actual background corpus — 2026-10-11

Sources now validates every retained embedding row against its owned index job,
root scope, generation, qualified literal evidence and recomputed row identity.
Archived captures validate the prepared/input digests, operation binding, role,
provider/provenance structure, token/byte bounds, finite normalized saved vector
and exact SDK publication arguments. This requires neither a current model
configuration nor SDK access. Rehashed artifact/text/vector mismatches refuse.
Pending and captured reservations are included in the8MiB integrity accounting;
new admission reserves space before model calls. Empty/oversized document inputs
are explicit embedding-input refusals rather than index-corruption claims.

The managed native dependency adds explicit host-only --single-current. It reads
and advances the committed activation revision under the existing residency
lease, so one unchanged private pinned config works for repeated process calls.
Strict --single/stream revision checking remains default. Checksum/readiness,
environment/loader, input-change, residency exclusion and rollback requirements
remain in force; public requests cannot enable the mode. Activation normal/SAN
gates pass118 mac/136 Linux, including concurrent residency refusal, repeated
advancement, bad-model checksum refusal and preservation of strict stale checks.
Mac private managed CLI refusal gate41 passes. Logs:
/private/tmp/samosa-managed-current-mac.log,
/private/tmp/samosa-managed-current-mac-cli.log;
Linux /tmp/samosa-managed-current-linux.log.

Final Source fixture645 normal/SAN passes on both platforms. It includes old/new
reindex generations, SDK/local phase SIGKILL recovery, offline publication,
unattended queue execution, rehashed archived corruption refusal and retirement
of the active model configuration. Added profile migration admits only the latest
generation, publishes without a callable and preserves all earlier profile rows.
Logs /private/tmp/samosa-embedding-profile-mac.log and Linux
/tmp/samosa-embedding-profile-linux.log. These are fixture/SDK proofs, not model
quality measurements.

The actual-model gate now indexes12 source files into an empty SDK store and
runs sources_queue_run until all12 representations are durably published. A
readonly check of Sources' own QA ledger supplies the bounded stop condition;
there is no private Chutni SQL. Exactly12 document callbacks prove publication
does not regenerate embeddings. It then checks12 predeclared paraphrases against
related distractors (backup scheduling, bread storage, eclipse, two-factor login,
rice, Mars, file deletion and email filtering), CLI query and exact offline replay.
All12 queries rank their intended document first. The managed config is written
once before the first callback, with expected_revision0 throughout; subsequent
calls use --single-current rather than QA rewrites. Actual model weights/runtime
remain the previously pinned EmbeddingGemma2 Q8_0/native CPU candidate.

Mac normal/SAN and Linux normal pass468 each. Linux's first SAN execution was
interrupted by the QA container's sleep86400 PID1 exiting normally (exit0,
OOMKilled=false); its exec ended137. Its log/data are retained at
/private/tmp/samosa-embedding-broader-linux-sanitize-interrupted{,.log}. The
container was restarted only after authoritative terminal inspection. Fresh SAN
gate /tmp/samosa-embedding-broader-linux-sanitize-r2 is live session75092; final
terminal result will be recorded below. Original normal handles64145/11798 and
Mac SAN42533 are terminal0. External llama.cpp and the SDK loaded by the actual
model gate remain uninstrumented, with their separate qualifications above.

Actual artifacts and .log transcripts:
/private/tmp/samosa-embedding-broader-mac-{normal,sanitize};
Linux /tmp/samosa-embedding-broader-linux-normal and
/tmp/samosa-embedding-broader-linux-sanitize-r2.

Area4 remains active pending its finite closure audit. Do not substitute an
unspecified broader-quality backlog: this gate qualifies the declared12-query
corpus and native path. Before closure, explicitly check declared input/admission
bounds and active-model queue/watch controls against the existing contracts and
earlier generic control gates. Arbitrary retrieval accuracy, extra chunking and
unrelated model/provider features are not inferred requirements. Areas5–7 stay
open, and the full goal remains active. No shipped Python runtime, default-model
switch, commit or deployment.

Final corpus terminal result: Linux SAN r2 session75092 terminal0, actual468.
Together with Mac normal64145/SAN42533 and Linux normal11798 terminal0, the
12-document/12-query actual background gate passes normal/SAN on both platforms.
Final profile fixtures645 normal/SAN: Mac67688/Linux58362 terminal0. Both Linux
corpus artifact directories, .log transcripts, profile fixture and managed
activation logs are copied to /private/tmp on the host under the same names.
### Declared embedding input and admission bounds — 2026-10-11

The native embedding fixture gate now includes a 60,000-byte document consisting
entirely of quotation marks, exercising JSON expansion through literal SDK
publication, saved vector capture and publication without another model call.
A 60,001-byte document returns `embedding_input_limit` before model execution.
Fifty-nine oversized pending rows reserve 8,260,000 bytes; the next admission
returns `embedding_capacity` without calling the model. Retained status exposes
the explicit refusal. These checks extend the existing recovery and profile
migration suite; no production implementation changes were needed.

macOS and Linux normal and ASan/UBSan pass 1,807 checks each, all terminal zero. Logs:
`/private/tmp/samosa-embedding-bounds-mac.log`,
`/private/tmp/samosa-embedding-bounds-mac-sanitize.log`, and Linux
`/tmp/samosa-embedding-bounds-linux.log` (copied to `/private/tmp` on the host).
Actual-model retrieval remains supported by the separate 468-check
four-platform-mode gate, not by fixture vectors. No Python runtime ships.

## Area4 closure — 2026-10-11

Final native actual-model control gates are terminal zero:
macOS normal `/private/tmp/samosa-embedding-controls-mac-normal-r6.log` (1331),
macOS ASan/UBSan `/private/tmp/samosa-embedding-controls-mac-sanitize-r6.log` (1743),
Linux normal `/private/tmp/samosa-embedding-controls-linux-normal-r8.log` (2252),
Linux ASan/UBSan `/private/tmp/samosa-embedding-controls-linux-sanitize-r8.log`
(10121). Counters include listener/control polling; substantive assertions match.
The final Linux modes use separate ports/private state. Earlier test-construction
failures and overlapping/old-binary attempts are retained and excluded from
acceptance. macOS sanitizer exposed asynchronous guardian cleanup; the final
test requires the listener to disappear within ten seconds before continuing.
No production code changes were required for this gate.

`test_embedding_real --controls` proves a native deadline refusal, cancellation
after the actual owned runtime listener opens, pending-phase retention, bounded
runtime cleanup, healthy queue retry and publication. Editing the indexed source
then enables background discovery: refresh, reindex, fresh actual-model vector
capture and SDK publication complete without explicit reindex/publication calls.
Both generation0 and generation1 remain in retained status. Native runtime and
loaded SDK are external dependencies; the C adapters/harness are instrumented.

The finite area4 completion audit is satisfied by the following evidence:

| Required behavior | Acceptance evidence |
|---|---|
| Literal indexing, immutable versions, lexical SDK read/search | Existing native index/publication gates and area4 reader records; index712 regressions remain passing |
| Audio and visual discovery/reindex, derived history/publication | Final audio admission and visual discovery/retained-history checkpoints; `/private/tmp/samosa-index-visual-final-{mac,linux}.log` |
| Summaries and automatic enrichment through declared public Inference | Actual native T5 capture/provenance/replay plus both-platform SDK enrichment, reader, durable phase, active-model recovery and control gates; enrichment dependency/real logs |
| Verified embedding model/runtime and generated vectors | Owned pinned EmbeddingGemma2 Q8_0/llama.cpp CPU receipts and numerical/provenance qualification; no fixture substituted for this claim |
| Automatic vector publication and natural-language retrieval | Empty-store12-document/12-query gates,468 checks normal/SAN on both platforms; expected top-one results against near distractors, CLI query and offline replay |
| Recovery, owner/model/profile separation and archived bindings | Final embedding fixture1807 normal/SAN on both platforms, SDK bound publication/recovery gates and retained ledger corruption refusal |
| Declared input/admission/resource limits |60,000-byte escaped text succeeds,60,001-byte input refuses before model; pending reservation overflow refuses before admission/model |
| Active-model controls and automatic refresh/reindex | Final actual-model control gates above, plus existing native CLI queue TERM/INT/SIGKILL/root/state/lease checks |
| Fixed private host configuration and public dependency boundary | Explicit native `--single-current` and strict-default activation gates; Sources uses public process DTOs and public SDK operations, no sibling private state or shipped Python |

Area4 is closed within its declared component scope. Retrieval acceptance covers
the predeclared corpus, not arbitrary model accuracy. Temporal video and
full-duration speech remain area5; authenticated service/admission/shutdown is6;
independent final release acceptance and replaced-path retirement remain7.
Areas1–3 remain closed. No completion percentage, default-model switch, commit,
push or deployment; the complete goal remains active.

## Area5 actual multi-window speech checkpoint — 2026-10-11

The native `test_whisper_real` harness now supports
`SAMOSA_TEST_FULL_DURATION=1`. A66-second PCM16/16kHz/mono recording repeats the
previously qualified English "coriander/sample" utterance at2s,31s and61s.
The Sources queue owns three ranges:[0,30000),[29000,59000),[58000,66000).
Each fresh-process capture calls the installed native Whisper model; each
fresh-process publication consumes the saved capture without model transport.
All three recognize the expected speech and retain nonempty monotonic segment
coordinates inside the corresponding absolute source-time range. Completion
requires three captured and three published windows. Removing the original
then disables model transport/SDK worker: all three saved reads match exactly
and the completed job remains readable. Derived uncertainty remains explicit.

Normal and ASan/UBSan adapters/harness pass on both platforms:
`/private/tmp/samosa-whisper-full-mac-normal-r3.log` and
`/private/tmp/samosa-whisper-full-mac-sanitize.log` (2631 checks each);
`/private/tmp/samosa-whisper-full-linux-{normal,sanitize}.log` (2620 each,
copied from the QA container). All four processes are terminal zero. Installed
Whisper runtime and loaded Chutni SDK are not sanitizer-built in these runs.
No production speech implementation changes were required.

Fixture `/private/tmp/samosa-whisper-full-duration.wav`,2112078 bytes, SHA256
`b470f7e383f7b8262b96fefd435bc1b1b532fffe225bb62dde0d8471a70a7677`;
base utterance SHA256
`927b006bef03f0c5b2430f5ddfa20629ea8fcabf1f0b422403f5143159e8eeb5`.
Native FFmpeg reproduces it with16kHz resampling, three split inputs delayed
2000/31000/61000ms, `amix=inputs=3:normalize=0`, finite
`apad=whole_len=1056000`, `-t66 -ac1 -ar16000 -c:a pcm_s16le`.
The first unbounded-padding construction was stopped and replaced before
acceptance; only the finite66-second fixture above is used. Early QA setup
failures were a private arguments file missing0600 and a wrong Linux build
target; neither is accepted evidence. Native test diagnostics now retain child
exit status. There is no Python implementation or shipped Python runtime.

Area5 remains active. The original Molmo2 engine's `analyze` video path uses
`decode_video_window` and `preprocess_video_frames` with at most16 bounded
timestamped frames. The actual installed macOS Q4 package and native engine
exist; its19 shards, manifest/config/tokenizer/processor contract are already
owned by that engine. Public Inference currently has only single-image
VisionPsy. Next expose the existing temporal path behind an owned native public
adapter, preserving source-time selection, sample timestamps, model/input
identity, controls and uncertain derived observations. Continue actual speech
qualification through encoded full-duration input and recovery/control gates;
do not close area5 from this WAV corpus or a single-frame image check.

## Area5 encoded speech and native temporal prerequisite — 2026-10-11

The full-duration C harness now accepts the existing compressed-media arguments
and independently hashes canonical PCM for each selected window. Native MP3
capture recognizes initial/middle/late speech across the66-second selection,
matches independent whole-file decodes at each of the three window boundaries,
publishes all captures to the actual SDK, completes and exactly reads retained
captures without original/model transport. macOS normal/SAN pass2602 each;
Linux normal/SAN pass2591 each, all terminal zero. Logs on the host:
`/private/tmp/samosa-whisper-full-encoded-{mac,linux}-{normal,sanitize}.log`.
Linux copies preserve the original logs. Model/SDK dependencies are not
sanitizer-built in this adapter/harness gate.

MP3 fixture SHA256
`e71bf46dab639e83d59637e2c334bdcb34ea428efa67da8ce4aca1ebfdf6b6d5`;
macOS independent reference SHA256
`82fa14ebbaaeda6c7f95ab6305e996ea915452bb5579d87b1de17da34a3e491f`.
It is encoded from the accepted66-second WAV with native FFmpeg/libmp3lame64k;
platform reference decodes use PCM16/16kHz/mono. No Python runtime is involved.

The existing native Molmo2 helper now accepts host-private `media_kind=video_frames`
with1..16 absolute prepared-image paths and matching finite strictly increasing
timestamps in0..14400s; prepared image axes are bounded to378. It uses the
existing `preprocess_video_frames` and model generation, not a new model engine.
Successful temporal replies return the exact sample timestamps; ordinary native
video replies also expose their decoder sampling labels. Precision17 preserves
subsecond labels. Source provenance and model uncertainty still belong to the
future public adapter/client. Existing image/multi-image/video paths remain.

`make samosa-molmo2 test-molmo2-processor` builds and passes existing processor
checks. Native C `build/test-temporal-engine-real{,-sanitize}` exercises the
installed macOS Q4 package through the candidate helper: duplicate timestamps
refuse; two prepared red/blue frames at10.125/11.375s return those exact labels
and an observation mentioning red before blue. Normal10238/SAN10106 checks pass,
including wait polls. Logs `/private/tmp/samosa-temporal-engine-real{,-sanitize}.log`;
receipts/artifacts in matching directories. The external MLX engine is not
sanitizer-built; the sanitizer qualification covers the native C harness.
Candidate engine SHA256
`fac4f0c4ead673ad60370f54b017eb087e6c3136a19abc09c5f00e3ba4e36034`.
It was built locally, without replacing the user's installed helper/default.
The model infers a smooth transition from sparse endpoints: this remains
uncertain model output and cannot establish exhaustive temporal coverage.

Area5 remains active. This native prerequisite does not yet provide the public
Inference temporal DTO or Sources capture/cache integration. Sources' existing
frame reader bounds axes to512; resolve that declared preparation compatibility
using the native temporal processor before wiring its client. Add model/package
pin checks, owner/operation controls, resource/input bounds, offline replay and
actual temporal integration evidence through the public boundary. Preserve the
macOS-only native MLX dependency explicitly; do not claim actual Linux MLX video
from protocol fixtures. Areas6–7 remain open; the full goal remains active.

## Public native temporal adapter checkpoint — 2026-10-11

Implemented optional standalone `samosa-inference-temporal`: owned C input and
package modules, shared guardian bridge, durable operation identity/replay and
native Molmo2 invocation. Contract: `components/samosa-inference/contracts/temporal-v1.md`.
Inputs have1..16 ordered timestamped RGBA frames, axes<=512, selected range<=4h
and request<=24MiB. Source reader compatibility is resolved: the native helper
accepts512px prepared inputs and uses its unchanged378px temporal processor.
The normalized operation stores ordered input hashes rather than raw images.
Output includes per-frame pixel/P6/time binding, model/runtime/manifest pins,
sampled uncertain coverage and the native one-decimal timestamp-label policy.

The actual model package's24 declared files (3,372,294,725 bytes) pass C pinned
pre/post verification normal/SAN, cancellation, wrong manifest-pin refusal and
changed-identity refusal. Model manifest
`a07230dd952e97969921223328e7812020bb58711728450cdfb2205e3a15b770`;
compound model identity
`5c8b663278eb213de67629c9b78058638acfdd50ea219ae9878d6f53914e798f`.
Logs `/private/tmp/samosa-temporal-package-real-mac{,-sanitize}.log`.

The first public actual-model gate exceeded the copied image adapter's60s default
and was not accepted. The original Molmo2 supervisor at samosa_gateway.c13223
declares120000ms. Temporal requests now use that120s default/ceiling; other
providers retain their existing limits. Final normal gate is terminal zero,
61.50s wall clock, `/private/tmp/samosa-temporal-public-real-mac-120.log` and
matching artifact directory. Final C adapter/harness ASan/UBSan is terminal zero,
70.12s, `/private/tmp/samosa-temporal-public-real-mac-sanitize.log` and matching
directory. Both pass two512px red/blue frames with a>2MiB public request, exact
pixel/P6/timestamp/package/runtime receipts, ordered model observation, foreign
owner refusal and byte-identical model/runtime-absent completed replay. Printed
524671 counters include pixel-fixture writes and do not represent independent
quality cases. The external MLX/native C++ engine is not sanitizer-built.
Current candidate helper SHA256
`c8aa53e06d30305f3077e260f5fbda258cd911941b109dfe18d286fc96d08a05`.
No installed runtime/default model was replaced.

Input boundary normal/SAN passes on macOS and Linux:16-frame admission,17/zero
refusal, duplicate timestamps, oversized axes, canonical base64/padding refusal,
hash-only normalization and runtime/input changes. Strict GCC initially refused
compact indentation and a missing X/Open test feature macro; formatting and
the feature declaration were corrected with `-Werror` retained. Public temporal
C binaries build normal/SAN on Linux. Shared image bridge and guardian regressions
pass normal/SAN on both platforms: image634/644 macOS,644/664 Linux (including
polling counters), guardian124 each mode. Logs
`/private/tmp/samosa-temporal-input-mac.log`,
`/private/tmp/samosa-temporal-shared-regression-mac.log`, and copied Linux
`/private/tmp/samosa-temporal-public-linux-r3.log`. Actual MLX model acceptance
is macOS only; Linux input/protocol checks do not imply actual-model execution.

Area5 remains active. Next wire Sources temporal preparation/capture/cache
through this public DTO, its required bounded process transport and the existing
native frame reader/sampler. Qualify retained source provenance, late/final
sampling, offline replay, native SDK publication where required, and dedicated
temporal corruption/controls/admission/requester-death cleanup before closure.
Shared guardian/image checks are regressions, not substitutes for that new
temporal integration. Areas6–7 and independent release acceptance remain open.
No shipped Python runtime, completion percentage, commit, push or deployment.
# Sources temporal transport checkpoint — 2026-10-11

Host-only `SourcesInferenceProcess.temporal_frames=1` allows the public native
temporal DTO (24MiB request, 48MiB private metadata, 120second supervised budget).
Default transports retain 2MiB/60second bounds. Responses remain bounded to256KiB.
Maximum-sized fixture input reaches the provider exactly; default-profile and
maximum-plus-one requests refuse before launch. Existing provider-error,
oversized/invalid response, cancellation, deadline and requester-death checks pass.
Normal and ASan/UBSan suites terminated successfully on macOS and ARM64 Linux:
`/private/tmp/samosa-temporal-source-transport-mac.log` (75/82 checks),
`/private/tmp/samosa-temporal-source-transport-linux.log` (73/73 checks).
Counters include polling. This is transport plumbing, not model quality or a
completed Sources temporal capture/cache integration. Area5 remains active.

## Sources temporal capture and lifecycle integration — 2026-10-11

`analyze_video` now uses an owned encoded snapshot, validated video duration,
uniform source selections up to16frames, and the last presentation frame in the
selected range. Sources sends only prepared RGBA/timestamps to the public native
Inference temporal API. Receipts retain decoder/preparation identities, per-frame
pixel/P6 hashes, exact public model receipt and partial uncertain coverage;
owner/source/range/prompt/recipe/model/runtime bindings protect cache and tracked
replay. Offline cache validation uses the owned retained snapshot and structured
receipt without the original, media helper or model executable. Different model
pins miss the cache; foreign owners and digest-repaired timestamp corruption
refuse. See `components/samosa-sources/contracts/temporal-v1.md`.

The temporal process profile explicitly preserves120seconds of wall/CPU budget
and8GiB Linux virtual address space for the3750MiB declared native model package.
Default media/image/audio transports retain32CPU seconds/2GiB and their existing
request bounds. Inherited limits are queried by a native provider fixture; both
profiles exercise deadline/cancellation/requester-death teardown and malformed
or excessive provider output. The first actual Sources run inherited the old
32second CPU budget and failed; it is excluded from qualification.

Actual native macOS Molmo2 package/runtime pins remain:

- Engine `c8aa53e06d30305f3077e260f5fbda258cd911941b109dfe18d286fc96d08a05`.
- Manifest `a07230dd952e97969921223328e7812020bb58711728450cdfb2205e3a15b770`.
- Compound model `5c8b663278eb213de67629c9b78058638acfdd50ea219ae9878d6f53914e798f`.

Source→native decoder→public temporal adapter→actual model→retained capture
passes normal/SAN for four512px red/blue frames, with independent FFmpeg RGBA/P6
hashes and exact timestamp/owner/model bindings. Initial-range logs are
`/private/tmp/samosa-source-temporal-real-mac-r4.log` and
`/private/tmp/samosa-source-temporal-real-mac-sanitize.log` (1048670 checks each).
Late-range60–62seconds logs are
`/private/tmp/samosa-source-temporal-real-late-mac.log` and
`/private/tmp/samosa-source-temporal-real-late-mac-sanitize.log` (1048673 each).
Both remove the origin and substitute missing helper/model executable paths,
then prove exact tracked replay and ordinary retained cache reuse. Counts mostly
include independent pixel writes, not accuracy cases. The model describes a
smooth transition from discrete samples; its output explicitly remains uncertain,
and no exhaustive frame/accuracy claim is made. External MLX engine/library and
FFmpeg libraries are not sanitizer-instrumented; the C adapters, media worker,
guardian and QA harness are instrumented in the SAN path.

Controlled native Source gates include16actual decoded512px frames and retained
reuse; actual model quality is not inferred from their callback fixture:

- macOS normal/SAN:991checks each,
  `/private/tmp/samosa-source-temporal-final-mac-{normal,sanitize}.log`.
- ARM64 Linux normal/SAN:991each,
  `/private/tmp/samosa-source-temporal-final-linux-normal.log` and
  `/private/tmp/samosa-source-temporal-final-linux-sanitize-r2.log`.

The first Linux full-SAN corpus was interrupted by the180second overall QA alarm.
The accepted rerun has a600second whole-corpus QA bound; individual production
operation budgets remain120seconds. Both Source parent and media worker are
instrumented. It is not an actual Linux MLX model qualification.

Dedicated public Inference temporal lifecycle tests validate16prepared PPMs,
framed output bounds/id/time/usage/quit semantics, provider failures, model
mutation, offline/foreign-owner receipts, busy admission and foreign active
cancellation refusal. Deadline, cancellation and requester death must reap the
owned engine and remove every prepared file and its workspace. Fixtures use
pinned real processor/config/tokenizer metadata and explicitly fake weights/
engine; they are lifecycle proof, not retrieval/model quality. Final macOS
normal/SAN logs `/private/tmp/samosa-temporal-lifecycle-mac-r3.log` (1023/1029);
Linux admission-expanded normal/SAN passes1033/1045, retained in
`/private/tmp/samosa-temporal-lifecycle-linux-r2.log`. Variable counters include
polling; these gates include the admission assertions.

Source transport profiles normal/SAN pass macOS140/158 and Linux139/138 checks;
logs `/private/tmp/samosa-temporal-source-transport-limits-{mac,linux}.log`.
Variable counts include polls. Mac current speech capture351/351 and SDK
transcript2268/2282, visual129/129 publication regressions pass. Linux current
speech capture351/351, transcript publication2298/2596 and visual publication
129/129 pass; `/private/tmp/samosa-temporal-speech-publication-regression-linux.log`.
Actual16frame model gates and the
finite area5 closure audit are next. Areas6–7 remain open; no Python runtime,
default model switch or release completion is claimed.


### Area5 closure — 2026-10-11

The finite checklist in ARCHITECTURE_REBUILD_PLAN.md is satisfied. This closes
Sources transcription/visual analysis, not the full rebuild or release.

| Required behavior | Accepted evidence |
| --- | --- |
| Full-duration speech and selected timeline | Actual66-second WAV and MP3 normal/SAN on macOS/Linux; three overlapping windows, initial/middle/final speech, independently decoded per-window PCM identity and absolute coordinates; full-duration logs above |
| Saved windows, publication and offline recovery | Every selected speech window captured/published/completed; origin/model-free exact retained reads; current transcript351/351 both platforms and publication2268/2282 Mac,2298/2596 Linux, including checkpoint/SDK interruption recovery |
| Owned native temporal interface and Source client | Public prepared RGBA/time DTO, native decoder/probe/uniform sampling/final-frame selection, selected initial and60–62second ranges through actual pinned Molmo2; retained owner/source/model-bound observations |
| Actual maximum temporal input | Source→decoder→public Inference→actual Molmo2 at16x512px frames passes normal/SAN,4194476 checks each; independent FFmpeg RGBA/P6/time bindings, first/last selection, actual ordered observation and origin/helper/model-free cache/tracked replay |
| Pins, bounds, ownership and controls | Complete pinned package/native executable checks; malformed result/time/usage/quit refusals; mutation, busy admission, foreign owner, deadline, cancellation and requester death; owned16-file workspace reaped/removed; current normal/SAN lifecycle Mac1023/1029, Linux1033/1045 |
| Native regressions and honest coverage | Source16-frame fixture991/991 each platform; default/temporal inherited transport limits qualified; image adapters and visual publication129/129 both platforms; no Python runtime. Speech/temporal output remains derived and non-exhaustive |

Actual16-frame logs: `/private/tmp/samosa-source-temporal-real-full-mac.log`
and `/private/tmp/samosa-source-temporal-real-full-mac-sanitize.log`, both
terminal0. The large counters include independent pixel writes, not accuracy
cases. Fixture video SHA256
`86faf709366aaf60cd96106162b641bc339fc2a6d08195f88b14fbf14d4d867f`;
independent selected RGBA SHA256
`65317d8dab859593b566a9ce04fab7124b8d74654bed0f44b95d732680a099c1`.
Late-range video SHA256
`54b92b5faa4a1f4f1e5efe69b531f8efe86d131876df8fcde150f0c2e86acaab`.
Actual Molmo2 is qualified on macOS through native MLX/Metal; Linux gates qualify
C adapters/controls with fixture engine/weights. External MLX/FFmpeg libraries
are not sanitizer-built. The original120second temporal operation budget is
preserved. No default engine install/switch, commit, push or release occurred.
Areas1–5 are CLOSED;6 is ACTIVE;7 and the full rebuild remain OPEN. Historical
area5 pending checkpoints are superseded by this finite closure audit.

### Area6 initial authenticated header boundary — 2026-10-11

Implemented native `service_http.c/h` and independent C tests. Host credential
JSON maps distinct bounded owners/tokens; complete8192byte HTTP headers enforce
strict framing, bearer authentication before body-size acceptance, JSON16384byte
and multipart64MiB+8192framing limits, Origin refusal, duplicate/folded/transfer
encoding refusal, Expect semantics and bounded correlation identity. No body
bytes are consumed by this parser. Refused headers leave no successful request.
Mac/Linux normal/SAN each pass63checks with strict C11 warnings, logs
`/private/tmp/samosa-service-http-{mac,linux}.log`. A signedness compile failure
was corrected before accepted gates. `git diff --check` passes tracked changes.
This qualifies the initial boundary only; it does not claim complete socket
service, multipart parsing, credential file loading, fairness or shutdown. Their
finite checklist is `contracts/service-v1.md`; area6 remains ACTIVE,7 OPEN.

### Area6 authenticated socket/multipart adapter — 2026-10-11

Implemented `service.c`, `service_upload.c` and CLI `--serve` configuration,
reusing public Source dispatch and immutable streaming ingestion. Host credentials
are loaded through owner-only descriptor checks before listening. Service requests
cannot override owners. Startup verifies private operation state, pins its
inode/device and the optional root scope. Authenticated model-less health works.
Normal work has two workers/eight waiting slots with owner FIFO and round-robin;
health/describe/operation status/cancellation have a separate worker/two waiters.
Headers, JSON buffers, peers, body lengths, responses and I/O waits are bounded.
Multipart closing framing is validated before ingestion publishes, including
additional-part refusal; filenames never select filesystem paths. Queued requests
use the public request budget from authenticated headers, and queued disconnects
release admission. SIGTERM/INT stop ingress and reap owned workers with bounded
grace; retained Source results survive cold restart. Worker transport also observes
service-parent death, but active-provider parent-death qualification is still pending.

Native C actual-socket normal/SAN final gates pass:

- macOS2734/2517 checks, `/private/tmp/samosa-service-final-mac-r3.log`.
- ARM64 Linux1806/2020, `/private/tmp/samosa-service-final-linux-r3.log`.

They include private-file/symlink auth refusal, wrong credentials before multipart
100Continue/body access, owner override refusal, authenticated health, distinct
owners' retained read refusal, actual64MiB upload, malformed closing/additional
parts, two blocked upload workers with control access, owner FIFO/round-robin,
eight-waiter overload refusal, queued-disconnect capacity recovery, queued20ms
request expiry, five-second slow-header expiry, active upload shutdown and cold
restart/readback. Counts include socket/poll checks and vary; they are not distinct
behavior counts. Current header unit normal/SAN63 each platform remain accepted
in `/private/tmp/samosa-service-final-{mac,linux}-r2.log`; no header implementation
changed afterward. Initial native compile/test failures (Darwin declarations,
GCC indentation, owner override expectation, EOF-based client framing) were
corrected before accepted gates; failed logs are excluded.

Area6 remains ACTIVE. Complete service-owned background queue ownership/progress,
shared-instance/global admission ownership, all-format socket upload coverage,
active native/provider scoped cancellation/deadlines/disconnect/shutdown,
state/root/lease replacement and full recovery/resource gates before its finite
closure audit. Area7 independent release acceptance remains OPEN. Native C/C++
only; no Python runtime, commit, push, default-model change or release occurred.

### 2026-10-11 Sources area6 finite closure

This completed audit supersedes the earlier socket checkpoint's remaining list.
Areas1–6 are CLOSED; area7 independent final release acceptance is ACTIVE.
The service-v1 finite checklist is satisfied by the current native implementation.

Actual socket normal/sanitizer evidence (counts include polling, not distinct cases):

| Gate | macOS normal/SAN | ARM64 Linux normal/SAN |
| --- | --- | --- |
|64MiB, owners/fairness/control/overload, headless indexing, shared leases, state replacement, restart |4420/4168 |3674/4436 |
| Active provider and background audio interruption/cold recovery |2152/2292 |2108/2522 |
| All seven upload formats, independent identity and retained literal readback |1541/1623 |1525/1829 |
| Private shared lease unit boundary |37/37 |37/37 |
| Root replacement independent of retained state, active upload stop/restart |760/788 |761/1037 |
| Header/auth boundary (unchanged) |63/63 |63/63 |

Logs: `/private/tmp/samosa-service-owned-final-{mac-r2,linux-r4}.log`,
`/private/tmp/samosa-service-background-control-{mac-r2,linux-r4}.log`,
`/private/tmp/samosa-service-root-{mac,linux}.log`; header evidence remains
`/private/tmp/samosa-service-final-{mac,linux}-r2.log`.

The daemon holds inode-bound private service/queue leases, rejects a concurrent
service or standalone index runtime, rotates owner-bound public background phases
within the foreground worker budget, and provides reserved control admission.
Two owners' real text indexing progresses without an App or client step commands.
All-format ingress uses real text/HTML/PDF/deflated DOCX/PPM/PCM WAV/container bytes;
format admission proof does not replace the already qualified decoder/model gates.

Native WAV/provider controls prove foreign cancellation refusal, owned cancel,
deadline, client disconnect, TERM and service SIGKILL cleanup, then retained
operation recovery. Background audio interruption retains a cancelled ready job
and permits transport-free cold reads without its executable. Transient public
queue `busy` is retried with a bounded wait by the independent client.
Linux daemon child-subreaper adoption fixes orphaned provider zombies under
non-reaping container PID1: owned descendants are adopted, killed only when
needed and reaped. The test also owns/reaps its SIGKILL-orphaned fixture tree.
Exact provider PID disappearance remains asserted; it was not relaxed to a zombie
check. Prior failed fixture runs and logs are excluded from acceptance.

Named lease, private state and configured root replacement stop active service
work and yield failure; restoring the original scope permits admission and
retained recovery. Host root/store/provider grants are common to configured
credentials, while Source durable objects and controls remain owner-scoped.
No Python runtime, SDK change, default-model switch, commit, push or deployment
was introduced. Area7 still requires native legacy-test conversion and independent
archive/release acceptance; this service closure does not claim release completion.

Area7 initial independent archive gate: the source/build/test/contracts/vendor
candidate was extracted outside this checkout at
`/private/tmp/samosa-source-final-candidate/mac` and Linux
`/tmp/samosa-source-final-independent`. Both clean `make all` commands terminated
successfully. Standalone header, lease, root-replacement and seven-format socket
clients pass against those newly built executables on both platforms. Logs are
`/private/tmp/samosa-source-final-candidate/{mac,linux}-{build,socket}.log`.
Mac binary links only libSystem; Linux links libc and its system loader. No Python
runtime dependency is present. These initial gates do not constitute final release
acceptance: full independent normal/SAN qualification and legacy publication
fixture conversion remain. The release tar command now excludes macOS extended
attributes/AppleDouble entries; the cleaned candidate lists without GNU tar
extended-header warnings and contains no AppleDouble paths. No release published.

### 2026-10-11 area7 native publication tooling

Replaced the remaining Python publication client with standalone native C
`tests/test_publication.c` and `tests/publication_readers.h`. Both existing Make
publication targets now invoke native binaries; the unused Python file is removed.
Full actual SDK/HTML/DOCX/PDF/OCR normal/SAN Make gates terminated successfully:
macOS3404/5884; Linux3872/22642. Counts include variable subprocess polling.
Logs: `/private/tmp/samosa-publication-native-make-{mac,linux}.log`.
These exercise current CLI, SDK worker and fault CLI binaries; sanitizer targets
instrument all three and the client, while external SDK/decoder libraries retain
their separately qualified builds. No SDK production change was made.

`components/samosa-sources/docs/publication-native.md` maps every former driver's
case to the native replacement. Preserved: owner/store grants; exact offline
receipt/capture replay; pending/committed checkpoints; lexical/artifact provenance;
post-SDK SIGKILL and changed live bytes; forged content/recognition refusal and
repair without duplicate publication; headless bounded UTF-8 publication; actual
readers, empty PDF layers, reindex history and uncertain OCR raster/confidence.
The old count assumed text-only PDF behavior even after OCR was configured. The
native test explicitly verifies the current distinct fallback artifact, one OCR
page and two retained text-only versions; subsequent recovery leaves counts fixed.
Initial native-port request-field, replay-ID, dependency-symlink and fixture-path
mistakes were corrected before accepted Make gates; failed runs are excluded.

Native-tool conversion is complete. Final independent archive normal/SAN
qualification and release audit remain; area7/full goal stays ACTIVE.

Final native archive candidate contains no Python files or AppleDouble entries.
SHA256: e41718a03358799fbc04bdadf635d0ad0313287379c9f2f699c9e68a0cb1e3ae.
Path: `/private/tmp/samosa-source-final-native/source-candidate.tar.gz`.
Fresh extracted copies run the complete default `make test sanitize` outside the
checkout: macOS `/private/tmp/samosa-source-final-native/mac` (session23694), Linux
`/tmp/samosa-source-final-native` (session71028). Both handles were confirmed live;
logs are `mac-suite.log` beside the candidate and Linux
`/tmp/samosa-source-final-native-suite.log`. They have not yet established a final
pass; poll those handles rather than restarting an unfinished suite. Explicit
external SDK/reader/media/model archive acceptance and the final audit follow.

Archive suite found and corrected an actual standalone-build defect: DOCX_SOURCES
incorrectly included publication, Chutni and acquisition modules, causing unresolved
SQLite/Source symbols in the parser-only target. The Makefile now lists only the
DOCX/HTML parser and native miniz sources/headers for that target. No parser or
runtime behavior changed. Normal/SAN parser gates pass175820 checks each on
macOS/Linux, logs `mac-docx-fixed.log` beside the candidate and Linux
`/tmp/samosa-source-final-native-docx-fixed.log`.
The initial macOS suite terminated with that link failure and is excluded from
acceptance. Its extracted Makefile is corrected; a complete `make test sanitize`
rerun is active in `mac-suite-r2.log`. The original e41718a candidate is superseded
by the Makefile correction and must not be delivered as the final archive. Linux's
original suite remains active until authoritative completion/failure; its running
copy has not been mutated. A corrected final archive and full release acceptance
still remain.

### 2026-10-12 Sources release qualification resumed

Authoritative recovery found Docker stopped and the original Linux archive suite
terminal at the known DOCX parser-only link defect. It is excluded from acceptance.
Docker and the retained ARM64 Linux container are running again. The corrected
Makefile was copied only after confirming no Linux suite remained live.
The corrected candidate is `source-candidate-r2.tar.gz` beside the original,
SHA256 `a28eb9943774f10c6decf3d815233aa1b584ddd72a3d17e0619b99b57d25a8d6`.

Live gates at this checkpoint (pending results, not acceptance claims):

- macOS independent default normal/SAN: session87224,
  `/private/tmp/samosa-source-final-native/mac-suite-resumed.log`.
- Linux independent default normal/SAN: session15805,
  `/tmp/samosa-source-final-native-suite-r2.log` inside `samosa-s17a-linux-qa`.
- macOS independent optional normal/SAN: session42490,
  `/private/tmp/samosa-source-final-native/mac-optional.log`.
- Linux independent optional normal/SAN: session24615,
  `/tmp/samosa-source-final-native-optional.log` inside that container.
- Actual-model archive sanitizer gates: macOS40337 and Linux26096,
  `mac-model-sanitize.log` beside the candidate and
  `/tmp/samosa-source-final-native-model-sanitize.log` respectively.

Optional suites use fresh extracted copies and cover native HTTP/TLS/public fetch,
search/all provider mappings, actual SDK/reader/OCR publication, Chutni retrieval,
transcript publication, semantic bounds/ranking, summary publication, enrichment
and embedding publication. Sources copies use no sibling private source or state;
explicit public SDK/reader libraries and public Inference executables are external
host dependencies. Actual-model normal archive logs already report468 checks on
each platform; final acceptance also requires sanitizer completion. Media corpus,
full-duration speech/temporal archive acceptance, exact final archive content audit
and final release remain to be collected. Goal and area7 stay ACTIVE.

Subsequent authoritative result: macOS actual-model archive SAN session40337
terminated0,468 checks. Twelve actual document/query rankings, native automatic
capture/publication and CLI/offline recovery pass against independently built
Sources workers. Linux26096 remains live. Byte-for-byte audit of corrected r2
archive found282 regular files, all matching the current component sources;
no Python or AppleDouble paths. This establishes candidate identity, not release
completion. Default and optional suites remain live on both platforms.

### 2026-10-12 archive media/model continuation

The independent media normal/SAN collections are live: macOS38212
`mac-media.log` beside the candidate, Linux54472
`/tmp/samosa-source-final-native-media.log`. They include codec/color/orientation,
late windows, admitted audio/long-window/gap corpora and visual publication/queue/
index targets. The trailing visual targets need the actual video fixture
`samosa-area4-visual-fixture.mp4`; the initial collection incorrectly configured
the speech MP3 for those trailing targets. Preserve corpus results, exclude any
resulting visual fixture failure, and run the visual targets with the video.

Actual full-duration archive speech terminated0: macOS WAV2710 (2648 checks),
macOS encoded SAN57025 (2602), Linux WAV70639 (2637). Logs are
`mac-speech-{normal,sanitize}.log` beside the candidate and Linux
`/tmp/samosa-source-final-native-speech-normal.log`. Linux encoded SAN97229 is
live, `/tmp/samosa-source-final-native-speech-sanitize.log`; its media helper was
still building at launch, so its result must be inspected rather than presumed.

Actual macOS temporal archive normal29879 and SAN83049 both terminated1 with
`deadline_exceeded` at the unchanged120000ms request budget. They ran alongside
the full default/optional/media collections and each other. Logs
`mac-temporal-{normal,sanitize}.log` preserve those excluded failures. Dedicated
normal then SAN reruns after competing collections finish remain required;
do not enlarge the120s budget or claim that contention is proven as the cause.

Linux actual-model SAN26096 was stopped after diagnostic evidence showed every
embedding pending with `chutni_response_invalid`, no Inference activation and
an older incorrectly selected SDK (`build-linux`, October9). This was a QA
dependency-selection error, not a model result. Fresh run51835 selects the
qualified `build-bound-linux` SDK (October11), retains original failure evidence,
and writes `/tmp/samosa-source-final-native-model-sanitize-r2.log`; it is live.

Added the component release acceptance matrix and corrected a literal escaped
newline in its README. Final archive must include these documentation changes;
the r2 candidate continues to identify the tested runtime/build/test sources.
Area7 remains active; no final release claim or replaced-App retirement.

Authoritative follow-up: Linux encoded speech SAN97229 terminated0,2591 checks.
Thus both platforms' archive WAV normal/encoded SAN full-duration captures,
three-window SDK publication and model/origin-free retained readback pass.
Linux default suite15805 terminated2 at `test_inference_process.c:48`, the busy
response assertion, after normal gates and initial SAN service/transcript gates
passed. Full Linux suite acceptance is not established. Focused existing-binary
diagnostic99233 is live, log
`/tmp/samosa-source-final-native-process-diagnostic.log`; inspect its result before
choosing the next action. Mac optional native publication normal/SAN has passed
actual HTML/DOCX/PDF/OCR cases (SAN8504 checks); the overall optional suite remains
live. No deadline or assertion was weakened.

Focused Linux transport99233 terminated0,141 checks with the existing binaries
and unchanged assertions/budgets. Complete Linux normal/SAN rerun is now live,
log `/tmp/samosa-source-final-native-suite-r3.log`, with explicit
`SAMOSA_SERVICE_FULL_UPLOAD=1` to include the64MiB native socket boundary.
The prior failure remains excluded; focused success does not replace the full
required suite. Temporal archive reruns still await reduced competing load.

### 2026-10-12 independent socket64 acceptance

macOS independent native socket gates3305/43455 both terminated0: normal4086,
SAN4252 checks, with explicit64MiB multipart upload/readback. Logs
`/private/tmp/samosa-source-final-native/mac-socket64-{normal,sanitize}.log`.
Owner isolation, FIFO/round-robin, reserved controls, shared service/queue leases,
two-owner headless indexing, scope refusal, shutdown/restart and SIGKILL recovery
remain asserted. Default/optional/media and Linux actual-model gates are still
live; Linux full suite-r3 handle43662 is the current complete-suite rerun.

Package-content coverage audit confirms the only workspace differences from
the tested r2 payload are README documentation and the new release acceptance
document. All runtime/build/test/dependency files still match. Their exact
SHA256 list is `/private/tmp/samosa-source-final-native/qualified-runtime.sha256`.
Linux actual-model queue diagnostic reports seven published embeddings, one
pending with no error; corrected SDK run51835 is making real progress.

macOS media collection38212 terminated2 only at the trailing visual test given
the incorrect MP3 fixture. All preceding declared corpora completed successfully,
including normal/SAN four-hour inspection/late selection and audio-gap refusal.
Visual rerun6805 used the correct video but incorrectly set
`SAMOSA_TEMPORAL_FULL_FIXTURE=1`; that variable requires an actual video path.
It failed at the fixture fopen and is excluded. Corrected visual rerun is now
live in `mac-visual-r2.log`, with the original area4 visual fixture plus explicit
`/private/tmp/samosa-temporal-source-16-frames.mp4` for16-frame fixture capture.

### 2026-10-12 archive suite results and remaining diagnostics

macOS full default normal/SAN87224 terminated0; complete
`mac-suite-resumed.log` establishes independent default acceptance. Mac full
optional42490 terminated0: native acquisition/TLS/all search mappings, actual
SDK/readers/OCR publication, retrieval, transcript queue, semantic, summary,
text/document enrichment and fixture embedding gates all passed.

Mac16-frame fixture capture and visual publication normal/SAN pass991/991 and
129/129 respectively in `mac-visual-r2.log`. Its trailing visual queue required
the61-second fixture and failed invalid_range with the2-second fixture; exclude
that target result. Corrected independent queue91586 terminated0 (normal/SAN,
`mac-visual-queue.log`, SAN1586). Independent visual index74047 terminated0
(normal/SAN, `mac-visual-index.log`, SAN256). No production limits changed.

Linux model51835 terminated1 at the exact one-call-per-document assertion after
all12 embeddings were published: transient inference_unavailable/protocol errors
caused retries while other qualification was running. Preserve
`model-sanitize-r2.log`; ranking/CLI/replay assertions after that point were not
reached. A dedicated lower-load complete model SAN run remains required.

Linux optional24615 terminated2 at actual PDF publication: retained item error
`pdf_unavailable`; its configured old host PDFium binary was absent. Recovered
the declared chromium7961 ARM64 Linux binary from the official release. Asset
digest974107999784a438149605024475d42d80dd306799d90e1af5f6fa63f976455f
matches GitHub release metadata and the downloaded archive. Extracted public
dependency is `/tmp/samosa-source-final-native-pdfium/lib/libpdfium.so`.
Remaining Linux SDK/reader/enrichment/semantic targets69950 are live in
`/tmp/samosa-source-final-native-optional-r2.log`; preceding fetch/search targets
had passed before the missing PDF dependency stopped the first collection.

Additional actual OCR enrichment initially used forbidden Homebrew library
symlinks and failed ocr_artifact_invalid; those configurations are excluded.
Resolved artifact paths yield macOS normal54411 terminal0,5136 checks. SAN93248
and repeat21745 terminate1 at the background runtime survival assertion530.
This repeat requires diagnosis; it is not presumed to be timing. Independent
debug-only copy adds queue output/exit diagnostics without changing assertions
or product code; no debug result counts as release acceptance.

Actual Mac temporal normal rerun59838 is live in `mac-temporal-normal-r2.log`,
after Mac default/optional/media compilation and fixture work ended. SAN will
follow only after this run terminates. The120s request budget remains unchanged.
Full Linux default43662 and media54472 remain live. Goal/area7 stay ACTIVE.

OCR enrichment diagnostic43885 terminated1 with an actual ASan
stack-use-after-scope report: SourcesConfig.ocr retained the test's block-local
SourcesOcrConfig after the reader branch ended, then the background queue read
it in native.c. Fixed only the fixture: OCR configuration now lives in main's
scope for all later background/control assertions. No Source runtime change,
assertion weakening or feature exclusion. Fixed normal/SAN builds are live on
macOS81603 and Linux82107 in independent copies. Linux has a separate fresh
enrichment-fixed extraction so the running optional/default suites are not
mutated. New test digest must join final package identity; the r2 candidate
remains the matching runtime/build/dependency snapshot, not the final archive.

Mac dedicated temporal normal59838 still terminated1 with deadline_exceeded;
it is excluded. The next temporal run should follow completion of the Linux
collections and removal of the QA VM's competing load; unchanged120s-budget
actual normal/SAN acceptance remains mandatory. Do not infer a cause yet.

Fixed OCR fixture macOS gate78468 terminated0: normal5165/SAN5604 checks,
`mac-enrichment-fixed.log`. Both include actual PDF/mixed-OCR/image-OCR retained
enrichment/publication plus the subsequent background, model-change, interruption
and corruption assertions. The out-of-scope pointer defect is eliminated without
altering product code or disabling ASan scope checking. Linux fixed build82107
remains live; after it terminates, run the same actual-reader normal/SAN cases
using resolved system library paths, recovered PDFium and the independent public
Sources helpers. No new runtime source changes are present.

Linux fixed reader enrichment normal/SAN host invocation36615 is live in
`/tmp/samosa-source-final-native-enrichment-fixed.log`; inspect both its result
and build82107 before accepting it. It uses resolved Tesseract/Leptonica/model
paths and recovered PDFium, with separately built public Sources helpers.

Linux fixed reader enrichment36615 terminated0, normal5329/SAN10733 checks.
Both platforms now pass the corrected fixture's actual-reader enrichment and
subsequent background/control assertions, including ASan scope checking.
Linux media54472 terminated2 at the anticipated trailing MP3-as-video fixture;
all preceding normal/SAN codec/color/orientation/late/audio/long/gap corpus
targets passed. Corrected Linux visual fixture/publication/index and the
separate61-second queue collections are live in
`/tmp/samosa-source-final-native-visual.log` and
`/tmp/samosa-source-final-native-visual-queue.log`. Their media helpers are
independently compiled, not workspace binaries. Goal/area7 remain ACTIVE.

Current continuation handles: Linux default43662, SDK/reader remainder69950,
visual fixture/publication/index47998, separate61-second visual queue79592.
All four are confirmed live; do not restart them on observation timeout. Source
runtime/build/vendor files still match r2 exactly; only README and the qualified
OCR fixture lifetime fix differ among its existing files. The added release
acceptance document is also required in the final package. Exact current
runtime/build/test/vendor SHA256 list (211 files) is
`/private/tmp/samosa-source-final-native/final-runtime.sha256`.
Actual semantic Linux SAN and macOS temporal normal/SAN are deferred until the
competing collections finish, preserving the strict call-count and120s gates.

Linux visual collection47998 terminated2: normal16-frame fixture991 passed;
SAN stopped at the10s `visual-inspection` deadline before its16-frame branch.
Log `/tmp/samosa-source-final-native-visual.log` preserves the failure. SAN
fixture and subsequent visual publication/index targets remain required, to run
after concurrent collections finish. No assertion or budget was changed.
Queue79592, default43662 and optional69950 remain confirmed live.

Linux SDK/reader remainder69950 terminated0: all recovered-PDFium publication,
Chutni retrieval, transcript queue, semantic/summary/enrichment/fixture embedding
normal/SAN targets passed. Combined with the earlier acquisition/search target
results, independent optional acceptance is established. Fixed actual OCR
enrichment5329/10733 also passed separately. Default43662 and visual queue79592
remain live; failed visual SAN inspection still requires a dedicated rerun.

Linux visual queue79592 terminated0, normal1925/SAN6016 checks. Actual native
frames and SDK publication, selected-window planning, transient capture, durable
phases/backoff, owner/quota, post-SDK SIGKILL recovery and retained uncertain
frames all pass. This closes independent queue acceptance on both platforms.
Only default43662 remains live among these collections. Dedicated Linux visual
SAN, publication/index, actual semantic-model SAN and actual macOS temporal
normal/SAN gates still precede final release packaging and completion audit.

Linux independent full default collection43662 terminated0.
The Linux suite-r3 log records complete make test sanitize success, with SAMOSA_SERVICE_FULL_UPLOAD=1; final inventory and separate-process immutable-byte CLI acceptance passed. This closes independent core acceptance on both platforms. Dedicated Linux visual SAN/publication/index collection20027 is running without competing builds; actual semantic SAN follows. No runtime, assertion or deadline was changed for these reruns.

Dedicated Linux visual collection20027 terminated0, without competing builds.
Log: /private/tmp/samosa-source-final-native/linux-visual-final.log.
Full16-frame visual SAN991, actual public SDK visual publication129/129, and
visual discovery/reindex normal261/SAN412 all passed. Variable reindex counts
include interruption polling. The earlier overloaded inspection failure remains
excluded; no assertion or operation deadline was changed. Actual semantic SAN
collection27664 is now running sequentially in linux-model-sanitize-final.log,
using the qualified build-bound-linux public SDK and a fresh private output.
Minimal linkage rechecked: macOS libSystem only; ARM64 Linux libc/loader only.

Linux actual semantic SAN collection27664 terminated0:468 checks, twelve
indexed documents and twelve paraphrase queries, actual pinned embedding engine
and public Chutni rankings, strict exactly-twelve document calls, CLI and exact
model/origin-free replay. Log:
 /private/tmp/samosa-source-final-native/linux-model-sanitize-final.log.
Read-only progress observations showed all12 published with zero recorded
failures; terminal success, not those observations, is acceptance. Combined
with prior normal468 and macOS normal/SAN468/468, archive actual semantic
acceptance is established on both platforms. All Linux collections are terminal.
Only macOS actual temporal normal/SAN and final release identity/audit remain.

macOS actual temporal normal collection23889 terminated0 after stopping the
owned Linux QA container and its VM. Log:
 /private/tmp/samosa-source-final-native/mac-temporal-normal-r3.log.
All4194476 checks pass at16 native512px frames: independent FFmpeg RGBA/P6/time
bindings, first/last selections, actual red-before-blue observation, runtime and
package pins, origin/helper/model-free tracked replay and retained cache.
The original120000ms request budget is unchanged. Earlier deadline failures are
excluded. SAN collection31538 follows sequentially in
mac-temporal-sanitize-r2.log with instrumented Sources/owned media/public
Inference clients; external Molmo2/MLX and FFmpeg retain their declared status.

### Sources area7 closure and final release — 2026-10-12

Actual macOS temporal SAN collection31538 terminated0:4194476 checks with the
same16-frame512px fixture, exact independent RGBA/P6/time bindings, actual ordered
red-before-blue observation, pinned package/runtime and model/origin-free replay.
Log: /private/tmp/samosa-source-final-native/mac-temporal-sanitize-r2.log.
Both actual temporal archive runs pass the unchanged120000ms request deadline.
Linux actual Molmo2 execution is not claimed; native adapters/lifecycle qualify.

Final release:
- components/samosa-sources/dist/samosa-sources-0.1.0.tar.gz
- components/samosa-sources/dist/samosa-sources-0.1.0.tar.gz.sha256
- SHA256 a82d972e573a0b343ffbfba096f059fef45f5a88e8d5d019f94149d87bbf98d8
-3301081 bytes;282 regular source/build/test/doc/contract/vendor/license files.
- Exact archive-to-workspace bytes and full qualified runtime/test identity pass.
  Candidate delta: README/dependency documentation, the already both-platform
  qualified OCR fixture lifetime fix, and packaging-only Makefile exclusions.
  Every compilation rule is unchanged from the independently tested candidate.
- The final audit discovered a stale generated tests/__pycache__ bytecode file
  in the development candidate/ledger. Earlier claims of no Python artifacts
  in that candidate were too broad. It was unused by native qualification,
  removed from the source tree and omitted from the final archive; the packaging
  rule now excludes bytecode caches and macOS metadata. The final exact-content
  audit confirms no .py/.pyc, __pycache__, AppleDouble, build/dist state, links or
  unsafe paths. Licenses, contracts, native clients and vendored pinned sources
  are retained. Audit: /private/tmp/samosa-source-final-native/final-archive-audit.txt.
- shasum -a256 -c succeeds for the delivered archive. Minimal actual binary
  linkage is libSystem on macOS, libc/loader on native ARM64 Linux.
- Packaging used make -o test release after successful independent core and
  optional qualification, avoiding redundant full suite repetition for docs
  and packaging-only changes. This is a local source release, not remote
  publication, a Git commit/push or an assembled replacement-App release.

Final finite audit:1 media,2 uploads/freshness/watch,3 acquisition/search,
4 reindex/enrichment/semantic/Chutni,5 public-Inference transcription/visual,
6 authenticated service/bounded queues/shutdown,7 independent qualification/
release all satisfy the established component checklists. Actual semantic
normal/SAN468/468 and full-duration speech pass on both platforms; actual
temporal normal/SAN4194476/4194476 passes on macOS. Normal/SAN core, socket64,
native optional readers/SDK/media/queue/reindex gates pass on both platforms.
External engines/SDKs retain their declared instrumentation; fixture mapping
does not assert paid-provider availability or actual Linux T5/Molmo2 support.
Generated analysis retains partial/uncertain coverage.

Retired replacements within Sources are the pathname-based SQLite reopen paths
and former Python publication test driver. The retained production gateway is
part of the separate App/assembly replacement milestones. Sources areas1–7 are
CLOSED; the standalone Sources initiative is complete. C01–C14 and the broader
four-component rebuild remain open. No required Sources release gate is pending.

## Inference provider closure and package continuation — 2026-10-16

Inference remains the active independent component in Sources → Inference →
Agent → App → pinned assembly/legacy retirement. The expanded provider audit now
closes Inference area1:1/7 complete, area2 active, areas3–7 retain their original
requirements. All nine provider kinds coexist through the existing registry.
OpenDecision public text and image decisions reuse native engines/tokenizers,
request/result validation, operations, admission and guardian ownership.

Actual public text passes all ten original cases, including8x512 relations.
Actual image coverage passes twenty original-engine cases normally and with
sanitizer controls, including128-candidate Choice/Score, all relation categories,
name fallback/ties, Unicode,512x512 and1x512 prepared frames. All51 tokenizer
matrices match on macOS/Linux; model-free processor/control and nine-provider
normal/SAN gates pass on both. Actual model quality remains a bounded macOS
ARM64 qualification; Linux fixtures/builds do not claim Linux model execution.

The complete shared check script passed in independent exported-source macOS
and Ubuntu ARM64 directories. Hosted macOS/Linux run38973353427 passed at
46612a4e492dc14701255a039c3c5f6d2b48d6e9; prior implementation CI also passed.
Exact artifact pins, datasets, commands and terminal logs are recorded in
`components/samosa-inference/docs/OPENDECISION_IMAGE_INTEGRATION.md` and
`components/samosa-inference/docs/PROVIDER_INTEGRATION_AUDIT.md`.

User-authorized cleanup of Bonsai, Maple and redundant build/temporary weights
remains in effect. Reuse existing retained text assets at
`/private/tmp/samosa-decision-flat-xn7bui8y`, the installed Molmo2 and retained
embedding/summary models. Image uses the one converted graph at
`/private/tmp/samosa-decision-image-onnx-v1`; no model weights were added to the
checkout. The existing single Linux qualification container is reused.

Area2 continues through existing catalog/HTTPS/cache/reservation/publication/
removal/setup machinery. Existing GGUF policy already covers text, embeddings
and T5 summaries; do not create another summary recipe or installer. The missing
OpenDecision profile is mapped in `docs/OPENDECISION_PACKAGE_INTEGRATION.md` in
the independent module. Its descriptor passes406 normal/SAN checks on both
platforms; concrete host-policy/asset verification is in progress and recipe
installation is not yet qualified. All full area2–7, C01–C14, independent release,
pinned App assembly and legacy-retirement requirements remain. No parent
integration pin was advanced or production route retired.

## Inference area2 closure — 2026-10-17

The final general-text normal/SAN cold published-URL collections46057/50277
terminated0. Both actual-model, cancellation/owner/shutdown/offline recovery and
full cache-only resume/removal/reinstallation/cleanup gates pass. Exact receipts
and pinned metadata now reside in components/samosa-inference/catalog/ornith-text-cpu-v1.
Logs: /private/tmp/inference-text-resume-{normal,sanitize}.log. The unchanged
seven-file index is f5a6faa589c9478f29cb5fe343be224152780fc0264a20a3ff5d9a8a0d3a7c22.

The full original installation audit passes: ten declared catalog entries,
normal/SAN receipt/catalog/source/index bindings and107 pinned metadata files;
credential/redirect/quota/crash/recovery/concurrency/compatibility controls;
current reservation97/identity61 normal/SAN gates; and exact tracked installation
source binding to reverified successful independent CI39054802760 atb08b4f7.
Actual curated package execution is macOS26.5.1 ARM64; Linux native controlled
coverage does not assert Linux model availability. External engines retain their
declared instrumentation. No replacement installer or model conversion was added.

Inference areas1–2 COMPLETE; area3 lifecycle/readiness ACTIVE, areas4–7 OPEN.
Next LIFE-01: inspect managed_provider.c qualification and existing managed/
activation fixtures, then reuse llama_generate to require a successful bounded
text request before readiness, preserving guardian/activation ownership and
failed-replacement rollback. See Inference's lifecycle inventory/extraction
checkpoint. App, pinned assembly, full C01–C14 and retirement remain open.
