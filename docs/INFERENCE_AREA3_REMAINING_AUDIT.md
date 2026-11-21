# Inference lifecycle: remaining requirements

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
| Extend guardian/activation ownership to the supported provider set | managed_host/managed_provider, summary_host, speech_host, decision_host and visual_host reuse Activation/EngineProcess and existing engine loops. Verified whisper.c owns a per-request guardian. | Legacy backend_start/backend_stop still own the selected primary engine in Gateway. Extracted legacy Whisper runner retains original direct fork/wait and caller coordination; it is not yet the verified guarded profile. All supported deployment paths have not converged on those owners. |
| Start/verify/load/unload/stop through public controls, with observed identity, health and successful qualification | Registry has managed text/embedding, summary, speech, text/image decision and visual lifecycle profiles. Their start hooks run actual bounded capability probes before qualified state; owned status rechecks retained identity. | Whisper entries in registry_host.c configure only WhisperConfig, without the common lifecycle profile. Nonresident CLI behavior must be represented honestly; do not invent a persistent Whisper server. Legacy App paths still use availability/handshake traces and separate launch state. Configuration or saved activation alone cannot prove current readiness. |
| Crash recovery, failed replacement, engine/owner death, stale readiness, mutation and unrelated-process survival | Existing owner/activation tests cover these within their qualified profiles. LIFE-07c covers resident visual control/parent death; LIFE-07d fixes the reproduced invalidated-session close leak; LIFE-07e preserves legacy Whisper control while retaining verified short-clip regressions. | These are profile-scoped results, not proof for every default/legacy launch path. The legacy Whisper runner lacks guardian parent-death cleanup. Replacing its long-window/text/SRT contract with the30-second structured API would lose behavior. Full supported-path native qualification remains required. |
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
2. **Move remaining primary-backend ownership.** backend_start currently forks
   Qwen, Maple or llama-server and stores PID/group/current selection in Gateway.
   The llama argv helper is already extracted; Qwen/Maple launch bodies remain
   there. Reuse those exact flags/environment, startup probing, group/reap and
   persisted settings, then wire the existing managed owner where contracts
   match. Model selection and App presentation remain explicit caller policy.
   Qwen's original native source/cache/vision/kernels and useful tests are now
   extracted in LIFE-07h. Maple's original implementation/build/native tests are
   extracted in LIFE-07i with declared native dependencies and preserved inputs.
   LIFE-07j consolidates remaining primary launch/group/reap/probe ownership;
   HTTP compatibility alone does not complete that source ownership requirement.
3. **Consolidate the extracted legacy Whisper execution with guarded ownership.**
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
