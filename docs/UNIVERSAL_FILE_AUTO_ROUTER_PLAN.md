# Universal file chat and automatic specialist routing

Status: partially implemented; the audio/document milestone is prepared for review.

This branch delivers the completed slices listed below, plus model-directed
document reading and the OCR/large-PDF corrections documented in
`DOCUMENT_HARNESS.md` and `OCR_REVIEW_FIXES.md`. It does not complete the entire
delivery plan. Subtitle/on-screen-text extraction, XLSX, server-driven browser
attachment limits, broader mixed-source qualification, and optional automatic
answering-model selection remain future work. The browser still permits six
attachments per turn.

Branch: `codex/universal-file-auto-router`

Prepared: 2026-09-01

## Outcome

Samosa presents one ordinary interaction:

> Drop one or more files, ask a question, and let Samosa decide how to read
> them locally.

The user does not choose OCR, vision, transcription, extraction, or a model for
each turn. A capability harness inventories the files, builds a validated
evidence plan, leases the smallest capable local specialist for each operation,
stores durable evidence, and gives that evidence to the conversation's
answering model.

Examples:

- A digital PDF uses its embedded text for a literal question. A chart question
  adds visual inspection only for the relevant pages.
- A photographed receipt uses OCR for exact numbers and visual intelligence for
  layout or object relationships.
- A podcast uses a speech-to-text provider, stores a timestamped transcript,
  retrieves relevant transcript segments, and cites times in the answer.
- A video question about dialogue uses its audio track; a question about an
  action uses sampled frames; a question relating speech to an action uses both.
- A mixed turn can compare a source file, a PDF specification, two screenshots,
  and a recorded meeting without asking the user to select five processing
  modes.

This is an extension of Samosa's implemented automatic image/PDF path, not a
second attachment system.

## Implementation status

The work is landing as tested vertical slices on
`codex/universal-file-auto-router`:

- Complete: versioned source, capability, evidence-plan, and evidence
  contracts; byte-sniffed inventories on attachment upload; authenticated
  live source-capability discovery.
- Complete: PCM WAV Auto path. Mono 16 kHz PCM16 WAV files are admitted from
  bytes, routed to the selected installed Whisper.cpp model, converted to
  timestamped `samosa.evidence.v1` transcript segments, atomically cached
  beside the content-addressed attachment, and handed to the selected chat
  model inside the untrusted-source boundary.
- Complete: transcript cache reuse across questions and gateway restarts,
  provider/runtime fingerprint invalidation, conversation source binding,
  task-ranked transcript retrieval, timestamp citations, and composer Audio
  plus multi-file drag/drop affordances.
- Complete: qualified long WAVs are split into ten-minute windows with a
  one-second overlap, each successful window is atomically checkpointed with
  its source/model/runtime identity, and overlap cues are reconciled onto one
  absolute timeline. A failed or interrupted later window resumes from the
  first missing checkpoint after retry or gateway restart.
- Complete: streaming turns publish exact transcript-window progress through
  the existing file-activity UI. The ordinary Stop action now cancels the
  active Whisper child, returns a typed cancellation result, and retains every
  previously completed checkpoint. The acceptance fixture stops inside window
  two and proves that retry reports a window-one cache hit and runs only the
  two missing windows.
- Complete: qualified macOS compressed-audio path for MP3 and AAC-in-M4A. A
  small AVFoundation sidecar probes the real container and stream inventory,
  rejects malformed files, video-bearing containers, unsupported codecs, and
  durations above four hours, then decodes only the current bounded Whisper
  window to mono 16 kHz PCM16. Decoder identity participates in evidence cache
  invalidation. Real stereo/resampling fixtures cover MP3, M4A, restart reuse,
  packaging, installation, and cancellation while the decoder child is active;
  non-macOS builds advertise WAV only.
- Complete: task-sensitive speech routing for MP4/MOV sources with one
  probe-qualified, full-span AAC track. Dialogue/speech questions transcribe
  through Whisper without loading Molmo; explicitly visual questions remain
  visual-only; general video questions select both operations. Video remains
  one `video` source whose per-file inventory conditionally exposes
  `transcribe_audio`. Its timestamped transcript is durable, conversation
  bound, restart-safe, and retained through compaction. Silent videos,
  partial-span tracks, unsupported codecs, malformed containers, and runtimes
  without the macOS sidecar remain honestly visual-only; a speech-only question
  against one returns typed `video_audio_unavailable` instead of silently
  asking the vision model to guess. Real AVFoundation
  video demux/decode plus deterministic router fixtures cover the boundary.
