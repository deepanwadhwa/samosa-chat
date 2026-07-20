# Context-window task program — scale to the machine, not the reference Mac

Read [ISSUE_TASKS.md](ISSUE_TASKS.md) first, including the Working agreement.
Per that agreement this card belongs on `main`; implementation and evidence
belong on a branch cut from `main`.

This program answers a requirement the project owner stated on 2026-07-19:

> *"It shouldn't be limited or hardcoded. The model is running on my machine
> and I should be able to use as much context as I want given that it doesn't
> kill my machine. What if someone with 64 GB unified memory is using Samosa?"*

The owner is right about the defect, with one necessary boundary: Samosa cannot
offer literally unlimited context. The installed Qwen3.6 checkpoint declares
`max_position_embeddings: 262144`. Samosa may expose any safe, tested window up
to that model limit; it must not imply that more RAM extends the model beyond
the context it was trained to use.

## Implementation update — 2026-07-19

The original 24,576-token hard cap described below has been replaced for Qwen:
the model-declared 262,144 limit, runtime Auto policy, explicit app/CLI
configuration, admission checks, health metadata, and durable compaction are
implemented. C5/C6 remain the longer qualification program for enormous
one-shot prompts and broader hardware evidence.

The gateway extension is also complete for Bonsai and Ornith. Auto now leaves
Prism's context argument unset, retains a 4 GiB device margin, bounds prompt
batches, and reads the fitted `n_ctx` back from `/props`. On the measured
16 GiB M3, real generation passed at 66,816 tokens for Bonsai and 94,464 for
Ornith. See
[the regression record](regressions/model-downloads/2026-07-19.md).

The remainder of this file preserves the original task rationale and acceptance
criteria; line references and statements about the former cap are historical
ground truth, not the current product behavior.

## Verified ground truth (2026-07-19)

