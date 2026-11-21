# Remaining gateway lifecycle: reuse audit

Progress, 2026-11-21: LIFE-07i completes Maple source/build/native test ownership
in Inference with preserved implementation and inputs.18 supported C++ builds,
9 native runs,3 sanitizer checks, independent builds and Gateway/package/install
checks pass;32 parent copies (7115 lines) are retired. Two inherited scratch
compile failures are reproduced/preserved. See Inference qualification/
life07i-maple-sources/README.md. Next LIFE-07j moves remaining primary launch/
group/reap/probe ownership; area3 and real-checkpoint readiness remain open.

Progress, 2026-11-21: LIFE-07h moves the original Qwen native implementation,
cache/vision/kernels/support and useful tests to Inference with preserved flags
and behavior. Native normal/SAN, independent builds, actual old/new Mac model
CLI, Maple cache consumer and package/install checks pass;16 parent copies
(10297 lines) are retired. Next LIFE-07i moves original Maple sources/builds/
tests, then primary launch/group/reap/probe ownership. See Inference
qualification/life07h-qwen-sources/README.md; area3 remains open.

Progress, 2026-11-21: LIFE-07g consolidates visual source/build ownership in
Inference, with unchanged original engines/tests/tool, declared native MLX
inputs and18 identical parent copies (3884 lines) retired. Standalone builds,
component normal/SAN, actual heading and VisionPsy/Molmo owner normal/SAN,
gateway and package/install checks pass; see Inference qualification/
life07g-visual-builds/README.md. Next is remaining primary-backend source/build/
launch ownership per INFERENCE_AREA3_REMAINING_AUDIT.md. Original inventory and
full lifecycle requirements below remain applicable; area3 stays open.

Progress, 2026-11-21: LIFE-07f moves the original visual resource probes/budget,
resident-estimate reserve and bounded reclaim wait into Inference with explicit
telemetry/control/traces and preserved policy. Native normal/SAN, standalone,
original visual workflows and packaging/install pass. The complete remaining
area3 requirement audit and ordered implementation gaps now live in
[INFERENCE_AREA3_REMAINING_AUDIT.md](INFERENCE_AREA3_REMAINING_AUDIT.md).
Next LIFE-07g consolidates existing visual engine source/build ownership; the
original inventory below remains the reference for the rest of the cutover.

Progress, 2026-11-21: LIFE-07e extracts the shared Whisper runner into Inference's
whisper_process.c/public archive API, preserving txt/SRT/long-window behavior and
all original coordination through explicit callbacks. Source audit, native/SAN,
actual retained Mac model, voice/durable-audio and packaging/install checks pass.
Shared voice admission and legacy/verified guardian consolidation remain open;
the earlier inventory below still governs those semantics. Next LIFE-07f moves
the existing Mac visual resource/reclaim policy, with unchanged thresholds and
retry behavior. Evidence: Inference qualification/life07e-whisper-runner/README.md.

Progress, 2026-11-21: LIFE-07d extracted the three original visual launch/close
bodies below into Inference's existing specialist module/public archive, with
thin gateway calls and preserved settings/traces. A reproduced inactive-session
cleanup bug was fixed. Native normal/SAN, standalone and gateway integration/
installation gates pass; see Inference qualification/life07d-visual-launch.
Next LIFE-07e is the shared Whisper runner. The original inventory below remains
the reference for preserving the remaining caller policy and workflow limits.

Read-only audit, 2026-11-20. Parent baseline b3c79c1; follows Inference
LIFE-07c. No gateway implementation changed in this audit. Governing priorities:
[extraction first, no Python, this Mac](ARCHITECTURE_REBUILD_PRIORITIES.md).

The new component visual owner is qualified for the existing component prepared
pixel/frame profiles. It does not cover every gateway input/limit/policy. Do not
replace the gateway calls with those narrower profiles and declare parity.

## Visual implementation already working

| Original entry point in src/samosa_gateway.c | Responsibility and next owner |
| --- | --- |
| `visionpsy_session_start`, `molmo2_session_start`, `visionpsy_session_close` | Existing provider launch profiles, start/error/stop trace events and SamosaMmSession calls. The process supervisor and original request/reply bodies already belong to Inference. Extract the remaining profile/lifecycle glue with explicit configured paths, caller control and trace callbacks. Keep availability/model selection in the caller until its owning API is wired. |
| `vision_turn_start`, `vision_turn_release_runtime` | Chooses Molmo2 vs VisionPsy, holds the shared generation gate, may stop/restart the selected text backend on the16-GiB Mac, waits for Metal memory reclamation, releases the exact turn's state. Preserve this working coordination when its shared resource ownership moves; do not add a second scheduler. |
| `vision_resource_budget`, `vision_wait_for_molmo_memory` | Existing Mac available-memory/thermal policy, adaptive image side and bounded reclaim wait. Explicit policy input/output belongs in the existing scheduling extraction; telemetry/trace cannot remain a hidden Gateway dependency. |
| `vision_turn_inspect`, `vision_lower_side` | One bounded lower-tile-budget retry after releasing/reacquiring the failed session and turn gate. Preserve the retry and resource-release order. |
| `molmo2_session_analyze`, `molmo2_session_analyze_images`, `visionpsy_session_inspect` | Thin calls to already extracted codec. Image/image-group/video path forms and observation/usage outputs remain useful behavior. |