- Complete: portable bounded DOCX extraction. The isolated document sidecar
  validates the ZIP package, required OOXML parts, paths, compression methods,
  entry/expansion/ratio limits, XML/UTF-8, revision text, and output size before
  admission. Runtime-only releases build this text/HTML/DOCX reader without
  claiming PDF support; verified PDFium artifacts add PDF capability.
- Complete: measured one-hour real-model acceptance across early, middle,
  late, and absent-fact questions. The clean run used Whisper Base English,
  resumed from durable evidence after a gateway restart, and answered through
  the installed Qwen model without retranscribing any window. Results are in
  `docs/regressions/auto-file/one-hour-audio-2026-09-06.md`.
- Next roadmap slice: subtitle extraction and exact on-screen video text.

## Product decisions

1. **Auto is the normal file-chat mode.** File type controls which operations
   are possible; the user's task controls which of those operations are needed.
2. **The harness routes evidence work, not just file extensions.** An MP4 may
   require frames, speech transcription, subtitle extraction, metadata, or a
   combination. A PDF may require embedded text, OCR, visual inspection, or all
   three.
3. **Deterministic rules are a safety floor.** A model-generated plan may add
   useful operations, but it cannot remove an operation that is obviously
   required by the media and request. Audio cannot be answered from pixels;
   exact visible wording cannot be delegated only to a visual captioner.
4. **One answering model remains stable within a conversation in the first
   release.** Auto chooses on-demand extraction and perception specialists. It
   does not casually swap Qwen, Maple, Bonsai, or Ornith mid-conversation and
   discard hot KV/session state. Selecting the primary answering model
   automatically is a later, separately measured phase and happens at a new
   conversation boundary unless an explicit migration contract exists.
5. **Derived evidence is durable and source-linked.** Exact extracted text,
   OCR, transcript segments, and visual observations survive follow-ups and app
   restart. Expensive work is not repeated when its complete cache key matches.
6. **Exact-source operations outrank generative perception.** Embedded text is
   preferred to OCR; OCR is preferred for literal visible characters; speech
   transcription is preferred for spoken words; vision is preferred for
   objects, layout, actions, and relationships.
7. **No silent downgrade.** Missing models, unsupported codecs, incomplete
   intervals, low-confidence OCR/transcription, resource pressure, and skipped
   pages are reported in structured state and user-facing copy.
8. **Files remain untrusted data.** Extracted instructions, prompt-like tokens,
   code comments, subtitles, and transcripts never gain system or tool
   authority.
9. **Local means local.** No file, pixel, audio sample, transcript, or derived
   evidence is sent to a cloud service by Auto mode.
10. **Work is bounded by resources and coverage, not one arbitrary global file
    count.** The server publishes its current limits. Files are processed
    sequentially where needed, with disk, memory, duration, decoded-pixel, and
    context budgets enforced at the appropriate layer.

## Baseline at branch start

At the branch point, the repository already contained most of the difficult
primitives, but they were split by feature:

- Content-addressed streamed attachment storage with byte sniffing and private
  metadata.
- UTF-8 text/code/structured-data extraction and PDFium text/page rendering.
- Durable conversation document manifests, full-file/retrieval modes, and
  line/page citations.
- Native PP-OCRv6 for images and scanned PDF pages.
- A vision-specific `VisionRoutePlan` that asks the active chat model for
  `read_text`, `inspect_visual`, detail, pages, and video coverage, then
  validates that plan against deterministic fallbacks.
- A provider-neutral one-specialist process supervisor used by VisionPsy and
  Molmo2, including process groups, bounded IPC, cancellation, timeout, and
  teardown.
- On-demand VisionPsy and Molmo2 routing, resource admission, primary-backend
  pause/restart, and evidence handoff.
- Two Whisper.cpp speech-to-text choices exposed only through the short
  hands-free voice endpoint. That endpoint accepted browser-produced mono
  16 kHz PCM WAV under two minutes and deleted its temporary transcript.

The main design gap is not a missing classifier. It is the absence of one typed
source/evidence contract shared by documents, images, video, and audio. Adding
podcast uploads directly to the existing voice handler would create another
special case and lose durability, timestamps, retrieval, mixed-media planning,
and continuation.

## Architecture

