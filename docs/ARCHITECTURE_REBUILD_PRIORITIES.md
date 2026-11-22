# Modular rebuild: priorities and agent handoff

Updated: 2026-11-21. Requested by the user as a shared record of the prioritized
remaining work. This file records the agreed direction and completion criteria;
it does not claim that the work below has been executed.

The user's explicit direction, reaffirmed 2026-11-20: **extract the working
monolith into separate modules/repositories. The monolith worked well; its size
and coupled ownership are the problem. Preserve its useful behavior and reuse
its implementation rather than figuring out or rebuilding the system again.**

The priority is to make the current work recoverable and reproducible, then finish
extracting working implementations while keeping the existing product usable.
Preserve the established component order:

**Sources → Inference → Agent → App → assembly and legacy retirement.**

Sources is recorded complete. Preserve unfinished Agent/App work while finishing
Inference. Do not reopen completed Sources areas without a demonstrated defect,
changed dependency or integration requirement.

## Governing rule: extraction first, preserve behavior

This rule applies to every priority below, including lifecycle, scheduling,
sessions, service controls, Agent workflows and App. Their checklist descriptions
are acceptance responsibilities, not instructions to design new implementations.

- Locate the working monolith implementation and useful tests, then inspect the
  destination module for code already extracted. Move, wire or consolidate those
  implementations before considering new code.
- Preserve working algorithms, protocols, limits, model choices and workflow
  semantics. Make the smallest interface/configuration/dependency changes needed
  to give each module explicit ownership and an independent build.
- For every extraction, record the original entry points/files, destination,
  boundary adaptation, existing behavioral tests and assembled caller. Verify
  the same useful behavior independently and through product integration.
- New code requires a specific missing boundary requirement or demonstrated
  defect, with an explanation of why the existing code cannot satisfy it.
  Structural preference or the monolith's size is not such an explanation.
- Track genuinely new capabilities separately from extraction. Do not turn a
  move into a scheduler redesign, new model project, installer replacement or
  speculative hardening effort. Do not delay moving usable code behind unrelated
  model/package qualification. Keep required release gates truthful and open.
- Avoid copying the shared `Gateway` object into smaller projects. Extract its
  working implementations with explicit owner inputs/state and public interfaces.
- Keep the product usable during migration. Retire each old path after its
  extracted replacement passes the relevant behavioral and integration checks.

## Governing constraint: no Python anywhere in the project

User direction, 2026-11-20: **do not use Python in this project at all.** This
applies to runtime code, development/build tooling, installation, packaging,
qualification scripts, tests and CI. Do not add or invoke Python helpers, install
Python dependencies or make a component require Python to build/test/release.

Reuse existing native C/C++ implementations and shell tooling. Where a remaining
Python script supplies required behavior, port that specific behavior and its
useful assertions into the owning component's native or shell tools, verify the
replacement, update callers and remove the Python path. Do not replace working
native engines or redesign workflows to remove a Python wrapper. Check declared
third-party build dependencies for Python requirements and record any unresolved
constraint; do not silently introduce a Python-dependent build.

Existing Python checks and older recorded passes are historical evidence only.
They do not satisfy the new Python-free build/test/release requirement. Keep
necessary coverage open until its native/shell replacement is verified. This
constraint applies throughout the priorities below, not only to shipped binaries.

## Required target: this Mac first

User direction, 2026-11-20: **the refactor must work on this machine first.**
Implement, build, test, install and qualify the components and assembled product
natively on this Mac. Do not start Docker, containers or VMs for this work.
Docker/Linux support and verification are deferred and do not block the Mac
refactor. Earlier cross-platform checklist requirements and evidence remain
historical or deferred; they are not instructions to run Linux qualification now.
Preserve portable existing code when moving it, without adding portability work
that delays the native Mac path. Report current supported/verified scope honestly.

## How agents should use this file

- Read this priority order, the verification plan's current Resume checkpoint,
  and the active component's extraction checkpoint before implementing work.
