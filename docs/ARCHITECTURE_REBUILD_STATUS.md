# Architecture rebuild status

Priority review and shared agent handoff, 2026-11-20:
[Modular rebuild priorities](ARCHITECTURE_REBUILD_PRIORITIES.md).
This records the ordered remaining work and completion criteria; no new build or
test results are claimed. Implementation checkpoints below retain their dates.
User constraint, 2026-11-20: all runtime, build/release tooling, tests and CI must
be Python-free. Existing Python-backed checks require native/shell replacements.
Required target, 2026-11-20: this Mac first. Docker/Linux work is deferred and
does not block native Mac extraction, verification or product qualification.

Native Mac checkpoint, 2026-11-20: resumed visual ownership fixture and inherited
supervisor pass normal/SAN, including a clean independent source build. Chat
summary/runtime-settings/build and existing Molmo gateway assertions pass. Shell
fixtures replace Python in that visual product check; native Makefile parsing no
longer probes Python. Parent/Agent ace72ef and Inference 214fe75 are committed and
backed up on refactor/extract-working-modules branches. Remaining visual lifecycle,
Python-tooling removal and full component/product qualification stay open.
Exact commands and limits are in the verification Resume checkpoint.

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

Updated: 2026-10-24, Sources → Inference → Agent → App execution order.

Current continuation: Inference area3 LIFE-01 readiness and LIFE-02 managed-host
extraction are qualified. LIFE-03 managed text and embedding registry lifecycle slices are qualified:
configuration-only discovery, existing managed ownership, identity-bound
text/embedding configurations, public start/status/stop and persistent registry
transport. Read the component's EXTRACTION_STATUS.md and REGISTRY_LIFECYCLE.md;
LIFE-04a guardian duplex and the orphan-group HUP cleanup fix now pass both-
platform normal/SAN and existing regressions. LIFE-04b controlled guardian-
bound T5 transport also qualifies, including actual macOS corpus and App checks.
Next is LIFE-04c pinned public T5 activation/registry lifecycle. Earlier
LIFE-01 next-action paragraphs below are historical.

The full rebuild goal is active. There are four component codebases: Sources,
Inference, Agent and App. The first three are implemented with substantial
qualified behavior; replacement App remains to build. C01–C14 are fourteen
cross-component completion milestones. None is fully closed yet. Component
qualification and a closed implementation task do not imply milestone completion.

Inference areas1 provider/modalities and2 model acquisition/installation are
complete. Area3 engine lifecycle/readiness is active; areas4–7 remain pending.
All ten declared production distributions have matching cold normal/SAN actual
inference/offline recovery/cleanup receipts, with explicit macOS ARM64 execution
scope and separate native Linux control/build evidence. Current ownership
extractions and the exact next action are in Inference's
[extraction checkpoint](../components/samosa-inference/docs/EXTRACTION_STATUS.md).

## Current Sources seven-area status

Areas1–7 are closed against their finite component checklists. The standalone
Sources0.1.0 source archive and SHA256 manifest are delivered in
components/samosa-sources/dist/. Replacement App and assembled milestones remain open.
This table is the current summary; detailed entries below record
the progression of narrower subsets. No measured completion percentage exists.

| Area | Implemented/qualified | Remaining |
| --- | --- | --- |
| 1. Broader media — CLOSED | Public audio/visual preparation/cache, PQ/HLG, orientations, late video/audio seeking, full inspection, admitted format matrix, worker controls and independent archive acceptance on both platforms | None within the declared preparation scope; model analysis belongs to5 and indexing to4 |
| 2. Uploads/freshness — CLOSED |64MiB capacity; direct/stream/session publication for every format; all-reader live/cache/native-work races; durable retries, cancellation and pre/postcommit death recovery; bounded eight-peer watches, removal/replacement and cleanup | None within host upload/watch lifecycle; authenticated transport/admission is6 and modality reindex execution is4 |
| 3. Acquisition/search — CLOSED | Actual HTTP/HTTPS capture/receipts,5MiB acquisition/separate1MiB search cap, TLS/DNS/pinning/IPv6, bounded redirects/final-URL provenance, robots/host politeness and offline replay; all six actual-worker request/reply mappings, live Parallel and independently rebuilt normal/sanitizer archive acceptance on both platforms | None within declared acquisition/search component scope; fixture mapping proof does not assert paid-provider availability; authenticated shared service/load is6 |
| 4. Indexing/Chutni — CLOSED | Retained-version text/HTML/DOCX/PDF-text/image-OCR indexing, SDK lexical retrieval/publication/readback; audio discovery/reindex/failure/capacity/media gates plus retained derived history/read/search; mixed PDF text/OCR indexing/reindex, controls, corruption refusal and retained SDK artifact reads; visual capture/timeline inspection and distinct caption publication with retained provenance; shared durable audio/visual queue engine, transient frames, runtime and SDK commit recovery; automatic visual discovery/reindex, retained version history/read and caption search with rollback/SIGKILL qualification on macOS/Linux normal/SAN; fixed-profile prepared-vector semantic retrieval through the actual public SDK on both platforms; public native T5 summary adapter with fixture/control/parent-death normal/SAN gates on both platforms and actual macOS checkpoint provenance/replay gates; Sources owned retained-evidence scalar summary API/cache with actual macOS public-Inference/native-T5 end-to-end gate; public native opening map/reduce with explicit reduction/truncation and actual macOS large-input normal/SAN qualification; Sources opening-summary capture/cache and actual SDK summary_short publication/recovery, with both-platform normal/SAN gates and actual macOS model integration; automatic retained text enrichment with watch/reindex generation history, model profiles, durable phases and SDK SIGKILL recovery on both platforms, plus actual macOS native model queue integration; HTML/DOCX/PDF/mixed-OCR/image-OCR enrichment through actual SDK on both platforms; background enrichment and active-model TERM/checkpoint recovery qualified on macOS/Linux normal/SAN; pinned owned EmbeddingGemma2 Q8_0 text generation with 768-dimensional normalized vectors, four paraphrase rankings, long-input refusal, offline replay and single/stream parity passes on macOS/Linux normal/SAN adapters; native Sources text-query generation, validated model/profile/input provenance, SDK ranking and CLI/offline replay gate implemented (actual owned document generation and native payload-checked publication/retry pass on both platforms normal/SAN; four source files indexed and automatically captured/published; queue, reindex, offline publication and SDK/local capture interruption recovery pass normal/SAN on both platforms) |    None within the declared indexing/enrichment/semantic component scope; actual-model controls, refresh and admission now qualify. Broader modality analysis is5; service is6; release acceptance is7 |
| 5. Transcription/visual analysis — CLOSED | Actual full-duration WAV/MP3 Whisper windows, absolute coordinates, publication and offline recovery on both platforms; native image integration; Source temporal capture through actual macOS Molmo2 at16 frames and late ranges, pinned provenance, offline replay; normal/sanitizer lifecycle and regression gates on both platforms | None within the declared analysis scope; actual Molmo2 availability is macOS MLX, Linux qualifies the native adapters. Independent release acceptance is7 |
| 6. Service/queues/shutdown — CLOSED | Authenticated native socket service; all-format/64MiB uploads; bounded admission, owner FIFO/round-robin and reserved controls; shared service/queue leases; two-owner headless indexing; active-provider and background interruption, owned cleanup, scope replacement and cold recovery; macOS/Linux normal/sanitizer gates | None within the finite service contract; independent release acceptance is7 |
| 7. Final qualification/release — CLOSED | Independent core, optional dependencies/media/reindex, actual semantic and full-duration speech normal/SAN on both platforms; actual16-frame Mac Molmo2 normal/SAN at unchanged deadlines; exact282-file native archive with SHA256 manifest, preserved licenses and minimal linkage; pathname SQLite reopen and Python publication-tool replacements retired | None within standalone Sources; replacement App/assembled production retirement remains separate |

