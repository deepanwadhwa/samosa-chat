# Samosa local gateway API

Start the gateway:

```sh
samosa serve
```

It listens at `http://127.0.0.1:8642`. The gateway is healthy with no installed
model so the app can provide onboarding and downloads.

## Health

```http
GET /healthz
```

Representative model-less response:

```json
{
  "gateway": true,
  "backend": "qwen",
  "installed": false,
  "ready": false,
  "loading": false
}
```

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
stops the current backend before starting the selected one.

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
For Bonsai and Ornith, `conversation_id` enables the durable per-model ledger.

## Cancel generation

```http
POST /v1/cancel
```

## Optional public Internet sources

```http
GET /v1/web/config
POST /v1/web/fetch
POST /v1/web/search
```

These are separate from model installation. Fetch/search validates public
destinations and caps content. See
[MODELS_AND_INTERNET.md](MODELS_AND_INTERNET.md).

## Shutdown

```http
POST /v1/shutdown
```

The gateway cancels an active download, stops the model process, and exits.
