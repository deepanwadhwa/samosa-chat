# Using Samosa

## Model management

```sh
samosa models
samosa pull qwen
samosa pull bonsai
samosa pull ornith
samosa pull all
```

`models` distinguishes missing weights, weights without a GGUF runtime, and a
fully installed model. Pulls are resumable and verified.

## App

```sh
samosa app
```

The gateway starts even when no model exists. In Settings → Model, each card
shows the model's size, license, and local state:

- **Download** installs missing weights and any required runtime.
- **Use** unloads the current model and starts the selected one.
- **Active** marks the current backend.

The first completed download activates automatically when no backend is
running. Downloading a second model does not interrupt a running chat.

## Image and document attachments

In ordinary Chat, choose **+ → Image** or **+ → Document**, attach the file,
and ask the task in natural language. Examples include “What color is the car?”,
“Quote the invoice number and total,” “Explain this diagram and read its
labels,” and “Inspect every chart in this PDF.” There is no OCR/vision switch.

The active chat LLM first returns a validated evidence plan. Deterministic
image/video/task classification is a floor: the planner may request more
evidence, but cannot downgrade an obvious visual request to text-only work.

- `read_text` uses a digital PDF's embedded text first and invokes Samosa's
  native PP-OCRv6 reader only for scans/images that need literal text;
- `inspect_visual` prefers the verified Molmo2 4B Native Q4 package whenever it
  is installed. Without Molmo2, Samosa falls back to VisionPsy-Nano 460M BF16;
  if neither auxiliary model is installed and the active model has native image
  support, Samosa preserves that resident native path;
- a task that needs literal labels and visual structure selects both. The
  active chat LLM receives the labelled evidence and writes the final answer.

When the pinned Molmo2 package is ready, Samosa selects it for every visual
image task as well as video, multi-image comparison, temporal tracking,
localization/pointing, and complex spatial reasoning. OCR-only work remains on
the native reader. Molmo2 remains an auxiliary provider: the gateway admits one
specialist globally, and on a Mac with 18 GiB RAM or less it stops the primary
chat backend before loading Molmo2. When a conversation starts with one image
or video, Samosa returns Molmo's own text and point markup directly, unloads it,
and starts restoring the selected text model without waiting on that restart.
Later/composite visual turns can still use a greedy, non-thinking grounded
synthesis pass when another model is actually needed.

VisionPsy is available in the first release on macOS Apple Silicon. A text-only
chat LLM can still use it: `/healthz` distinguishes native chat-model image
support from end-to-end attachment support through the auxiliary helper.

### Automatic image sizing and document coverage

Users do not need to resize a high-resolution photo first. Samosa validates the
decoded dimensions and resizes the image with the pinned VisionPsy
preprocessor. The gateway chooses a maximum side of 2048, 1536, 1024, or 512
pixels from total RAM, currently available memory, memory-pressure state,
thermal state, and requested detail. A 16 GB Mac starts no higher than 1536;
the gateway samples again between PDF pages and can reduce resolution if the
machine becomes busier.

PDF coverage is task-driven and has no five-page or other fixed product cap.
Explicit-page questions use those pages; whole-document visual tasks enqueue
all required pages; other tasks use the planner's relevant pages. Pages run
sequentially through one helper process for the turn so the complete model is
not loaded once per page. The Settings UI deliberately has no maximum-pages or
RAM-budget control.

### Download, cancellation, and failure behavior

If the plan requires vision and the weights are absent, an inline
**Download and continue** card shows the exact model and size. After the user
accepts, Samosa verifies and installs the model, then resumes the exact pending
turn once—without duplicating the message or attachments. A failed poll or
installation exposes Retry and does not lose the pending work. A download can
be cancelled; an active response can be stopped with the normal Stop control.
The helper is terminated when the visual turn finishes or fails.

If visual analysis fails after text/OCR succeeds, the streamed answer begins:

> Partial answer — visual analysis failed; this answer uses text/OCR only.

It is constrained to the available literal evidence and must not claim it saw
layout, images, colors, objects, charts, or relationships. If some PDF pages
were visually inspected before a later failure, Samosa labels the answer as an
incomplete visual analysis and keeps the completed-page observations. If
vision was the only viable evidence source, Samosa returns a retryable error
instead of fabricating an answer. Under critical memory or thermal pressure,
close memory-heavy work or let the Mac cool, then Retry.

## Video attachments

Choose **+ → Video** for MP4, MOV, or M4V and ask a visual question. Uploads are
streamed to the private content-addressed attachment store while being SHA-256
hashed; a movie is never buffered wholesale in gateway memory. Video blobs are
limited to 4 GiB. Molmo2 is required for this route—Samosa does not silently
replace video understanding with VisionPsy or filename guesses.

The evidence planner selects one of three bounded modes:

- **overview** samples up to 16 timestamps across the video;
- **temporal** performs a timestamped coarse pass and one 7.5-second dense pass
  around the first cited candidate moment (or the midpoint fallback);
- **exhaustive** performs a coarse pass and then sequential 7.5-second windows
  at up to 2 fps, with one-second overlap and an eight-window per-turn ceiling.

Only one window is decoded at a time. The response evidence lists the actual
timestamps, covered intervals, and any unprocessed interval. Resource pressure,
cancellation, the eight-window ceiling, the primary-model evidence budget, or
helper failure stops at a completed boundary; the final answer must not imply
coverage beyond it.