Concrete compatibility differences observed:

- Gateway VisionPsy uses adaptive512/1024/1536/2048 sides,120-second request
  timeout,64-KiB frames and2-second shutdown grace. The existing component
  prepared-pixel profile caps dimensions at512 and requests at60 seconds.
- Gateway Molmo2 accepts image, joint images and native video/media paths; its
  profile has600-second requests,1-MiB frames and3-second shutdown grace.
  Component `analyze_video_frames` accepts prepared frames, at most16, with a
 120-second budget and its existing strict prepared-frame result checks.
- Gateway source-aware routing, citation/progress and direct selected-visual-model
  behavior must survive. App/model-selection policy is distinct from Inference
  engine ownership. Sources should supply prepared media through its public
  interfaces where already available; do not move private upload paths into a
  new public service request merely to connect these profiles.

The existing public `SamosaMmSession`/provider/trace boundary already supports
the original protocols and configurable limits. Reuse that boundary for the
first mechanical lifecycle extraction; audit prepared-source API integration
separately before retiring the old path. Do not invent a third codec/model loop.

## Whisper implementation already working

`whisper_transcribe_path` is the shared gateway runner for short microphone text
and durable file-evidence SRT. It calls `voice_stt_fingerprint` to resolve the
selected base/tiny model, owns the existing voice busy gate, tracks the exact
child PID, honors the existing audio cancellation scope, caps output at16 MiB,
validates nonempty UTF-8 and removes all temporary results. Arguments preserve
English, `-nt`, and either `-otxt` or `-osrt`. This is an on-demand CLI, not a
persistent server; do not replace it with a newly designed Whisper server.

`components/samosa-inference/src/whisper.c` already provides verified engine/model
descriptors, guardian workspace ownership, cancellation/deadlines and strict SRT
validation for canonical PCM16 mono16-kHz input. Reuse that implementation for
process ownership. Its current public input caps duration at30 seconds and
returns structured derived segments. Gateway durable audio windows default to
600 seconds (configured10–3600), use1-second overlap, preserve transcript cache
identity/resume and can request plain text. Those workflows cannot be silently
truncated to the current30-second adapter limit.

Smallest remaining Whisper boundary: expose/reuse the existing verified native
runner with explicit selected engine/model, owned prepared audio, output mode,
request control and result/error mapping. Keep existing busy/cancel coordination
until it is consolidated with the shared owner. Preserve model fingerprint and
transcript-cache semantics until their Sources/Agent callers are migrated.
Record this mapping before modifying either runner; retire the original fork/
wait/unlink block only after microphone and durable-window behavior passes.

## Required checks and order

1. First extract the original visual launch profiles/close glue through the
   existing public supervisor and explicit traces. Compare original provider
   values/error/trace behavior; keep the same engine paths and algorithms.
2. Cover start failure, both protocols, stop, active cancellation/owner death,
   lower-side retry and the existing turn-gate release/reclaim behavior with
   native C/shell fixtures. Reuse existing owner/control tests.
3. Extract the shared Whisper runner with preserved text/SRT modes, busy gate,
   exact-child cancellation, complete temporary cleanup and error mapping.
   Retain original microphone and long-window/resume assertions.
4. Wire supported prepared-source/public registry workflows only after limits,
   semantics and model pins match their original callers. Then retire each
   duplicate lifecycle block. Full Inference area3 remains open until all of
   docs/INFERENCE_MODULE_PLAN.md area3 is satisfied.

Existing tests to preserve: `tests/test_molmo2_gateway.sh` (native shell fixture,
previous LIFE-07b passing evidence), `tests/test_runtime_settings.c`,
`tests/test_visionpsy_components.cpp`, `tests/test_visionpsy_real.cpp`,
`tests/test_visionpsy_e2e.sh`, `tests/test_voice_gateway.sh`,
`tests/test_audio_attachments.sh`, and owner-native Vision/temporal/Whisper tests.
Some original shell drivers embed Python. Review/port those sections and fake
workers to native C/shell/Node before invoking them; do not drop their assertions
or run a Python-backed aggregate target. This read-only audit did not rerun those
broader voice/visual workflows or qualify a gateway cutover.