The established Sources execution order1–7 is complete. Native publication-tool
conversion and independent release acceptance pass on both platforms. Finite
closure evidence is recorded below and in ARCHITECTURE_REBUILD_VERIFICATION.md;
the shipped component includes docs/release-acceptance.md. The standalone Sources
initiative is complete. The broader four-component rebuild remains open.

## Implemented and qualified subsets

| Area | Current evidence |
| --- | --- |
| Component boundaries | Standalone native Sources, Inference and Agent contracts, dependency manifests, tests and releases. Independent macOS/native ARM64 Linux builds use no sibling private source/state. |
| Sources | Immutable text/PDF snapshots, selected literal evidence, provenance/coverage, scoped caches/claims, inventory/index/locator recovery. Actual PDFium extraction/control now qualified on both platforms. |
| Inference | Exact count/generation, opaque session recovery, bounded admission, tracked status/cancellation, owned engine lifecycle and managed transport. Actual model qualification on macOS; actual Linux model remains open. |
| Provider setup | Trusted catalog, verified resumable downloads/checkpoints, private credentials and setup CLI. HTTPS CLI checks qualified on both platforms. |
| Agent | Durable conversations/runs/events, revisions, context budgets/replay, checked finding and approved copy/move/undo with receipts and crash recovery. |
| Parent/child control | Public exact-child Inference and Sources control, scoped cancellation and shared deadlines. Exact offline retries and last-good preservation qualified. |
| S14d Sources operation control | Qualified on macOS and ARM64 Linux: worker death, peer isolation, owner refusal, actual PDF reader cancellation/deadline, no partial evidence, saved retries and healthy survivors. Strict confirmation and the separate fully instrumented truthful-unconfirmed case both pass. |
| Linux helper reliability | Reproduced ENOENT from executing a detached `/proc/self/exe` resolved pathname. Fixed by executing the kernel reference. Deterministic before/after normal/sanitizer regression and full control suites pass. Broad assembled reliability remains open. |
| S14d releases | Independent full tests/sanitizers/releases pass. Sources and Agent extracted archive contents match across platforms and the qualified workspace snapshot. Paths, hashes and logs are in the verification checkpoint. |

## Sources HTML reader (S16a): qualified

The independent Sources package now owns the native HTML parser and public
register_html/read_html operations. Original-byte immutable identity and owner
claims remain authoritative; selected ranges identify extracted UTF-8 text.
Coverage explicitly remains partial/static_readable_text_only. Existing cache
and durable control capacities apply. Parser,176 public checks and CLI tests pass
normally and under sanitizers in independent macOS/Linux copies. Full Sources
regressions and real PDFium controls pass. Extracted releases match both platforms
and the qualified workspace snapshot. S16a is qualified; Sources is not complete.
Next is bounded DOCX extraction and public delivery, with native fixtures. See
the S16a scope, html-v1 contract and verification checkpoint.

## Sources DOCX reader (S16b): qualified; OCR next

Public immutable registration/read, owner claims, cache and durable controls are
implemented. Native stored/Deflate fixture, parser/public normal/sanitizer and
separate CLI checks pass in independent macOS/Linux builds. XML/package validation
and resource bounds are tested; generated auxiliary filenames are excluded from
literal evidence. Full Sources/PDF regressions pass. Final release contents match
across platforms and the qualified workspace snapshot. S16b is qualified; whole
Sources remains incomplete. Next is bounded native OCR and its public contract.

## Sources OCR (S17a): bounded image/PDF slice qualified