- Recheck Git status and relevant code before acting; the observations below
  describe the 2026-11-20 review and will become historical.
- Choose one bounded assignment. Identify its existing implementation, owner,
  public interface, acceptance checks and retirement condition before editing.
- Extract and adapt existing working code and useful tests. Record a concrete
  missing requirement or demonstrated defect before adding a new implementation.
- Preserve unrelated work. Use isolated fixture state rather than personal
  files, normal installed state or active user conversations.
- Record exact revisions, commands, results and remaining work at handoff.
  Historical passes, planned checks and missing prerequisites are not current
  passing results. Do not mark an entire milestone complete for one passing slice.

The [architecture plan](ARCHITECTURE_REBUILD_PLAN.md) defines C01–C14 and the
full acceptance scope. The [verification record](ARCHITECTURE_REBUILD_VERIFICATION.md)
holds execution evidence. This file orders the remaining work; it does not replace
those contracts or add an independent set of completion gates.

## Review baseline

The latest recorded implementation checkpoint is 2026-10-25. The 2026-11-20 review
inspected the checkout and saved evidence; it did not rerun builds or test suites.

| Component | Observed position |
| --- | --- |
| Sources | Separate Git repository/submodule. All seven component acceptance areas recorded complete, including standalone release qualification. |
| Inference | Separate Git repository/submodule. Areas 1–2 complete; area 3 engine lifecycle/readiness active; areas 4–7 remain open. Substantial local changes are uncommitted. |
| Agent | Substantial native implementation, including durable conversations/runs, evidence, finding and checked copy/move/undo. Still an untracked directory in the parent checkout, without a separate Git repository. |
| App | Replacement component not built yet. Existing gateway still serves the product. |

The parent branch was 27 commits ahead of its locally recorded `origin/main`;
this is not a fresh remote comparison. Parent integration changes and architecture
documents were uncommitted. The gateway still contained 26,660 lines.

LIFE-07a visual transport cancellation/deadline checks have saved passing evidence.
The subsequent specialist ownership test's saved macOS/Linux attempts failed
compilation on missing `posix_spawn`/`environ` declarations. Treat that as the next
specific issue to inspect, not proof that the complete current tree has been tested.

## Priority order

### 1. Preserve the current work in Git

This is the immediate maintenance risk. Inventory changes in each repository and
preserve intentional work in explicit commits. Distinguish passing checkpoints
from unfinished work; do not label a preservation commit as a qualified release.

Commit component changes before updating parent submodule references. Check
generated files, model assets and credentials before staging; avoid a blanket add.
Preserve Agent's files now; finish its component implementation and independent
repository work in the established sequence. Establish a remote backup of the
intentional source checkpoints and verify its refs.

**Done when:** another checkout can recover every intentional source change,
unfinished work is identified, and parent integration references recoverable
component revisions.

### 2. Establish a buildable baseline

Inspect and repair the unfinished visual ownership test's declaration failures.
Run its normal and sanitizer checks; compiling successfully alone does not prove
ownership, cleanup or recovery behavior.

Verify the existing gateway build, delegated supervisor/summary tests, installer
and package staging against the exact component revisions using native/shell
checks. Replace Python-backed entry points before invoking them. Keep new results
separate from historical evidence. Record any failing prerequisite explicitly.

Relevant starting points:

- `components/samosa-inference/tests/test_specialist_owned.c`
- `components/samosa-inference/src/specialist_supervisor.c`
- `components/samosa-inference/include/samosa_inference/specialist_supervisor.h`
- `components/samosa-inference/docs/qualification/life07a-specialist-owned/`
- `components/samosa-inference/docs/qualification/life07a-specialist-control/README.md`

**Done when:** an exact revision set has known passing checks and an explicit
list of remaining failures.

### 3. Make progress and compatibility maintainable

