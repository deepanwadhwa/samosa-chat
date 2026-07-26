# Real-model verification: prefill fix + model-assisted intent decode (2026-07-20)

Two things that were previously either broken or only mock-tested, verified
against the real Qwen3.6-35B-A3B backend on the reference machine (16 GB M3
MacBook Air) — not simulated.

## Setup

- `qwen36b` rebuilt from source after the `tok_encode` fix
  ([src/qwen36b.c](../../../src/qwen36b.c), `samosa_serve_prefill`).
- Real gateway (`tools/samosa_gateway.py`) launched with
  `SAMOSA_QWEN_MODEL=~/Documents/samosa-models/qwen36_group32_i8`,
  `SAMOSA_TOKENIZER=~/Documents/samosa-models/tokenizer_qwen36.json` — the
  actual 24 GB model, real tokenizer, `SAMOSA_HOME`/`SAMOSA_JOBS_DIR` pointed
  at a scratch directory (deleted after).
- Memory checked before/after: 3.3 GB free before, 0.25 MB swap used
  throughout (never grew), free pages fully recovered after shutdown. No
  visible pressure on the reference machine for this workload.

## 1) `/v1/chat/prefill` — the `tok_encode` fix, against the real engine

```
$ curl -X POST http://127.0.0.1:8798/v1/chat/prefill -d '{"messages":[
    {"role":"user","content":"organize my downloads folder by file type"}]}'
{"object":"chat.prefill","status":"cached_prefill_ready","prompt_tokens":8,"prefill_kv_size":8}
```

`prompt_tokens: 8` for an 8-word prompt — the fixed `samosa_count_tokens()`
helper is calling the real tokenizer correctly (previously this endpoint could
not compile at all; see commit `c1df960`).

## 2) Model-assisted `decode_intent` — a genuinely ambiguous goal, real backend

`tools/samosa_jobs.py`'s `decode_intent()` only calls the model when a goal
matches neither the `organize` nor `report` keyword patterns. Used a goal
engineered to hit neither: "please handle these downloads for me".

```
POST /v1/jobs/run {"goal":"please handle these downloads for me",
                    "folder":"<scratch>/inbox", "mode":"confirm"}

seq1 01:58:30Z  decode_intent
seq2 01:58:44Z  intent   kind=organize  rule={"by":"extension"}
seq3 01:58:44Z  counting total=2 by_type={text/plain:1, application/pdf:1}
seq4 01:58:44Z  plan     2 moves (a.txt -> TXT/, b.pdf -> PDF/)
seq5 01:58:44Z  await_apply  moves=2
```

~14 seconds between `decode_intent` and `intent` — the real inference round
trip (`Handler.jobs_model_call` → `backend_chat` → real Qwen server →
64-token classification reply → `decode_intent` maps it to `organize`).
Consistent with the reference machine's documented ~5–7 tok/s decode plus a
cold-start disk read for the classification call.

## Scope

- This is one real run, not a benchmark — it confirms the wiring works
  end-to-end against the real model, not the model's judgment quality across
  many phrasings.
- Bonsai/Ornith backends were not exercised here (models are installed
  locally per `~/.samosa/models/`, but this check used the Qwen path only,
  since that's what `jobs_model_call` uses regardless of active backend).
- Confirm-mode stopped at `await_apply`; the actual file move was not applied
  in this run (already covered by the deterministic apply/undo tests and the
  separate app E2E in `app-e2e-organize-2026-07-20.md`, which does execute a
  real move+undo — just not through real inference).