Optional native Tesseract/Leptonica adapter and public register_image/read_image
are implemented with immutable owner claims, recognized UTF-8 selection,
explicit uncertain coverage, raster/confidence provenance, artifact fingerprints,
cache and durable controls. Explicit read_pdf_ocr renders bounded150dpi pages,
preserves absolute page numbers and handles image-only PDFs. Native calls now
execute in owned POSIX-spawned helpers. Expanded normal/sanitizer checks pass on
macOS and native ARM64 Linux: real monitor control, artifact change, abort/stall,
invalid output, child reaping, caller/descendant death and old cache preservation.
Existing PDF and separate OCR CLI checks pass. Helper RSS/descriptor/CPU/core
resource checks pass normally and under sanitizers on both platforms. Durable
native cancellation with cached/non-native peers and simultaneous healthy native
workers passes normal/sanitizers on both platforms. Actual BMP/JPEG/PNG/TIFF/GIF/
WebP OCR CLI and truncated decoder errors with healthy survivors pass both
platforms, including the production ASan+UBSan corpus. Refreshed full Sources/PDF
regressions and sanitizers pass both platforms. Extracted macOS archive acceptance
passes; Linux extracted archive acceptance also passes. Both archives match the
captured97-file qualified snapshot. Current workspace is newer because media
development has started. These bounded format/English OCR checks qualify S17a;
they do not establish accuracy for every raster/model or multi-image coverage.
Whole Sources is still incomplete; this is not component or milestone closure.

## Sources media preparation (S18a): in progress

Native WAV registration and lossless PCM16 mono16000Hz time-window preparation
are implemented with original absolute timestamps, distinct derived artifacts,
immutable owner snapshots and durable retry/cancellation. Initial normal and
ASan+UBSan checks pass macOS/Linux; expanded parser/public and separate production
CLI checks also pass both platforms. Active copying deadline, largest30s window
with exact full PCM/durable retry and snapshot corruption refusal pass normal/
sanitizers on both platforms. Owner-scoped prepared artifact caching is now
implemented with a shared literal/derived128-entry/8MiB budget. Integrity,
capacity, old-hit preservation and scoped cache cancellation pass both platforms;
refreshed full Sources tests/sanitizers pass. Compressed audio/video conversion,
larger bounded uploads and public Inference transcription/analysis remain.

## Sources compressed media (S18b): decoder primitive in progress

Optional native FFmpeg demux/decode/resample primitive is implemented with custom
immutable memory IO, secondary IO refusal, stream/packet/frame/thread bounds and
explicit timeline checks. Actual WAV, MP3, AAC/MP4 and48kHz stereo windows decode
on macOS/Linux. MP3/AAC/stereo and video-container audio with a delayed track
match selected native reference PCM exactly in normal and ASan+UBSan builds.
Actual executable and loaded demux/codec/resampler/utility artifact fingerprints
now pass mapped-file identity checks on both platforms, normally and under
ASan+UBSan, including refusal of a different inode at the same claimed artifact.
Owned worker audio routing, parent DTO validation and queried fingerprint binding
are implemented. Crash/protocol/output/stall cancellation/deadline, exact reaping
and healthy-survivor tests pass normally and under ASan+UBSan on both platforms.
Public encoded-media candidate registration now reuses immutable snapshots and
owner claims; separate MP3/MP4 CLI checks pass on both platforms. Public compressed
preparation/cache behavior is implemented with decoder fingerprint binding and
original time/stream provenance. Initial MP3 CLI preparation/cache checks pass
both platforms. Durable retries with decoder absent, range conflicts, malformed
input refusal and cache preservation pass normally and under sanitizers. Cache
count/byte refusal and fresh-digest payload/decoder tampering pass normally and
under sanitizers on both platforms. Refreshed standard full Sources tests and
sanitizers pass both platforms. Public durable cancellation in fingerprint and decoder phases passes normal/
sanitizer checks on both platforms.
Timed visual frame decoding and worker/parent routing are implemented. The
actual timed-frame worker and parent DTO validation now pass macOS normal and
ASan+UBSan checks, including malformed dimensions/timestamp/color/payload,
fingerprint change, crash/protocol/output/stall, scoped cancellation/deadline,
RSS limit, exact reaping, concurrent healthy visual peers and caller/descendant
death. Native ARM64 Linux normal and ASan+UBSan checks also pass.
Public `prepare_visual` and its owner-scoped cache are now implemented with a
visual contract identity, original-time/stream/dimension/color provenance and
shared audio/visual cache limits. macOS normal and ASan+UBSan public CLI,
cache-integrity and durable control checks pass; default compressed-audio
regressions pass. Linux final public CLI/control and parent scaled-dimension validation pass normally
and under ASan+UBSan. Full standard normal and sanitizer regressions pass on macOS and native ARM64
Linux. The expanded MPEG-4, H.264/BT.709, full-range MJPEG
portrait and HEVC reference corpus matches exact pixels normally and under
sanitizers on both platforms; native generated corpus runner checks also pass normally and under sanitizers. Broader media corpus and final optional release remain open.

## Sources visual presentation (S18b): orthogonal orientation qualification

Sources now honors exact orthogonal frame/stream display matrices after bounded
scale/color conversion to RGBA. It supports all four rotations and four
reflections, reports required orientation metadata, and validates oriented output
dimensions in the parent and prepared cache. Shear, non-unit affine scale and
perspective receive typed unsupported errors. Worker-v3 fingerprints bind the
method; minimal builds add no SDK/runtime dependency.

Eight native reference cases pass through primitive, worker, public prepare and
cache on macOS normally and under ASan+UBSan. A shear case proves no-artifact
public refusal and exact offline durable retry. Final native ARM64 Linux normal/ASan+UBSan orientation, public refusal, codec,
cache/control, compressed-audio regressions and isolated full-standard suites
also pass. Existing macOS codecs, public control/
cache, compressed-audio and isolated full standard normal/sanitizer regressions
pass. Broader color/timeline and final optional release remain open. This is
presentation progress, not whole Sources or C01–C14 closure.

## Sources media color/timeline and packaged subset (S18b)

The expanded native corpus now has ten acceptance cases, including VP9/MP4,
full-range H.264, 10-bit SDR BT.2020 HEVC, delayed video with earlier audio,
uncovered selections and variable frame timing. Exact pixels and independently
expected source timestamps pass through primitive, worker, public API and cache
normally and under ASan+UBSan on macOS and native ARM64 Linux. Uncovered intervals
produce no artifact and retain exact offline durable failure retries. These cases
do not establish HDR tone mapping or general gamut/transfer conversion.