```text
drop/picker/API
      |
      v
content-addressed attachment store
      |
      v
deterministic media inventory
  magic/container/codec, duration/pages/dimensions, available exact text
      |
      v
capability harness
  deterministic operation floor + validated model-assisted task plan
      |
      v
resource scheduler and provider registry
      |
      +--> exact extractors: text, PDF, DOCX/HTML, subtitles, metadata
      +--> OCR provider: PP-OCRv6
      +--> vision providers: Molmo2 / VisionPsy / native projector
      +--> speech providers: Whisper Base / Whisper Tiny / future STT providers
      |
      v
durable typed evidence ledger
      |
      +--> full evidence when it fits
      +--> source-aware retrieval when it does not
      |
      v
stable conversation answering model
      |
      v
answer + file/page/line/time citations + coverage/limitation state
```

### 1. Typed source inventory

Replace capability booleans such as `image`, `video`, and `document` as the
sole routing input with a versioned inventory. The booleans remain in legacy
API responses for compatibility.

Representative shape:

```json
{
  "schema": "samosa.source.v1",
  "attachment_id": "<sha256>",
  "filename": "episode-12.m4a",
  "media": {
    "kind": "audio",
    "container": "mp4",
    "codec": "aac",
    "media_type": "audio/mp4",
    "duration_seconds": 3842.75,
    "streams": [
      {"kind": "audio", "index": 0, "channels": 2, "sample_rate": 48000}
    ]
  },
  "available_operations": ["transcribe_audio", "inspect_metadata"],
  "limitations": []
}
```

Inventory is deterministic and runs before model planning. It uses bytes and
container structure, never the browser MIME type or extension as authority.
Malformed or deceptive inputs fail before a specialist is loaded.

The inventory implementation must be bounded:

- regular, non-symlink private files only;
- bounded container tables and string sizes;
- no whole-media decode to discover metadata;
- decoded dimensions, duration, stream count, page count, and expansion limits;
- stable error codes for malformed, encrypted, unsupported, or too-large input;
- an inventory fingerprint in every derived-evidence cache key.

### 2. Closed operation vocabulary

The harness plans operations, not model names. Initial operations are:

| Operation | Evidence produced | Typical provider |
| --- | --- | --- |
| `extract_text` | verbatim text with page/line anchors | native text/PDF/DOCX/HTML extractor |
| `ocr_text` | literal visible text with page/region anchors and confidence | PP-OCRv6 |
| `inspect_visual` | objects, layout, diagrams, relationships, image meaning | Molmo2 or VisionPsy |
| `analyze_video` | timestamped actions/scenes/objects | Molmo2 |
| `transcribe_audio` | timestamped transcript segments | selected STT provider |
| `extract_subtitles` | verbatim timed subtitle cues | native container/subtitle extractor |
| `inspect_metadata` | bounded deterministic file/media metadata | native inventory sidecar |
| `retrieve_evidence` | ranked verbatim evidence spans | local lexical/source-aware index |

New providers implement operations; they do not add provider-specific branches
to the browser or chat request shape.

### 3. Auto plan contract

The current `VisionRoutePlan` becomes a generic `SamosaEvidencePlan`. A plan is
per attachment because two files with the same extension may require different
work in the same turn.

```json
{
  "schema": "samosa.evidence-plan.v1",
  "attachments": [
    {
      "attachment_id": "<sha256>",
      "operations": ["transcribe_audio"],
      "coverage": {
        "kind": "relevance_first",
        "time_ranges": [],
        "pages": []
      },
      "requirements": ["spoken claims", "timestamp citations"],
      "quality": "balanced"
    }
  ],
  "cross_source_task": false,
  "can_return_partial": true
}
```

Allowed coverage kinds initially are:

- `explicit`: named page, time range, file, or image;
- `relevance_first`: use cheap exact evidence and cached indexes to select
  likely spans, expanding when evidence is insufficient;
- `overview`: representative bounded coverage with explicit limitations;
- `exhaustive`: process all task-required pages/intervals until complete or a
  real cancellation/resource boundary is reached.

Plan validation enforces attachment identity, operation/media compatibility,
range bounds, output budgets, and a closed enum for every field. Unknown fields
are ignored. A malformed model plan cannot choose an executable, model path,
filesystem path, URL, or arbitrary command.

The deterministic floor is applied before and after the model-assisted plan.
Examples:

- “What did the guest say about battery life?” on audio requires
  `transcribe_audio`.
- “What happens when she says ‘launch’?” on video requires both
  `transcribe_audio` and `analyze_video`, followed by time alignment.
- “Read the total on this receipt” requires `ocr_text`; visual inspection may
  supplement layout but cannot replace OCR.
