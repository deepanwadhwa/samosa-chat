# Molmo2 4B native vision and video integration

Status: native implementation and local real-Q4 qualification complete; public artifact publication and BF16 parity corpus pending
Target: the 16 GiB Apple Silicon reference Mac
Runtime rule: no Python interpreter, Transformers, vLLM, Conda, or model server

## Outcome

Samosa will use Molmo2 4B as an auxiliary specialist, not as a second always-on
chat model. The gateway starts it only for an admitted image/video operation,
feeds it a bounded unit of work, collects structured evidence, and terminates it
before the primary chat model writes the answer. Text-only turns never load it.

The production artifact is a pinned, pre-converted MLX package. The Samosa app
never downloads or loads AllenAI's approximately 19.4 GB FP32 repository. An
owner may explicitly run the guarded native converter against reviewed local
source files; this was qualified successfully on the reference Mac. The package
keeps the Qwen decoder's large matrices at Q4
(group size 64), starts with the vision tower and connector in BF16, and keeps
normalization and numerically sensitive attention operations in BF16/FP32. A
release is permitted only after the resulting package passes the quality and
memory gates below.

This document is also a stop condition: a Python fallback, an unpinned model, a
silent capability downgrade, or a configuration that causes new swapouts is not
"complete."

## Implementation status (2026-08-26)

The code path described here is implemented. The provider-neutral process
supervisor, native MLX model, native image/video preprocessing, streamed
attachment store, bounded video scheduler, gateway admission/interlock, UI,
catalog contract, installer staging, and deterministic tests are present. The
relevant entry points are:

- `src/samosa_multimodal.c` for the single-specialist process supervisor;
- `src/molmo2/` for the package contract, image/video processor, MLX model, and
  AVFoundation decoder;
- `tools/molmo2_pack.cpp` for the no-Python checkpoint converter;
- `src/samosa_gateway.c` for admission, provider selection, primary-model
  interlock, evidence coverage, and failure behavior;
- `tests/test_molmo2_*` and `make test-molmo2-real` for deterministic and
  real-package gates.

The checked-in runtime and a measured local package are complete. On 2026-08-26
the exact pinned 19.4 GB source checkpoint was converted on the 16 GiB reference
Mac under a 4,096 MiB conversion guard with zero swap growth. The resulting
3,372,298,903-byte package passed guarded real image and AVFoundation video
inference, was installed locally, and is reported as **Ready** by the UI. The
full Ornith-to-Molmo-to-Ornith route also passed with zero swap growth.

The public model entry deliberately remains `native_pack` with no artifact URL.
Publishing a 3.37 GB binary to an external host requires an explicit release
decision and credentials; local qualification does not silently authorize that
external write. Samosa never downloads the source checkpoint or performs
conversion automatically.

## Immutable upstream reference

- Model: `allenai/Molmo2-4B`
- Revision: `042abfa7a38879a376cec03d949eff0aefaa0600`
- License: Apache-2.0
- Base language model: Qwen3-4B-Instruct-2507
- Vision encoder: SigLIP2
- Upstream page: <https://huggingface.co/allenai/Molmo2-4B>

The reference configuration is captured in checked-in native fixtures rather
than interpreted at runtime. The important fixed values are:

| Component | Contract |
| --- | --- |
| Text decoder | hidden 2560, intermediate 9728, 36 layers, 32 query heads, 8 KV heads, head dimension 128 |
| Text positions | 36,864 maximum, RoPE theta 5,000,000, Q/K normalization |
| Vocabulary | 151,936 base tokens plus 128 model tokens |
| Vision tower | hidden 1152, intermediate 4304, 27 layers, 16 heads, head dimension 72 |
| Image input | 378 x 378, patch size 14, 729 learned positions |
| Connector | hidden 1152, intermediate 9728, 16 heads, output width 2560; selected vision layers -3 and -9 |
| Image processing | mean/std 0.5, bilinear resize, global plus overlap crops, maximum 8 crops, 2 x 2 pooling |
| Multi-image processing | two labelled native image inputs in one decoder sequence; shared crop budget and sequential per-image vision encoding |
| Video processing | timestamped 378 x 378 frames, 3 x 3 pooling, task-bounded sampling up to 2 fps |

