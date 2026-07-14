# Samosa resident server

Implementation status: developer preview. The model stays resident, requests
are serialized through a bounded FIFO, and every connection is local-only on
`127.0.0.1` (port 8642 by default).

## Launch and stop

```sh
samosa serve          # foreground, logs visible
samosa app            # background single instance, then open the browser
samosa serve --stop   # cooperative cancellation + clean shutdown
```

`SAMOSA_PORT` overrides the port. Background state is under `~/.samosa/`:
`server.pid`, `server.log`, and `chats/`.

## Endpoints

- `GET /` — temporary technical landing page until the Phase A1 UI lands.
- `GET /healthz` — RSS, uptime, queue state, and last-generation speed.
- `GET /v1/models` — OpenAI-shaped model listing.
- `POST /v1/chat/completions` — JSON or SSE chat response.
- `POST /v1/cancel` — cooperatively stop the active generation between tokens.
- `POST /v1/shutdown` — cancel active work, reject queued work, and stop.

The chat body accepts one or more text messages and uses the last user message.
Supported controls are `stream`, `max_tokens`/`max_completion_tokens` (1..8192),
`temperature`, `top_p`, `top_k`, `seed`, `thinking` (`off`, `general`, or
`code`), `thinking_budget` (0..8192), and `conversation_id`.

`conversation_id` is limited to 64 letters, digits, dashes, or underscores.
The current developer preview persists a sealed `session.qws` under
`~/.samosa/chats/<id>/`, so later turns avoid history prefill and survive a
restart. The planned four-slot in-RAM LRU is not implemented yet; turns restore
the snapshot from disk.

Example:

```sh
curl -N http://127.0.0.1:8642/v1/chat/completions \
  -H 'Content-Type: application/json' \
  --data-binary '{
    "messages": [{"role": "user", "content": "Explain the invariant."}],
    "thinking": "general",
    "thinking_budget": 1024,
    "max_tokens": 8192,
    "conversation_id": "demo",
    "stream": true
  }'
```

Streaming chunks place pre-closure text in `delta.reasoning` and final answer
text in `delta.content`. The terminal chunk reports `finish_reason`, token
usage, `samosa.thinking_closure` (`natural`, `budget_transition`, `repetition`,
or `cancelled`), tokens/s, RSS, and `session_saved` (`true`/`false` for a
conversation request, otherwise `null`). A snapshot failure is therefore
visible instead of being silently reported as durable.

## Admission and safety

- The default queue holds four waiting requests. Excess requests receive 429
  with `Retry-After: 1`; shutdown rejects waiters with 503.
- Model mutation is strictly single-request even though socket handling is
  concurrent.
- Client send failure, `/v1/cancel`, SIGINT/SIGTERM, and shutdown share an
  atomic cancellation flag checked between generated tokens.
- Request headers are capped at 64 KiB and bodies at 4 MiB. Chunked request
  bodies are rejected. The listener is hard-bound to IPv4 loopback.
- Per-request KV/DeltaNet state and sampler bitmaps are explicitly reclaimed;
  the tokenizer and model/expert cache remain resident.

## Verified 2026-07-14

- Socket component test: health/models/root/cancel/shutdown, 20 sequential
  connections without RSS growth, bounded-queue rejection, JSON escaping, and
  clean listener termination.
- Real group-32 server: health/model discovery, one-token SSE (`OK`), correct
  terminal telemetry, 3.29 GB peak RSS, 4.05 GB expert reads, clean shutdown.
- Real launcher/session path: double `samosa app` kept one PID; two
  `conversation_id` turns returned `OK` then `YES`, restored the sealed session,
  and `samosa serve --stop` exited cleanly.
- Real cancellation/admission path: with a zero-length wait queue, a second
  request received `429 queue_full`; `/v1/cancel` stopped the active request at
  zero completion tokens, emitted a terminal `cancelled` event, read 4.94 GB
  of experts, peaked at 3.32 GB RSS, and shut down cleanly.
- After acceptance: unchanged swapouts, zero throttled pages, and no macOS
  thermal/performance warning.

Still required before calling the app shippable: the Phase A1 browser UI,
in-RAM conversation slots with write batching, the bounded long-context
regression described in `APP_TASKS.md`, exact artifact fingerprints in health
telemetry, and the declared soak/package release checks. The server foundation
itself is suitable for continued app development, but the page at `/` is
intentionally still a technical status page.
