# Start and resume the Samosa rebuild

Current priority order and shared agent handoff, 2026-11-20:
[Modular rebuild priorities](ARCHITECTURE_REBUILD_PRIORITIES.md).
Read it before continuing implementation. It prioritizes preserving the working
tree, establishing a buildable baseline and then finishing the existing component
sequence. The implementation checkpoints below retain their original dates.
User constraint reaffirmed 2026-11-20: no Python anywhere in the project, including
runtime, tooling, packaging, tests and CI. Use existing native/shell paths and port
required Python helpers before invoking their entry points; preserve their coverage.
Required target, 2026-11-20: this Mac first. Build/test/install natively here;
do not start Docker/VMs. Docker/Linux checks are deferred and do not block the
current refactor. This overrides older cross-platform execution instructions.

Latest checkpoint, 2026-10-25: LIFE-07a is in progress. Added bounded caller
control to the existing extracted visual supervisor; both original protocols keep
their framing and retain ownership until close/reap. Evidence and remaining work:
[transport checkpoint](../components/samosa-inference/docs/qualification/life07a-specialist-control/README.md).
Next: borrowed guardian endpoints/retryable close, workspace/duplex ownership,
then reuse visual verification/preparation and real generation readiness.
Full Inference/Agent/App/assembly/retirement and the repo-split goal stay active.

Latest checkpoint, 2026-10-25: LIFE-06b image decisions now reuse a loaded model
across changing prepared frames. Original algorithms/limits/public receipts stay
intact; native macOS/Linux and actual macOS image/text resident/legacy gates pass.
New engine is a local candidate pending distribution. Next LIFE-07a reuses the
existing SamosaMmSession and VisionPsy/Molmo2 loops with verified workspace/GPU
ownership. See verification Resume checkpoint; earlier next-action text is history.
Full Inference/Agent/App/assembly/retirement and the repo-split goal remain active.


Latest checkpoint, 2026-10-25: LIFE-06a public persistent text decisions qualifies
using the unchanged engine/typed algorithms, existing asset verifier, guardian and
activation store, with complete recoverable pin snapshots. Native macOS/Linux
normal/SAN and actual macOS reference/lifecycle gates pass. Next LIFE-06b reuses
the existing image backend for changing prepared images on one loaded model.
See verification Resume checkpoint; earlier next-action text is historical.
The full independent repo split/Inference/Agent/App/assembly/retirement stays active.


Latest checkpoint, 2026-10-25: LIFE-05d Kokoro passes on the existing public
resident speech owner for all6 voices, normal/SAN, with original package/profile
and exact retained audio checks. Existing streaming/Pocket/native regressions
pass. Next LIFE-06a reuses the persistent NativeText decision engine, pinned
DecisionAssets and typed validation for registry lifecycle/readiness. See the
verification Resume checkpoint; earlier next-action text is historical. The full
repo split and remaining Inference/Agent/App/assembly/retirement remain active.


Latest checkpoint, 2026-10-25: LIFE-05c public resident speech lifecycle reuses
existing activation, package/runtime/guardian and audio receipt paths. Native
macOS/Linux normal/SAN and actual macOS Pocket pass. Next LIFE-05d qualifies
existing Kokoro on the same owner, then remaining modality lifecycle extraction.
Persistent session PCM events remain an explicit area5 gate. See verification
Resume checkpoint; earlier next-action text is history. The goal remains splitting
existing code into independent repos, with areas3–7 and assembled work still open.


Latest checkpoint, 2026-10-25: LIFE-05b resident speech worker reuses the existing
native session, guardian duplex ownership and PCM validators. Normal/SAN native
macOS/Linux, actual macOS Pocket and existing summary/speech/App/install/package
gates pass. Next LIFE-05c public speech activation/readiness/registry integration.
See verification Resume checkpoint; earlier next-action paragraphs are history.
The task remains extracting existing code into independent repos.