Molmo2 prefill is not a stock causal Qwen prefill: visual query and visual key
tokens use bidirectional attention. The native attention mask must preserve that
contract. Reusing the existing Maple decoder without this mask would produce an
incorrect implementation.

## Architecture

```text
composer / API
      |
      v
attachment store -- native container check --> typed attachment metadata
      |                                      (image/video/document/audio)
      v
capability planner --> resource admission --> one-specialist lease
      |                                      |
      | image/basic                          | advanced image/video
      v                                      v
samosa-visionpsy                       samosa-molmo2
                                             |\
                                             | \--> first visual turn: direct text + points --> response
                                             ^
                                             |
                                  bounded timestamped frame window
                                             ^
                                             |
                                  AVFoundation module (in helper)
      |
      v
structured evidence ledger --> later/composite primary synthesis --> response
```

### Provider-neutral supervisor

The gateway owns a single multimodal supervisor with provider descriptors,
rather than hard-coding VisionPsy process state into the turn. A descriptor
contains the executable, installed model directory, supported capability bits,
memory class, protocol version, request deadline, and teardown grace period.

The supervisor guarantees:

1. At most one auxiliary MLX specialist is active globally.
2. The process is placed in its own process group.
3. Readiness is confirmed before any attachment path is sent.
4. Requests and replies are bounded and correlated by ID.
5. Cancellation terminates the entire process group.
6. Timeout, EOF, malformed JSON, over-sized replies, and protocol mismatch fail
   closed.
7. The helper is reaped at the end of the admitted visual phase, including all
   error paths.
8. The primary backend and Molmo2 do not decode concurrently.
9. Exhaustive mode admits at most eight sequential dense windows per user turn;
   longer videos retain the coarse full-duration pass and an explicit uncovered
   interval instead of creating unbounded work.

VisionPsy remains the fallback image provider when Molmo2 is absent. Once the
verified Molmo2 package is installed, Molmo2 is selected for visual images,
video, multi-image comparison, temporal localization, pointing/tracking, or
detailed visual reasoning. A request that requires Molmo2 returns an actionable
`molmo2_model_required` or `molmo2_resource_pressure` result; it is never
silently answered by a provider lacking the required capability.

For two image attachments, the helper receives one bounded `images`
command containing every validated private path. Its prompt and visual tokens
follow the upstream `Image 1`, `Image 2` chat-template convention.
Each image is vision-encoded sequentially to prevent crop memory from scaling
with the batch, while the projected visual features are concatenated before
the language decoder so cross-image reasoning remains genuinely joint. Three
or more images fail explicitly rather than being silently split into unrelated
observations.

### Native helper and IPC

`samosa-molmo2` is a C++20 MLX/Metal executable. It reads only local, verified
artifacts and exposes these commands over private inherited pipes:

- `hello`: protocol and capability negotiation;
- startup load: validate manifest hashes, shapes, dtypes, tokenizer, and
  processor fingerprint before replying to `hello`;
- `analyze`: inspect one image or one bounded timestamped video interval;
- `cancel`: cooperative cancellation, followed by process-group termination if
  the deadline expires;
- `quit`: explicit teardown.

The protocol has a fixed maximum frame size and uses length-prefixed JSON so
embedded newlines cannot desynchronize it. Large pixels never travel inside
JSON. The gateway resolves content-addressed IDs to paths under its private
attachment root; the helper accepts only absolute, non-symlink regular files
and independently enforces size and decoded-dimension limits. The upload path
has already streamed and verified the SHA-256, so exhaustive video windows do
not rehash the same multi-gigabyte blob for every window.