- “Describe this photo” requires `inspect_visual`; OCR is added only when the
  task or inventory indicates meaningful visible text.
- “Summarize this digital PDF” starts with `extract_text`; it does not render
  every page merely because vision is installed.

### 4. Catalog-driven provider registry

Provider selection must stop being a chain of model-ID conditionals. Extend
catalog/runtime descriptors with fields that the harness can score safely:

```json
{
  "role": "auxiliary",
  "operations": ["transcribe_audio"],
  "input_kinds": ["pcm_s16le_mono_16khz"],
  "languages": ["en"],
  "quality_tier": "balanced",
  "latency_tier": "medium",
  "resource_class": "cpu-medium",
  "load_policy": "on_demand_per_job",
  "runtime_protocol": "samosa.specialist.v1",
  "supports_timestamps": true,
  "supports_resume": true
}
```

Selection is deterministic after the evidence plan:

1. Filter to verified, installed providers that implement every required
   capability for that operation and platform.
2. Reject providers that cannot meet required language, timestamp, accuracy,
   input, or coverage properties.
3. Apply live memory/thermal/concurrency admission.
4. Score the remaining candidates by user policy (`quality`, `balanced`, or
   `fast`), measured latency, quality tier, and work already cached.
5. Record the selected provider and reason in the trace and evidence ledger.

For the current STT catalog, Whisper Base English should normally win balanced
or quality mode; Whisper Tiny English may win fast mode or a resource-constrained
retry. Tiny must not silently replace Base for a quality request. The registry
must support future multilingual, diarization, or higher-accuracy STT providers
without changing chat orchestration.

### 5. Unified specialist scheduler

Generalize the existing one-multimodal-specialist supervisor into a resource
scheduler while retaining its hardened process supervision. Not all helpers
need the same exclusivity:

- `metal-large`: Molmo2 and other large MLX specialists; mutually exclusive
  with primary Metal generation on the 16 GiB reference host;
- `metal-medium`: VisionPsy or OCR packs when measured admission permits;
- `cpu-stt`: Whisper.cpp transcription;
- `io-extractor`: PDF/text/container extraction;
- `primary-generation`: the conversation answering model.

The scheduler owns admission, ordering, cancellation, and teardown. V1 should
prefer sequential evidence phases even when two operations could theoretically
overlap: predictable memory, zero swap growth, and prompt/session preservation
matter more than maximum throughput on the reference machine.

A specialist lease carries:

- provider and operation;
- turn/job/attachment correlation IDs;
- declared memory and scratch estimates;
- timeout and cancellation token;
- primary-backend pause/restart requirement;
- progress and completed-coverage checkpoints;
- guaranteed release callback.

The scheduler must never hold a lease while waiting for user approval to
download a model.

### 6. Durable evidence ledger

Add a content-addressed derived-evidence store rather than placing transcripts
or observations only in the current prompt. A representative record is:

```json
{
  "schema": "samosa.evidence.v1",
  "evidence_id": "<sha256>",
  "attachment_id": "<sha256>",
  "operation": "transcribe_audio",
  "provider": {
    "model_id": "voice-stt-whisper-base-en",
    "model_revision": "...",
    "runtime_fingerprint": "..."
  },
  "source_range": {"time_start": 900.0, "time_end": 1200.0},
  "status": "complete",
  "segments": [
    {
      "start": 902.42,
      "end": 907.18,
      "text": "...",
      "confidence": null
    }
  ],
  "limitations": ["english_only", "speaker_labels_unavailable"]
}
```

Cache identity includes:

`attachment hash + exact source range/rendered pixels + operation + provider
model hash/revision + runtime/preprocessor/inventory fingerprint + normalized
task parameters + decode settings`

Exact extractors and generic transcription can be reused across questions.
Task-prompted visual observations retain the normalized task prompt in their
key, so a generic caption is never mistaken for evidence answering a different
question.

Conversation state stores source/evidence references, not duplicate raw bytes.
Detaching a source removes the conversation reference and retrieval state; it
does not rewrite original bytes or unrelated cache entries. Garbage collection
removes only unreferenced derived records under a documented age/size policy.

### 7. Podcast and long-audio path

The short microphone endpoint is not the podcast implementation. Factor a
shared STT runner beneath both paths, then give file audio its own bounded,
durable workflow.

#### Media support

First vertical slice:

- PCM WAV, sniffed and parsed natively on every supported platform;
- MP3 and M4A/AAC on macOS through a small native media-decode sidecar using
  system AVFoundation/AudioToolbox, without loading Molmo2;