Latest checkpoint, 2026-10-25: LIFE-05a extracted the existing speech worker's
verified loaded-session setup into speech_session.c/h. Native macOS/Linux
normal/SAN and actual macOS public Pocket regressions pass. Next LIFE-05b reuses
that helper for sequential guarded-worker requests, then public speech lifecycle
qualification. See verification Resume checkpoint; older next-action text below
is historical. Extraction/reuse into independent repos remains the constraint.



Current steering (2026-10-24): extract and reuse existing code into independent
repos; preserve behavior and tests. LIFE-04c local public resident T5 lifecycle
now qualifies; production distribution remains open. Audit existing speech/other
modality owners for the next area3 extraction. See the verification Resume
checkpoint and component EXTRACTION_STATUS.md; older next-action text is history.

Status: native implementation in progress. Read the [current status](ARCHITECTURE_REBUILD_STATUS.md) and verification Current state checkpoint first.

Read this file first, then the relevant sections of the
[architecture plan](ARCHITECTURE_REBUILD_PLAN.md) and
[verification plan](ARCHITECTURE_REBUILD_VERIFICATION.md). Those files define
the destination, execution sequence, task/handoff rules and acceptance gates.
This file identifies the current component; the original S01 assignment below
is retained as historical scope.

## Working method

Current priority, 2026-10-24: Sources is complete; Inference areas1–2 are complete
and area3 lifecycle/readiness is active. LIFE-01 and LIFE-02 are qualified;
LIFE-03 text/embedding registry lifecycle and LIFE-04a guardian duplex are
qualified, as is LIFE-04b controlled summary transport. Continue LIFE-04c public
pinned T5 activation/registry lifecycle via the checkpoint and
`components/samosa-inference/docs/REGISTRY_LIFECYCLE.md`. Read
`components/samosa-inference/docs/EXTRACTION_STATUS.md` and
`INFERENCE_MODULE_PLAN.md` first. Reuse existing implementations and preserve
all seven Inference acceptance areas. The Sources checkpoints below are
historical evidence, not instructions to restart those completed areas.

### Historical Sources progression

Latest area1 implementation: PQ/HLG native conversion and color provenance,
followed by native keyframe seeking for late video selections. The current
verification checkpoint records the completed matrix and exact remaining work.
Late encoded audio, full inspection, admitted formats and final archive acceptance
also pass. Area2 now qualifies every reader and upload mode, warmed-cache/native
work races, watch lifecycle and process-death recovery. Next: audit area3's existing
HTTP/TLS acquisition policies, provider mappings and actual search evidence. Native
search CLI conversion and monolith5MiB acquisition capacity are implemented;
verification checkpoint records final gates. Public-address/DNS binding, offline
receipt replay, IPv6 and bounded redirects/final-URL provenance are qualified on
both platforms. Robots/parent rate integration, six provider mappings and final
standalone archive acceptance also pass;31427(macOS) and8866(Linux) are terminal0.
Live Parallel search plus exact offline replay/readback pass both platforms.
Area4 audit found the automatic audio linkage missing. Private queue admission
now joins the index transaction/lease and passes both-platform normal/sanitizer
checks. macOS public queue/publication regression61771 and Linux46673 are terminal0.
Initial discovery now connects immutable audio registration and inspect_audio
duration to atomic typed queue linkage and waiting_audio status. Final expanded
discovery checks are recorded at the end of the verification journal. Linked queue
identities/progress/completion now reuse public queue validation; expanded gates
are recorded there as well. Audio watch/refresh/reindex now executes with atomic
version/linkage/history commits; expanded WAV fixture checks pass both platforms.
Native WAV history/quota/crash/control gates now also pass both platforms.
Compressed discovery/reindex now passes its nine-case normal/sanitizer matrix on
both platforms. Retained audio history/read endpoints now qualify owner/root and
model binding, exact capture replay and bounded history pagination. Derived audio
candidate search passes both-platform normal/sanitizer and literal regression
gates. Mixed PDF text/OCR initial indexing and SDK publication now reuse public
read_pdf_ocr and monolith page-enrichment behavior. PDF OCR reindex/failure and
retained artifact-read gates now pass; the verification journal records exact
normal/sanitizer evidence and the full OCR regression. Next: video/non-text
indexing. The public Inference visual-analysis dependency now reuses the monolith's
native VisionPsy interaction, with both-platform protocol/control tests and a real
macOS checkpoint heading gate. Sources' analyze_visual now implements owned visual
captures, exact selected-frame timing, decoder/model provenance and the shared
cache; normal/sanitizer native-frame and actual macOS pipeline gates qualify this
subset. Native/public inspect_visual now shares the bounded frame-decoder probe
and passes664-check normal/sanitizer visual gates on both platforms. Next
implement durable visual index admission/discovery/reindex/publication. Public
publish_visual already reuses the Chutni derived-artifact writer;129 normal/
sanitizer SDK checks pass on both platforms, including tracked offline replay
and distinct caption provenance. Queue integration remains open. Retain
remaining modality/enrichment/semantic/Chutni requirements and reuse the monolith/SDK.

