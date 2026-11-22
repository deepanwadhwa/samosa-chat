# Remaining legacy-to-guardian integration

2026-11-22, after LIFE-07o structured Whisper common lifecycle integration. Remaining
common lifecycle work follows this preserved-contract mapping; it is not a new
runtime design. Reuse the working EngineProcess/Activation and host adapters.
Preserve the original Qwen/Maple/llama, Whisper and visual implementations.
Native Mac first; no Python or Docker/VM work.

LIFE-07o checkpoint, 2026-11-22: the existing structured on-demand Whisper
request now supports common start/status/stop through Activation. Actual pinned
speech must qualify before last-good; idle status means qualified unchanged
assets with no resident engine. Managed requests retain ownership through pending
guardian cleanup; unknown cleanup retains the live host's fence. A reproduced
SIGXFSZ during native Metal-cache writes is fixed by an explicit guardian opt-in
without a process-wide file cap, preserving original transcript/log/deadline checks.
Final native/independent normal/SAN lifecycle 469/469, Activation 158/168,
original guardian 525/525 and Whisper 508/511 pass. Installed-engine normal/SAN
qualification/transcription/requalification passes with zero swap growth. See
[scope, commands, failures and limits](../components/samosa-inference/docs/qualification/life07o-whisper-lifecycle/README.md).
The legacy long-window txt/SRT CLI remains unchanged and still needs common
controls. Default primary/visual controls, complete identity/readiness/last-good,
unknown-group and pre-admission recovery remain open. The original structured
profile is not a replacement for these contracts. Area3, later Inference areas,
Python removal, Agent/App, assembly and qualified retirement remain required.
Sources stays closed; Inkling stays deferred until the entire refactor finishes.

Historical LIFE-07n checkpoint, 2026-11-21: the original shared text/visual generation gate,
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

Historical LIFE-07l reuses EngineProcess for the original long-window txt/SRT Whisper CLI,
keeping English/-nt,16-MiB output/UTF-8/error semantics and on-demand execution.
It reserves actual PID/workspace through result validation/tracking withdrawal;
parent death/cancel finish helpers and workspace. Explicit handoff includes only
caller-prepared microphone/decoded/PCM audio; borrowed durable audio remains.
Final522 normal/SAN/independent, original54 assertion audit, actual Gateway
SIGKILL/prepared cleanup, full voice/audio/video/cache/cancel/restart, retained
actual Whisper and exact native package/install pass. Evidence: components/
samosa-inference/docs/qualification/life07l-whisper-guardian/README.md.
Next default visual helpers must retain all contracts in the table; common
activation/identity/readiness, guardian-death and pre-admission recovery remain open.

Previous LIFE-07k reuses the existing EngineProcess guardian for the primary server,
retaining the actual worker PID/marker, unlimited lifetime, append/fallback logs,
original launch/environment,8-second TERM grace, identity through acknowledgement
and retained ownership through pending shutdown. Final primary676 normal/SAN,
standalone676 normal/SAN, actual Gateway SIGTERM/SIGKILL plus helper cleanup,
original selection/settings/App/LAN, package/native install and retained actual
Qwen generation pass. See components/samosa-inference/docs/qualification/
life07k-primary-guardian/README.md. Primary common activation/readiness/identity
and guardian-death policy remain open; guardian reuse alone is not full area3.
The next default path is visual under the unchanged wide/native-video contracts.

Historical LIFE-07j moves primary launch/probe/stop/PID-marker code into backend_process.c.
It retains original caller coordination and fork-success/HTTP-availability
semantics. Its reproduced descendant-cleanup defect is fixed with the existing
guardian WNOWAIT pattern. That does not provide parent-death ownership or actual
generation qualification for every legacy/default deployment path.

| Existing path | Contracts to retain during reuse of the guardian |
| --- | --- |
| backend_process.c primary HTTP server | Existing process lifetime has no arbitrary24-hour cap. Preserve Qwen CPU/context locks/auto/compaction, Maple model-dir, llama aliases/projector/overrides, loopback/LAN environment removal, log behavior, literal paths, observed backend PID/group and PID marker. Gateway selection/fingerprint/settings/rollback remain explicit caller policy. A successful fork or health response is insufficient common lifecycle readiness. |
| whisper_process.c shared CLI | On-demand execution, not a persistent server. Preserve English/-nt, selected engine/model fingerprint, txt/SRT, long-window inputs,16-MiB nonempty UTF-8 output, exact operation cancellation and complete temporary cleanup. Do not substitute whisper.c's short-clip profile or truncate a long durable window. |
| specialist_supervisor.c default framed visual helpers | Preserve original adaptive VisionPsy sides through2048,64-KiB frames/120-second requests/2-second grace, Molmo image/joint-image/native-video forms with1-MiB frames/600-second requests/3-second grace, trace events and lower-side retry. The prepared-pixel/frame host profiles are not full replacements for these contracts. |

EngineProcess already supplies guardians, parent-death control, retained leases,
bound assets/workspaces, separate protocol/stderr, descendant cleanup and sized
results. Reuse those mechanisms. Its original bounded trusted argv/lifetime/log/model
interfaces must be checked against the table before adopting them. Extend an
existing boundary only where the preserved contract requires it; do not add a
second guardian/server or silently narrow an existing workflow to fit a profile.

In particular, primary integration must preserve what the public PID/marker
identifies, how external shutdown reaches it and how child exit is observed.
Do not simply replace the backend PID with a guardian PID and retain the old
signal/wait code. Preserve environment inputs without exposing filesystem/model
paths as public service requests. The original host serializes launch/stop;
state-mutex publication alone is not full operation/admission serialization.

The complete area3 acceptance still requires supported public controls,
observed identity/health plus actual successful capability probes, mutation and
stale-readiness rejection, last-good replacement behavior, host/engine death,
all descendants gone and unrelated processes alive. Audit default paths as well
as modern component profiles. Retain useful original assertions, port required
Python drivers before invoking them and record exact native results.

LIFE-07m reproduces the specialist stubborn-descendant leak in both original
profiles and qualifies cleanup through the existing guardian. Its evidence is
profile-scoped; do not treat it as full common lifecycle/guardian-death coverage.

LIFE-07n now owns the original shared turn gate/pause/resume/retry coordination
through explicit host callbacks; the direct visual count-reset defect is fixed.
Global reservations/fairness remain full area4 work. Remaining Inference
areas, project-wide Python removal, Agent/App, pinned assembly and qualified
monolith retirement remain open. Inkling-2-Mini stays after the refactor.