- audio tracks in MP4/MOV after the video router can keep dialogue-only work
  separate from frame inspection;
- retain an explicit unsupported-codec result on platforms without a reviewed
  decoder.

Before claiming cross-platform compressed-audio support, select and qualify a
portable decoder strategy. Candidates must be reviewed for license,
distribution size, malformed-media isolation, seeking accuracy, and bounded
streaming. Do not quietly make FFmpeg a new runtime dependency or claim Linux
support from a macOS-only decoder.

Later qualified formats may include FLAC and Ogg/Opus. File extensions and
browser MIME declarations never grant support.

#### Decode and transcription

1. Probe container/streams without full decode.
2. Select one audio stream by deterministic policy; preserve the choice in
   metadata.
3. Decode/resample/downmix sequentially to the provider's required PCM shape.
   Never materialize an hours-long uncompressed copy in RAM.
4. Transcribe bounded windows with overlap and stable absolute timestamps.
5. Reconcile overlap deterministically so words are not duplicated or lost at
   boundaries.
6. Publish each completed window atomically. Cancellation or a crash preserves
   completed windows and a resumable next offset.
7. Build a transcript index with timestamp anchors and paragraph/topic-friendly
   chunks.
8. For a broad summary, aggregate bounded transcript sections hierarchically
   while retaining the segment ledger. For a narrow question, retrieve
   relevant verbatim transcript spans.
9. Cite answers as `episode-12.m4a · 15:02–15:07` and expose the exact evidence
   segment used.

Speaker diarization is not implied by Whisper Base/Tiny. Until a qualified
provider supports it, transcripts use neutral speaker-unknown segments and the
UI states that speaker labels are unavailable. The answering model must not
invent speaker identities from turn order.

Language is also explicit. The two cataloged STT models are English-only. An
audio file detected or requested as another language returns
`stt_language_unsupported` unless a verified capable provider is installed; it
does not generate an English-looking transcript and pretend success.

### 8. Video becomes a composite source

Current Molmo2 video analysis intentionally ignores audio and subtitles. Auto
mode changes video inventory into streams and plans against them:

- visual question: `analyze_video`;
- dialogue question: `transcribe_audio` or `extract_subtitles`;
- visual + spoken relationship: both, aligned on the same media timeline;
- exact on-screen wording: `ocr_text` on selected frames, optionally with
  visual context;
- metadata-only question: `inspect_metadata`, with no model load.

Each provider emits absolute media timestamps. Cross-modal synthesis receives
an aligned timeline of source-labelled evidence. Coverage reports visual frame
sampling separately from audio transcription coverage; a complete transcript
does not make sparse visual sampling exhaustive.

### 9. Document and code breadth

Preserve the deep-file-chat rule: canonical text is verbatim evidence and is
never replaced by a generated summary.

Near-term format additions:

- bounded DOCX ZIP/XML extraction with expansion, entry-count, path, and ratio
  limits;
- complete: attached HTML through one shared portable readable-text extractor,
  preserving title and useful block/list/table boundaries while stripping
  active and non-readable containers;
- UTF-8 text regardless of extension, including more source/config/log
  filename hints without extension-based trust;
- XLSX as structured sheet/cell evidence after a bounded ZIP/XML extractor is
  available; CSV/TSV continue through the exact text path.

Legacy `.doc`, RTF, encrypted office documents, arbitrary archives,
executables, and unknown binary formats remain explicit unsupported types until
they have a reviewed bounded extractor. “Drop anything” means one interaction
for all supported inputs, not silently parsing every byte sequence as text.

Code-aware retrieval should boost exact identifiers, paths, quoted strings,
JSON/YAML keys, symbols, headings, and adjacent chunks. It should cite line
ranges, not generated summaries.

### 10. Quantity and mixed-file behavior

The current browser limit of six pending attachments is a UI constant, while a
conversation can bind many more documents. Replace that mismatch with a
server-published capability response:

```json
{
  "max_files_per_turn": 0,
  "max_file_bytes_by_kind": {"video": 4294967296},
  "max_queued_bytes": 0,
  "disk_reserve_bytes": 0,
  "supported_media": [],
  "operations": [],
  "providers": []
}
```

Zero above means “reported from live policy,” not unlimited. Actual defaults
must be chosen from measured tests. The gateway is authoritative and rejects
before expensive processing when disk reserve or queue policy cannot be met.

The harness builds one plan across all sources, then processes by dependency:

1. cheap inventory and exact extraction;
2. retrieval/candidate selection;
3. OCR/STT/visual specialist work in admitted batches;
4. cross-source alignment/comparison;
5. final synthesis.

Large collections do not concatenate every derived character into one prompt.
They use source-aware retrieval and bounded hierarchical aggregation. The final
answer states which files and ranges were actually examined.

### 11. UI contract

- Add one prominent **Files** action and drag/drop target accepting multiple
  supported sources. Separate image/video/document shortcuts may remain as
  convenience affordances, but they call the same upload path.
- Add audio picker hints only after gateway support exists. The picker `accept`
  attribute mirrors the capability endpoint; it is never the security boundary.
- Show the server-sniffed type, size/duration/pages, and durable source status
  on each chip.
- Show truthful stages such as **Inspecting**, **Reading text**, **Transcribing
  with Whisper Base**, **Inspecting frames with Molmo2**, **Searching the
  transcript**, and **Answering**.
- Make Auto visible as the routing policy, not as a fake model that hides the
  selected answering model. Example: **Auto evidence · Ornith answers**.
- Offer `Quality`, `Balanced`, and `Fast` as optional policy preferences. They
  influence eligible-provider scoring and coverage, never override a required
  capability or safety limit.
- Persist source cards across follow-ups/reload. Provide detach, retry failed
  evidence, and cancel active processing.
- Render page, line, image, and timestamp citations through safe text nodes.
- Missing-model continuation names the exact required provider, download size,
  source, and reason. No download begins without user action.
- Never display “fully read” when only selected pages, transcript ranges, or
  sampled frames were processed.

### 12. API and state contracts

Add or evolve these authenticated contracts:

- `GET /v1/capabilities/sources` — supported sniffed types, operations, current
  limits, provider readiness, and platform limitations;
- `POST /v1/attachments` — unchanged raw-byte upload, enriched typed inventory
  in the response;
- `GET /v1/attachments/<id>/inventory` — deterministic source metadata;
- `GET /v1/conversations/<id>/sources` — durable attached sources and evidence
  readiness/coverage;
- `DELETE /v1/conversations/<id>/sources/<attachment-id>` — detach;
- chat request `routing_policy: auto|manual-compatible` and optional
  `quality_policy: quality|balanced|fast`;
- versioned SSE events for inventory, plan, provider selection, extraction,
  transcription windows, vision/OCR progress, retrieval, coverage, handoff,
  and failure;
- response `samosa.evidence` metadata listing evidence IDs, citations,
  providers, and limitations actually used.

Keep existing `attachment_ids`, document routes, voice endpoints, and OpenAI
response shapes compatible. An ordinary text-only request must remain the
current byte-identical fast path with no planner call.

### 13. Security, privacy, and failure boundaries

- Every parser/decoder runs out of process when malformed input could hang,
  crash, or allocate unpredictably.
- Helpers receive only validated private attachment paths or inherited file
  descriptors. They bind no network ports and make no network requests.
- Provider output is parsed against a bounded schema before it becomes
  evidence. Raw child diagnostics never enter the model prompt or browser.
- Audio/video temporary PCM/frame files are private, bounded, and removed after
  atomic evidence publication. A crash leaves recoverable job metadata, not a
  trusted partial record.
- Transcript, OCR, document, subtitle, and visual text is delimited as
  untrusted source data in synthesis prompts.
- Cancellation works during inventory, decode, extraction, model load,
  transcription, page/frame processing, retrieval, and synthesis.
- Resource pressure stops at a completed evidence boundary and records exact
  unprocessed ranges. It never fabricates completion.
- Missing/corrupt provider packages fail closed and enter the existing verified
  download/repair flow.
- Developer tracing remains opt-in because it can include raw derived evidence.
  Normal telemetry records identifiers, operation decisions, timing, counts,
  coverage, and error codes without file contents.

## Delivery plan

### Phase 0 — freeze behavior and regression fixtures

- Add a mixed-media routing corpus with fake providers: digital PDF, scanned
  receipt, diagram with labels, source code, WAV podcast, video with dialogue,
  and a video requiring both dialogue and frame reasoning.
- Capture current image/PDF/video behavior so the refactor cannot regress it.
- Add late-file/late-audio sentinels, prompt-injection text, malformed media,
  cancellation, restart, and resource-pressure cases.
- Define `samosa.source.v1`, `samosa.evidence-plan.v1`,
  `samosa.evidence.v1`, provider descriptor, progress, and error schemas.