Treat C01-C14 as milestones, not single agent assignments. Work on one bounded
behavior at a time. Build only the contracts and dependencies that behavior
needs; mark partial milestones as in progress, not passed. Every required gate
still applies before its milestone is complete.

Follow the main plan's **Required execution sequence** and **Required task and
handoff record**. The implementation target is an independent component folder
that can become its own repository; the first owner folder is
`components/samosa-sources/`.

For each assignment, record:

- Behavior to deliver and relevant change IDs.
- Working monolith implementation and tests to reuse; dependency/boundary changes
  needed for extraction, or the concrete evidence that replacement is necessary.
- Owned component, public interface and state ownership.
- Included work and excluded work.
- Acceptance scenarios and focused test commands.
- Old path being replaced, or why this is preparatory work.
- Completion evidence and one exact next action.

Apply the main plan's **Reuse working monolith implementations first** rule.
Preserve working workflows by moving or adapting their existing code into the
owning component. Distinguish extraction, necessary boundary fixes and new
capabilities; do not treat migration as a reason to rebuild working engines.

Use the verification file's **Resume checkpoint** as the authoritative current
work record. Update it after a bounded task and before handing off. Keep durable
decisions in the architecture plan with their reason. Avoid another competing
backlog or an ever-growing transcript of investigation.

## First assignment: bounded plain-text source reading

**Goal:** a caller registers a small local UTF-8 text fixture, identifies its
source/version, and requests an explicit byte range through a Sources-owned
interface. The result reports exact text, provenance, the inspected range and
coverage. It works without the gateway, Agent, App or an inference provider.

**Change scope:** C01/C02 for this behavior, then a first C05 implementation.
Assignment ID: **S01**. This assignment cannot complete all of C02 or C05.

**Owned files:** minimal source, contract, task specification, build/test entry
point and tests inside `components/samosa-sources/`; update the main verification
record for evidence/handoff. Do not put the new implementation in the old `src/`
and leave folder independence to a later redesign.

**Start with a short task specification.** Name the input/result/error fields,
define byte-range and UTF-8 boundary behavior, choose source-size/read limits,
state whether registration captures immutable bytes or tracks a live file, and
identify the minimal existing primitives worth reusing. Use an isolated test
state directory. Establish one standalone build/test entry point. A library
with a small CLI adapter is sufficient; a network daemon is unnecessary here.

**Implementation scope:** registration/version identity, bounded text reading,
structured evidence/results and the small build/test surface they need. Plain
reading should reuse suitable low-level primitives rather than import gateway
coordination. Do not create an empty platform of unused interfaces.

**Acceptance:**

1. Exact bounded content and source/version/range metadata are returned.
2. Invalid/out-of-range selectors and oversized input/read requests fail clearly.
3. Stale live identity is rejected, or an immutable registration remains pinned
   to its captured bytes. Never silently rebind a source version to changed bytes.
4. Defined UTF-8 boundary behavior and authorized path/symlink policy are tested.
5. A clean standalone build and test run succeeds with the gateway unavailable
   and model packages absent. No private gateway headers or cache formats are used.

