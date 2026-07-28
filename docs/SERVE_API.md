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

## Shutdown

```http
POST /v1/shutdown
```

The gateway cancels an active download, stops the model process, and exits.
