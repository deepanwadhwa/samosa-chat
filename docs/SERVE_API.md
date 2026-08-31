# Samosa local gateway API

Start the gateway:

```sh
samosa serve
```

It listens at `http://127.0.0.1:8642` by default. `samosa serve --lan` is the
explicit, password-protected mode for clients on the same local network;
see [LAN_ACCESS.md](LAN_ACCESS.md). The gateway is healthy with no installed
model so the app can provide onboarding and downloads.

## LAN authentication

LAN mode binds the authenticated gateway to `0.0.0.0` but forces the selected
model backend to remain on `127.0.0.1`, normally on the next port. A remote
browser first receives the password page from `GET /`, then authenticates with:

```http
POST /v1/lan/login
Content-Type: application/json

{"password":"password1234"}
```

Success returns `200 {"authenticated":true}` and sets the `samosa_lan`
browser-session cookie with `HttpOnly`, `SameSite=Strict`, and `Path=/`.
Incorrect passwords return `401 invalid_lan_password`. Other unauthenticated
remote API calls return `401 lan_access_denied`. The authenticated app also
uses its per-launch `X-Samosa-Token` for protected routes and the gateway
rejects a browser `Origin` that does not match the request `Host`.

Loopback clients on the host Mac remain directly accessible. Restarting the
gateway rotates the internal token and invalidates existing LAN sessions. This
is shared-instance authentication: it does not create user identities,
per-user chats, or per-user model selections. See the full guide for security,
concurrency, and operational limitations.

## Health

```http
GET /healthz
```

Representative model-less response:

```json
{
  "gateway": true,
  "listen_address": "127.0.0.1",
  "lan_access": false,
  "lan_auth": "none",
  "backend": "qwen",
  "supports_images": false,
  "supports_image_attachments": true,
  "supports_video_attachments": true,
  "supports_documents": true,
  "vision": {
    "auxiliary_runtime_available": true,
    "visionpsy_runtime_available": true,
    "molmo2_runtime_available": true,
    "molmo2_model_ready": false,
    "video_available": false
  },
  "ocr": {
    "runtime_available": true,
    "pack_ready": false,
    "ready": false
  },
  "installed": false,
  "ready": false,
  "loading": false
}
```

`supports_images` describes the active chat backend's native image input.
`supports_image_attachments` describes the complete app path and can therefore
be true for a text-only LLM when an auxiliary visual runtime is available.
An installed valid Molmo2 package is preferred; VisionPsy is the fallback.
`supports_video_attachments` reports that the installed app contains
the native video runtime; `vision.video_available` additionally requires a
structurally valid, pinned local Molmo2 package. The helper verifies every
package file hash before mapping tensors. The composer can therefore accept a video and
show actionable setup without pretending the model is ready. `ocr.runtime_available`
reports the native executable, `ocr.pack_ready` reports the detector,
recognizer, and charset files, and `ocr.ready` requires both; the UI must not
claim scanned-page OCR is ready from the executable alone. Digital PDF text can
still be available when the OCR pack is missing.

When ready, the response also includes the active label/model, actual
`context_limit_tokens`, context mode, generation state, and compaction status.

## List models and download state

```http
GET /v1/backends
```

```json
{
  "active": "ornith",
  "download": {"active": false, "phase": "idle"},
  "backends": [
    {
      "id": "ornith",
      "label": "Ornith 1.0 9B",
      "size_bytes": 5629108704,
      "license": "MIT",
      "model_downloaded": true,
      "runtime_ready": true,
      "installed": true,
      "available": true,
      "active": true
    }
  ]
}
```

`GET /v1/downloads` returns the same download snapshot plus the model array.

## Install a model

```http
POST /v1/backends/install
Content-Type: application/json

{"backend":"bonsai"}
```

Returns `202` with a background download snapshot. Only one download runs at a
time. Poll `/v1/backends` or `/v1/downloads`.