Independent source archives built after their full normal release suites pass.
Their extracted127-file contents match each other and the captured workspace;
both extracted archives rebuild minimal/optional executables and pass separate
compressed-audio/visual CLI acceptance. Source archives now preserve the root
Apache2 LICENSE alongside vendored licenses. This qualifies the documented media
subset and its packaging; Sources and C01–C14 are still incomplete.

Next implementation focus is larger bounded immutable uploads, followed by full
live-file freshness/watch behavior. Acquisition/search, indexing/enrichment/
semantic/Chutni, public Inference media extraction, authenticated bounded service
and complete Sources qualification remain required.

## Preserved unfinished work: S15a concurrent durable admission

Workspace implementation now permits eight independent conversations to run in
one Agent state directory. A short admission gate protects quota/revision checks;
each run retains its own kernel worker lease. Recovery skips live peers, and
compaction refuses a live conversation. Semantic/database quotas are32/64MiB,
with three MiB reserved per active run. Refusal precedes user/run/event admission.

New native process tests pass on macOS normally and under ASan/UBSan: eight
simultaneous generations, ninth refusal, scoped cancel, SIGKILL recovery of only
one worker, live compaction refusal and offline terminal retries. Existing normal
Agent tests and focused cancellation/child sanitizer regressions pass. Linux
S15a, final full sanitizers, actual Sources integration and releases are still
required. A repeat reproduced state_unavailable at concurrent first creation of
work.lock (openat errno2) on macOS. That cause remains unresolved; evidence and
diagnostics are preserved. The already-running Linux control qualification
finished successfully against its snapshot, including concurrency and precommit
SIGKILL cleanup. Later workspace reply-snapshot edits remain unqualified. Agent
development is deferred until Sources and Inference are complete. Do not call
S15a qualified yet.

## Sources larger originals (S19a): bounded subset qualified

Original snapshots now accept up to64MiB. Registration copies cancellable64KiB
chunks and supports durable status/deadline/offline retry across all seven formats.
Actual64MiB text acceptance,64MiB+1 refusal,4MiB+ text and WAV, and4MiB padded
visual transport pass on macOS/Linux normally and under ASan/UBSan. Existing full
regressions pass. Total storage and derived-output bounds remain independent.
See [S19a scope](../components/samosa-sources/docs/S19a.md). Larger-upload lifecycle,
full freshness/watch behavior and complete Sources release remain open. Prior
S18b releases preserve their earlier qualified snapshot; S19a is not packaged yet.

Live observation hashing now honors host cancellation/deadlines per64KiB chunk
and before commit. Actual64MiB cancellation/no-history/offline retry checks and
existing2608-check locator regressions pass normally and with sanitizers on both
platforms (S19b). Continuous watchers and live-read freshness gates remain open.

## Sources live reads (S19c): text path qualified

Public locator_read validates current content and optional physical identity,
then rechecks file/parent/root after immutable extraction before returning evidence.
Actual warm-cache mutation, changed/missing/foreign refusal and retained historical
citations pass both platforms normally and with sanitizers. Existing locator
regressions pass. See [S19c scope](../components/samosa-sources/docs/S19c.md).
Other modality freshness races, public durable live-read controls, watchers and
index invalidation remain required; full Sources and C05–C06 remain open.

## Sources durable live reads (S19d)

locator_read now supports public operation status/cancellation, parent correlation,
deadlines and normalized exact offline retries. Saved responses preserve their
original observed_at_ms; new operation identifiers recheck current files. Focused
live/control checks pass normal/sanitizers on both platforms. Durable-operation
regressions also pass on both platforms after aligning the persisted result bound
with the live wrapper allowance. See [S19d scope](../components/samosa-sources/docs/S19d.md).
Watchers, index invalidation, modality/race qualification and full release remain
open. All C01–C14 obligations and the full rebuild goal remain active.

Live-read binding regressions additionally qualify actual same-byte file,
parent-directory and root replacement during warm-cache extraction on both
platforms normally and with sanitizers (S19e). In-flight evidence is refused;
new content-only checks and historical inode constraints remain distinct.
Continuous watching/index freshness integration is the next Sources focus.

## Index capture freshness (S19f): qualified

Text indexing now extracts through Sources locator_read after identity-bound
registration. A file changed during extraction becomes a qualified failed item,
with no stale search candidate; inner immutable evidence preserves historical
retrieval. Actual mutation regression and existing593-check indexing suite pass
normally and under sanitizers on both platforms. See
[S19f scope](../components/samosa-sources/docs/S19f.md). Watching and invalidating
previously indexed snapshots remain open, along with the other full Sources work.

## Observed index freshness (S19g): qualified

Indexed candidates now expose committed locator observation status and timestamp,
scoped to the owner and captured root. Changed/missing versions retain historical
evidence; legacy jobs without root scope remain unobserved. Subdirectory mapping,
transitions, offline/owner/host/root isolation and existing index/locator suites
pass on both platforms normally and with sanitizers. See
[S19g scope](../components/samosa-sources/docs/S19g.md). Continuous watching,
fresh-only policy and reindex scheduling remain required.

## Sources watch registry (S19h): qualified groundwork

Public root/owner-bound watch registration, offline recovery/status, stop and
eight-watch admission pass macOS/Linux normal/sanitizer tests alongside locator
regressions. Registry reports monitoring not_running until its continuous runtime
is implemented. See [S19h scope](../components/samosa-sources/docs/S19h.md).
Continuous runtime/checkpoints, cleanup/replacement and reindex scheduling remain
required; this does not complete watching or Sources.

## Watch checkpoints (S19i): qualified

Sources now atomically publishes bounded watch observations and a latest checkpoint,
without exhausting regular locator retry history.261 cycles, offline last-cycle
retry, cycle conflict, stop, precommit cancellation and corrupt-result refusal pass
normally and with sanitizers on both platforms. See
[S19i scope](../components/samosa-sources/docs/S19i.md). Continuous monitoring,
worker lifecycle/removal and reindex scheduling remain open; monitoring reports
not_running until that runtime is implemented.

## Continuous watch runtime (S19j): bounded path qualified