| Fact | Evidence |
|---|---|
| **24,576 is a Samosa product limit, not a Qwen limit.** It is the compile-time literal `SAMOSA_MAX_CONTEXT_TOKENS`. | [qwen36b.c:3564](../src/qwen36b.c#L3564) |
| The limit is enforced for new prompts, resumed sessions, saved-session header validation, HTTP preflight, and `/healthz`. It is therefore not safely changed by editing one comparison. | [qwen36b.c:3583-3601](../src/qwen36b.c#L3583-L3601), [:3732](../src/qwen36b.c#L3732), [:4314-4367](../src/qwen36b.c#L4314-L4367), [:4718-4737](../src/qwen36b.c#L4718-L4737), [:4907-4911](../src/qwen36b.c#L4907-L4911) |
| The active converted checkpoint declares a **262,144-token** native window under `text_config.max_position_embeddings`. The engine does not currently carry that field in `Cfg`; it parses RoPE parameters but not the model's position limit. | `~/Documents/samosa-models/qwen36_group32_i8/config.json`, verified locally; [qwen36b.c:110-134](../src/qwen36b.c#L110-L134), [:988-1073](../src/qwen36b.c#L988-L1073) |
| The ten full-attention GQA layers retain roughly **40 KiB/token** in f32 KV. The present cap therefore bounds variable KV to roughly **960 MiB**. Only the active conversation is restored, so saved chats do not multiply resident KV. | [APP_TASKS.md:20-28](APP_TASKS.md#L20-L28), [SERVE_API.md:42-47](SERVE_API.md#L42-L47) |
| Approximate KV alone is 2.5 GiB at 65,536 tokens, 5 GiB at 131,072, and 10 GiB at 262,144. These are arithmetic estimates, not measured whole-process footprints. MTP and temporary buffers must be counted separately. | 40 KiB/token × token count |
| A fresh prompt is processed as one `S=np` batch. `x_prompt` and several layer temporaries scale with all prompt tokens at once; full-attention work also grows with context. A conversation accumulated through small resumed turns does not have the same temporary-memory peak as one enormous initial paste. | [qwen36b.c:2392-2518](../src/qwen36b.c#L2392-L2518), [:3419-3450](../src/qwen36b.c#L3419-L3450) |
| The separate completion ceiling is also hardcoded: the server accepts at most **8,192 generated tokens**. This must not be confused with the total context setting. | [qwen36b.c:4795-4799](../src/qwen36b.c#L4795-L4799) |
| The 24,576 cap was deliberately added to preserve the zero-new-swap guardrail on the measured 16 GB M3 reference machine. It was a conservative release bound, not a discovered model/runtime maximum. | [WORK_LOG_2026-07-14.md:349-365](WORK_LOG_2026-07-14.md#L349-L365) |

## Product contract

Samosa must have three distinct values and name them precisely:

1. **Model context limit** — read from the checkpoint; 262,144 for the current
   Qwen model. This is an absolute upper bound.
2. **Configured context limit** — an explicit user choice, or `auto`.
3. **Effective context limit** — the value actually enforced after validating
   the configured choice against the model and runtime.

The default remains safe on the measured 16 GB machine. Larger machines are
allowed to use larger windows without rebuilding Samosa. An informed user may
override the automatic policy up to the model limit. Samosa estimates the cost,
warns clearly, and rejects an allocation that is not safe; it must not silently
truncate history, silently slide the window, or let `falloc()` kill the server.

The API and UI must say **total context**, not “input length”: saved history,
the new turn, thinking tokens, and output all share the window.

## Dependency order

```text
C0 measurements and exact memory accounting
  └── C1 model-declared limit
       └── C2 runtime configuration and auto policy
            ├── C3 allocation preflight and failure safety
            ├── C4 sessions, API, health, and UI
            └── C5 chunked prefill for very large first prompts
                 └── C6 long-context qualification and defaults
```

Do not advertise a 262K usable window merely because C1 accepts the number.
C6 owns that claim, after both accumulated-session and one-shot-prefill paths
have passed.

---

## C0 — Measure the real envelope and make memory accounting exact  ~1–2 days

**Status: open. Run first.**

Add a single checked function that derives KV bytes from the loaded geometry:
number of full-attention layers, KV heads, head dimension, element width,
requested capacity, and any enabled MTP cache. Do not preserve “40 KiB/token”
as a hidden constant; it is documentation shorthand for the current model.
Use checked `size_t` arithmetic and reject overflow before allocation.

Measure two different paths at 24K, 64K, 128K, and 262K where the available
hardware permits:

- **W-ACCUMULATED:** build a session through bounded continuation chunks, then
  resume it for a short answer. Records steady KV, restore time, snapshot size,
  decode speed, memory pressure, and swap delta.
- **W-ONE-SHOT:** submit the same total prompt as a fresh conversation. Records
  peak temporary memory, prefill time, time to first token, and whether the
  current all-at-once prefill survives.

Record total physical memory, available memory at admission, current process
footprint, peak footprint, pressure state, swap before/after, token counts,
thread count, model fingerprint, and exact command. A skipped 262K run on a
smaller machine is honest evidence; an extrapolation is not a passing result.

**Acceptance:** computed KV bytes agree with observed KV allocations; the report
separates KV, fixed recurrent state, expert cache/model residency, temporary
prefill memory, and session-file size; accumulated and one-shot results are
never combined into one “context works” claim.

---

## C1 — Read and enforce the model's declared limit  ~0.5–1 day

Add `max_position_embeddings` to `Cfg` and parse it from the same effective text
configuration used for the other Qwen text fields. Validate it as a positive
integer during model load. Decide and document a conservative compatibility
fallback for old converted snapshots that lack the field; never treat a missing
field as unlimited.

Replace the compile-time constant in context validation and saved-session
validation with an effective runtime value carried by the model/server context.
Error messages must print the actual value. `/healthz` must expose both:

```json
{
  "model_context_limit_tokens": 262144,
  "context_limit_tokens": 24576
}
```

**Acceptance:** the current checkpoint reports 262,144 as its model limit;
requests above it fail before allocation; a fixture with a different declared
limit proves this is parsed rather than renamed hardcoding; corrupt, missing,
zero, negative, and overflowing values have deterministic tests.

---

## C2 — Add a hardware-aware default and an explicit override  ~1–2 days

Support one stable public setting across CLI and serve mode:

```sh
SAMOSA_CONTEXT_TOKENS=auto
SAMOSA_CONTEXT_TOKENS=131072
samosa --context-tokens 131072 "..."
qwen36b --serve --context-tokens 131072
```

Precedence is CLI flag → environment → `auto`. The installed service must pass
the setting through rather than requiring a source edit or rebuild. Invalid
values fail startup with a plain error. Explicit values above the model limit
fail; they are not silently clamped.

`auto` must be based on a stable host-capability profile and C0 measurements,
not on whatever free-memory sample happens to exist during startup. Preserve
24,576 on the measured 16 GB reference machine until evidence supports a
different safe default. Derive and test larger-memory tiers; do not guess that
“64 GB” automatically means 262K, because prefill temporaries, page cache,
other applications, and swap headroom still matter.

An explicit override is the expert escape hatch. Print its estimated KV cost
and retain a safety reserve for the rest of the process and OS. If the owner
later requests a force-through-pressure mode, scope that separately; this task
does not add an “ignore OOM” flag.

**Acceptance:** changing context capacity requires no rebuild; precedence and
all invalid cases are tested; 16 GB `auto` preserves the existing safe behavior;
each larger auto tier cites a measured report; startup logs state model,
configured, effective, and estimated-KV values.

---

## C3 — Reject unsafe work without killing the resident server  ~1–2 days

Extend request preflight to distinguish:

- `context_limit`: token total exceeds the effective configured limit;
- `model_context_limit`: requested configuration exceeds the checkpoint;
- `insufficient_memory`: the projected allocation does not fit the current
  safety budget even though its token count is otherwise valid;
- `invalid_session`: saved state is corrupt or incompatible.

Before allocating, estimate the incremental KV and known temporary buffers for
the actual path. Recheck after queue admission, as the current code correctly
does for same-conversation races. Allocation failure in a request must unwind
the request and leave `/healthz` responsive; it must not call a process-wide
`exit(1)`.

Do not weaken the existing atomic session and SHA-256 validation behavior.
Check free disk space before writing multi-gigabyte snapshots and retain the
previous valid session if a save cannot complete.

**Acceptance:** fault-injection tests cover every large allocation and session
write; no tested failure terminates serve mode, sends partial success headers,
or destroys the previous session; integer-overflow and queue-race tests pass.

---

## C4 — Make capacity visible and controllable  ~1 day

Expose model/configured/effective limits, estimated KV at the effective limit,
current conversation tokens, and remaining tokens through health/API metadata.
The web settings UI must offer `Auto` plus a validated custom total-context
value, explain that output and thinking consume the same budget, and show an
estimated memory cost before applying a custom value.

Starting a new conversation must not reset the configured capacity. Switching
models must recompute it against the new model limit. Opening an existing chat
whose saved length exceeds the current effective limit must produce recovery
guidance: raise the setting or start a new chat. Never silently discard the
old chat.

Keep the 8,192 completion ceiling unchanged in this phase. If it becomes
configurable, implement and test it as a separate subtask so context capacity
and answer-length policy remain independently understandable.

**Acceptance:** UI, CLI, API errors, `/healthz`, README, and
[SERVE_API.md](SERVE_API.md) use the same terms and values; accessibility labels
and keyboard behavior pass; old browser state and saved conversations migrate
without data loss.

---

## C5 — Bound temporary memory with chunked prefill  ~2–4 days

The current full-prompt batch can make “262K supported” technically accepted
but practically unusable. Add bounded chunked prefill so temporary activation
memory depends on a configured chunk size rather than the entire fresh prompt.
Chunks must advance GQA KV, DeltaNet recurrent/conv state, RoPE/M-RoPE
positions, vision positions, expert routing, and cancellation correctly.

Run numerical parity against the current one-shot path at contexts where both
fit. Define the parity criterion before implementation: byte identity if the
operation order is unchanged, otherwise token identity plus a bounded logit
error justified by the changed batching. Measure chunk sizes for throughput,
peak memory, and time to first token; do not select one by intuition.

Cancellation and client disconnect must be observed between chunks. Progress
telemetry may report prefill tokens processed, but must not expose partial model
output before generation begins.

**Acceptance:** a large fresh prompt no longer allocates activation buffers
proportional to its full length; accumulated and chunked-one-shot transcripts
pass the declared parity gate; text, vision, resumed session, cancellation, and
thinking modes have regression coverage; the chosen default chunk cites data.

---

## C6 — Qualify long context and choose what Samosa advertises  ~1–2 days

Run the full matrix on every hardware tier available:

| Context | Accumulated resume | One-shot text | Vision + text | Short decode | Snapshot restore |
|---:|---|---|---|---|---|
| 24,576 | required | required | required | required | required |
| 65,536 | required | required | required | required | required |
| 131,072 | where auto/override permits | where auto/override permits | required once | required | required |
| 262,144 | explicit override; hardware permitting | explicit override; hardware permitting | exploratory | required if prefill passes | required |

Check model quality as context grows, not only absence of crashes: retrieval
from early/middle/late positions, instruction retention, continuation parity,
and a small perplexity/logit comparison against upstream Qwen at matched
positions. Record prefill duration honestly—a window that takes hours to ingest
may be valid but must not be marketed as effortless.

**Acceptance:** Samosa advertises only the largest context that passed on named
hardware and workload paths; `auto` tables match those results; explicit
override remains available up to the model limit with honest warnings; the
16 GB regression shows no new swap or thermal-policy regression at its default.

## Definition of done

- No unconditional `24576` compile-time ceiling remains in runtime logic.
- The model limit comes from checkpoint metadata and is never exceeded.
- Context capacity is configurable without recompilation and visible everywhere.
- Defaults are hardware-aware, stable, measured, and safe.
- A 64 GB user can select and use a materially larger context than a 16 GB user.
- Large-request admission cannot kill the resident server or corrupt a session.
- Very large fresh prompts use bounded prefill memory before Samosa claims they
  are supported.
- Tests cover token arithmetic, memory arithmetic, configuration precedence,
  session compatibility, API behavior, allocation failure, and queue races.
- Documentation distinguishes model limit, effective total context, completion
  limit, KV estimate, one-shot prefill cost, and accumulated-session cost.

## Risks that must remain visible

- More context consumes RAM, disk space, restore time, attention time, and
  prefill time even when the model weights fit comfortably.
- Unified memory is shared with the OS and GPU; installed RAM is not an
  allocation budget.
- Available-memory checks are race-prone. They supplement a stable configured
  limit; they do not replace it.
- A giant saved session can fill the user's disk even when it fits in RAM.
- Native 262K metadata is a model boundary, not proof that Samosa's converted
  engine preserves upstream quality at every position. C6 must measure it.
- Sliding-window eviction or context summarization would change conversation
  semantics. Neither is part of this task and neither may be introduced
  silently as a memory fix.