**Exclude:** PDF/OCR, indexing/cache orchestration, Chutni integration, model
installation, planners, conversations, UI, file mutations, repository splitting
and changes to the legacy gateway. This first proof is preparatory; its delivery
alone does not justify deleting the gateway's source-reading paths.

**Done:** the narrow behavior and tests pass; the checkpoint records commands,
results, files changed and the next assignment. Broader milestones remain open.

## After S01

Resume with S02-S06 from the main plan: first interaction with fake inference,
bounded PDF reading, a real provider, persisted conversations, then qualified
hot/cold/compacted recovery. Keep partial milestones open and follow the main
plan's retirement rules. This guide does not define a second execution sequence.

## Starter instruction for an implementation agent

> Read docs/ARCHITECTURE_REBUILD_START.md and the relevant C01/C02/C05 sections
> of the architecture and verification plans. Complete only the first bounded
> plain-text Sources assignment. Write its short task specification, implement
> the minimal standalone component and meaningful acceptance tests, and run the
> focused checks. Respect the defined exclusions. Record partial milestone
> status and a resumable checkpoint with exact results and the next action.
> Treat S01 as one assignment; continue to later assignments only when they are
> included in the requested implementation scope.

This suggested bounded instruction applies only when S01 alone is requested.
The active full rebuild follows the main plan and verification Resume checkpoint.

Current area4 continuation: shared durable audio/visual queue implementation and
public/runtime visual jobs added. Actual native61-second video/SDK macOS normal
and sanitizer gates pass; final expanded Linux gate16433 is live. MacOS CLI/
control/index gate43151 is terminal0 (135/712). Next finish that same Linux gate,
then integrate transactional visual discovery/reindex and retained version
history/derived candidate retrieval. Enrichment and semantic retrieval remain.

Correction: expanded Linux16433 exposed a runtime child early exit under SAN
and is terminal2 after the failing harness was stopped. Diagnostic41437 is
running with child-output/early-exit capture; resolve that failure before final
queue qualification or moving to automatic discovery/reindex.

Current checkpoint: diagnostic41437 confirmed fatal store_failed from schema contention. Index/locator/shared media schema setup now preserves SQLITE_BUSY/SQLITE_LOCKED as retryable store_busy. Fixed Linux SAN gate14047 is terminal0 (4128 checks); macOS56247 normal/SAN is terminal0 (1391/1504 checks). macOS index/control/audio-admission41836 is terminal0 (712/135 and audio admission gate). Final Linux normal queue/index/control/audio-admission regression is running; see /private/tmp/samosa-visual-queue-busy-regression-linux.log. Next integrate automatic visual discovery/reindex/history using the existing audio indexing path. Area4 remains active; areas5–7 and all full-rebuild milestones remain open.

Final Linux normal/regression gate53799 is terminal0: durable visual queue1715 checks, native index712, request control135 and audio discovery/freshness/reindex admission all pass. Together with Linux SAN14047 and macOS56247/41836, the SQLite schema contention fix and expanded durable visual queue/runtime slice are qualified. No live qualification handles remain. Next: automatic visual discovery/reindex, retained visual version history and derived caption retrieval; then enrichment and semantic/Chutni retrieval to close area4. Areas5–7 remain open. Reuse existing audio transaction/queue paths and working monolith media/model paths; preserve temporal Molmo2 analysis for area5. Full architecture goal remains active; no milestone/release closure claimed.

Current continuation: visual discovery/reindex/history/retained lexical candidate APIs implemented via shared audio association code (index_media_impl.h). macOS44154 normal/SAN210 each + index712/audio admission pass. Linux67936 is live; do not restart on observation timeout. Resolve any actual failure, then qualify the same implementation and update status. Next required area4 work includes enrichment and semantic/Chutni retrieval; areas5–7 and full milestones stay open.