Package revision and processor identity are enforced at startup. Successful
gateway evidence includes the content-addressed attachment ID, requested and
covered intervals, actual decoded-frame count, duration, generated observation,
token counts, and explicit sampling/audio/subtitle limitations. Timing and
process lifecycle remain in the local developer trace. The primary model sees
the bounded evidence, not arbitrary helper diagnostics.

### Native model package

The production package ID is reserved as `molmo2-4b-mlx-q4-v1`. It contains:

- `manifest.json`: format version, immutable upstream revision, per-file
  SHA-256 and byte count, quantization layout, and processor fingerprint;
- `model.safetensors.index.json`: canonical tensor-to-shard index;
- sharded tensor files so no monolithic 19 GB mapping is needed;
- tokenizer vocabulary/merges and the frozen chat template contract;
- native processor configuration and special-token IDs;
- a license and upstream provenance record.

`molmo2-pack` is a native release-engineering tool. It consumes reviewed local
safetensor shards, validates every expected name/shape/dtype, quantizes in
bounded chunks, writes to a staging directory, fsyncs, verifies the finished
package, and atomically publishes it. It never runs automatically during
installation and never needs Python; an owner may invoke it explicitly against
reviewed local source files. It refuses to start unless the output volume has
at least 6 GiB free. The public catalog entry is enabled only after real artifact
URLs, byte counts, and SHA-256 values exist.

Quantized matrices are dequantized or multiplied in bounded MLX operations.
Tensor files are not copied into a second whole-model buffer. The loader rejects
unknown tensors, duplicate tensor names, shape mismatches, non-finite scale
metadata, incompatible format versions, and incomplete shards.

## Memory and thermal envelope

The reference machine has 16 GiB unified memory and may already be running the
primary model, browser UI, and macOS. Admission therefore uses current available
memory, memory-pressure level, thermal state, active specialist lease, and
package-estimated resident bytes—not installed RAM alone.

The converted 706-tensor package contains 3,360,724,138 weight bytes
(3.130 GiB). Adding the manifest's conservative 384 MiB runtime allowance
yields 3,763,377,322 bytes (3.505 GiB), about 161 MiB below the 3,750
MiB MLX hard limit. Safetensors headers and non-weight package files are covered
by the separate 4 GiB disk-package limit.

Initial release budgets:

| Item | Hard policy |
| --- | --- |
| Molmo2 quantized weights | statically estimated 3.130 GiB for the pinned v1 tensor contract |
| Vision tower + connector | BF16 first; quantize only if measured gates require it |
| Complete MLX allocation | hard limit 3,750 MiB; package admission includes a 384 MiB runtime allowance |
| Per-call sequence | prompt plus requested generation at most 2,048 tokens, below the architecture's 36,864-position maximum |
| Frame decode queue | one source `CGImage` at a time, at most 16 retained 378 x 378 RGB frames, plus one normalized batch |
| Admission reserve | preserve at least 3 GiB on 16 GiB machines (4 GiB above 18 GiB total RAM) and reject critical pressure |
| Swap | zero new swapouts during the release qualification scenario |
| Concurrency | exactly one specialist; sequential frame windows |

Those are safety ceilings, not promises that all of them may be consumed at
once. The runtime obtains a lease before spawn and re-checks pressure between
windows. If pressure becomes critical, it stops at a completed evidence
boundary, releases the helper, and reports the exact unprocessed interval.

Each Samosa model call is capped at 16 frames as a deliberately conservative
16 GiB-machine runtime policy. The pinned upstream checkpoint supports a much
larger long-context frame envelope; Samosa obtains long-video coverage through
sequential bounded windows instead of exposing that high-memory envelope on the
target Mac. This keeps K/V plus vision activation peaks inside the allocator
ceiling.
AVFoundation draws each requested frame directly into a 378 x 378 RGBA buffer;
the helper does not copy full-resolution pixels into an app-owned frame buffer
and releases the source `CGImage` immediately after the draw. Dense scheduling
uses 7.5-second windows at 2 fps with one-second overlap.