If the Q4 package is missing, the pending turn shows **Molmo2 4B Native Q4
required** and links to model setup. Unlike VisionPsy, it cannot be downloaded
from the app until a reviewed hash-pinned artifact is published. See
[INSTALL.md](INSTALL.md#optional-molmo2-4b-package).

## Context capacity

Settings → Total context capacity controls history + new prompt + thinking +
generated answer.

**Auto** is recommended:

- Qwen calculates capacity from machine memory and the K/V bytes per token.
- Bonsai and Ornith let Prism fit current device memory. The app displays the
  actual context returned by `/props`, not a static default.

An explicit integer up to 262,144 overrides Auto. Changing GGUF capacity
restarts that backend. A high manual setting can create memory pressure even
when the model initializes, so use it deliberately.

## Compaction

Automatic compaction is enabled at 80% projected use by default. The app offers
70%, 75%, 80%, 85%, and 90% presets and a manual **Compact this conversation
now** button.

Compaction keeps:

- dense model-written continuation memory for older turns;
- recent turns verbatim;
- the same browser chat and conversation ID.

It replaces only the durable model-facing context. The next request rebuilds
K/V state from the compacted memory. If the summary is not smaller, Samosa
keeps the old ledger.

## Thinking modes

- **Direct**: shortest path to the answer.
- **General thinking**: permits additional internal reasoning.
- **Precise code / WebDev**: uses the code-oriented thinking control.

The exact behavior depends on the active model and its chat template.

## Terminal chat

Direct terminal prompts currently use Qwen:

```sh
samosa "explain how DNS works"
samosa --continue "now explain DNSSEC"
samosa --think "solve this logic problem"
samosa --think-code "write a parser"
samosa --fast "use the warmer, faster thread profile"
samosa --seed 11 "make this sampling path reproducible"
samosa --max-tokens 4096 "allow a longer answer"
samosa --context-tokens 65536 "use this explicit total capacity"
```

Install Qwen first with `samosa pull qwen`. Use the app or HTTP gateway for
Bonsai and Ornith conversations.

`--continue` resumes `~/.samosa/last_session.qws`. App conversations instead
use IDs under `~/.samosa/chats`.

## Server commands

```sh
samosa serve
samosa serve --lan
samosa serve --context-tokens auto
samosa serve --context-tokens 65536
samosa serve --stop
```

The gateway listens only on `127.0.0.1:8642` by default.

## LAN access

The complete operational and security documentation is in
[LAN_ACCESS.md](LAN_ACCESS.md). The summary below covers the normal startup
path.

Run this on the Mac that has Samosa and the models installed:

```sh
samosa app --lan
```

The command still opens `http://127.0.0.1:8642` on the Mac, and prints a line
like this for the phone or second computer:

```text
Other devices: http://192.168.1.20:8642/
Password:      password1234
```

Connect both devices to the same local network, open the link, and enter the
password. A successful login creates a browser session that expires when the
Samosa gateway restarts. Access grants use of chats, models, attachments, and
any folders you deliberately make available through Samosa, so do not
port-forward it or use LAN mode on a network you do not trust. The temporary
default password is `password1234`; it can be overridden before launch with
`SAMOSA_LAN_PASSWORD`.

The Mac must remain awake and connected. If macOS Firewall asks whether to
allow incoming connections for `samosa-gateway`, choose **Allow**. Guest Wi-Fi
often isolates devices from one another; use the normal trusted Wi-Fi instead.
If the Mac's local IP changes, run `samosa app --lan` again to print the current
link. Stop all access with:

```sh
samosa serve --stop
```

Starting `samosa app` or `samosa serve` without `--lan` switches a running
server back to loopback-only mode. LAN access uses plain local HTTP; many mobile
browsers therefore disable microphone capture, while typed chat, model use,
attachments, and audio playback continue to work.

All connected browsers control one shared Samosa instance. Only one primary
model generation runs at a time; additional requests wait without loading a
second copy of the model. Model choice is also global. A model selected from
one device becomes the active model for every device, and an active generation
or another in-progress switch can reject a competing switch. See
[Concurrent requests](LAN_ACCESS.md#concurrent-requests) and
[Selecting different models](LAN_ACCESS.md#selecting-different-models-from-different-devices)
for the exact behavior and queue math.

## Settings persistence

- model selection: `~/.samosa/model-backend`
- gateway context/compaction: `~/.samosa/gateway-settings.json`
- Qwen settings: `~/.samosa/config.json`
- GGUF durable conversations: `~/.samosa/chats/CHAT_ID/MODEL.json`
- app transcript and display preferences: browser local storage

## Diagnostics

```sh
samosa doctor
samosa models
curl -s http://127.0.0.1:8642/healthz | python3 -m json.tool
```

`healthz` reports the active model, readiness, actual context capacity,
generation state, and compaction settings.

For reproducible model-pipeline debugging, open **Settings → Advanced →
Developer mode**. It records the full local request, attachment routing, raw
OCR/document/VisionPsy output, final evidence prompt, backend response, errors,
and timings under `~/.samosa/logs/developer`. It is off by default because the
trace can contain complete prompts and private document contents. Authentication
tokens are never logged. See [DEVELOPER_MODE.md](DEVELOPER_MODE.md).