Authoritative visual integration checkpoint: final macOS65289 terminal0 (242 normal/249 SAN); final Linux27978 terminal0 (260 normal/400 SAN), native index712 and audio admission/freshness/reindex regressions pass. Variable counters include waitpid polls; all substantive assertions are identical. No live gates remain. Logs /private/tmp/samosa-index-visual-final-{mac,linux}.log. Visual discovery, automatic initial processing of later files, queue/capture deduplication, immutable replacement, retained history/read/lexical caption search, owner/corruption refusal and SIGKILL admission rollback are qualified. Actual native workers are exercised; model callback is a fixture, and no broader model/video-understanding claim is made. index_media_impl.h shares those paths with audio, whose existing regressions pass. Next: finish area4 enrichment and semantic/Chutni retrieval through existing public SDK representations and profile checks, auditing monolith reuse first. Broader temporal visual/speech qualification is area5; complete authenticated service/load/shutdown is6; assembled native qualification/release/legacy retirement is7. The complete architecture objective and C01–C14 remain active/open.

Current authoritative checkpoint: prepared-vector memory_search_semantic is
qualified through the real public Chutni SDK on macOS/Linux normal/SAN. Final
combined logs /private/tmp/samosa-semantic-final-{mac,linux}.log; all handles
terminal, no live work. Existing SDK retrieval and control regressions pass,
and actual CLI private-profile/store-pin queries pass on both platforms.
This is the lower semantic retrieval interface; area4 remains active until
automatic enrichment, representation publication and public Inference query
embedding are done. Next reuse the existing native T5 summarizer behind public
Inference, preserving pins/controls and derived uncertainty, then adapt the
monolith enrichment/batch/provenance paths. Existing engine/model are present
and their SHA256s are recorded in the verification and plan. Areas5–7 and all
full-rebuild C01–C14 milestones remain open. No commits/push/legacy retirement.

Native summarizer continuation (2026-10-10): added Inference summary_protocol.c/h using the existing monolith scalar framing and summarize-prefix convention. Input is bounded to 60,000 valid UTF-8 bytes; replies require exact framing, nonempty valid UTF-8 and at most 8,192 bytes. Native boundary checks pass macOS/Linux normal and ASan/UBSan through test-summary-protocol and sanitize-summary-protocol. Linux used /tmp/samosa-summary-protocol-qa copied from the current source (the container /workspace mount does not expose components). This is codec qualification only: no public summarize operation, engine execution, automatic enrichment or area4 completion is claimed. Next extend the existing guardian with separately bounded protocol stdin/stdout: it currently discards stdin and merges stdout/stderr into logs, so it cannot run this engine protocol unchanged. Preserve model/executable pins, lease retention, parent-death cleanup, cancellation/deadlines, and derived uncertainty before Sources integration. Areas1–3 remain closed;4 active;5–7 pending. No live checks remain.

Public native summarizer continuation (2026-10-10): implemented optional samosa-inference-summary using the actual unchanged monolith T5 engine. Added summary.c/h/main, public summarize normalization and request controls, model/executable verification and receipts, descriptor-bound native bridge, partial/uncertain provenance with null consumed-token count, and shared guardian regular-file framed stdin/stdout separated from stderr. Guardian retains existing admission lease/group/parent-death ownership and cleans request.frame/result.frame. New summary tests cover pins, unknown overrides, malformed/empty/truncated/oversized replies, owner isolation, offline replay, changed-request conflict, cancellation/deadline, SIGKILL workspace cleanup and subsequent admission. macOS final public normal/SAN pass (see /private/tmp/samosa-summary-final-mac.log); shared guardian normal/SAN124 both platforms pass. Linux normal public361 passes; final SAN is still live handle73515 until polled. Actual macOS T5 public reference normal/SAN1049 each pass (handle14572 terminal0); separately packaged actual engine is not sanitizer-built. Actual normal/SAN JSON receipts /private/tmp/samosa-summary-real-{normal,sanitize}.json. Existing operations/admission/Whisper/vision normal regressions pass macOS/Linux; logs /private/tmp/samosa-summary-regression-{mac,linux}.log. Initial hard-linked model refused as designed; QA uses a private copy. Unbundled native runtime failed; packaged runtime eventually passed actual checks, with initial packaged startup failure retained as development evidence, not a quality claim. Linux source candidate /tmp/samosa-summary-inference-candidate required native rebuilds after a copied Mac build directory; final gates use Linux binaries. Actual Linux T5 model execution remains unqualified. Area4 remains ACTIVE: next wire automatic Sources enrichment/publication using this public transport, then public embeddings and representation/query integration. Areas1–3 stay closed,5–7 remain pending, and the full rebuild/C01–C14 remain open. No commits/push/legacy retirement.

