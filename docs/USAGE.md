# Using Samosa Chat

The terminal, the web app, and thinking modes in full. The
[README](../README.md#two-ways-to-use-it) has the short version.

## Chat in your terminal

This is the main way to use Samosa. Ask a question, get an answer:

```sh
samosa "explain how a hash table handles collisions"
```

Keep the conversation going with `--continue`. Your chat resumes from a saved
snapshot, so a follow-up does not re-read the whole history:

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"
samosa --continue "show me the CPython source for that"
```

The rest of the options:

```sh
samosa --think "solve this logic puzzle"                # general reasoning
samosa --think-code "build a responsive settings page"  # precise coding profile
samosa --fast "summarize this design"                   # adaptive threads, runs warmer
samosa --seed 11 "give me a deterministic sample"       # reproducible sampling
samosa --max-tokens 2048 "write a long explanation"     # change the ceiling
samosa --thinking-budget 512 "..."                      # cap internal reasoning
samosa --context-tokens 131072 "..."                    # larger total context, if memory permits
samosa doctor                                           # check the installation
```

An answer can run up to 8,192 new tokens. That is an outer ceiling, not a target
— the model usually stops earlier on its own when it emits its end-of-turn
token. Two threads is the default so the Mac stays cool; `--fast` enables
adaptive thermal thread scaling — it uses more cores when the machine
has thermal headroom, and backs off when it gets hot.

By default the model answers directly. `--think` and `--think-code` turn on
reasoning, which is slower and consumes more battery/power because it does
many more SSD read passes. Use direct mode unless you need deep thinking
to get results faster and keep the machine cooler. See
[SSD speed](#ssd-speed-the-one-thing-to-be-deliberate-about) for details.

**Context capacity.** Saved history, your new message, thinking, and the answer
share one total-context budget. Samosa reads the model's native maximum from
the checkpoint (262,144 tokens for the shipped Qwen model), then selects a
stable hardware-aware default: 24,576 on a 16 GB-class machine, 65,536 on
32 GB-class hardware, and 131,072 on 64 GB-class hardware. It also checks
currently available memory before allocating KV state.

Set `SAMOSA_CONTEXT_TOKENS=auto` for the default policy, or explicitly choose
a safe value up to the model maximum with `SAMOSA_CONTEXT_TOKENS=131072` or
`--context-tokens 131072`. An explicit value is not a promise that a giant
one-shot paste will be fast: the current prefill path still has temporary
memory and time costs that grow with the prompt.

## The web app (a demo)

`samosa app` starts a local server and opens a chat page in your browser.
Everything runs on your machine. The page makes no outside requests.

```sh
samosa serve          # start the server in the foreground on 127.0.0.1:8642
samosa app            # start the server in the background and open the chat page
samosa serve --stop   # stop the server
```

What the app does:

- Streams the answer as the model writes it.
- Shows the model's thinking separately from its final answer.
- Lets you stop a generation at any time.
- Saves your conversations so you can continue them later.
- Shows live speed (tokens per second) and current memory use.
- Has settings for thinking mode, maximum answer length, and a fixed seed.
- Lets you choose automatic, preset, or custom total-context capacity; the
  choice is stored locally in the browser and applied to the local server.
- Offers **Compact this conversation now** and configurable automatic
  compaction, enabled by default at 80% projected context use. The conversation
  and visible browser history stay in place.

The server answers these HTTP endpoints:

- `GET /healthz` — status, memory use, the context limit, queue state, last speed
- `GET /v1/models`
- `POST /v1/chat/completions` — reply as JSON, or stream token by token (SSE)
- `POST /v1/settings` — apply context and automatic-compaction policy
- `POST /v1/compact` — compact a saved conversation under the same ID
- `POST /v1/cancel` — stop the current generation
- `POST /v1/shutdown` — stop the server cleanly

Only one request runs at a time. Extra requests wait in a short queue.

**Stopping an answer is safe.** When you stop an answer partway through, Samosa
saves the conversation only up to the last complete sentence. This matters:
before this fix, if you stopped an answer in the middle of a sentence, the next
answer in that chat would copy the cut-off style and reply with only a word or
two before stopping. That is now fixed. If a stopped answer has no complete
sentence yet, Samosa keeps the previous saved state instead of overwriting it.

**Context capacity.** The server uses the same total-context setting as the
terminal. `GET /healthz` reports the model maximum, effective limit, mode, and
KV bytes per token. The server checks a turn before queueing it and again after
admission; it rejects an oversized or currently unsafe request before allocating
KV. Only the conversation you are using is loaded into RAM, so opening other
saved chats does not add resident KV.

**Compaction.** Samosa resumes the current sealed session and asks Qwen for a
structured continuation memory, retains recent complete turns verbatim, frees
the old inference state, and builds a fresh smaller K/V snapshot. The old
snapshot is not replaced until the compacted one is completely sealed and
fsynced. Automatic compaction checks projected use—history, the incoming turn,
and its answer ceiling—so it runs before the hard limit. Manual and automatic
compaction keep the same conversation ID; browser messages are not deleted.

## Thinking modes

Samosa uses Qwen's published sampling settings:

| mode | temperature | top-p | top-k | presence penalty | thinking budget |
|---|---:|---:|---:|---:|---:|
| direct | 0.7 | 0.80 | 20 | 1.5 | off |
| general thinking | 1.0 | 0.95 | 20 | 1.5 | 1,024 tokens |
| precise code | 0.6 | 0.95 | 20 | 0.0 | 2,048 tokens |

The maximum answer length is an outer limit, up to 8,192 new tokens. It is not a
fixed length. The model decides when to stop within that limit. If the thinking
reaches its budget, Samosa adds Qwen's trained wind-down text before closing the
`</think>` block, rather than cutting it off with a bare token. Closing the
thinking block keeps the output well-formed; it does not prove the answer is
correct.

One test compared this against an upstream FP8 reference on a small set of
arithmetic problems. The reference used 353–616 thinking tokens. A matching
local group-32 run answered correctly and stopped on its own after 933 tokens
with a 1,024-token thinking budget. This confirms the path works for that one
kind of problem. It is not proof of broad benchmark quality. See the
[upstream-control report](UPSTREAM_CONTROL_2026-07-14.md) and the
[regression ledger](REGRESSION_LEDGER.md).
