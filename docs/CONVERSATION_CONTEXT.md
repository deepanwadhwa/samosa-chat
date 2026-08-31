# Conversation context and document reuse

Samosa keeps inference on the Mac while browsers on the Mac, a phone, or
another LAN device remain thin clients. A browser sends messages and displays
the stream; it does not load a model, hold model K/V memory, or process an
attached document.

The context design has two complementary layers:

1. Keep the current conversation's model state hot in memory so its next turn
   can continue without repeating a full prompt prefill.
2. Keep enough private local state on disk to recover after an eviction,
   conversation switch, model restart, app restart, or compaction.

The in-memory layer is the fast path. The disk layer is a recovery and
durability path; writing it does not unload the hot state.

## What happens on a document conversation

On the first turn with a PDF or another text document, the gateway resolves the
content-addressed attachment, extracts embedded text or runs local OCR when
needed, and admits a bounded evidence block to the selected text model. It
records the document identity and extraction mode in the conversation manifest.

For a document small enough for full-context mode, Samosa also saves the exact
evidence admitted on that turn in:

```text
~/.samosa/chats/<conversation_id>/document-context.txt
```

The sidecar is mode `0600`, is stored only on the Mac, and is limited to 256
KiB. It is plaintext private conversation state, so anyone with access to the
Mac user account and that directory can read it. It is never served as a public
asset or sent to a remote service.

On an ordinary follow-up:

- extraction and OCR are not run again;
- Qwen and Maple continue from native model state and receive only a small
  document-continuity note on the hot path;
- Bonsai and Ornith reuse the saved evidence as an identical early prompt
  prefix, allowing llama.cpp's prompt cache to reuse the prefix K/V state;
- the full sidecar remains available if native state must be rebuilt or the
  conversation must be compacted.

The sidecar is accepted only while every bound document is in full-context mode
and its recorded extractor fingerprint matches the installed reader. Detaching
any document invalidates the sidecar immediately. A missing, stale, or unreadable
sidecar never causes Samosa to pretend it still has the document; the gateway
falls back to the normal extraction/retrieval path or returns a clear context
error.

Oversized documents use retrieval mode instead. Their extracted/OCR text is
still cached, so a follow-up does not OCR the source again, but Samosa must run a
new local passage search for the new question. That query-dependent search is
expected work and should not be confused with rereading the original file.

## Text-backend behavior

| Backend | Hot continuation | Cold recovery | Full-document follow-up |
|---|---|---|---|
| Qwen native | Keeps one live transcript plus native K/V state and appends only the new user turn. | Restores the sealed `QWSESS01` checkpoint for that conversation. | Uses the document already represented in K/V. The sidecar is read only for recovery or compaction. |
| Maple native | Keeps one live transcript plus MLX K/V caches and appends only the new user turn. | Rebuilds K/V once from the complete browser transcript; recovery-only document evidence is appended once when required. | Uses the document already represented in the hot MLX caches. |
| Bonsai (llama.cpp) | Uses the server's single prompt-cache slot. The gateway sends `cache_prompt: true`. | Re-prefills the complete request prompt after a cache miss or backend restart. | Places the exact saved evidence in the first user message on every request, before later transcript messages, so the long prefix remains byte-stable and cacheable. |
| Ornith (llama.cpp) | Same single-slot prompt-prefix cache as Bonsai. | Same cold full-prompt prefill as Bonsai. | Same stable first-user evidence prefix as Bonsai. |

This is capability-based rather than model-name special casing: native engines
retain their own live K/V representation, while OpenAI-compatible llama.cpp
engines receive a stable cacheable prefix. All current text-model families are
covered.

Hidden reasoning is part of native continuation state even though it is not
copied into the visible browser transcript. This is why Maple and Qwen append to
their live native transcript instead of reconstructing a hot turn from the
browser's visible messages.

## One hot conversation, not one model copy per user

Samosa deliberately keeps one hot conversation per active text engine. It does
not create a model instance or K/V cache for every browser or LAN user. Model
generation is also serialized: one primary-model response runs at a time and
other admitted requests wait.

The one-hot policy bounds memory on a fanless MacBook Air. If Device A uses
conversation A and Device B then submits conversation B, B takes the model slot
and A's hot state is evicted. Returning to A is a cold restore or rebuild, after
which A becomes hot again. Multiple devices using the same conversation can
hit the same hot state, but their generations still run sequentially.