Final summarizer checkpoint: Linux handle73515 terminal0; final public normal361/SAN365 and protocol gates pass. macOS handle6450 terminal0, final public normal360/SAN360; actual macOS model handle14572 terminal0 normal/SAN1049 each. All summarizer/regression handles are terminal; no live gates remain. Counters include native file-hash reads and timing-dependent polls, not feature totals. Next is Sources automatic enrichment/publication through the now-implemented public summary transport; public embedding and representation/query generation remain. Area4 and the full architecture goal stay active.

Sources summary continuation (2026-10-10): implemented summarize_evidence using a separately pinned summary_inference callback/process transport and existing owned literal captures/shared prepared-artifact cache. sources_literal_capture verifies snapshot/cache/selector identity without reopening origin or loading extractor. New native summary module validates exact public Inference input/model/recipe/partial-coverage DTO, stores owner/model/evidence-bound derived artifacts and preserves original selector/coverage. Scalar opening sample is1400 UTF-8 bytes and output128; recipe renamed samosa-native-summary-opening-chunk-v1 because monolith opening-sample-v3 actually uses larger batched input. This is a transport/capture prerequisite, NOT automatic enrichment or full monolith batching. Original native engine is reused unchanged. CLI private summary transport/pins wired and described; describe string split to keep strict ISO literal bounds. Fixed controlled evidence_id canonicalization as a string; empty valid snapshots now reach empty_evidence refusal, not false corrupt_snapshot. Source test fixture development failures (absolute registration path, tracked vs untracked comparison and missing evidence_id type) fixed before passing gates. Mac normal/SAN243 and control135/transcript351/public-process66/index712 pass; Linux normal/SAN243 pass, regression handle28849 still live until polled. Actual end-to-end Mac normal/SAN41 each pass via Source worker -> public Inference -> packaged native T5 checkpoint; native model engine itself not sanitizer-built. Real handle84644 terminal0. Model checkpoint /private/tmp/samosa-summary-real-model.gguf retains known pinned SHA256. Latest recipe/build/test Mac handle80955 still live; do not restart on an observation timeout. Next: reuse full monolith native_summary opening batching through public Inference, then durable automatic enrichment and existing Chutni publication for summary_short, followed by public embeddings and semantic representations/query generation. Area4 and all full-goal milestones remain open; no commits/push/legacy retirement.

Sources summary authoritative follow-up: latest scalar recipe is samosa-native-summary-opening-chunk-v1; full monolith batched opening-sample-v3 is NOT reproduced by this scalar primitive. Mac final normal/SAN243 both pass with that recipe, log /private/tmp/samosa-sources-summary-qualified-mac.log; handle80955 terminal0. Actual final Mac Sources->Inference->native T5 normal/SAN41 both pass; handle88591 terminal0. Shared regressions control135/transcript351/public process66/index712 pass BOTH platforms, logs /private/tmp/samosa-sources-summary-final-{mac,linux}.log; handles8414/28849 terminal0. Native CLI describe is valid JSON and advertises summarize_evidence/summary configuration. Linux final recipe normal243 passes; SAN recompilation/check is still live handle31403 until polled. Pending area4: full legacy opening batching through public Inference, durable automatic enrichment and Chutni summary_short publication, then embeddings/representations/query generation. Areas1–3 closed;4 active;5–7 and full C01–C14 open.

