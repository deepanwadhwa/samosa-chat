# Using Samosa

## Model management

```sh
samosa models
samosa pull qwen
samosa pull bonsai
samosa pull ornith
samosa pull all
```

`models` distinguishes missing weights, weights without a GGUF runtime, and a
fully installed model. Pulls are resumable and verified.

## App

```sh
samosa app
```

The gateway starts even when no model exists. In Settings → Model, each card
shows the model's size, license, and local state:

- **Download** installs missing weights and any required runtime.
- **Use** unloads the current model and starts the selected one.
- **Active** marks the current backend.

The first completed download activates automatically when no backend is
running. Downloading a second model does not interrupt a running chat.

Only Qwen supports image attachment in the current app. Bonsai and Ornith are
text models.

## Context capacity

Settings → Total context capacity controls history + new prompt + thinking +
generated answer.

**Auto** is recommended:

- Qwen calculates capacity from machine memory and the K/V bytes per token.
- Bonsai and Ornith let Prism fit current device memory. The app displays the
  actual context returned by `/props`, not a static default.

An explicit integer up to 262,144 overrides Auto. Changing GGUF capacity
restarts that backend. A high manual setting can create memory pressure even
when the model initializes, so use it deliberately.

## Compaction

Automatic compaction is enabled at 80% projected use by default. The app offers
70%, 75%, 80%, 85%, and 90% presets and a manual **Compact this conversation
now** button.

Compaction keeps:

- dense model-written continuation memory for older turns;
- recent turns verbatim;
- the same browser chat and conversation ID.

It replaces only the durable model-facing context. The next request rebuilds
K/V state from the compacted memory. If the summary is not smaller, Samosa
keeps the old ledger.

## Thinking modes

- **Direct**: shortest path to the answer.
- **General thinking**: permits additional internal reasoning.
- **Precise code / WebDev**: uses the code-oriented thinking control.

The exact behavior depends on the active model and its chat template.

## Terminal chat

Direct terminal prompts currently use Qwen:

```sh
samosa "explain how DNS works"
samosa --continue "now explain DNSSEC"
samosa --think "solve this logic problem"
samosa --think-code "write a parser"
samosa --fast "use the warmer, faster thread profile"
samosa --seed 11 "make this sampling path reproducible"
samosa --max-tokens 4096 "allow a longer answer"
samosa --context-tokens 65536 "use this explicit total capacity"
```

Install Qwen first with `samosa pull qwen`. Use the app or HTTP gateway for
Bonsai and Ornith conversations.

`--continue` resumes `~/.samosa/last_session.qws`. App conversations instead
use IDs under `~/.samosa/chats`.

## Server commands

```sh
samosa serve
samosa serve --context-tokens auto
samosa serve --context-tokens 65536
samosa serve --stop
```

The gateway listens only on `127.0.0.1:8642` unless source code or environment
settings are deliberately changed.

## Settings persistence

- model selection: `~/.samosa/model-backend`
- gateway context/compaction: `~/.samosa/gateway-settings.json`
- Qwen settings: `~/.samosa/config.json`
- GGUF durable conversations: `~/.samosa/chats/CHAT_ID/MODEL.json`
- app transcript and display preferences: browser local storage

## Diagnostics

```sh
samosa doctor
samosa models
curl -s http://127.0.0.1:8642/healthz | python3 -m json.tool
```

`healthz` reports the active model, readiness, actual context capacity,
generation state, and compaction settings.