Keep one short current checkpoint linking to detailed evidence: active assignment,
files, acceptance criteria, results and next action. Mark older next-action entries
as history so a resuming agent does not restart completed work.

Establish component CI for public contracts, standalone builds and meaningful
normal/sanitizer checks. Keep heavier actual-model qualification separate. Parent
integration checks must cover pinned component compatibility and product packaging.
CI, test orchestration and release tooling must also be Python-free.
Apply these checks as components become ready rather than postponing CI until
the full split is finished.

**Done when:** a maintainer can resume without reconstructing project history,
and CI catches broken component boundaries and integration packaging.

### 4. Finish Inference lifecycle ownership

LIFE-07a owned endpoints/close, LIFE-07b original visual protocol extraction and
LIFE-07c component visual residency now qualify on this Mac. The resident owner
reuses existing verification/preparation/replies, guardian workspace/duplex
cleanup, resource leases and GPU cache policy. Public readiness requires actual
generation; native normal/SAN and retained actual models pass.
Next audit and extract remaining gateway-owned visual/Whisper lifecycle through
those existing module interfaces. Component readiness does not retire the old
gateway process state. Then close other remaining area3 checklist gaps.

Preserve working engine algorithms and protocols. Public readiness requires a
successful bounded inference request. Exercise failed startup, cancellation,
engine death, parent death, failed replacement and unrelated-process survival.

**Done when:** Inference owns launch, readiness, residency and shutdown through
public interfaces without gateway process state. This closes area 3 only after
all of its checklist requirements pass.

### 5. Complete Inference scheduling and admission

Extract and consolidate the existing admission, lease and scheduling code first;
do not design a new scheduler merely to split repositories.
Coordinate reservations across text, vision, speech and background work. Bound
active requests, queued work and retained output. Begin with the planned
configurable single large-model slot on constrained hosts.

Verify fairness, queued cancellation, expired deadlines, memory refusal and
crash cleanup. Release exactly the failed operation's resources. Do not hold a
model reservation while waiting for user approval.

**Done when:** mixed workloads stay within configured limits, background work
progresses and failed operations release their resources. Cover the complete
area 4 acceptance checklist.

### 6. Finish Inference sessions, streaming and service controls

Move the existing generation/session, streaming and transport/control paths into
their owner. Inventory existing behavior before implementing any missing check.
Preserve exact prompt counting, context/output reservations and owner-bound
session revisions. Qualify incremental output, bounded buffering, slow consumers,
disconnects, UTF-8 boundaries and exactly one terminal result. Qualify cold recovery
separately from reuse of a running model.

Complete authenticated bounded ingress, operation-scoped status/cancellation,
retry identity and shutdown that stops admission and cleans up owned engines.
Public requests cannot select private executable commands or storage paths.

**Done when:** clients use Inference independently, recover from interruptions
and cancel their own work without disturbing others. Cover areas 5 and 6 in full.

### 7. Release Inference independently

Build, test and package outside this checkout using declared dependencies only.
Include public headers, native dependency declarations, licenses and supported
platforms. Port remaining Python tooling and test drivers without losing their
behavioral coverage; remove Python invocations and dependencies from the component.
Explicitly distinguish native control tests from actual-model execution on this
Mac; defer other-platform verification.

Resolve production distribution gaps for changed protocols, including the local
T5 and decision-engine candidates wherever required by the final interface. Reuse
existing models and packaging machinery. Package qualification supports extraction;
it must not delay otherwise reusable ownership moves behind unrelated package work.

Pin the qualified revision in product integration. Rerun checks affected by changes;
broaden verification when failures or unresolved integration concerns justify it.

**Done when:** area 7 passes and a consumer can install and use the released
component without private parent or sibling source files.

### 8. Finish Agent and establish its separate repository

Reuse both the substantial existing Agent implementation and working gateway
task policies, workflow code and tests. After Inference completion, close
remaining task policies, organization/scheduling, admission and service gaps.
Interactive and scheduled actions must share the checked executor. A one-time
plan approval does not authorize arbitrary future scheduled actions.