Sources CLI now owns registered-watch polling with per-watch duplicate refusal,
checked running status, bounded attempts and signal/public shutdown. Actual changes,
deletion, SIGKILL restart and root/state replacement refusal pass normally and
with parent/worker sanitizers on both platforms; checkpoint regressions also pass.
See [S19j scope](../components/samosa-sources/docs/S19j.md). Multiwatch fairness,
health/resources, removal/replacement, service lifecycle and reindex scheduling
remain required; full freshness/upload area and Sources remain incomplete.

## Watch health/eight-worker workload (S19k): qualified

Public watch status now reports durable completed-attempt health alongside lease
and checkpoint. Eight concurrent workers each make observed progress; stopping
one preserves peers. Scope-error reporting and corrupt-health refusal pass on both
platforms normally and with fully instrumented workers/tests. See
[S19k scope](../components/samosa-sources/docs/S19k.md). General fairness/resources,
removal/replacement, automatic lifecycle and reindex scheduling remain open.

## Watch removal/replacement (S19l): qualified

Owned stopped watches can be removed with runtime-lease exclusion, atomic metadata
cleanup, capacity recovery and durable saved receipts. Recreated registrations get
fresh identities; immutable source/locator/index history remains retained. Removal,
held-lease refusal, runtime regressions and committed-receipt cancellation races
pass macOS/Linux normal/sanitizer checks. See
[S19l scope](../components/samosa-sources/docs/S19l.md). Crash-time private-file
cleanup, broad resources/fairness, service lifecycle and reindex policy remain open.

## Interrupted-removal cleanup (S19m): recorded path qualified

Removal persists owner-scoped cleanup intent atomically with metadata deletion.
Public cleanup recovers SIGKILL leftovers, preserves held leases and owner isolation,
and repeats safely. Native crash tests pass both platforms normally and under
sanitizers; runtime regressions pass. See
[S19m scope](../components/samosa-sources/docs/S19m.md). Its identified late-admission
race is addressed in S19n below; full cleanup/resources and reindex scheduling
are not yet qualified.

## Atomic watch admission (S19n): identified ordering race qualified

Owner/root/registered-state validation and private runtime lease acquisition now
share the observer lock with stop, removal and cleanup. Stale admission after
removal refuses without recreating a lease. Native SIGKILL tests pause before and
after lease creation; concurrent stop refuses busy and recovery completes. Watch
registry and continuous runtime suites pass macOS/Linux normally and under
ASan/UBSan, including eight-worker startup contention. See
[S19n scope](../components/samosa-sources/docs/S19n.md). Broad resources/fairness,
service lifecycle, automatic reindex scheduling and full Sources remain open.

## Remaining sequence

Latest Sources progress: S19o adds observation-driven durable reindex intent,
coalesced per indexed item and committed atomically with reconcile/watch history.
Original-byte restoration clears intent; cancellation before commit preserves
both prior history and prior queue. Owner/host/root isolation and indexed-version
qualification pass macOS/Linux normal and sanitizer freshness tests, with normal
index/watch regressions. See [S19o](../components/samosa-sources/docs/S19o.md).
S19p now adds bounded explicit execution with retained immutable versions, actual
SIGKILL/resume and newer-observation refusal. Normal and sanitizer reindex tests
pass macOS/Linux; native tests reach the public 256-version cap. See
[S19p](../components/samosa-sources/docs/S19p.md). Fair draining, retry/backoff,
automatic execution, missing/new/failed-file policy and legacy observation catch-up
remain required; automatic reindexing is not complete.
S19q adds durable least-attempted ordering within a selected job: missing items
retain their attempts across watch checkpoints, allowing healthy pending peers
to proceed. Counters clear with intent, are exposed offline and reject corrupt
values. Normal/sanitizer reindex tests and normal freshness/index/watch regressions
pass macOS/Linux. See [S19q](../components/samosa-sources/docs/S19q.md).
Timed backoff and a continuous automatic runner remain open.
S19r now adds index_queue_step with durable job turns across ready jobs in the
configured owner/root scope. A recurring hot job yields to an older ready peer.
Automatic steps avoid client receipt growth and remain usable at the explicit
operation limit. Normal/sanitizer reindex and normal freshness/index/watch checks
pass macOS/Linux, including scope isolation and schedule corruption/overflow.
See [S19r](../components/samosa-sources/docs/S19r.md). Continuous runtime and timed
backoff remain required, with full missing/new/failed-file and legacy policies.
S19s now supplies the continuous local --index-run process with bounded polling,
one runtime lease, attempt deadlines and signal cancellation. Actual duplicate,
TERM/INT, SIGKILL/restart and root/state/lease replacement checks pass normally
and under sanitizers on macOS/Linux; reindex and continuous-watch regressions pass.
See [S19s](../components/samosa-sources/docs/S19s.md). Per-item backoff, remaining
precheck/store-open races and long-running resources/service lifecycle remain open.
S19t adds durable per-item automatic cooldown: one-second exponential delay capped
at60 seconds, admission reservation across process death, full delay after failure,
and changed-target wakeup. Ready peers proceed; explicit retry can bypass cooldown.
Normal/sanitizer reindex and actual continuous runtime checks pass macOS/Linux,
including a delayed failing worker and SIGKILLed admission. See
[S19t](../components/samosa-sources/docs/S19t.md). Remaining race/resource/clock
qualification and complete missing/new/failed-file/legacy policies remain open.
S19u carries admitted state device/inode pins into core store admission and the
index runtime's startup root scope into queue selection/commit calculations.
Actual replaced/absent-state tests refuse without namespace creation; pinned
selection does not adopt pending replacement-root jobs. Pins/runtime checks pass
macOS/Linux normal and sanitizer configurations. See
[S19u](../components/samosa-sources/docs/S19u.md). The inner SQLite filename-open
race and remaining helper/control-store/resource qualification remain required.