Exit: fixtures fail only because the generic contracts and audio attachment
path do not yet exist; current routes remain green.

### Phase 1 — extract the generic capability harness

- Introduce source inventory and operation enums without changing visible
  behavior.
- Adapt the current `VisionRoutePlan` to the generic per-attachment plan.
- Put VisionPsy, Molmo2, PP-OCR, PDF/text extraction, and the current Whisper
  choices behind provider descriptors.
- Add the resource scheduler abstraction around the existing hardened
  multimodal supervisor and current voice mutex.
- Emit a developer-trace plan showing deterministic floor, validated
  model-assisted additions, provider choice, and coverage.

Exit: all existing document/image/video and microphone tests pass through the
new harness with byte-equivalent evidence and no extra model call for text-only
turns.

### Phase 2 — durable audio/podcast vertical slice

- Accept sniffed PCM WAV attachments and add the `audio` source kind.
- Factor the Whisper runner below the short microphone and file-transcription
  paths.
- Implement bounded window transcription, overlap reconciliation, absolute
  timestamps, resumable progress, durable transcript evidence, lexical
  retrieval, and time citations.
- Bind audio sources to conversations and restore them after restart.
- Add missing-STT download-and-continue without duplicating the user turn.

Exit: a one-hour English WAV podcast can answer an early, middle, late, and
absent-fact question after restart without retranscribing completed windows.

Status: complete on the 16 GiB Apple M3 reference machine. The measured gate
also caught and fixed overlap-aware tail counting, short-tail Whisper timestamp
clamping, and term-balanced transcript retrieval before it was accepted.

### Phase 3 — compressed audio and video audio tracks

- Complete: add and package the isolated native media probe/decode sidecar.
- Complete: qualify MP3 and M4A/AAC on macOS with real decode fixtures.
- Complete: qualify full-span AAC tracks in MP4/MOV on macOS and route
  dialogue-only questions without loading Molmo.
- Complete: align Whisper transcript and Molmo2 frame evidence on one absolute
  timeline for speech/action relationship questions, with separate labelled
  audio and visual coverage passed to the answering model.
- Next: add subtitle cues and exact on-screen text with explicit cross-evidence
  coverage.
- Record platform/codec gaps rather than accepting files that cannot be
  decoded.

Exit: the same video can answer a dialogue-only question without Molmo, a
visual-only question without STT, and a dialogue/action relationship question
using both with correct coverage metadata.

### Phase 4 — document and structured-format breadth

- Complete: land attached HTML readable-text extraction with byte-sniffed
  admission, hard UTF-8/output bounds, durable evidence, and release gates.
- Complete: bounded DOCX extraction and clean/runtime-only packaging.
- Expand source/config/log display typing while retaining byte-based UTF-8
  admission.
- Add bounded XLSX sheet/cell extraction and citations after its ZIP/XML safety
  corpus passes.
- Improve code/structured-data chunking and identifier/path/key ranking.

Exit: supported format claims match real portable extractors; malicious ZIP,
XML, encoding, and expansion cases fail specifically without affecting the
resident model.

### Phase 5 — mixed-source scale and UI Auto experience

- Replace the six-file browser constant with server-reported live policy.
- Add multi-select and drag/drop, persistent source cards, provider/stage
  progress, coverage, cancellation, retry, detach, and citations.
- Add bounded cross-source retrieval/aggregation so quantity does not become
  prompt concatenation.
- Qualify duplicate uploads, simultaneous uploads, mixed source orders, and
  partial provider availability.

Exit: a mixed turn containing documents, code, images, audio, and video routes
each operation correctly and answers from labelled evidence without manual
mode selection.

### Phase 6 — optional automatic answering-model selection

- Measure whether choosing the primary text model improves quality enough to
  justify cold starts and session migration.
- If justified, choose an answering model at new-conversation creation from
  verified capability/resource data.
- Define an explicit conversation migration/replay contract before any
  mid-conversation switch. Never drop pinned documents, source bindings, or
  provenance merely to load a different model.

Exit: model selection is shipped only if it beats the stable-synthesizer
baseline on quality, latency, memory, restart, and long-context gates.

## Verification matrix

### Deterministic tests

- Magic/container sniffing beats filename and declared MIME type for every
  supported source.
- Unsupported codec/container, malformed input, binary-as-text, XML expansion,
  duration/dimension overflow, symlink, and path replacement fail closed.
- Deterministic floors survive malformed or under-specified model plans.
- Provider selection respects required operation, language, timestamps,
  quality policy, installation, platform, and resource admission.