Verify conversation revisions, crash recovery, exact retries, approvals, undo and
Sources reconciliation. Agent owns conversation/run state and consumes public
Sources/Inference interfaces; it must not read their private stores.

**Done when:** Agent builds and releases independently, its supported headless
workflows pass without the gateway, and its separate repository/revision is
recoverable and pinned for integration.

### 9. Build the thin App

Extract and adapt the existing working UI, launcher and ingress; do not rebuild
the product's workflows from scratch. Put authoritative conversation state, task execution and
model lifecycle behind their owning APIs. App displays committed progress,
reconnects to runs, renders citations, collects approvals and exposes generic
capability/setup status.

The launcher distinguishes processes it started from existing headless services.
Verify reload, reconnect, a second client, optional component failure and minimal
installation without model downloads.

**Done when:** supported workflows operate through the released components,
with App acting as their client and owning product assembly.

### 10. Qualify assembly, migrate valuable state and retire replaced paths

Pin compatible releases and test ordinary chat, document/folder questions, search,
media, approved copy/move/undo and scheduled work through the real App. Include
restart, interrupted operations and resource limits.

Decide explicitly how existing conversations, settings and useful datasets are
migrated or retained. Test recovery before switching defaults.

As each replacement passes integration, remove its old routes, lifecycle code,
private storage readers, duplicate installers and implementation-coupled tests.
Retain useful behavior tests against public interfaces. Remove the gateway and
alternate `jobsd` build once supported replacement workflows qualify. Temporary
development coexistence must not become two permanent default implementations.

**Done when:** each responsibility has one maintained implementation, releases
are reproducible, C01–C14 pass in full and the product no longer needs the monolith.

## Immediate next batch

1. LIFE-07k primary, LIFE-07l legacy Whisper and LIFE-07m default visual guardian
   integration are qualified for their recorded profiles. Preserve the
   [original contracts](INFERENCE_GUARDIAN_COMPATIBILITY.md); reuse existing
   owners and tests. Do not rebuild the models, guardian or server.
2. Follow the [remaining lifecycle audit](INFERENCE_AREA3_REMAINING_AUDIT.md)
   through common activation/readiness/observed identity/stale rejection,
   last-good/guardian-death qualification and shared turn/pause/resume/retry
   coordination. Keep primary selection/settings/rollback explicit caller policy.
   Guardian ownership and HTTP200 do not close all common lifecycle requirements.
3. Verify affected behavior on this Mac, preserve an exact passing revision and
   retire each old lifecycle path only after its public replacement passes.

Do not expand this batch into new models, unrelated features or a fresh architecture
redesign. Subsequent work follows the ordered priorities and existing acceptance gates.

LIFE-07m checkpoint, 2026-11-21: default VisionPsy/Molmo launch ownership now
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

Previous native Mac progress, 2026-11-21: LIFE-07j extracts primary launch/probe/group/
marker/reap behind Inference's public owner, preserving original caller policy
and mutex coordination. A reproduced stubborn-helper leak is fixed using the
existing WNOWAIT ownership pattern. Native normal/SAN398 checks, independent
archive, original selection/rollback/settings/LAN/app/llama, source staging/
clean install and retained actual Qwen before/after checks pass. See
[primary-owner evidence](../components/samosa-inference/docs/qualification/life07j-primary-process/README.md).
This does not close common guardian/readiness/identity or full area3. The next
contract-preserving integration is recorded above. No Python/new model/Docker ran.

Native Mac progress, 2026-11-21: LIFE-07i extracts Maple's existing implementation,
packer and original native tests/diagnostics, preserving algorithms and flags.
18 supported C++ builds,9 native runs,3 sanitizer checks, independent source/data
builds, Gateway quarantine/settings and package/install/upgrade pass.32 parent
source/test/header copies (7115 lines) are retired. Two inherited scratch compile
failures are reproduced/preserved, not presented as passing qualification.
See [Maple source evidence](../components/samosa-inference/docs/qualification/life07i-maple-sources/README.md).
Actual Maple checkpoint/catalogue and full area3 qualification remain open.