The gateway never retries by increasing image resolution, crop count, frame
count, context, or parallelism. A resource retry may lower one bounded tier
once, after recreating the helper so a failed Metal command stream is not reused.

## Video path

The former attachment endpoint read a body into memory and had a 4 MiB limit;
the implemented video path now uses a streamed upload path:

1. Read request data into a private temporary file in bounded chunks while
   hashing it and enforcing a configured byte ceiling.
2. Identify MP4/MOV/M4V by magic/container structure, never by extension.
3. Atomically publish the blob and metadata only after validation and fsync.
4. Open the private blob through AVFoundation, obtain duration, and preserve
   preferred-track orientation while decoding requested presentation times.
5. Decode only the requested timestamps through `AVAssetImageGenerator`, one
   bounded window at a time. Preserve actual presentation timestamps.
6. Never decode an entire movie or retain a full-resolution frame; retain at
   most sixteen 378 x 378 RGB frames for the current inference call.

The Objective-C++ AVFoundation module is linked into the otherwise C++
`samosa-molmo2` helper, so there is no extra daemon or pixel IPC. Each decoded
RGBA frame is compacted to RGB and immediately passed to the processor. Audio
is not hallucinated from pixels; the v1 Molmo path reports visual evidence only.

### Long-video scheduler

There is no duration-based upload rejection. Processing is sequential, every
inference is frame-bounded, dense work has a hard per-turn ceiling, and
coverage is explicit:

- overview: one uniform, timestamped coarse pass across the full duration;
- temporal localization/tracking: the coarse pass plus one 7.5-second dense
  refinement around the first cited candidate timestamp, or the midpoint when
  no parseable citation is present;
- exhaustive request: walk consecutive bounded windows until the full requested
  range is covered or the eight-window, evidence-context, resource,
  cancellation, or inference boundary stops it.

Every response records covered and unprocessed intervals. The assistant may not
make claims about an interval that was not processed.

## Capability plan

Before loading a specialist, the planner returns the validated fields
`read_text`, `inspect_visual`, `detail`, `extended_visual`, `video_mode`,
`visual_scope`, and `pages`. Deterministic attachment/keyword routing is a
floor: obvious image, chart, video, and multi-image work cannot be downgraded
by a model-generated plan. `extended_visual` requires Molmo2; standard
image/document visual work also prefers Molmo2 whenever its package is
installed, otherwise falling back to VisionPsy or an already-resident native
image backend. Audio transcription and
subtitle reading are not v1 capabilities and are always disclosed as
unprocessed. Model-generated plans are parsed against a strict schema and safe
defaults; unknown values are ignored.

## Local real-Q4 qualification (2026-08-26 through 2026-08-27)

The conversion and smoke gates were run on an Apple M3 with 16 GiB unified
memory, macOS 26.5.1, and no Python process in the conversion or inference
path. The exact evidence is recorded in
[`regressions/molmo2-4b-q4-2026-08-26.md`](regressions/molmo2-4b-q4-2026-08-26.md).

| Gate | Result |
| --- | --- |
| Native FP32-to-Q4 conversion | PASS; 3,366 MiB peak process-tree RSS; 0 MiB swap delta |
| Package verification | PASS; 3,372,298,903 bytes; manifest SHA-256 `a07230dd952e97969921223328e7812020bb58711728450cdfb2205e3a15b770` |
| Real image inference | PASS; coherent observation; 3,373 MiB peak RSS; 0 MiB swap delta |
| Real AVFoundation video inference | PASS; coherent observation; 3,415 MiB peak RSS; 0 MiB swap delta |
| Live Ornith handoff | PASS; 107 ms bounded reclaim wait; 3,186,512 KiB Molmo peak RSS; zero system swap before/after |
| Live Bonsai-bound first visual turn | PASS; Molmo returned 12 exact grounding points directly in 24.990 s; 3,949 MB footprint; Bonsai absent during Molmo inference and ready again afterward; zero system swap before/after |