Selecting a different model is global. The old backend is stopped before the
new backend starts, so its in-memory cache disappears. Durable conversation and
document state remains local for a later switch back.

## Eviction and memory protection

All backends evict live state on backend shutdown. Native state is also dropped
when a stateless request or a different conversation needs the single slot, and
on failures where retaining a partially advanced cache would be unsafe.

Qwen additionally evicts its resident session when:

- it has been idle for 300 seconds by default;
- available memory falls below 768 MB by default;
- automatic or manual compaction is about to replace the saved context;
- runtime context settings change.

The Qwen thresholds can be changed before launch:

```sh
SAMOSA_HOT_SESSION_IDLE_SECONDS=300
SAMOSA_HOT_SESSION_MIN_AVAILABLE_MB=768
```

Set either value to `0` to disable that particular Qwen eviction trigger. The
reaper acts only while generation and its request queue are idle, so it cannot
free K/V state while a turn is using it.

Maple is bounded to one hot cache and evicts on conversation switch, stateless
request, disconnect, generation error, or shutdown. Bonsai and Ornith are
launched with one llama.cpp server slot (`-np 1`), so their cache is likewise
bounded by the active backend process.

## Durability, checkpoints, and the write tradeoff

Qwen still writes an atomic `QWSESS01` checkpoint after every successfully
completed turn. The file is sealed against model geometry and content, then
written through a temporary file, `fsync`, and rename. The important change is
that the engine no longer frees the live K/V state after writing it. A normal
next turn is therefore a memory hit, not a checkpoint reload.

The checkpoint provides:

- exact recovery after idle or memory-pressure eviction;
- exact recovery after model/app restart;
- safe switching between conversations without keeping several large K/V
  allocations resident;
- a stable source for manual and automatic compaction.

The remaining cost is a full synchronous checkpoint write at successful Qwen
turn boundaries. Incremental or asynchronous checkpointing is a possible
future optimization, but it must preserve atomic recovery and the rule that a
failed/cancelled turn cannot corrupt the last good state.

Maple does not currently write a native K/V checkpoint. Its cold path rebuilds
from the full browser transcript and the recovery-only document sidecar. The
llama.cpp backends also treat prompt K/V as an in-process cache and cold-prefill
from the request after a restart.

## Compaction

Automatic compaction is checked before a turn when projected context usage
crosses the configured threshold. Compaction necessarily replaces model-facing
context, so Samosa first evicts native hot state, creates a shorter durable
context, and then rebuilds from it. Streaming clients receive visible
`file_activity` events while this happens instead of appearing frozen.

For full-context document conversations, compaction reads
`document-context.txt` directly and preserves that exact evidence outside the
model-written summary. Retrieval-mode documents continue to use their normal
query-specific path.

## Health and diagnosis

Qwen's private backend `/healthz` response includes:

```json
{
  "resident_session": {
    "capacity": 1,
    "hot": true,
    "tokens": 2048,
    "idle_evict_seconds": 300,
    "min_available_mb": 768,
    "hits": 3,
    "restores": 1,
    "evictions": 0
  }
}
```

Maple reports the same one-slot idea with `available`, `hot`, `tokens`, `hits`,
and `evictions`. `available: false` means health sampled the backend during
inference and deliberately did not block waiting for the cache lock.

The browser's document activity wording distinguishes the three meaningful
paths:

- **Using the already extracted text**: full-context fast path;
- **Searching the previously extracted text**: retrieval over cached text;
- **Reading with Samosa's local file reader**: first extraction or a required
  rebuild.

## Implementation and regression coverage

The shared document policy and payload construction live in
`src/samosa_gateway.c`. Qwen resident sessions and durable checkpoints live in
`src/qwen36b.c`. Maple's native hot session lives in
`src/maple/samosa_maple.cpp`.

Synthetic tests cover Qwen K/V growth and eviction, Qwen compaction activity,
resident health metadata, saved document-context creation/invalidation, absence
of full evidence on a native Qwen hot follow-up, and a byte-stable cached
document prefix across two Ornith-style llama.cpp requests. These tests use
generated fixture text and never inspect a user's chat directory.