S19v adds opt-in descriptor-bound core and durable operation stores. Direct
adapter normal/sanitizer qualification passes Linux and macOS upstream SQLite;
integrated binding-pin, operation and reindex checks pass both, including operation
sanitizers. macOS system SQLite remains incompatible. Runtime qualification,
reproducible dependency packaging and default-path retirement remain open. See
[S19v](../components/samosa-sources/docs/S19v.md). This does not close the complete
uploads/freshness lifecycle or release Sources.

Latest S19v state: bundled SQLite is now the Sources default and both core and
read-operation pathname-reopen branches are removed. Standard normal suites and
clean builds pass both platforms; the broader bundled macOS sanitizer suite
passes. Linux broader sanitizers and workspace default-command checks also pass.
The dispatch state pin passes normal/sanitizer operation checks on both platforms;
final default watch and queue worker regressions also pass. This retires the replaced Sources storage paths only, not
the legacy application or the unfinished Sources lifecycle and release gates.

S19w adds root-independent immutable host uploads through the shared snapshot
transaction. Normal and sanitizer checks pass both platforms, including actual
64MiB ingestion, cancellation during hashing with no accepted rows, and late-range
evidence after caller-buffer release. Host streaming is implemented; durable session begin/status/abort and bounded
transactional chunk append and atomic snapshot publication are added. Fresh-process resume and two-process exact
append/commit retries and precommit cancellation/SIGKILL rollback recovery are
qualified. Actual64MiB staged publication also passes both-platform normal and
sanitizer gates. Broader crash/concurrency/control qualification and service
admission remain open. See
[S19w](../components/samosa-sources/docs/S19w.md).

Sources now also implements explicit index_discover: it inventories an existing
job's bound directory, appends unseen locators for index_step, preserves earlier
items/versions and exact retries, and retains the 256-item cap. Job responses now
report checksum-validated latest discovery coverage, scan cycle and observed count,
separately from the original inventory. Combined coverage stays partial after
extension. Discovery now rechecks host controls and directory/root bindings after
receipt preparation, with precommit cancellation/deadline, SIGKILL recovery,
root/directory replacement and accumulated quota rollback qualification. Host
opt-in periodic discovery and initial indexing now use bounded per-job turns and
cooldowns in the existing index runtime, with no background receipt growth.
Supported failed/skipped text items now enter a bounded, identity-sensitive recovery
queue; successful recovery preserves the initial cursor and historical evidence,
with conservative combined coverage. Recovery commit-boundary cancellation,
deadline, SIGKILL and root/directory replacement tests pass; owned offline queue
health is available through index_get. Periodic refresh now rotates bounded
indexed-locator hash observations into retained reindexing, including metadata-
preserved content changes and missing/restored files without a separate watch.
Its staged observation/intent transaction now receives final physical validation;
commit-boundary controls, SIGKILL, file/root/directory races and revision overflow
are qualified. Further race/resource qualification and
full freshness/index policies remain open. See
[S19x](../components/samosa-sources/docs/S19x.md).

Sources #3 now has its first actual host HTTP acquisition implementation: a
separate optional libcurl worker, exact host-granted origin, bounded response
and header streaming, parent shape/base64/hash validation, and immutable owner
admission with URL provenance. Native HTTP fixtures pass normal and ASan/UBSan
checks on macOS/Linux. Public JSON/CLI fetching and atomic acquisition receipts
are added, with controlled execution attempts and offline recovery; extended
commit-boundary and actual CLI checks pass normal and ASan/UBSan on both
platforms. Host-captured PEM trust and actual TLS issuer/hostname/expiry/CLI
checks now pass both platform normal/sanitizer gates, and real public HTTPS
capture/offline HTML extraction produces matching hashes. Broader TLS, larger remote bodies,
provider credentials and broader search qualification remain open. See
[initial fetch contract](../components/samosa-sources/contracts/fetch-v1.md).

Sources #3 now also implements the legacy W1 search adapter behavior behind its
public interface, without linking the gateway. All six provider declarations
and generic host templates are available. Actual POST/JSON/SSE fixture and
private CLI configuration checks pass normal and ASan/UBSan on macOS/Linux;
real keyless Parallel searches succeed on both, with bounded results, exact
completed-operation replay and immutable evidence reading offline. Paid provider
presets have not been qualified against live services. This does not close #3
or the full Sources release; broader provider/acquisition policies and remaining
indexing/enrichment, Inference, service and release work are still pending. See
[search contract](../components/samosa-sources/contracts/search-v1.md).

Sources #4 now has a real existing-store Chutni retrieval adapter. Public
memory_search calls the external SDK in a read-only isolated worker, preserving
store/source/artifact references and bounded lexical scores. Results use Sources'
owned immutable metadata snapshots and completed-operation offline replay/read.
Actual Chutni0.3.0 store integration and independent parent validation pass
normal/sanitizer checks on macOS and Linux. The reference SDK Linux C99 build
requires explicit POSIX/XSI feature declarations; an initially broken SDK was
diagnosed and rebuilt with those declarations. Indexed text now publishes via
Chutni's existing public SDK transaction, using durable Sources intents and
receipts. Actual automatic runtime publication, catalog registration/refresh,
lost-acknowledgment recovery after SIGKILL and refusal of changed artifact text
pass normal and ASan/UBSan integration tests on macOS and native ARM64 Linux.
The qualification record distinguishes these paths from remaining release gates. Public memory_read now exposes referenced text ranges and producer/derivation
provenance through that SDK, with immutable offline captures and parent validation.
Actual search-to-read and published-text reads,360KB UTF-8 later ranges,
identity/range refusals and offline owner claims pass normal and sanitizer checks
on both platforms. The SDK's own range tests and existing regressions also pass;
Linux sanitizer qualification includes a repaired fixture-only allocation leak.
HTML/DOCX/PDF text now reuse those existing readers for indexing, retained-version
reindexing and Chutni publication; original evidence and partial coverage remain
intact. Actual initial and changed-file retained-version indexing, publication and
artifact reads pass normal/ASan/UBSan checks on macOS and native ARM64 Linux,
using real PDFium and Chutni SDK artifacts. Existing index, reindex, freshness,
discovery and retrieval regressions pass. See the verification checkpoint.
Image OCR now reuses the existing reader for indexing, retained-version reindexing
and Chutni publication. Complete recognition evidence, confidence, raster identity
and uncertainty remain intact; recovery refuses altered saved recognition metadata.
Actual English raster integration and prior publication/index/retrieval regressions
pass normal and ASan/UBSan qualification on macOS and native ARM64 Linux.
PDF OCR fallback, audio/video index outputs, non-text artifact retrieval,
enrichment and semantic retrieval remain open; #4 is not complete. See
[Chutni contract](../components/samosa-sources/contracts/chutni-v1.md).

