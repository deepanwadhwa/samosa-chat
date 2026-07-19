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

`SAMOSA_PORT` overrides the port. `SAMOSA_CONTEXT_TOKENS=auto` selects the
hardware-aware total-context policy; set it to an integer such as `131072` to
choose an explicit limit (never above the checkpoint's model limit). The CLI
equivalent is `--context-tokens 131072`. Background state is under `~/.samosa/`:
`server.pid`, `server.log`, and `chats/`.

## Endpoints

- `GET /` — dependency-free interactive Samosa Chat application.
- `GET /assets/samosa-chat.png` — local transparent app mascot.
- `GET /healthz` — macOS physical footprint, model/effective context limits,
  KV bytes per token, uptime, queue state, and last-generation speed.
- `GET /v1/models` — OpenAI-shaped model listing.
- `POST /v1/settings` — update total-context and auto-compaction settings.
- `POST /v1/compact` — compact one saved conversation in place.
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
the snapshot from disk. Only the active request's conversation state is loaded,
so multiple saved chats do not accumulate KV allocations in RAM.

The exact tokenized request is checked before queue admission or stream
headers. Saved history + the new turn + the requested completion ceiling must
not exceed the effective total-context limit. The model's own declared position
limit is an absolute ceiling; the current Qwen checkpoint declares 262,144.
Oversized turns receive `400 context_limit` without allocating KV state. A
request that fits the configured token limit but fails the current-memory
preflight receives `503 insufficient_memory`. The server reports the actual KV
bytes per token in `/healthz` rather than assuming a model-specific constant.
The session choice and token count are checked again after queue admission, so
two requests for the same conversation cannot race against a stale snapshot.

The browser’s **Total context capacity** setting calls the local-only settings
endpoint and remembers the choice in browser-local storage. It accepts
`context_tokens` as `"auto"` or a positive integer no larger than the model
limit. The same object can set `auto_compact` and
`compact_threshold_percent` (an integer from 50 through 90):

```json
{
  "context_tokens": "auto",
  "auto_compact": true,
  "compact_threshold_percent": 80
}
```

The update waits for the active generation slot, so it cannot change a context
budget mid-generation. Existing conversations are retained.

## Conversation compaction

Automatic compaction is enabled by default at 80% projected context use.
“Projected” means the sealed history plus the exact incoming turn and its
requested completion ceiling. This leaves room for Qwen to produce the
continuation memory before the hard context limit is reached. Settings can turn
automatic compaction off or choose a 70%, 75%, 80%, 85%, or 90% threshold.

Compaction is a real state replacement, not an extra summary message appended
to the old cache:

1. Samosa resumes the sealed session and asks Qwen for a structured continuation
   memory, so the summarizer sees the actual prior K/V state.
2. The temporary summarization state is freed.
3. Samosa creates a new transcript containing the continuation memory and a
   recent verbatim tail (15% of the configured window, aligned to a message
   boundary).
4. The smaller K/V and DeltaNet state are prefilled in bounded chunks and written to a temporary
   sealed session. `session.qws` is replaced by atomic rename only after the new
   file is hashed, flushed, and fsynced.

The conversation ID, browser transcript, and next-turn behavior stay the same.
If any phase fails, the previous `session.qws` remains authoritative. Image
understanding is captured by the resumed summary; raw image placeholder tokens
are not copied into the text-only recent tail.

Manual compaction uses the same path:

```sh
curl http://127.0.0.1:8642/v1/compact \
  -H 'Content-Type: application/json' \
  --data-binary '{"conversation_id":"demo"}'
```

Success reports `before_tokens`, `after_tokens`, and
`retained_recent_tokens`. Compaction requires enough unused context for its
bounded 256–2,048-token summary. Auto-compaction is designed to preserve that
headroom; a very full session manually compacted after the threshold may fail
safely and retain the old snapshot.

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
or `cancelled`), tokens/s, RSS, `session_saved` (`true`/`false` for a
conversation request, otherwise `null`), and auto-compaction metadata
(`compacted`, `compacted_from_tokens`, `compacted_to_tokens`). A snapshot
failure is therefore visible instead of being silently reported as durable.

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
- On macOS, API/UI `rss_gb` is the process's current physical footprint from
  `TASK_VM_INFO`, matching Activity Monitor and `footprint`. CLI regression
  logs retain their separate historical peak-RSS metric.
- After each turn, Samosa frees surplus evicted-expert slabs that are outside
  the live byte-budgeted cache; the 64 miss-scratch slots already provide the
  needed cross-turn allocation reuse. It then asks Darwin malloc to return
  free KV/scratch pages to the OS. Live model weights and cache entries are
  untouched.

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

The Phase A1 browser UI now streams the real local model, separates thinking
from visible answers, stops generation, keeps browser-local transcripts, and
reports speed/RSS/closure telemetry. A bounded group-32 app-path check returned
the exact requested answer in 8 generated tokens, stopped on Qwen's end-of-turn
token, saved the session, decoded at 5.13 tok/s, and peaked at 3.28 GB RSS.

After the context-cap and telemetry correction, a fresh resident process
reported 2.51 GiB while macOS `footprint` reported 2,566 MiB. A real two-turn
sealed-session check returned exactly `OK` then `YES` at 7.05/6.96 tok/s;
both the API and `footprint` agreed on 4.07 GiB / 4,170 MiB after the turn.
The user separately confirmed the app value matched Activity Monitor. The
temporary 64 MB test snapshot was removed.

After the evicted-slab pool fix, an eight-turn repeated test on one resumed
conversation (including a 64-token generation) loaded fresh at 2.51 GiB,
warmed to 3.91 GiB on the first turn, and plateaued at 3.91–3.92 GiB;
`footprint` reported 4,010–4,017 MB for the same samples. Before the fix the
identical test grew about 210 MB per turn.

Still required for the broader app program: in-RAM conversation slots with
write batching, server-side transcript management, the bounded long-context
regression, exact artifact fingerprints in health telemetry, and the declared
soak/package release checks.
