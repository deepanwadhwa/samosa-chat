# Remaining legacy-to-guardian integration

2026-11-21, after LIFE-07l primary/Whisper guardian integration. This is the next implementation boundary, not a new
runtime design. Reuse the working EngineProcess/Activation and host adapters.
Preserve the original Qwen/Maple/llama, Whisper and visual implementations.
Native Mac first; no Python or Docker/VM work.

LIFE-07l reuses EngineProcess for the original long-window txt/SRT Whisper CLI,
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

The existing specialist cleanup also deserves the stubborn-descendant case:
its early waitpid-return shape was observed in source, but LIFE-07j's dynamic
defect proof and fix cover the primary owner only. Qualify that supported path
with evidence rather than treating another owner's passing test as coverage.

Shared turn/admission/pause/resume/retry coordination remains its next ownership
step. Global reservations/fairness are still full area4 work. Remaining Inference
areas, project-wide Python removal, Agent/App, pinned assembly and qualified
monolith retirement remain open. Inkling-2-Mini stays after the refactor.