Sources #5 now has an optional public Inference transcription dependency reusing
the monolith's installed whisper.cpp engine. Native C fixtures qualify timed
derived text, bounded30s input, durable retries, owner isolation, artifact/input
refusals, cancellation/deadlines and active-requester SIGKILL workspace cleanup
on macOS/Linux in normal and ASan/UBSan modes. Actual public Sources prepared
audio -> installed Whisper interoperability and offline retry pass normal and
sanitizer checks on macOS. Actual WAV and MP3 capture/cache now also pass on native
ARM64 Linux in normal and sanitizer modes, using the same pinned engine source
and English model. This is bounded workflow qualification, not broad accuracy.
Sources-owned transcript capture/cache and original timeline mapping now pass
native fixture and ASan/UBSan checks on macOS/Linux, including128 cues, model
changes, offline replay, cancellation and altered cache metadata with recomputed
digests. The public library host supplies the Inference transport callback;
production process transport and CLI wiring are now implemented. Actual macOS
Whisper capture validates the exact prepared input and serves an identical cached
transcript with original/provider transport absent. Native ARM64 Linux now passes
the same actual capture/cache workflow for WAV and MP3, including sanitizers.
Linux Whisper's measured20ms end-grid overrun is clipped only to its first grid
boundary and input duration, with timing policy validated/preserved by Sources;
larger/off-grid overruns still refuse. Broader speech/media qualification,
full-duration audio indexing and existing vision adapter extraction remain open;
#5 is incomplete. See the transcript contract and
verification record.
The plan/resume guide now explicitly require reuse of working monolith
implementations before replacement and distinguish migration from new features.
The existing Chutni retrieval fixture has now been converted to native C, preserving
its real public-SDK acceptance checks. Normal and ASan/UBSan runs pass on macOS
and native ARM64 Linux. Two existing Sources Python integration fixtures
(publication and search CLI) still require native conversion to satisfy the plan's
no-Python release constraint; that gate remains open.

The private Chutni publication worker now adapts the monolith model-output format
for derived transcripts, reusing its existing SDK transaction/recovery path.
Native real-SDK fixtures pass normal/sanitizer checks on macOS/Linux, including
model provenance, source-time selectors, missing-original recovery, conflicting
capture refusal and the existing literal writer. Sources now exposes
publish_transcript for owner-authorized cached windows, with durable local receipts
and existing operation controls. It reuses the SDK transaction and recovery;
missing local receipts recover after original removal without rerunning Inference.
Public fixtures use real registered/prepared bytes and fixture model output.
Actual Whisper WAV/MP3 capture-to-publication and full retained-capture readback
now pass through public Sources/Inference CLIs on macOS and native ARM64 Linux,
including ASan/UBSan. Original/provider/worker-absent receipt replay also passes.
Automatic filesystem audio discovery and broader speech/media qualification remain next;
#4/#5 remain open.

The audio queue's per-window preparation/capture/publication path is now extracted
from those existing implementations. It keeps new PCM/captures in bounded working
buffers instead of filling the persistent8MiB cache, while preserving public cache
retention and admission behavior. The monolith's one-second overlap and exact-tail
accounting are adapted to30s windows. Native fixtures publish eight overlapping
WAV windows through the actual SDK with no prepared-cache growth under a1KiB
admission budget. Durable selected-range audio job checkpoints and the existing
runtime integration are now implemented. Capture and publication are separate
atomic phases; model/decoder/root/owner identities are bound, saved captures and
receipts are revalidated, and dependency failure records typed backoff. Native
actual-SDK fixtures qualify public queue progression, worker-absent retry without
another model call, recomputed-digest corruption refusal, runtime/TERM and SIGKILL
after SDK commit before local acknowledgment, recovered with original/model gone.
Actual Whisper now qualifies cold-state selected-range WAV/MP3 queue capture,
separate publication, completion and offline saved-capture readback through public
CLIs in normal and ASan/UBSan on macOS/Linux. These are short2672ms English fixtures,
not a full-duration recognition benchmark. Filesystem discovery/reindex/freshness
and job/history lifecycle qualification remain open; #4/#5 remain incomplete.

1. Complete Sources: remaining readers/OCR/media, acquisition, indexing/enrichment,
   Chutni adapter, service bounds and full independent platform qualification.
2. Complete Inference: supported providers/modalities, broader actual-model checks,
   scheduler fairness/resources, service bounds and independent qualification.
3. Complete Agent: finish preserved S15a admission and remaining policies,
   scheduling, authorized services, action and failure qualification.
4. Complete replacement App against authoritative component contracts, with
   generic setup/status, safe rendering and owned-instance lifecycle.
5. Assemble pinned independent releases and qualify minimal installation.
6. Audit C01–C14, retire replaced legacy paths, and finish assembled platform,
   failure, concurrency, quality and resource acceptance.

No legacy production switch, commit or deployment has occurred. Rebuild folders
and architecture documents remain untracked. Truly Typed Docker images were
preserved during the requested obsolete-image cleanup.

Evidence: [verification checkpoint](ARCHITECTURE_REBUILD_VERIFICATION.md),
[full plan](ARCHITECTURE_REBUILD_PLAN.md),
[S15a scope](../components/samosa-agent/docs/S15a.md).