The live handoff exposed and fixed two issues that fixture-only tests could not:
the visual-token insertion needed MLX `scatter_add_axis`, and macOS released the
stopped Ornith process's Metal/wired pages asynchronously. The gateway now polls
for at most three seconds after `waitpid`, without lowering the manifest-derived
resident estimate. Admission preserves 3 GiB on the qualified 16 GiB host (and
4 GiB on larger-memory hosts) and aborts immediately on critical memory or
thermal pressure.

The 2026-08-27 first-turn acceptance used the user's exact
`qwen3.6_35b_a3b_score.png` attachment while the conversation remained bound to
Bonsai. Molmo's unmodified response contained twelve 0–1000 coordinate points
for the twelve charts and no Bonsai synthesis pass. A mid-turn process sample
showed only `samosa-molmo2`; after the response, Molmo was absent and Bonsai was
ready again. The accepted client socket is close-on-exec so the asynchronous
text-backend restart cannot hold a completed browser stream open.

## Delivery phases

### Phase 1 — freeze contracts and fixtures (implemented)

- Check in this plan, the pinned configuration, special token contract,
  processor fingerprint, model manifest schema, and tiny deterministic tensor
  fixtures.
- Add tests that distinguish causal text attention from bidirectional visual
  attention and validate image/video token layout.

### Phase 2 — generalize existing visual orchestration (implemented)

- Replace `VisionPsySession` ownership with the provider-neutral supervisor.
- Preserve current VisionPsy image/PDF behavior and error codes.
- Add the global one-specialist lease, bounded protocol parser, timeout,
  cancellation, and guaranteed teardown tests.

### Phase 3 — native Molmo2 image inference (implemented; Q4 smoke gate complete)

- Implement tokenizer, SigLIP2 tower, selected-layer connector, multimodal token
  assembly, visual attention mask, Qwen decoder, KV cache, and greedy generation.
- Load native BF16 fixtures first, then the Q4 package format.
- Real single-image inference is qualified locally. The broader reviewed
  upstream-reference parity corpus remains a public-release gate. The real
  browser regression now also covers a vague `what is this?` ornamental image
  turn, including neutral description-before-classification prompting and
  exact UI system-message ordering.

### Phase 4 — deterministic Q4 release tooling (conversion complete; publication pending)

- Implement `molmo2-pack` and package verification.
- The pinned artifact was produced and installed atomically on the reference
  Mac. Filling public catalog URLs remains pending external hosting approval.
- The real image/video smoke corpus passes. BF16-to-Q4 quality comparison across
  captioning, visual QA, OCR-sensitive cases, counting, pointing, and
  adversarial images remains a public-release gate.

### Phase 5 — streamed native video (implemented)

- Add file-backed bounded request-body streaming and typed video metadata.
- Implement the linked AVFoundation frame extractor and hostile-container tests.
- Implement frame sampling and timestamp/token preprocessing parity.

### Phase 6 — routing, evidence, API, and UI (implemented)

- Add Molmo2 installation/readiness state and required-capability errors.
- Add video selection, upload/progress/cancel, timeline coverage, and model
  download-and-resume affordances without changing text-only behavior.
- Document the attachment and evidence API.

### Phase 7 — real-machine qualification (local Q4 gate complete)

- Complete: run the Q4 conversion, native image path, native AVFoundation video
  path, and live Ornith handoff on the 16 GiB reference Mac with footprint,
  pressure, swapout, and memory-return telemetry.
- Pending for public release: run the real pinned BF16 parity corpus where
  sufficient hardware is available, plus hostile-media and cancellation stress.
