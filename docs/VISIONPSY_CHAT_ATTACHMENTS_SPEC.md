# VisionPsy-Nano 460M for automatic chat-attachment understanding

Status: implemented; targeted regression and real-checkpoint gates pass (2026-08-23)
Target release: first release on macOS Apple Silicon only
Scope: Samosa chat attachments only
Model: standard `VisionPsy-Nano-460M`, using the BF16 MLX conversion published at
[`KaedeTai/VisionPsy-Nano-460M-MLX`](https://huggingface.co/KaedeTai/VisionPsy-Nano-460M-MLX)

This file is both the product contract and the implementation audit. The
feature is implemented in the current worktree. The dated evidence report is
[`regressions/visionpsy-460m-2026-08-23.md`](regressions/visionpsy-460m-2026-08-23.md).
That report deliberately separates tests that passed from release claims that
still require a combined active-LLM/desktop qualification run.

## 1. Confirmed product decisions

These are requirements, not open questions:

- Falcon-OCR is not part of this work.
- Samosa keeps its existing native OCR reader. That reader is Samosa's C port of
  the pinned PP-OCRv6 detector and recognizer; it is not Tesseract or olmOCR.
- Add the standard VisionPsy-Nano 460M model as an auxiliary vision specialist.
  Do not substitute the Flash variant.
- Use the BF16 MLX artifact currently available in the named repository. The
  repository is approximately 1.01 GB on disk. It does not currently publish a
  smaller standard-model quantization.
- The first supported platform is macOS on Apple Silicon. Do not show the model
  as installable on unsupported platforms.
- The distributed Samosa application must not contain, install, launch, or
  depend on a Python runtime. Python may be used by development-only parity
  tests, but those tools and dependencies must not enter the application or
  release package.
- VisionPsy loads only after the active chat LLM decides that visual evidence is
  needed. It must not remain resident between ordinary text turns.
- The active chat LLM automatically decides whether a request needs document
  text extraction/OCR, VisionPsy, or both. V1 has no manual OCR/vision routing
  switch.
- V1 covers attachments in ordinary chat. Jobs, Chutni, background folder
  analysis, and other workflows are explicitly out of scope.
- There is no fixed page cap, including no five-page cap. There is also no
  user-facing maximum-pages setting. Coverage is determined by the user's task,
  the document, and live hardware conditions.
- If VisionPsy is required but not installed, Samosa offers the download,
  preserves the pending turn, and automatically continues that same turn after
  a successful verified installation.
- If text extraction/OCR succeeds but VisionPsy fails, Samosa returns a clearly
  labelled OCR-only partial answer. It must not represent that answer as a full
  visual analysis.

## 2. Goals

1. Let a user attach a photograph, scan, image, or PDF and ask a natural-language
   question without selecting a processing mode.
2. Use the existing reader for literal text and VisionPsy for visual semantics
   such as captions, objects, spatial relationships, page layout, diagrams, and
   visual question answering.
3. Combine the resulting evidence with the active text LLM to answer the user's
   actual question, with attachment and page provenance retained.
4. Work safely on the 16 GB Apple Silicon reference configuration while Samosa,
   its active LLM, and normal user applications remain usable.
5. Add a clear model catalogue and category-based Models settings experience
   for text LLMs, vision, OCR, and voice.
6. Fail safely, explain what completed and what did not, and offer a concrete
   recovery action.

## 3. Non-goals

- Replacing or benchmarking the existing PP-OCRv6 reader as part of this work.
- Adding Falcon-OCR, Tesseract, or olmOCR.
- Making VisionPsy a selectable conversation/chat model.
- Jobs, Chutni, filesystem-wide indexing, or unattended document processing.
- Windows, Linux, or Intel macOS support in the first release.
- Shipping `mlx-vlm`, Transformers, Pillow, PyTorch, or any other Python runtime.
- Creating a new quantization without a separately reviewed conversion and
  quality-validation project.
- Promising support for every language. Repository tags or isolated examples do
  not constitute a Samosa support claim; document only the languages Samosa
  actually tests.

## 4. Why this is a native auxiliary model

The Hugging Face repository contains MLX weights and configuration, not a native
executable. Safetensors describe tensors; they do not describe an executable
model graph, image preprocessing, generation, or a service protocol. The
repository's current examples use Python and a VisionPsy-specific `mlx-vlm`
integration that is not a drop-in C++ runtime.

Samosa uses the compiled `samosa-visionpsy` helper, built on the same vendored
MLX C++ foundation already used by
Samosa's native Maple runtime. The helper should reuse existing Samosa
components for:

- safetensors and configuration loading;
- tokenizer loading and token generation;
- MLX device/Metal execution;
- KV-cache and deterministic decoding patterns;
- model download, verification, locking, and atomic installation;
- process supervision, cancellation, logging, and health reporting; and
- attachment containment and image/PDF page rendering.

The VisionPsy-specific work is limited to the architecture adapter: SigLIP2
vision encoder, pixel-shuffle projector, SmolLM2 decoder wiring, the exact image
preprocessor, tensor-name mapping, and the model's prompt format. This is not a
new inference framework and must not duplicate MLX kernels, Metal kernels, the
model manager, downloader, tokenizer infrastructure, or HTTP server stack.

An in-process adapter was rejected for v1 because a separate helper gives Samosa
an enforceable load/unload boundary, isolates model crashes and Metal failures,
and makes memory reclamation at the end of a vision turn observable. The helper
is local-only and is never exposed as a general network service.

## 5. Model artifact and release qualification

### 5.1 Artifact policy

The catalogue and release manifest record:

- an immutable Hugging Face revision rather than `main`;
- every required filename and exact byte size;
- SHA-256 for every downloaded file;
- model and preprocessor configuration fingerprints;
- tokenizer fingerprint;
- upstream model identity and licence;
- the community MLX conversion identity and licence/attribution; and
- the VisionPsy runtime adapter version.

Installation is staged in a temporary model directory, verified in full, and
atomically promoted. A partial, unverified, or wrong-revision download is never
reported as installed. Existing model-manager locking must prevent two app
windows or processes from installing the same model concurrently.

### 5.2 Precision decision

BF16 is the v1 candidate because it is the precision actually published at the
user-selected MLX repository. The model card reports an average peak GPU-memory
measurement of roughly 2.64 GB for its sample workload; that is upstream
self-reporting, not a Samosa capacity guarantee. Samosa must reproduce memory,
latency, thermal, and quality measurements under its own combined workload.

If BF16 cannot pass the 16 GB qualification gate in section 16, the release is
blocked. The implementation must not silently switch to the Flash model, invent
a quantization, or reduce correctness until the result happens to fit. A future
quantized standard artifact requires its own conversion provenance, numerical
parity tests, quality comparison, and catalogue entry.

## 6. End-to-end turn architecture

The active chat LLM remains responsible for understanding the user request and
writing the final answer. VisionPsy produces evidence; it does not replace the
chat LLM.

The turn has six stages:

1. **Inventory.** The gateway validates attachment IDs and records media type,
   page count, dimensions, text-layer availability, and cached reader results.
2. **Plan.** The active LLM receives the question plus safe attachment metadata
   and returns a strict processing plan: `read_text`, `inspect_visual`, or both,
   along with the evidence requirements and coverage strategy.
3. **Acquire evidence.** The native reader extracts embedded text and invokes
   OCR only where necessary. If visual inspection is requested, the gateway
   starts VisionPsy on demand and schedules the necessary image or pages.
4. **Assess sufficiency.** After each meaningful evidence batch, the active LLM
   or a compact deterministic planning pass determines whether the task is
   answered or whether more pages/regions are required.
5. **Synthesize.** The active LLM receives bounded, labelled evidence with page
   provenance and produces the answer. Document content is treated as untrusted
   data, not as system or tool instructions.
6. **Release.** VisionPsy exits at the end of the vision-using turn. The gateway
   confirms termination and memory release. Cached evidence remains subject to
   the cache policy in section 13.

The text LLM and VisionPsy do not generate concurrently in v1. Planning occurs,
then visual inference, then synthesis. This keeps combined transient memory and
Metal contention controlled. The active LLM may remain resident if the measured
admission policy permits it, but its generation is quiescent while VisionPsy is
running.

### 6.1 Migration from the current attachment path

The existing chat attachment implementation treats images as input that the
active chat backend itself must support. It rejects an image when the selected
chat backend lacks image support and otherwise embeds a base64 `image_url`
directly in that backend request. This must no longer be the only path.

For this feature:

- `attachment_augment()` inventories the attachment and participates in the
  evidence plan instead of rejecting it solely because the active text LLM is
  text-only;
- a text-only chat LLM can request VisionPsy evidence and then synthesize from
  that evidence;
- existing chat backends with their own image support do not bypass automatic
  planning merely because they accept an image payload—the same routing policy
  decides which specialist path is used; and
- document extraction/retrieval remains available, but its result becomes one
  labelled evidence source rather than an unstructured prompt suffix.

Direct-image injection can remain behind a compatibility path for conversations
that explicitly depend on an existing multimodal chat backend, but it must not
make VisionPsy installation mandatory for text-only or OCR-only turns and must
not create two uncoordinated visual analyses in one turn.

## 7. Automatic routing contract

### 7.1 Planner input

The planner receives:

- the user's exact question or task;
- attachment IDs, types, page counts, and image dimensions;
- whether a PDF has an embedded text layer;
- compact text samples or reader metadata already available in cache;
- whether OCR and VisionPsy are installed and healthy; and
- supported operations, expressed as a closed schema.

It does not receive arbitrary filesystem paths or raw attachment bytes.

### 7.2 Planner output

The output is validated JSON, not free-form prose. A representative schema is:

```json
{
  "attachments": [
    {
      "attachment_id": "att_...",
      "operations": ["read_text", "inspect_visual"],
      "evidence_needed": [
        "literal wording and numbers",
        "relationship between labelled objects"
      ],
      "coverage": {
        "kind": "relevance_first",
        "explicit_pages": []
      }
    }
  ],
  "answer_requirements": ["cite attachment and page"],
  "can_return_partial": true
}
```

Allowed `coverage.kind` values are:

- `explicit_pages`: the question names pages or the required pages are known;
- `relevance_first`: cheap text/metadata evidence can rank likely pages, with
  automatic expansion if the first candidates are insufficient; and
- `whole_document`: the task requires complete-document coverage.

The planner expresses task requirements, not a fixed processing quota. It may
identify initial candidates, but it cannot impose an arbitrary page ceiling.

### 7.3 Validation and fallback

The gateway validates operation names, attachment IDs, page numbers, and all
schema bounds. It never follows a model-generated path or URL. If the first
planner result is malformed, Samosa retries once with the validation error. If
the retry is also malformed:

- a PDF or document falls back to the existing safe reader path;
- a directly attached image falls back to VisionPsy if it is installed, because
  the attachment has no useful text-only document path; and
- the final answer discloses that automatic routing degraded if the fallback
  could have omitted a requested visual analysis.

Routing telemetry records the selected operations and reason codes, but not raw
document content.

### 7.4 Meaning of `read_text`

`read_text` is a capability request, not a command to OCR everything. For a
digital PDF, the reader uses the embedded text layer first. OCR is used for
scanned pages, image-only regions, or when the embedded layer is absent or
unusable. This preserves fidelity and avoids needless OCR work.

### 7.5 Required routing examples

These examples are regression cases for intent, not keyword rules:

| User task | Required route | Reason |
|---|---|---|
| “What is the exact invoice total and invoice number?” | `read_text` | Literal characters and numbers are the evidence |
| “What colour is the vehicle in this photo?” | `inspect_visual` | No document-text operation can answer it reliably |
| “Explain this diagram and quote its labels” | both | Vision supplies structure/relationships; OCR supplies literal labels |
| “Summarize this digital PDF” | `read_text`, expanding to vision only when the task or pages require visual content | A usable text layer is cheaper and more faithful than rendering every page |
| “Audit every chart in this report for contradictory trends” | both, whole-document chart coverage | The task explicitly requires exhaustive visual evidence plus labels/numbers |
| “Which page contains the red warning box, and what does it say?” | both, relevance-first with automatic expansion | Vision locates the visual feature and OCR reads its wording |

The planner may reach the same route for differently worded requests. The
gateway must not implement the table as a string/keyword classifier.

## 8. Hardware- and task-adaptive page scheduling

There is deliberately no numeric page limit. A two-page scan can require both
pages; a forty-page report may need one chart for a narrow question or every
page for a complete review. The scheduler combines task coverage with actual
hardware state.

### 8.1 Coverage algorithm

1. Build the complete page inventory before claiming coverage.
2. Honour explicit page references first.
3. For relevance-first work, use cheap evidence such as PDF text, OCR text,
   headings, page dimensions, and prior cached results to rank pages. This is a
   starting order, not a stopping limit.
4. For whole-document work, enqueue every required page.
5. Render and inspect one page at a time because VisionPsy is a single-image
   model. Reuse the loaded model for the rest of the turn.
6. After each useful evidence batch, evaluate task sufficiency. If evidence is
   insufficient, continue to the next relevant page. If the task requires
   exhaustive visual review, continue until every required page has been
   processed.
7. Finish only when the task has sufficient evidence, all required pages have
   been processed, the user cancels, or a real recoverable/unrecoverable error
   occurs.

Samosa must never ask the user to narrow a request merely because an internal
arbitrary count has been reached. It may ask for clarification when the user's
task itself is ambiguous—for example, “compare this” with no indicated item—or
when the attachment is corrupt or inaccessible. Resource pressure is handled by
resolution downshifting, safe refusal, or a partial result; it is not
disguised as a question about page count.

The existing `doc.read` tool currently exposes a page-window count of up to five
for another workflow. Chat vision must not inherit that value as a turn limit.
If a bounded reader call is reused internally, it is only a transport window:
the scheduler issues as many windows as the task requires. Changing the Jobs
tool contract itself remains out of scope for this release.

### 8.2 Admission and pressure control

There is one VisionPsy inference at a time. The chat LLM plans first, VisionPsy
runs second, and the chat LLM synthesizes after the helper has exited. Before
loading the model and before every PDF page, the gateway checks live available
memory, macOS memory-pressure level, thermal-pressure level, total RAM, and the
visual-detail requirement. Available memory inherently reflects the resident
chat LLM and other desktop work at that moment.

The implemented controller samples available memory, macOS memory pressure,
thermal pressure, total RAM, and requested visual detail. It chooses a maximum
input side of 2048, 1536, 1024, or 512 pixels; a 16 GB-class host starts no
higher than 1536. It samples again before every PDF page and may only move to a
lower resolution during a turn. It refuses to start or continue visual work
when available memory is below 2.5 GB, memory pressure is critical, or thermal
pressure is critical. Completed page and text/OCR evidence is retained. The
turn then returns a labelled partial answer when evidence permits, or the
retryable `vision_resource_pressure` error when vision is the only evidence.

The current implementation does not keep a hidden page queue alive while it
waits indefinitely for pressure to clear. This is deliberate: it gives the
user an immediate, honest result/retry state and cannot wedge a turn. There is
no user RAM slider or maximum-pages control.

### 8.3 Long-document evidence management

Visual outputs are stored as structured per-page evidence, not concatenated
without bound into one LLM prompt. Each item includes attachment ID, page,
operation, model fingerprint, concise observation, and confidence/limitations.
For large result sets, the active LLM performs hierarchical aggregation in
bounded chunks while page-level provenance remains attached. A final answer can
therefore cite pages without overflowing the chat model context.

## 9. Native helper contract

### 9.1 Lifecycle

- Start only after a validated plan requests `inspect_visual` and the model is
  installed and admitted by the resource controller.
- Load the model once for that turn.
- Accept sequential image/page requests.
- Support cooperative cancellation during load, prefill, and decode.
- Exit after synthesis no longer needs visual work, on cancellation, or after a
  fatal error.
- Apply a bounded shutdown timeout, terminate the helper process group if it is
  stuck, and verify that the child is gone.

### 9.2 Protocol

Use a versioned framed protocol over inherited pipes or a private Unix-domain
socket. Do not put prompts, attachment paths, or document text in command-line
arguments. Requests include only validated IDs, an already-contained local
input handle/path, task prompt, decode limits, and correlation ID. Responses
include typed progress, structured evidence, usage/timing, and stable error
codes.

The helper must:

- bind no public TCP port and make no network request;
- refuse paths outside the gateway-provided contained attachment area;
- validate decoded dimensions before allocating image tensors;
- cap input pixels and output tokens according to measured model/preprocessor
  constraints, while resizing with the pinned preprocessor rather than silently
  dropping a page;
- use deterministic greedy decoding for parity and reproducibility in v1;
- cap log volume and never return raw internal stack traces to the UI; and
- distinguish malformed model output from an empty but valid answer.

### 9.3 Numerical parity

The adapter must reproduce the selected MLX reference for:

- image resize/crop, colour conversion, normalization, and tensor layout;
- SigLIP2 patch embeddings and selected intermediate blocks;
- pixel-shuffle projector output;
- prompt tokens and image-token placement;
- decoder logits; and
- greedy output tokens.

“Looks plausible” is not an acceptance test. Fixed fixtures and tolerances are
defined before optimizing the native implementation.

## 10. Model catalogue changes

Extend catalogue entries with backward-compatible role/category metadata rather
than inferring behaviour from a display name. Existing entries without the new
fields retain their current meaning.

A representative VisionPsy entry is:

```json
{
  "id": "visionpsy-nano-460m-mlx-bf16",
  "version": "<immutable-hugging-face-revision>",
  "label": "VisionPsy Nano 460M",
  "family": "vision",
  "category": "vision",
  "role": "auxiliary",
  "backend_kind": "mlx_vision_native",
  "supported_platforms": [
    {"os": "macos", "architecture": "arm64"}
  ],
  "required_runtime_abi": "samosa-model-runtime-v1",
  "precision": "bf16",
  "capabilities": [
    "image_understanding",
    "captioning",
    "visual_qa",
    "objects",
    "relationships",
    "document_vision"
  ],
  "routing": "automatic",
  "load_policy": "on_demand_per_turn"
}
```

The actual entry also contains the immutable revision, file manifest, hashes,
licences, measured disk size, minimum Samosa version, runtime protocol version,
and measured compatibility claims. Placeholder hashes or `main` are forbidden
in a release catalogue.

Required catalogue/model-manager work:

- add and validate the auxiliary `vision` family/category and
  `mlx_vision_native` backend;
- preserve the new fields through catalogue APIs rather than stripping them;
- ensure auxiliary models cannot become the active conversation model;
- exclude auxiliary models from first-run chat-model selection;
- filter install availability by macOS/arm64 and validated OS minimum;
- reuse download progress, pause/cancel, checksum, repair, remove, and status
  machinery; and
- keep old catalogues and installed-model records readable.

If the current catalogue schema validator requires a version increment, ship a
documented migration and compatibility test. Do not increment the version solely
for optional passthrough fields if the existing schema contract already permits
them.

### 10.1 Repository integration map

The implementation is expected to touch these existing areas. Exact new source
filenames can follow the repository's conventions, but responsibilities must not
be collapsed into the UI or hard-coded by model display name.

| Area | Current integration point | Required change |
|---|---|---|
| Catalogue | `assets/models.json` | Add the pinned auxiliary VisionPsy entry and artifact manifest |
| Catalogue validation/API | `catalog_validate()`, `emit_catalog_entry()`, artifact/runtime dependency resolution in `src/samosa_gateway.c` | Accept and preserve vision role/category/precision/routing fields, add `mlx_vision_native`, and resolve VisionPsy artifacts/helper without treating it as a chat backend |
| Installation | model install handlers in `src/samosa_gateway.c` | Install auxiliary models without changing the selected chat profile; support repair/remove and continuation wake-up |
| Attachment flow | `attachment_augment()` and chat request assembly in `src/samosa_gateway.c` | Replace the text-model-must-accept-images assumption with plan/acquire/synthesize evidence orchestration |
| Reader/OCR | `attachment_document_json()`, `doc_read_handler()`, `samosa-ocr`, and `read_cache.h` | Preserve embedded-text/OCR cascade and provenance; expose it to the planner without changing Jobs/Chutni behaviour |
| MLX runtime | vendored MLX build plus `src/maple/` native loading/tokenization/generation patterns | Reuse infrastructure and add the isolated VisionPsy architecture adapter/helper |
| Build/package | MLX and `samosa-maple` targets plus install/package targets in `Makefile` and release tools | Build, link, sign, install, and audit `samosa-visionpsy` as arm64 native code |
| Settings UI | the Models markup/rendering and catalogue filters in `assets/app.html` | Replace the flat non-voice list with role-driven categories; prevent VisionPsy from entering setup/chat selection |
| Tests | `tests/test_model_catalog.sh`, `tests/test_model_catalog_ui.mjs`, `tests/test_attachments.sh`, compiled-gateway and reader suites | Add catalogue, UI, routing, lifecycle, continuation, adaptive scheduling, and partial-answer coverage |

Two current hard-coded behaviours require deliberate cleanup:

- the UI currently treats every non-voice catalogue entry as a chat model; it
  must filter by explicit role/category instead; and
- installed-artifact and runtime-dependency resolution contains model/package ID
  branches. Prefer a safe catalogue-driven resolver for auxiliary artifacts and
  an allowlisted `samosa-visionpsy` runtime package over scattering another
  display-name check through the gateway.

## 11. Settings → Models redesign

Models are grouped by purpose rather than presented as one flat list:

### Text LLMs

- installed/available state, download/repair/remove;
- active-chat selection where applicable;
- context and runtime controls already supported by that backend; and
- measured hardware compatibility and current runtime state.

### Vision

- VisionPsy Nano 460M name, standard/BF16 identity, source, licence, and disk
  size;
- Download, cancel download, retry/repair, and Remove actions;
- status: not installed, downloading, verifying, ready, loading, in use,
  unavailable under current resource pressure, failed, or update available;
- platform/support message; and
- read-only policy text: “Used automatically for chat attachments when the
  active LLM needs visual understanding. Loaded only for that turn.”

There is no maximum-pages control and no RAM-budget slider. Automatic routing
is product behaviour, not an advanced switch the user must understand before a
document works.

### OCR

- identify the existing native PP-OCRv6 pack accurately;
- installed/version/verification state and repair action;
- explain that digital PDF text is used before OCR; and
- expose no VisionPsy settings in this category.

### Voice

- preserve the existing speech-to-text and text-to-speech models and their
  backend-specific settings, grouped under Voice (with STT/TTS subsections if
  both are present).

Each category renders only settings its models actually support. Catalogue role
drives placement; hard-coded model-name tests are forbidden. Keyboard access,
screen-reader labels, focus behaviour, and narrow-window layouts are part of
acceptance, not follow-up polish.

## 12. Missing-model download and automatic continuation

When a validated plan first requests VisionPsy and it is absent:

1. Keep the user's message visible in the conversation and mark the turn as
   waiting for its vision model.
2. Present the exact model, download size, source, and a **Download and
   continue** action. Do not begin a network download without this user action.
3. Persist a continuation record containing a turn/idempotency ID, conversation
   ID, immutable attachment IDs/content hashes, validated processing plan, and
   already completed OCR evidence. Do not duplicate attachment bytes.
4. Download, verify, and atomically install through the model manager.
5. On success, automatically resume the same turn from the first incomplete
   stage. Do not insert a duplicate user message and do not rerun successful OCR
   work unless its cache is invalid.
6. Consume the continuation exactly once. App reload/reconnect may rediscover
   and resume a verified waiting turn without generating duplicate answers.

If the user cancels the download, preserve the ordinary chat message and OCR
evidence, mark visual processing as cancelled, and offer Retry. If OCR evidence
can answer part of the request, offer/return an explicitly labelled OCR-only
partial answer. If visual evidence is essential and no text evidence exists,
state that the question cannot yet be answered rather than guessing.

Installation failure never loses the prompt or attachment. The UI shows a
stable reason and Retry/Repair action. A checksum failure deletes or quarantines
only the staged corrupt artifact, never a previously verified installation.

## 13. Evidence, synthesis, and caching

### 13.1 Evidence rules

Every evidence item carries:

- conversation turn and attachment ID;
- page number or direct-image identity;
- operation and model/runtime fingerprint;
- observation/text and relevant confidence/limitation metadata; and
- success, partial, or failure state.

When OCR and VisionPsy both run:

- OCR/embedded text is the primary evidence for exact spelling, literal quotes,
  serial numbers, and table cell text;
- VisionPsy is the primary evidence for objects, layout, relationships, scene
  interpretation, and visual meaning;
- the synthesizing LLM must surface material disagreement rather than silently
  choosing the more convenient output; and
- attachment content that tells the model to ignore Samosa instructions is
  quoted or described as document content and never gains tool authority.

### 13.2 Partial answer rule

If OCR/text extraction succeeded and VisionPsy failed, the response begins with
an unambiguous label such as:

> Partial answer — visual analysis failed; this answer uses text/OCR only.

It then answers only what the text evidence supports, identifies affected pages
when known, and offers Retry visual analysis. The failure is also represented in
structured turn metadata so clients do not need to parse prose.

If VisionPsy was the only viable evidence source and it failed, Samosa returns
no synthesized factual answer. It explains the failure and offers a retry.

### 13.3 Cache keys and storage

Reuse the reader's existing content-addressed OCR/text cache. A VisionPsy result
is reusable only when this complete key matches:

`attachment content hash + page/image identity + rendered pixels hash + model
revision/hash + runtime/preprocessor fingerprint + normalized task prompt hash
+ decode settings`

This prevents a generic caption from being mistaken for the answer to a later,
different visual question. Cache records retain provenance, use user-private
permissions, are written atomically, and participate in a documented bounded
LRU/size policy. They never write beside the user's original document.

## 14. Error model and user experience

All layers propagate a stable machine code, a safe user message, retryability,
completed-evidence state, and a correlation ID. Raw child stderr, internal paths,
prompt text, model tokens, and stack traces stay in size-bounded local diagnostic
logs and are not rendered as the error message.

Required errors and behaviours include:

| Code | User-facing behaviour | Recovery |
|---|---|---|
| `vision_model_required` | Offer model details and **Download and continue**; preserve turn | Install, verify, resume |
| `vision_unsupported_platform` | State that v1 requires macOS Apple Silicon | Continue without vision if text evidence exists |
| `vision_install_failed` | Keep prompt/attachments and show concise cause | Retry download |
| `vision_model_corrupt` | Never load the model | Repair/redownload verified files |
| `vision_load_failed` | State that visual analysis could not start | Retry once after cleanup; then partial/fail |
| `vision_resource_pressure` | Explain that visual analysis needs more free memory or a cooler system; retain completed evidence | Return labelled partial evidence when possible; otherwise offer retry |
| `vision_timeout` | Identify affected image/page | One bounded retry, then partial/fail |
| `vision_invalid_image` | Identify unreadable attachment/page | Skip only if task permits and label omission |
| `vision_invalid_output` | Do not feed malformed evidence to synthesis | Retry once; then partial/fail |
| `vision_process_failed` | Isolate crash and keep completed evidence | Clean child state, retry once, then partial/fail |
| `vision_cancelled` | Stop promptly without presenting it as a fault | Keep completed evidence; user may retry |

Additional requirements:

- A retry is idempotent and cannot create a second answer for the same turn.
- Page-level failures do not erase successful page results.
- OCR success plus any vision failure follows the labelled OCR-only partial rule.
- A helper crash cannot crash the gateway or corrupt the installed model.
- Cancellation works during download, verification, model load, page render,
  prefill, and decode.
- Temporary render files, pipes/sockets, locks, and child processes are cleaned
  up after success, cancellation, and failure.
- Backoff is bounded. Samosa never waits forever under memory or thermal pressure;
  it eventually offers a partial result/cancel/retry state while preserving work.

## 15. Gateway/API and progress events

Reuse existing model-manager and chat-turn endpoints where possible. Add only
the typed state necessary for auxiliary models and resumable turns. API details
must be documented in `docs/SERVE_API.md` before release.

The chat event stream needs versioned events for:

- `attachment.plan.started` / `attachment.plan.completed`;
- `attachment.ocr.started` / page progress / completed;
- `attachment.vision.model_required`;
- `attachment.vision.download` and verification progress;
- `attachment.vision.loading` / `ready`;
- `attachment.vision.page.started` / `completed` with page and total inventory;
- resource tier/admission state in visual evidence and stable
  `vision_resource_pressure` failures;
- `attachment.vision.completed` / `failed` / `cancelled`;
- `attachment.synthesis.started`; and
- `attachment.answer.partial` with completed and missing capabilities.

Progress may say “page 8 of 37 processed” but must not imply that only a fixed
subset is allowed. Event payloads expose attachment IDs, not arbitrary local
paths. Old clients ignore unknown events and retain a usable final response.

## 16. Test and qualification plan

### 16.1 Native/reference parity tests

Development tooling creates fixed reference fixtures using the exact pinned
KaedeTai MLX implementation. That Python environment is test-only and absent
from release artifacts.

For several fixed photographs, scanned pages, diagrams, and resolutions, compare:

- decoded/resized/normalized pixel tensors;
- vision-encoder patch embedding output;
- selected early, middle, and final encoder block activations;
- projector output;
- prompt token IDs and image-token placement;
- selected decoder logits; and
- greedy generated token IDs.

Define numeric tolerances from BF16 reference variability before native
optimization. Store compact fixture provenance and generation scripts; do not
commit opaque fixtures without their source and licence.

### 16.2 Unit tests

- Catalogue parsing, optional-field compatibility, backend/category validation,
  platform filtering, checksums, and immutable revision enforcement.
- Vision auxiliary models never enter chat-model selectors or conversation
  binding.
- Settings categorization is driven by metadata and each category renders only
  supported controls.
- Planner schema accepts all valid routes and rejects unknown operations,
  attachment IDs, pages, paths, and malformed JSON.
- `read_text` selects embedded text before OCR and invokes OCR for image-only
  input.
- Evidence merge preserves page provenance and exposes OCR/vision disagreement.
- Cache keys change for model, preprocessing, rendered pixels, prompt, or decode
  changes.
- Error mapping never leaks a path, raw stderr, or document content.
- Cancellation and cleanup are safe at every lifecycle stage.

### 16.3 Adaptive scheduling tests

Use synthetic 2-, 7-, and 40-page documents to prove behaviour, not to encode
those counts as limits:

- an explicit page question inspects only the required page(s);
- a relevance-first question starts with ranked pages and expands when evidence
  is deliberately insufficient;
- a whole-document visual audit processes every required page sequentially;
- a late relevant page is found rather than missed because of an internal cap;
- memory-pressure and thermal events downshift resolution or stop safely without
  losing completed evidence;
- cancellation stops the queue promptly; and
- long evidence is hierarchically summarized without losing page citations.

Add a regression assertion that neither UI settings nor the scheduler contains
a product-level maximum visual page count.

### 16.4 Download-and-resume integration tests

- Missing model → offer download → verified install → same turn resumes.
- Successful OCR done before/during download is reused, not repeated.
- Download cancellation preserves the prompt, attachments, and partial evidence.
- Network interruption and app restart resume or safely restart installation.
- Bad checksum never marks the model ready.
- Two windows cannot duplicate installation or the answer.
- Reloaded UI reconnects to one pending continuation.
- Consumed continuation cannot run twice.

### 16.5 Failure-injection tests

Use a fake helper to inject load failure, out-of-memory/pressure signal, malformed
JSON, empty result, timeout, crash, killed process, corrupt image, cancellation,
and partial page success. Verify stable codes, cleanup, retry bounds, and final
answer labelling. Specifically verify that successful OCR plus every vision
failure produces an OCR-only partial answer rather than a failed whole turn.

### 16.6 Quality tests

Create a versioned attachment suite covering:

- natural photographs and captioning;
- object identification, counting, and spatial relationships;
- screenshots and UI interpretation;
- diagrams, plots, charts, forms, and tables;
- digital PDFs, scanned PDFs, and mixed text/image PDFs;
- rotated, blurred, low-contrast, and high-resolution pages;
- tasks requiring OCR only, vision only, and both;
- material OCR/VisionPsy disagreement;
- long documents with relevant evidence early and late;
- prompt-injection text embedded in images/documents; and
- tested non-English samples used to define, not presume, language support.

Score task correctness, groundedness, page attribution, unsupported-claim rate,
route accuracy, and partial-answer honesty. Upstream benchmark numbers may be
quoted as upstream results but do not replace Samosa end-to-end evaluation.

### 16.7 16 GB hardware qualification

The primary gate is a real 16 GB Apple Silicon Mac with Samosa's supported text
LLMs, not an isolated model-card measurement. For each supported active LLM
profile, record a control run and then a combined attachment run containing
model load, repeated visual pages, synthesis, and ordinary concurrent desktop
activity.

Record at minimum:

- Samosa gateway/UI and active-LLM responsiveness;
- per-process and system physical footprint;
- memory-pressure events and swap trend;
- VisionPsy load, first-page, subsequent-page, and unload latency;
- thermal state and downshift/refusal behaviour;
- crash/OS-kill evidence;
- text-LLM prompt/decode performance before and after vision use; and
- memory reclaimed after helper exit.

Pass criteria:

- no process is killed by the OS and no unrecovered helper/gateway crash occurs;
- the UI, cancellation, and health endpoint remain responsive;
- the runtime controller prevents sustained unsafe memory pressure and sustained
  swap growth beyond the noise measured in its paired control run;
- thermal pressure causes a lower visual tier or a visible retryable refusal
  rather than runaway work;
- the active chat can synthesize after vision completes; and
- helper exit returns memory sufficiently close to the measured post-turn
  control envelope.

The numeric envelopes and latency expectations are written into the benchmark
report after measurement. They are not invented in this specification. Test at
least the project's 16 GB M3 reference system. Claims covering other Apple
Silicon generations require corresponding smoke/compatibility runs; otherwise
the release notes state only what was measured.

### 16.8 Packaging and regression gates

- Inspect the signed release bundle and dependency graph: no Python executable,
  Python framework, site-packages, `mlx-vlm`, PyTorch, or runtime shell-out to
  Python.
- Verify the helper is arm64, linked only to approved shipped/system libraries,
  codesigned, and launchable from the installed app path.
- Run existing model-manager, compiled-gateway, attachment, reader/OCR, runtime
  settings, UI, serve, and packaging tests.
- Verify a text-only chat with no attachment never downloads, starts, or loads
  VisionPsy and has no measurable steady-state memory regression.
- Verify a document request routed to OCR only never starts VisionPsy.

## 17. Documentation requirements

The feature is not complete until user and developer documentation matches the
shipped behaviour. Update:

- `README.md`: automatic attachment understanding, platform scope, and concise
  no-Python statement;
- `docs/INSTALL.md`: optional VisionPsy download, disk size, verification, and
  Apple Silicon requirement;
- `docs/USAGE.md`: attach-and-ask examples, automatic routing, progress,
  download-and-continue, cancellation, and partial answers;
- `docs/MODELS_AND_INTERNET.md`: exact source/revision, when network access is
  required, offline behaviour after install, hashes, licences, and repair;
- `docs/SERVE_API.md`: catalogue additions, continuation semantics, stable error
  codes, and progress events;
- `docs/TASKS_READER.md` and `docs/TASKS_DOCUMENTS.md`: reader/OCR versus visual
  responsibilities and page provenance;
- `docs/DESIGN.md`: planner → evidence tools → synthesis architecture, helper
  isolation, resource controller, and cache keys;
- `docs/UI_DESIGN.md`: grouped Models settings and all ready/loading/resource/error
  states;
- model card/third-party notices: official VisionPsy and KaedeTai conversion
  attribution and limitations; and
- a dated regression/benchmark report containing parity, quality, routing, and
  real 16 GB measurements.

The shipped Developer mode described in
[`DEVELOPER_MODE.md`](DEVELOPER_MODE.md) must also preserve, under one turn
correlation ID, the exact router request/raw response/validated plan, adaptive
resource decision, OCR output, VisionPsy command/raw observation/errors, final
evidence prompt, and chat-model response. A process-exit log alone is not
sufficient evidence that the model contributed to a turn.

Documentation must say that routing is automatic and hardware/task adaptive. It
must not claim a five-page limit, a user-configurable visual page limit, broad
language support, or hardware support that was not tested.

## 18. Delivery sequence and gates

### Gate VP0 — Pin and reproduce

- Pin artifact revision/files/hashes/licences.
- Reproduce the KaedeTai example and record upstream-vs-local outputs.
- Capture model graph, preprocessor, prompt template, and deterministic fixtures.
- Measure BF16 alone and with current Samosa workloads on the 16 GB reference.

Exit: provenance is complete and BF16 is viable enough to justify the native
port. If it fails the combined workload, stop and report the evidence.

### Gate VP1 — Native helper parity

- Implement the minimal VisionPsy MLX C++ adapter and protocol.
- Pass tensor/logit/token parity and lifecycle/cancellation tests.
- Prove the built helper has no Python runtime dependency.

Exit: deterministic fixtures pass and repeated load/use/unload is clean.

### Gate VP2 — Catalogue and Settings

- Add auxiliary model metadata, install/verify/repair/remove, platform filtering,
  and grouped Models settings.
- Exclude VisionPsy from chat-model selection everywhere.

Exit: catalogue compatibility and Settings UI/accessibility tests pass.

### Gate VP3 — Chat routing and evidence

- Implement validated auto-planning, reader/OCR/VisionPsy routing, adaptive page
  scheduling, evidence provenance, caching, and final synthesis.
- Keep Jobs and Chutni paths unchanged.

Exit: route matrix, adaptive scheduling, quality, and prompt-injection tests pass.

### Gate VP4 — Continuation and resilience

- Implement Download and continue, durable/idempotent turn continuation, progress,
  cancellation, partial answers, and all error mappings.

Exit: install/reload/failure-injection scenarios pass without lost or duplicated
turns.

### Gate VP5 — Release qualification and docs

- Run the 16 GB combined workload and existing regression suites.
- Record measured safety envelopes and compatibility claims.
- Complete all documentation, attribution, packaging, and release audit items.

Exit: every acceptance criterion below has evidence linked from the release
report.

## 19. Acceptance and implementation status

| Requirement | Status and evidence |
|---|---|
| Immutable standard 460M BF16 manifest | Implemented. Exact revision, byte counts, and seven SHA-256 values are catalogue-validated. |
| No Python in the application runtime | Implemented. The runtime-only packaging audit installs the native helper and rejects Python-dependent adapters. |
| Auxiliary model, never a chat backend | Implemented and covered by catalogue/API/UI tests. |
| Models grouped as Text LLMs, Vision, OCR, and Voice | Implemented. Controls are role/category-driven; OCR readiness comes from live runtime and pack state. |
| Automatic LLM routing | Implemented. Planner JSON is schema-validated; a model-planned OCR-only fixture proves routing is not merely the fallback keyword path. |
| Load only when vision is requested | Implemented. OCR-only and text-only paths do not start the helper; one helper is reused within a visual turn and then terminated. |
| Digital text/OCR/vision evidence cascade | Implemented. Digital PDF text remains the preferred reader path; visual-only PDF work does not invoke OCR as a side effect. |
| Hardware/task-adaptive images and pages | Implemented. Oversized images are resized to a live 2048/1536/1024/512 tier; PDFs have no fixed product page cap and are processed sequentially. |
| Critical resource handling | Implemented. Resolution downshifts as pressure rises; critical pressure produces a labelled partial result or `vision_resource_pressure`, retaining completed evidence. |
| Download and continue exactly once | Implemented and browser-fixture tested, including concurrent polls, late timers, the per-job status URL, exact payload preservation, and Retry. |
| Honest partial/failure answers | Implemented. Streaming text/OCR fallbacks receive a gateway-enforced visible label; visual-only failure returns no synthesized factual answer. |
| Native model execution | Passed on the exact pinned checkpoint and a real PDF render under MLX/Metal. Component tests cover preprocessing sizes, tiling, token contract, and unsafe image headers. |
| Package/install/cleanup | Passed targeted package, atomic-install, install-path, gateway-installer, runtime-only, helper retry, and process-cleanup tests. |
| Documentation | Updated in the user, API, architecture, catalogue/network, UI, install, and dated regression documents listed in section 17. |
| Full release qualification with a resident supported chat LLM plus ordinary desktop workload | **Not yet a product claim.** The isolated real gate measured the helper, but a paired combined-workload qualification has not been run. |

## 20. Release evidence not yet claimed

The implementation is complete for the scoped chat-attachment feature. These
remaining items govern the breadth of release/marketing claims, not the routing
or resizing behavior:

- paired active-LLM plus ordinary-desktop measurements of peak memory, swap,
  latency, thermal behavior, and post-turn reclamation;
- an explicit minimum supported macOS version derived from the final signed
  MLX release build;
- a versioned quality/language suite broad enough to publish accuracy and
  language-support claims; and
- compatibility runs for Apple Silicon systems beyond the tested 16 GB M3.

Until those runs exist, documentation must say “macOS Apple Silicon first
release” and report only the standalone measurement in the dated regression
record. It must not turn an unmeasured combined workload into a pass.

## 21. Upstream sources to pin during VP0

- [KaedeTai VisionPsy-Nano-460M MLX conversion](https://huggingface.co/KaedeTai/VisionPsy-Nano-460M-MLX): user-selected BF16 MLX artifact, conversion notes, example runtime, and self-reported MLX measurements.
- [qvac VisionPsy-Nano-460M](https://huggingface.co/qvac/VisionPsy-Nano-460M): official model architecture, limitations, evaluation, attribution, and Apache-2.0 model licence.
- [qvac VisionPsy release article](https://huggingface.co/blog/qvac/visionpsy): official release context and standard-versus-Flash distinction.

These links identify sources; release manifests still use immutable revisions
and verified file hashes rather than mutable page URLs.