- No specialist is loaded for a plain text-only turn or a metadata-only file
  question.
- OCR, STT, vision, subtitle, and exact-text evidence retain source anchors and
  untrusted-data delimiters.
- Cached evidence invalidates on source, model, runtime, preprocessor,
  inventory, range, prompt, or decode-setting changes.
- Cancellation/restart resumes only completed atomic windows and never emits a
  duplicate answer.
- Mixed files are order-independent; duplicate bytes share storage without
  merging display identity or conversation state incorrectly.
- Every success, partial, and failure path releases provider leases and child
  process groups.

### Real-model acceptance gates

1. **Scanned receipt:** exact total/invoice number from OCR; layout question
   from vision; combined answer cites the image and exposes disagreement.
2. **Diagram:** OCR preserves labels and Molmo/VisionPsy explains relationships.
3. **Digital and scanned PDF:** narrow and exhaustive questions choose the
   correct text/OCR/vision mix and retain page citations after restart.
4. **Source + specification:** compare a late-defined symbol to a late PDF
   requirement with correct line/page citations.
5. **One-hour podcast (core gate complete):** early/middle/late fact questions
   and absent-fact refusal with timestamp citations; second questions do not
   retranscribe. Overview and exact-quote quality remain part of the broader
   evaluation corpus rather than the Phase 2 shipping gate.
6. **Long/noisy podcast:** cancellation and resume, explicit English-only and
   speaker-label limitations, and no invented speaker identity.
7. **Video with dialogue:** dialogue-only, visual-only, on-screen-text, and
   speech/action relationship questions select the minimum sufficient
   operations and report separate audio/visual coverage.
8. **Mixed-source turn:** at least ten varied sources on a measured policy,
   including duplicate content and one unsupported input, without losing
   successful evidence from the others.
9. **16 GiB resource gate:** zero new swapouts, no critical pressure event,
   bounded peak process-tree RSS, primary-model restart measured separately,
   and no auxiliary child left resident after the turn.

Record model hashes, runtime fingerprints, machine/OS, source duration/pages,
selected plan/providers, evidence coverage, wall time, time to first progress,
time to answer, peak RSS, and swap delta under `docs/regressions/auto-file/`.

## Initial code map

- `src/samosa_gateway.c`: split attachment inventory, evidence planning,
  provider selection, evidence acquisition, and synthesis out of the current
  vision-specific chat block; retain HTTP and durable conversation ownership.
- New `src/samosa_evidence.{h,c}`: versioned source/plan/evidence types,
  validation, provenance, and ledger publication.
- New `src/samosa_router.{h,c}`: deterministic operation floors and
  catalog-driven provider scoring. Use “evidence router” in names to avoid
  confusion with Qwen/Maple MoE routing.
- `src/samosa_multimodal.{h,c}`: preserve process hardening; evolve or wrap it
  with resource-class leases rather than weakening the single-specialist
  invariant accidentally.
- New native media sidecar: bounded probe, seek, decode, downmix, and resample;
  exact filename depends on the decoder decision in Phase 3.
- `src/samosa_extract.c`, `src/samosa_html.{h,c}`, and
  `src/samosa_docx.{h,c}`: shared HTML and bounded DOCX extraction are
  complete; XLSX exact extraction and later version bumps remain.
- `assets/models.json`: operation/provider metadata for OCR, vision, and STT;
  future providers become catalog entries rather than gateway ID branches.
- `assets/app.html`: unified multi-file drop/picker, Auto policy display,
  source/progress/coverage cards, timestamp citations, and server-driven limits.
- `tests/fake_multimodal_helper.c` and new fake STT/media helpers: deterministic
  plan/provider/lifecycle/error coverage without model downloads.
- `docs/SERVE_API.md`, `docs/USAGE.md`, and `README.md`: update only after each
  claimed capability passes its release gate.

## Definition of done

The feature is complete when a user can drop supported documents, code, images,
audio, and video together and ask an ordinary question; Samosa automatically
selects the necessary exact extractors and local specialists, preserves
source-linked evidence across follow-ups and restart, answers through a stable
conversation model, cites pages/lines/times, reports incomplete coverage
honestly, and unloads transient models without swap growth on the reference
machine.

It is not complete if audio is only accepted by the short microphone endpoint,
if every video always loads both vision and STT, if a generated summary replaces
canonical evidence, if model choice is hard-coded by filename, if follow-ups
repeat expensive perception work, or if “Auto” hides unsupported formats and
partial coverage.