- Pending external action: publish the measured artifact and fill the catalog's
  immutable URLs, byte counts, and hashes.

### Phase 8 — direct Molmo visual chat (implemented)

- Promote the verified Molmo package to a first-class selectable chat model in
  the same catalog and Settings picker as Qwen, Ornith, Bonsai, and Maple.
- Treat selection as a virtual backend: stop the previous resident text model,
  persist the Molmo catalog identity, report readiness from strict package
  verification, and keep the Molmo process absent between turns.
- Require one image or video for a direct turn. A follow-up with no new upload
  reuses the conversation's most recent visual attachment while sending recent
  user/assistant context to Molmo.
- When a conversation bound to Qwen, Bonsai, Ornith, or Maple begins with one
  image or video, bypass planning and primary-model synthesis for that turn.
  Return Molmo's own text/coordinates, unload Molmo, and begin restarting the
  selected text model without delaying delivery of the visual answer. Later
  text-only turns stay bound to the user's selected text model.
- Return Molmo's generated text and grounding/coordinate markup directly in
  OpenAI-compatible streaming and non-streaming responses. Do not run a primary
  text model, do not rewrite the answer, and do not advertise raster image
  generation: Molmo2 4B is image/video-to-text.
- Decode both Molmo percentage `xN`/`yN` markup and Molmo2's 0–1000 `coords`
  markup in the browser. Render the original attachment with accessible pink
  point markers and a marker count while keeping raw coordinate XML out of the
  visible prose.
- Preserve the global specialist lease, manifest-derived memory admission,
  cancellation, bounded video frames, attachment validation, conversation
  model binding, and guaranteed helper teardown used by automatic routing.

## Release gates / definition of done

All gates are mandatory:

1. Text-only and OCR-only turns do not start Molmo2. Visual image turns prefer
   Molmo2 when its verified package is present; VisionPsy is the fallback.
2. Molmo2 starts only after successful resource admission. It is gone before
   primary-model synthesis. When Molmo itself is selected there is no primary
   synthesis, and Molmo is gone before the response completes.
3. The shipped runtime contains no Python invocation or dependency.
4. Native image preprocessing, video timestamps, special-token placement, and
   visual attention masks match the pinned contract, including no column tokens
   on the global low-resolution crop and retained column tokens on the
   high-resolution image grid.
5. BF16 logits/generation match approved reference tolerances; Q4 passes the
   declared task-quality margin rather than merely loading.
6. The 16 GiB scenario has zero new swapouts, no critical memory pressure, a
   responsive UI, and no Molmo/VisionPsy overlap.
7. Cancellation and deadlines terminate all child processes within the bounded
   grace period.
8. Memory and mapped-file pressure return close to baseline after every visual
   phase.
9. Corrupt/truncated weights, malicious media, symlink paths, decompression
   bombs, malformed IPC, and helper crashes fail closed with actionable errors.
10. Long-video answers enumerate coverage and never imply analysis of an
    unprocessed interval.
11. Installer publication is hash-pinned, atomic, rollback-safe, and contains
    correct Apache-2.0 provenance.
12. Existing VisionPsy, document, attachment, model-catalog, and text chat tests
    continue to pass.
13. Selecting direct Molmo leaves zero model children resident, direct visual
    responses contain Molmo's own output, text-only turns fail with an explicit
    visual-required error, and every completed/cancelled turn returns to zero
    Molmo child processes.
14. An image/video turn under any selected text model uses Molmo's grounded
   output as evidence for that selected model's bounded synthesis pass. A
   multi-image turn sends all original pixels in one Molmo inference and
   preserves exact coordinate markup through the handoff.

If the Q4 artifact misses either the quality or machine-safety gate, the catalog
entry remains unavailable. The accepted remedy is further native quantization,
streaming, or scheduling work—not an FP32 download, Python sidecar, cloud call,
or silent downgrade.
