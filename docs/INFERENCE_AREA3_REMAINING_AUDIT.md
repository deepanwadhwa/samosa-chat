# Inference lifecycle: remaining requirements

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

Historical LIFE-07l progress, 2026-11-21: legacy Whisper now reuses EngineProcess with
actual PID/workspace reservation, parent-death/group cleanup and explicit
handoff for caller-prepared audio. Original long txt/SRT/16-MiB/UTF-8/return/CLI
semantics remain; borrowed durable inputs stay borrowed. Final native/SAN and
independent522, original54-assertion audit, old-runner leak proof, actual Gateway
SIGKILL/prepared cleanup, full voice/audio/video/cache/cancel/restart, actual
Whisper and native packaging/install pass. See LIFE-07l evidence README.
Next default visual guardian integration must preserve wide/native-video
contracts. Common lifecycle/readiness/identity, guardian-death, pre-admission
recovery/shared coordination and full area3 remain open.

Historical LIFE-07k progress, 2026-11-21: primary ownership now reuses EngineProcess with
actual PID/marker reservation, retained ownership/lifeline, unlimited lifetime,
original append/fallback logs and8-second TERM grace. Final native/SAN676 and
standalone676, guardian525, existing modality profiles, assembled Gateway
SIGTERM/SIGKILL/helper cleanup, selection/rollback/App/LAN/install and short
actual Qwen generation pass. See the LIFE-07k evidence README. Next guard the
legacy Whisper/visual paths without shrinking contracts. Common activation/
readiness/identity, guardian-death policy and full supported-path qualification
remain open; no area3 completion claim.

Historical LIFE-07j progress, 2026-11-21: primary launch/probe/group/marker/stop/PID state
mutations now belong to Inference's public backend_process owner, with original
caller policy/shared-mutex callbacks. A reproduced helper leak is fixed using
existing WNOWAIT ownership. Native normal/SAN398 checks, standalone archive,
original selection/settings/LAN/app/llama, actual retained Qwen old/new and
package/install pass; see qualification/life07j-primary-process in Inference.
Next LIFE-07k reuses existing guardians/host adapters under
INFERENCE_GUARDIAN_COMPATIBILITY.md. This is not common parent-death/actual-
generation readiness/identity qualification for all default paths; area3 is open.

LIFE-07i progress, 2026-11-21: original Maple source/build/native test ownership
now belongs to Inference.18 supported builds,9 native runs,3 sanitizer checks,
fresh source/data builds, Gateway quarantine/settings and package/install pass;
32 parent copies (7115 lines) are retired. Two original scratch compile failures
are reproduced/preserved separately. No actual Maple checkpoint/catalogue claim.
Next LIFE-07j extracts existing primary launch/group/reap/probe ownership with
unchanged flags, environment, persisted selection/settings and startup behavior.
Evidence: Inference qualification/life07i-maple-sources/README.md. Area3 stays open.

LIFE-07h progress, 2026-11-21: Qwen's original model/cache/vision/kernels/support
and useful native tests now belong to Inference. CPU/OpenMP/schedule builds,
normal/SAN regressions, fresh independent source builds, actual old/new Mac
model CLI compatibility, Maple cache consumer and package/install checks pass.
16 parent source/test/fixture copies (10297 lines) are retired. Next LIFE-07i
moves original Maple sources/builds/tests, then primary launch/group/reap/probe
ownership. This does not close any full lifecycle requirement or area3.
Exact scope: Inference qualification/life07h-qwen-sources/README.md.

LIFE-07g progress, 2026-11-21: visual engine source/build consolidation below
is complete. Delegated native builds, original component normal/SAN and real
heading gates, rebuilt actual VisionPsy/Molmo owner normal/SAN, gateway workflows
and packaging/install pass.18 byte-identical parent files are retired, including
13 engine files,4 tests and the original packer now owned by Inference. Exact
evidence/scope: Inference qualification/life07g-visual-builds/README.md.
Next is item2; all four lifecycle requirements and area3 remain open.

Read-only implementation audit, 2026-11-21, after LIFE-07f. Baselines are Chat
ce06f94/Inference4ad140d plus the recorded visual-budget extraction. This is not
an area3 completion claim or a rerun of all historical qualification.
The four requirements below are unchanged from Inference's INFERENCE_MODULE_PLAN.
User direction remains extraction/reuse, no Python and native Mac first.

