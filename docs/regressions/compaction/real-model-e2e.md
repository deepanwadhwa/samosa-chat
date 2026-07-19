# Real-model conversation compaction — 2026-07-19

## Result

**PASS for the controlled automatic-compaction path.**

The real Qwen3.6 35B-A3B checkpoint summarized a sealed 1,724-token
conversation, Samosa atomically rebuilt it as a 263-token snapshot while
retaining 72 recent tokens verbatim, the triggering turn continued under the
same `conversation_id`, and a clean server restart recalled both an early
summary-only fact and a recent fact exactly.

This was not a stub/model-free test. It used the 24 GB converted checkpoint and
the feature-branch engine at commit `d655567`.

## Environment

- macOS 26.5.1 (25F80), arm64
- Apple M3, 16 GiB unified memory
- Engine: `make omp`, `OMP_NUM_THREADS=4`
- Model: `~/Documents/samosa-models/qwen36_group32_i8`
  (`groupwise-symmetric-q4-v1`, group 32 experts; whole-row resident quant)
- Tokenizer: `~/.samosa/current/tokenizer_qwen36.json`
- Isolated server: `127.0.0.1:8765`
- Isolated chat storage: `/tmp/samosa-compaction-e2e.AoATkj/chats`
- Initial context setting: 2,048 tokens; trigger setting: 2,560 tokens
- Auto-compaction threshold: 80% projected use

The deliberately small context made the state transition practical to exercise
while using exactly the same session, summarization, K/V rebuild, atomic save,
restart, and continuation code as a production-sized window.

## Launch and health

```sh
make omp

SNAP="$HOME/Documents/samosa-models/qwen36_group32_i8" \
TOKENIZER="$HOME/.samosa/current/tokenizer_qwen36.json" \
SAMOSA_CHATS_DIR="/tmp/samosa-compaction-e2e.AoATkj/chats" \
SAMOSA_CONTEXT_TOKENS=2048 \
OMP_NUM_THREADS=4 \
./qwen36b --serve --port 8765 \
  --tokenizer "$HOME/.samosa/current/tokenizer_qwen36.json"
```

Initial health:

```json
{
  "rss_gb": 2.94,
  "model_context_limit_tokens": 262144,
  "context_limit_tokens": 2048,
  "kv_bytes_per_token": 40960,
  "compaction": {"auto": true, "threshold_percent": 80}
}
```

## Controlled conversation

All requests used `conversation_id: "compaction-e2e"`, `thinking: "off"`,
`temperature: 0`, and non-streaming JSON responses.

### 1. Early fact plus long ballast

The first user turn stated:

> EARLY MEMORY: The project codename is ORCHID-731, its launch color is amber,
> and its owner is Mira.

It then contained 80 numbered archival-ballast lines and requested the exact
answer `ACK-ORCHID`.

Observed response:

```json
{
  "content": "ACK-ORCHID",
  "usage": {"prompt_tokens": 1659, "completion_tokens": 6, "total_tokens": 1665},
  "samosa": {"session_saved": true, "compacted": false}
}
```

The sealed snapshot was 134.0 MB. Prefill took 76.330 seconds
(21.73 prompt tok/s); decode was 4.52 tok/s.

### 2. Recent fact before compaction

Automatic compaction was temporarily disabled only to place a second marker
near the end of the old snapshot:

```sh
curl http://127.0.0.1:8765/v1/settings \
  -H 'Content-Type: application/json' \
  --data-binary '{"auto_compact":false}'
```

The next user turn stated:

> RECENT MEMORY: The verification code is LIME-882.

Observed response:

```json
{
  "content": "ACK-LIME",
  "usage": {"prompt_tokens": 1720, "completion_tokens": 4, "total_tokens": 1724},
  "samosa": {"session_saved": true, "compacted": false}
}
```

The pre-compaction snapshot was 136.4 MB.

### 3. Automatic compaction trigger

The context was raised enough to guarantee bounded summary headroom, then
automatic compaction was re-enabled:

```sh
curl http://127.0.0.1:8765/v1/settings \
  -H 'Content-Type: application/json' \
  --data-binary \
  '{"context_tokens":2560,"auto_compact":true,"compact_threshold_percent":80}'
```

The trigger turn requested `max_tokens: 400`. The projected total exceeded
80% of 2,560 tokens, so compaction ran before the trigger turn.

Engine evidence:

```text
[session] resumed .../session.qws: 1724 tokens, 1723 KV rows
[stats] prompt=1822 generated=149 stop=model ... total=30.319s
[session] saved .../session.qws: 263 tokens, 76.6 MB
[compaction] .../session.qws: 1724 -> 263 tokens (72 recent retained)
[session] resumed .../session.qws: 263 tokens, 262 KV rows
```

The trigger request then continued normally:

```json
{
  "content": "TRIGGER-DONE",
  "usage": {"prompt_tokens": 295, "completion_tokens": 5, "total_tokens": 300},
  "samosa": {
    "session_saved": true,
    "compacted": true,
    "compacted_from_tokens": 1724,
    "compacted_to_tokens": 263
  }
}
```

After saving the trigger answer, the durable snapshot contained 300 tokens and
was 78,112,044 bytes. The old 136.4 MB file was replaced only after the new
sealed session had been written.

## Restart and recall

The server was stopped through `POST /v1/shutdown`, then relaunched against the
same isolated chat directory and 2,560-token setting. The first post-restart
turn asked for the early and recent markers exactly.

Observed response:

```json
{
  "content": "ORCHID-731 | amber | Mira | LIME-882",
  "usage": {"prompt_tokens": 355, "completion_tokens": 19, "total_tokens": 374},
  "samosa": {"session_saved": true, "compacted": false}
}
```

The early ORCHID/amber/Mira marker was more than 1,600 tokens before the end of
the old transcript and outside the 72-token retained tail, so its successful
recall exercises the generated continuation memory. The LIME-882 marker was in
the recent pre-compaction turn covered by that retained tail.

## Machine-safety observations

- Peak API-reported physical footprint: approximately 4.65 GB.
- Swap before/after: `0.00M used`.
- `pmset -g therm`: no thermal, performance, or CPU-power warning recorded.
- No normal `~/.samosa/chats` data was used.

## Scope and remaining qualification

This proves one controlled real-model automatic-compaction and restart path. It
does **not** yet qualify:

- a naturally accumulated 20K+ production conversation;
- summary quality across broad coding, tool, or document workloads;
- image-history compaction;
- manual `/v1/compact` with the real model (it calls the same
  `compact_session` backend; its HTTP validation is component-tested);
- compaction on machines other than this 16 GiB M3 Mac.

Those remain separate soak/quality work and must not be inferred from this
single passing regression.