Final Sources scalar-summary gate: Linux handle31403 terminal0, normal243/SAN243 pass with latest opening-chunk-v1 recipe; log /private/tmp/samosa-sources-summary-qualified-linux.log. All Source summary, actual model and regression handles are terminal; no live gates remain. The full goal stays active. Next implement the inherited native batch/opening policy through public Inference and automatic Sources enrichment/publication; do not close area4 for the scalar cache API.

Opening summary continuation (2026-10-10): added public Inference summarize_opening with separate normalized operation identity, existing batch/memory framing C0000001, unchanged native tokenizer chunk partitioning, and ported native_summary_chunk_length/native_summarize_text_impl shrinking reducer (sentence-aware1400-byte chunks, up to4 rounds, UTF-8-safe1800-byte final cap). Recipe samosa-native-summary-opening-map-reduce-v1; provenance records map_output_bytes, reduction_rounds, chunk output budget, partitioning and output_truncated. Scalar DTO remains unchanged. Native failures refuse rather than publishing a digest from a failed reducer. Fixture tests cover same-request scalar/opening idempotency conflict, opening replay, native batch framing, shrink/no-progress/four-round cap; Mac478 normal/SAN478 and Linux478 normal/SAN483 pass, logs /private/tmp/samosa-opening-final-{mac,linux}.log. Inference operation832/admission79 regressions pass Mac, log /private/tmp/samosa-opening-regression-mac.log. Actual Mac varied long-input native model normal1286 passes with positive reduction rounds; SAN still live handle66727 until polled. Earlier repeated corpus mapped below1800 and could not prove real reduction; replaced by varied report corpus rather than weakening that assertion. Development fixture assertion/type-edit mistakes corrected before gates. Corrected scalar documentation: actual engine refuses >512-token prompts, it does not clip them; large scalar actual refusal is tested. Source scalar API remains qualified; next integrate this full public opening path into Sources durable automatic enrichment and existing Chutni summary_short publication, then embeddings/representations/query generation. Area4/full goal remain active and all full C01–C14 open. No commits/push/legacy retirement.

Final public opening-summary checkpoint: real-model handle66727 terminal0, actual Mac normal1286/SAN1286 pass with positive reduction rounds on large varied input, scalar overflow refusal and opening replay. Mac fixture handle92095 terminal0 (478/478), Linux37380 terminal0 (478/483), operation/admission1350 terminal0. All checks are terminal; no live gates remain. Counters include file-hash reads and timing polls, not feature percentages. Next is Sources automatic enrichment/publication using this public opening map/reduce path; scalar cache capture remains separate. Area4 and the full objective remain active.

Latest Sources continuation, 2026-10-10: opening-summary capture/cache and public
summary_short publication through the actual SDK are qualified on macOS/Linux
normal/SAN310 each; final logs /private/tmp/samosa-summary-publication-final-{mac,linux}.log.
Actual Mac Sources->public Inference->inherited native T5 scalar/opening47/47 pass;
shared transcript/visual publication regressions pass both platforms. All handles
terminal, no live gates. Next finish area4 automatic retained-version enrichment
using existing lease/retry/receipt machinery and monolith behavior, then public
embedding/representation publication and text-query embeddings. Area4 active;
areas1–3 closed,5–7 pending; full C01–C14 open. See newest verification checkpoint.

Latest automatic-enrichment checkpoint, 2026-10-10: Sources now implements
index_enrich_step/index_enrich_queue_step/index_enrichment_status with retained
version/model identity, durable capture before SDK publication, retry/history and
existing index lease. Text/reindex/actual SDK/SIGKILL/cache-independence/history
normal/SAN pass both platforms; actual Mac native T5 queue normal63/SAN63 pass.
All handles terminal, no live gates. Next qualify bounded document/OCR enrichment
and remaining controls/runtime, then public embedding generation/representation
publication/text-query embeddings. Area4 remains active; full goal unchanged.
See newest verification checkpoint and contracts/enrichment-v1.md in Sources.