Automatic embedding checkpoint (2026-10-10): native retained-generation capture
and publication now run through the existing queue. Fixture559 and actual-model454
pass normal/SAN on macOS and Linux; four paraphrase rankings, CLI query, offline
replay, reindex, phase SIGKILL recovery and unattended fixture publication pass.
See the automatic retained-generation checkpoint in ARCHITECTURE_REBUILD_VERIFICATION.md.
The native QA host manages activation revisions; an unchanging CLI managed
configuration is not qualified for repeated activations. Area4 remains active
for broader retrieval acceptance and the remaining controls/capacity/lifecycle
audits. Areas1–3 closed;5–7 open. No completion percentage or shipped Python runtime.


2026-10-11 embedding checkpoint: actual background queue publication and12
predeclared paraphrase rankings among12 documents pass468 normal/SAN on both
platforms. Fixed private managed configuration now works through explicit native
--single-current; strict revision checking stays default. Archived input/vector/SDK
bindings and profile migration pass645 fixture checks normal/SAN on both platforms.
Linux SAN was rerun only after the finite QA container main process exited and
its original interrupted evidence was preserved. See the latest verification
checkpoint. Area4 remains active solely for the finite declared input/admission
and active-model queue/watch control audit; extra models, arbitrary quality
claims and unrequested chunking are not introduced as closure requirements.
No shipped Python runtime, completion percentage or default-model switch.

Declared embedding input/admission bounds now pass1,807 checks normal/SAN on
macOS and Linux:60,000-byte quotation-heavy text publishes,60,001 bytes refuses
before model execution, and storage reservation admission stops before overflow.
Area4's final active-model queue/watch control audit is now complete.

2026-10-11 final closure: area4 is CLOSED. Native actual-model deadline,
active-listener cancellation, bounded guardian cleanup, healthy retry, automatic
refresh/reindex and retained vector publication pass normal/SAN on macOS/Linux.
The12-document/12-query actual-model corpus, fixed private configuration,
archived bindings, model migration, phase recovery and declared admission/input
bounds also pass. See the area4 closure audit in the verification document.
Areas1–4 are closed; area5 is active;6–7 remain open. No shipped Python runtime
or completion percentage. Historical checkpoints above retain their original
statuses; this closure supersedes area4 pending labels.

Area5 checkpoint: actual66-second speech now passes normal/SAN on macOS/Linux,
with three overlapping30-second-or-shorter windows, recognized initial/middle/
late speech, absolute source timestamps, SDK publication/completion and exact
offline retained readback. Area5 remains active for encoded full-duration
qualification and the existing Molmo2 temporal video path's public native
adapter/integration/actual-model controls. Single-frame VisionPsy remains a
qualified foundation. Areas6–7 remain open; no shipped Python runtime.

Latest area5:66-second MP3 actual speech also passes normal/SAN on both
platforms, including exact per-window PCM hashes against independent decodes,
three completed SDK publications and offline retained reads. The native Molmo2
helper's prepared-frame prerequisite builds and recognizes ordered red-to-blue
frames with exact timestamps through the installed macOS Q4 package; native C
normal/SAN harness gates pass. Public temporal Inference DTO, Sources integration
and their lifecycle/bounds/actual-model qualification remain. The native helper
was not installed or selected as a default; area5 stays ACTIVE.

Latest area5 public temporal checkpoint: native C Molmo2 adapter now builds,
verifies the full pinned package, consumes ordered RGBA/time frames and returns
uncertain derived temporal observations. Actual macOS normal/SAN passes512px
frames and a>2MiB request, exact input/preparation/time/model/runtime bindings,
foreign-owner refusal and offline operation replay. The original120s temporal
budget is preserved. Linux normal/SAN input and shared lifecycle/image tests pass;
this is not actual Linux MLX model proof. Sources temporal client/capture/cache
and dedicated lifecycle/integration acceptance remain. Area5 stays ACTIVE;
areas6–7 remain open. No shipped Python runtime or default-model switch.

2026-10-11 subsequent checkpoint: Sources `analyze_video` now implements native
uniform/last-frame sampling, public temporal inference and owner/model/source
bound retained observations/cache. Actual macOS model integration normal/SAN
passes initial and late60–62second ranges with independent FFmpeg pixel/P6/time
bindings and origin/helper/model-free replay. Both-platform normal/SAN Source
16frame fixture gates pass; native temporal lifecycle16file cleanup, mutation,
malformed output, foreign-owner replay and cancellation pass. Inherited temporal
CPU/address limits are corrected and qualified separately from default profiles.
Final admission/regression collection and actual16frame model acceptance remain
before the finite area5 closure audit. Area5 stays ACTIVE;6–7 remain open.

2026-10-11 area5 closure: actual16-frame Source temporal capture passes normal
and SAN through pinned native Molmo2, independent pixel/P6/time bindings and
origin/helper/model-free replay. Full-duration speech, late temporal ranges,
both-platform lifecycle/admission/transport and existing publication regressions
all pass. The finite area5 checklist is closed; actual MLX availability is macOS,
Linux native adapters/controls are qualified separately. Areas1–5 CLOSED,
area6 authenticated service ACTIVE,7 independent release acceptance OPEN.
No completion percentage or shipped Python runtime.

Area6 initial implementation: native credential/header boundary and63-check
normal/SAN gates pass macOS/Linux. Public HTTP ownership derives from configured
credentials; body/header limits and strict framing are checked before body work.
Complete socket ingress, streaming multipart, admission/fairness, queue ownership
and shutdown remain on the finite service checklist. Overall goal remains active.

2026-10-11 area6 socket checkpoint: authenticated loopback HTTP JSON and native
streaming multipart ingress now work, with pinned state/root, credential-owned
requests, two normal workers/eight waiters, separate control worker, owner FIFO/
round-robin, request budgets, disconnect cleanup and bounded shutdown/restart.
Actual64MiB socket normal/SAN gates pass macOS2734/2517 and Linux1806/2020;
counts include polling. Service-owned background queues, shared global admission,
all-format socket coverage and active native/provider control/recovery qualification
remain. Area6 ACTIVE,7 OPEN; overall goal remains active without a percentage.