| Area3 requirement | Current implementation evidence | What is still unproven/incomplete |
| --- | --- | --- |
| Extend guardian/activation ownership to the supported provider set | managed_host/managed_provider, summary_host, speech_host, decision_host and visual_host reuse Activation/EngineProcess and existing engine loops. Verified whisper.c owns a per-request guardian. | Default backend_process.c now reuses EngineProcess in LIFE-07k, preserving its original primary-server contracts and parent-death cleanup. Common Activation/readiness is still open. LIFE-07l also guards the legacy Whisper CLI and explicitly handed-off prepared audio, preserving long-window/txt/SRT/16-MiB behavior. LIFE-07m now guards both default visual profiles and prepared PDF pages without narrowing wide/native-video contracts. Common controls/identity/readiness remain open. |
| Start/verify/load/unload/stop through public controls, with observed identity, health and successful qualification | Registry has managed text/embedding, summary, speech, text/image decision and visual lifecycle profiles. Their start hooks run actual bounded capability probes before qualified state; owned status rechecks retained identity. | Whisper entries in registry_host.c configure only WhisperConfig, without the common lifecycle profile. Nonresident CLI behavior must be represented honestly; do not invent a persistent Whisper server. Default App paths still use legacy health/handshake checks and trusted process state outside the common lifecycle qualification controls. Configuration or saved activation alone cannot prove current readiness. |
| Crash recovery, failed replacement, engine/owner death, stale readiness, mutation and unrelated-process survival | Existing owner/activation tests cover these within their qualified profiles. LIFE-07c covers resident visual control/parent death; LIFE-07d fixes the reproduced invalidated-session close leak; LIFE-07e preserves legacy Whisper control while retaining verified short-clip regressions; LIFE-07j fixes reproduced primary descendant cleanup with WNOWAIT; LIFE-07k adds actual primary owner-death/group/marker cleanup, PID reservation, pending-stop ownership and unrelated-process survival. | These are profile-scoped results, not proof for every default/legacy launch path. LIFE-07l qualifies legacy Whisper owner-death/group/workspace/prepared-audio cleanup; LIFE-07m also qualifies default visual owner-death/group/workspace/prepared-page cleanup. Common guardian-death/pre-admission recovery remain open. Replacing its long-window/text/SRT contract with the30-second structured API would lose behavior. Full supported-path native qualification remains required. |
| Retain last-good configuration and refuse ambiguous interrupted generation | Activation owns revision/last-good semantics for the managed/resident profiles; existing transports invalidate interrupted framing and retain cleanup ownership. | Original App-selected backend/configuration and legacy on-demand execution have not fully moved behind the common owner. Their persistent selection/caching/error semantics must be preserved when consolidating, not silently replaced by a new global configuration model. |

The new Mac visual policy API owns the original probes, thresholds, reserve and
bounded reclaim wait. It observes resources; it does not reserve memory or prove
global fairness. The original turn-wide generation gate, backend pause/resume
and lower-side retry still reside in Gateway. Global reservations, bounds and
fair scheduling remain the full separate area4 acceptance responsibility.

## Concrete ownership work, in order

1. **LIFE-07g: completed visual engine source/build extraction.** At the audit
   baseline, parent builds still compiled src/visionpsy and src/molmo2 although their
   native implementations already exist in Inference/engines. Direct comparison
   finds all3 VisionPsy and all10 Molmo2 C/C++/Objective-C++ source/header files
   identical, plus stb_image.h/tok.h/tok_unicode.h. engines/json.h has the already
   documented named Unicode-buffer portability change. Reuse the owner files.
   Delegate the existing Mac development builds and useful original component
   tests to owner targets with explicit native MLX source/build dependencies.
   Keep existing output names, flags, media frameworks, kernel pairing and
   packaging behavior. Retire each parent duplicate after original behavior and
   assembled callers pass. Use the existing independent release builders too;
   do not require a new model or rebuild working algorithms.
2. **Complete common primary-backend lifecycle integration.** LIFE-07j extracts
   Qwen/Maple launch bodies, existing llama helper reuse, group/probe/marker/reap
   and authoritative PID mutation into backend_process.c/public API. Gateway
   passes trusted inputs and retains original selection/settings/fingerprint/
   rollback policy and shared coordination callbacks. LIFE-07k reuses EngineProcess
   for primary parent-death/group/marker ownership while preserving unlimited
   lifetime, actual PID and log/termination behavior. Next complete common
   readiness/identity controls and other default paths using existing owners,
   extending only required
   boundaries per INFERENCE_GUARDIAN_COMPATIBILITY.md. Model selection and App
   presentation remain explicit caller policy.
   Qwen's original native source/cache/vision/kernels and useful tests are now
   extracted in LIFE-07h. Maple's original implementation/build/native tests are
   extracted in LIFE-07i with declared native dependencies and preserved inputs.
   Source/build and legacy launch ownership are now extracted; common guarded
   controls/identity and successful-generation readiness are still required.
3. **Complete common controls for the guarded legacy Whisper/visual paths.**
   LIFE-07l/m finish guardian ownership under the original contracts.
   Reuse whisper.c and EngineProcess rather than making a second worker/server.
   Preserve text/SRT, selected model fingerprint, long windows, output limits,
   exact-child cancellation and existing durable audio assertions. Adapt the
   existing owner boundary where required instead of truncating inputs. Specify
   nonresident start/readiness/stop semantics before implementing public controls;
   successful configuration/metadata is insufficient readiness.
4. **Finish shared lifecycle/admission wiring and qualify area3 in full.** Move
   the existing turn gate/reclaim/pause/resume coordination into its owner,
   retaining traces and resource retry behavior. Keep actual generation probes,
   identity/mutation and last-good/cleanup checks for every declared profile.
   Port required Python drivers before executing them; preserve assertions.
   Recheck all four area3 requirements and supported native Mac paths before
   closing the area. Linux/Docker work is deferred by user direction.

The first step removes an observed duplicate implementation/build path, not a
new feature. It follows the extraction-first rule and does not postpone usable
ownership moves behind unrelated model-distribution work. The remaining steps
are implementation gaps, not blockers requiring user approval.

## Scope of current evidence

LIFE-07f's native normal/SAN threshold/wait and live Mac sample checks, source
comparison, independent source build, gateway Molmo/VisionPsy and packaging/
installation gates are in Inference qualification/life07f-visual-budget. Earlier
profile qualification remains in its respective versioned evidence folders.
The registry/host implementation inspection above establishes current code
boundaries; it does not recast old passes as a complete current suite.

Sources' completed areas are not reopened. Inference areas4–7, project-wide
Python tooling/CI removal, Agent's independent repository/release, the thin App,
pinned assembly and legacy retirement remain required. Inkling-2-Mini stays a
post-refactor catalogue follow-up. The full goal remains unchanged.