Molmo2 uses `provisioning: "native_pack"`. Until a reviewed public artifact is
published, requesting its installation returns `409 native_pack_required` and
does not download the upstream FP32 checkpoint. See
[INSTALL.md](INSTALL.md#optional-molmo2-4b-package).

```http
POST /v1/backends/install/cancel
```

Cancellation retains the resumable `.partial` file.

## Select a model

```http
POST /v1/backends/select
Content-Type: application/json

{"backend":"ornith"}
```

Returns `202`. A missing model or an active generation returns `409`. Switching
stops the current backend before starting the selected one. Selection is global
to the gateway, not scoped to a browser session. Simultaneous switches are
serialized; a competing switch can return `409 selection_busy`. All model
backends are configured for one active primary-model generation at a time.

## Context and compaction settings

```http
POST /v1/settings
Content-Type: application/json

{
  "context_tokens": "auto",
  "auto_compact": true,
  "compact_threshold_percent": 80
}
```

`context_tokens` is `"auto"` or an integer from 2 through 262,144. The
threshold accepted by the API is 50–90.

For GGUF Auto, the initial response can report zero while the fitter is loading.
The next health response reports Prism's actual `n_ctx`.

## Compact a conversation

```http
POST /v1/compact
Content-Type: application/json

{"conversation_id":"chat-abc123"}
```

The active local model summarizes older durable context and retains recent
turns. Success includes before/after token counts. A non-shrinking summary,
missing ledger, active generation, or invalid ID is rejected without replacing
the prior conversation state.

## Chat completions

```http
POST /v1/chat/completions
Content-Type: application/json

{
  "model": "ornith-1.0-9b",
  "conversation_id": "chat-abc123",
  "messages": [
    {"role": "user", "content": "Explain a B-tree."}
  ],
  "stream": true,
  "max_tokens": 512
}
```

The route is OpenAI-compatible. The gateway normalizes Qwen and GGUF streaming
and appends a `samosa` metadata object to the terminal event when available.
`conversation_id` selects backend-appropriate continuation state: native live
K/V plus a durable checkpoint for Qwen, native live K/V with browser-transcript
recovery for Maple, and a stable cached prompt prefix for Bonsai and Ornith.
Only one conversation is hot per active text engine; switching conversations
causes a cold restore or rebuild without loading another model instance. See
[CONVERSATION_CONTEXT.md](CONVERSATION_CONTEXT.md) for the lifecycle, document
reuse, eviction, privacy, and recovery contracts.

Qwen's private backend `/healthz` includes a `resident_session` object with
one-slot capacity, hot token count, eviction policy, hit/restore/eviction
counters, and scheduler state. Maple reports its one-slot resident state and
uses `available: false` rather than blocking a health probe during inference.

## Cancel generation

```http
POST /v1/cancel
```

## Developer pipeline trace

The authenticated `GET /v1/developer/trace` route reports whether full local
pipeline capture is enabled and returns its current path, directory, bytes, and
event count. `PUT /v1/developer/trace` with `{"enabled":true}` or `false`
changes and persists the mode. `POST /v1/developer/trace/clear` removes prior
trace files only while the mode is disabled; otherwise it returns `409
developer_mode_active`.

Trace JSONL correlates the incoming chat request, router prompt/raw decision,
validated OCR/Vision plan, adaptive resource tier, document/OCR results,
VisionPsy command/raw observation, exact final backend request, raw backend
response, timings, process lifecycle, and errors using one `turn_id`. It can
contain private document text and is therefore off by default and stored with
mode `0600`. UI/authentication tokens are never captured. See
[DEVELOPER_MODE.md](DEVELOPER_MODE.md) for the schema and diagnostic workflow.

## Optional public Internet sources

Built 2026-07-28 (Phase W, then Phase WK,
[TASKS_WEB_SEARCH.md](TASKS_WEB_SEARCH.md)). An earlier version of this section
listed three routes before any of them existed; they exist now. What has and
has not been verified is recorded in
[regressions/web-search/report.md](regressions/web-search/report.md) and
[regressions/web-search/keyless-2026-07-28/report.md](regressions/web-search/keyless-2026-07-28/report.md).

All four require the `X-Samosa-Token` UI session token.

```http
GET /v1/web/config
```

```json
{"offline": false, "fetch_available": false, "search_configured": true,
 "consent": "unset", "keyless": true, "searches_today": 0, "daily_limit": 100,
 "web_available": false, "provider": "parallel", "reason": ""}
```

That is a fresh install: search is *configured* (the default provider needs no
credential) but not yet *allowed*. The two are separate fields because only one
of them is something the user can fix by editing a file.

- `search_configured` — a search request can be built. Independent of consent.
- `consent` — `"unset"` (ask), `"granted"`, or `"denied"`. Unset is distinct
  from denied: one means the question is open, the other means it was answered.
- `keyless` — the active provider needs no credential.
- `web_available` — the effective answer: not offline **and** consent granted.
- `fetch_available` — same condition; reading a page is an outbound request too.
- `searches_today` / `daily_limit` — Samosa's own cap on the free tier, by
  local calendar day. Only meaningful while `keyless`; `0` means uncapped.

Never returns a credential — only whether a usable one is present. `reason`
explains an unavailable capability and is safe to show a user.

```http
POST /v1/web/consent    {"granted": true}
```

Records the one-time answer in `search.consent` in `~/.samosa/config.json` and
replies with the same body as `GET /v1/web/config`. The write merges: every
other key in the file, including API keys, is preserved, and it is atomic and
`0600`. A non-boolean `granted` is `400 granted_required` and changes nothing.

```http
POST /v1/web/fetch      {"url": "https://…"}
```

Returns `{ok, url, title, truncated, text}` for one public page. `url` is the
final URL after redirects. Rejections (SSRF, scheme, port, credentials in the
URL, robots, unreadable JS-only page) are `400 fetch_failed` with the reason.

```http
POST /v1/web/search     {"query": "…"}
```

Returns `{ok, provider, results: [{title, url, description}]}`, at most 8
results. `409 search_not_configured` when no usable provider is configured;
`409 offline` in offline mode; `502 search_failed` when a configured provider
fails; `429 search_daily_limit` when Samosa's own free-tier cap is reached
(never raised for a provider the user supplied a key for).

Both outbound routes refuse before consent is answered: `403 consent_required`
when it is unset, `403 consent_denied` when it was declined. Neither makes a
network call in that state.

Chat turns opt into the web per request, on `POST /v1/chat/completions`:

- `"web_urls": ["https://…"]` — read exactly these pages (max 3).
- `"web": true` — let the model choose `web_search`/`open_url`, max 3 calls.

Both splice the resulting text into the final user message and, on a streaming
turn, emit tool activity as `delta.reasoning` SSE events before the answer. A
request with neither field is forwarded to the backend unchanged.

Without consent, `"web": true` is **also** forwarded unchanged — no planner
call, no added latency, byte-identical to a request that never asked. A
`"web_urls"` turn instead streams one line saying why the page was not read,
because there the user explicitly asked for a specific page.

See [MODELS_AND_INTERNET.md](MODELS_AND_INTERNET.md) for configuration.

## Attachments and vision understanding

```http
POST /v1/attachments
Content-Type: image/png (or image/jpeg, application/pdf, text/plain, video/mp4,
              video/quicktime)
X-Filename: example.png

<binary bytes>
```

Returns `{id: "<sha256>", filename: "example.png", media_type: "image/png", size_bytes: 12345}`.

The attachment route has a separate 4 GiB body ceiling. It streams the request
into a private temporary file while hashing and enforcing the limit, then
atomically publishes the content-addressed blob and metadata. MP4/MOV/M4V are
accepted only when their container contains an `ftyp` signature; extension or
client MIME alone is insufficient. Other ordinary API requests retain the 4
MiB parser cap.

`GET /v1/attachments/{sha256}` streams the private blob and supports one byte
range, including suffix ranges. Valid ranges return `206`, `Accept-Ranges`, and
`Content-Range`; unsatisfiable ranges return `416`. `DELETE` retains the
existing attachment-reference safety checks.

### Chat with Attachments

In `POST /v1/chat/completions`:
```json
{
  "messages": [{"role": "user", "content": "Explain this diagram"}],
  "attachment_ids": ["<sha256>"]
}
```

The gateway asks the active LLM for strict route JSON (`read_text`,
`inspect_visual`, detail, visual scope, and explicit pages), validates it, and
then acquires only the requested evidence. Deterministic visual classification
is a floor; the planner cannot downgrade an obvious image/video task to
text-only work. An installed Molmo2 package is the preferred visual provider,
with VisionPsy and then resident native image input as fallbacks. A digital PDF text layer is preferred over
OCR; a visual-only PDF route does not run OCR as an accidental side effect.

For PDFs, explicit pages and whole-document scope are honored. Relevant-page
selection scans the whole page inventory in internal extractor windows; the
window size is not a turn cap. Required pages are rendered and processed
sequentially through one helper session. Before load and before every page, the
gateway selects a 2048/1536/1024/512 input tier from live hardware state and
may downshift during the turn.

Video attachments always route to the validated Molmo2 capability. The planner
chooses `overview`, `temporal`, or `exhaustive`. Every inference request uses at
most 16 uniformly timestamped frames. Temporal mode adds one 7.5-second dense
refinement around a cited coarse timestamp (or the midpoint fallback).
Exhaustive mode first takes a coarse pass, then walks 7.5-second windows with
one-second overlap and no parallel frame batches, with at most eight dense
windows per user turn. Evidence records actual decoded frame counts, source
duration, covered intervals, the selected planner mode, and any unprocessed
interval. Molmo2 is terminated before the primary backend synthesizes the final
answer; on constrained Macs the primary backend is stopped before Molmo2 starts.
Final visual synthesis runs greedily with model thinking disabled and a grounded system
contract: answer from the specialist observation, never from the filename, and
never claim that the image or video was inaccessible after successful inspection.

Important attachment errors are:

| HTTP/code | Meaning and recovery |
|---|---|
| `422 vision_model_required` | The validated route needs visual evidence but the weights are absent. Preserve the request, install `visionpsy-nano-460m-mlx-bf16`, then retry the same payload. |
| `422 vision_resource_pressure` | Available memory or thermal state makes starting/continuing unsafe. Text/OCR evidence is returned as a partial when possible; otherwise free resources/cool the Mac and retry. |
| `422 vision_page_selection_failed` | Required PDF pages could not be inventoried or selected. The client may retry after checking the file. |
| `422 vision_inference_failed` and helper-provided vision codes | Visual inference failed after its bounded lower-resolution retry. Return a partial if text/completed-page evidence exists; otherwise do not synthesize an answer. |
| `422 molmo2_model_required` | Video/extended vision needs the pinned native Q4 package. Preserve the request, follow native-pack setup, then retry. |
| `422 molmo2_resource_pressure` | Starting or continuing Molmo2 would violate live memory/thermal admission. No higher-cost retry is attempted. |
| `422 molmo2_video_analysis_failed` | Native media decode or Molmo2 generation failed; do not imply that the affected interval was inspected. |

The browser's **Download and continue** action starts
`POST /v1/models/install` with the exact model/version and an idempotency ID,
then polls the returned per-job `status_url`. It keeps the exact pending chat
payload and continues once after verified completion. Concurrent/late polls are
collapsed so they cannot create duplicate answers. Installation or polling
failure leaves a visible Retry action.

For streaming text/OCR fallback, the gateway mechanically prepends this exact
visible content before any model tokens:

```text
Partial answer — visual analysis failed; this answer uses text/OCR only.
```

It also inserts a trusted synthesis constraint that forbids claims about
unseen images, layout, charts, colors, objects, or relationships. If earlier
PDF pages completed before a later failure, the corresponding label says the
required visual inspection is incomplete and evidence contains only completed
pages. Vision-only failure returns an error instead of a fabricated answer.

## Shutdown

```http
POST /v1/shutdown
```

The gateway cancels an active download, stops the model process, and exits.