Native Mac progress, 2026-11-21: LIFE-07h extracts Qwen's working native model,
cache, kernels, vision/support and original useful tests. CPU/OpenMP/schedule
builds delegate to Inference; Maple consumes the owner cache. Original bodies/
flags are preserved. Independent native builds, original tests normal/SAN,
actual old/new Mac model CLI compatibility, runtime settings and package/clean
install/upgrade pass.16 parent files (10297 lines) are retired. See
[Qwen source evidence](../components/samosa-inference/docs/qualification/life07h-qwen-sources/README.md).
Maple source/build and primary lifecycle ownership remain next; area3 stays open.

Native Mac progress, 2026-11-21: LIFE-07g delegates visual engine builds/tests to
Inference with explicit existing MLX inputs, preserves output/flags/media/kernel
pairing, moves original tests/packer and retires18 identical parent files
(3884 lines). Fresh standalone builds, original components normal/SAN and real
heading gate, rebuilt actual VisionPsy/Molmo owner normal/SAN, gateway workflows,
package and clean-install/upgrade checks pass. See
[visual source/build evidence](../components/samosa-inference/docs/qualification/life07g-visual-builds/README.md).
Area3 remains open; remaining primary-backend, Whisper and shared coordination
work is next. No Python, new models or Docker work ran.

Native Mac progress, 2026-11-21: LIFE-07f extracts the existing visual resource
policy into Inference: native probes, adaptive detail/reserve and bounded reclaim
wait with explicit sampling/control/trace inputs. Original bodies and thresholds
are preserved. Native normal/SAN25-case boundaries, actual Mac probes, fresh owner
build, original Molmo/VisionPsy workflows and clean packaging/install pass.
See [budget evidence](../components/samosa-inference/docs/qualification/life07f-visual-budget/README.md).
The [full remaining-area3 audit](INFERENCE_AREA3_REMAINING_AUDIT.md) records
implementation gaps and the next concrete extraction. Area3 and all later full
component/Agent/App/assembly/retirement requirements remain open.

Previous native Mac progress, 2026-11-21: LIFE-07e extracts the original shared Whisper
runner into Inference with explicit configuration/coordination callbacks.
Source audit proves original execution/cleanup, model fingerprint, busy/cancel
and return behavior retained. Native normal/SAN, standalone owner, original
voice/durable-audio workflows, clean packaging/install and retained actual Mac
Whisper text/SRT recognition checks pass. The two workflow drivers' Python
fixtures/assertions are ported to Node/shell without lost coverage. No Python or
Docker was invoked. Actual SRT timing accuracy is not established by that check.
See [Whisper extraction evidence](../components/samosa-inference/docs/qualification/life07e-whisper-runner/README.md).
Shared admission, guarded execution consolidation, full area3/later areas,
Python removal/CI, Agent/App/assembly and retirement remain open.

Previous native Mac progress, 2026-11-21: LIFE-07d extracts the original visual launch/
close bodies into Inference with thin gateway calls and unchanged profiles,
traces and product policy. A reproduced inactive-session cleanup bug is fixed.
Native normal/SAN, standalone archive, gateway summary/runtime/Molmo/VisionPsy
workflows and clean package/install checks pass. VisionPsy's Python assertions/
worker were ported to Node/shell. Its stale image text-only expectation was
reproduced on the original gateway; the test now retains document text-only
coverage and checks the established image OCR-plus-visual floor.
See [launch extraction evidence](../components/samosa-inference/docs/qualification/life07d-visual-launch/README.md).
Next is the original shared Whisper runner. Full area3/later areas and the
overall refactor remain unfinished.

Previous native Mac progress, 2026-11-20: LIFE-07c qualifies optional component resident
VisionPsy/Molmo2 ownership by sharing the original one-shot verification,
P6 preparation, request encoders and reply validation. Existing Activation,
guardian and specialist transport provide actual-generation readiness,
last-good state and cleanup. Native owner/public-registry normal/SAN, isolated
actual retained Mac models, fresh source-subset builds and affected original
native regressions pass. Model algorithms and gateway policy are unchanged.
See [resident ownership evidence](../components/samosa-inference/docs/qualification/life07c-visual-residency/README.md).
The next [read-only lifecycle audit](EXTRACTION_GATEWAY_LIFECYCLE_AUDIT.md) records
original entry points and concrete input/limit differences before any cutover.
Full Inference area3, areas4–7, Python removal/CI, Agent/App and assembled
retirement remain open. Inkling-2-Mini is still deferred until after completion.

Previous native Mac progress, 2026-11-20: LIFE-07b moves the original Molmo2 analyze and
VisionPsy inspect functions into Inference with thin gateway adapters and explicit
trace callbacks. Native codec normal/SAN, standalone archive/control regressions,
Chat summary/runtime/Molmo assertions and clean/atomic/PATH/runtime-only install
and upgrade checks pass. The release packager, manifest/version tests and HTTP
fixture are now native shell/C; their replaced Python packager/test were removed.
See [visual extraction evidence](../components/samosa-inference/docs/qualification/life07b-specialist-protocol/README.md)
and [native packaging replacement](EXTRACTION_NATIVE_PACKAGING.md).
This is a bounded passing slice, not complete Inference or full-refactor closure.

Current qualified Inference pin: `f06d5c1` (LIFE-07l), committed and backed up on
`origin/refactor/extract-working-modules`. Parent integration uses that revision.
The original source-preservation pins below are historical recovery checkpoints.

Source preservation: the parent/Agent checkpoint `ace72ef` and Inference checkpoint
`214fe75` are committed and backed up on their respective repositories' remote
`refactor/extract-working-modules` branches. The parent pins that Inference
revision and Sources `520550d`. Agent is now tracked in the parent pending its
independent-repository extraction. These commits preserve pre-existing work;
they do not qualify every preserved feature or remove the remaining Python tools.
The four pre-existing modified Chutni files remain unchanged and outside these
commits, with a verified local recovery copy in the snapshot noted in the
verification checkpoint.

Previous Inference extraction commit: `3e9b6f1` on the same refactor branch. The
parent's updated pin assembles that implementation through public headers and
the owner archive. Remaining Python drivers/default CI still require migration;
passing native gates do not imply a Python-free whole project yet.

## Deferred follow-up after refactor completion

User request, 2026-11-20: after this refactor is finished, add
[thinkingmachines/Inkling-2-Mini](https://huggingface.co/thinkingmachines/Inkling-2-Mini)
to the model catalogue through the independently released Inference component.
Start this only after the refactor's assembly and retirement completion gates
pass. Preserve the Python-free project constraint. At that point inspect model
format, license, native backend support, platform/resource requirements and the
existing catalogue/installer interfaces, then reuse those interfaces to qualify
and add the model. The URL records the requested model; compatibility, availability
and catalogue qualification have not been established by this handoff.

## Related records

- [Start and resume guide](ARCHITECTURE_REBUILD_START.md)
- [Architecture status](ARCHITECTURE_REBUILD_STATUS.md)
- [Architecture and completion milestones](ARCHITECTURE_REBUILD_PLAN.md)
- [Verification and Resume checkpoint](ARCHITECTURE_REBUILD_VERIFICATION.md)
- [Inference extraction checkpoint](../components/samosa-inference/docs/EXTRACTION_STATUS.md)
- [Inference seven-area acceptance](../components/samosa-inference/docs/INFERENCE_MODULE_PLAN.md)
- [Agent implementation and public usage](../components/samosa-agent/README.md)
