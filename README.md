<div align="center">
  <img src="assets/samosa-chat_medium.png" alt="Samosa Chat mascot" width="210">
  <h1>Samosa Chat</h1>
  <p><strong>Three local models. One private chat app.</strong></p>
  <p>Qwen3.6 35B A3B &nbsp;·&nbsp; Bonsai 27B 1-bit &nbsp;·&nbsp; Ornith 1.0 9B</p>
  <p>No cloud account &nbsp;·&nbsp; No telemetry &nbsp;·&nbsp; Hardware-aware context &nbsp;·&nbsp; Durable compaction</p>

  <p>
    <a href="https://github.com/deepanwadhwa/samosa-chat/actions/workflows/ci.yml"><img src="https://github.com/deepanwadhwa/samosa-chat/actions/workflows/ci.yml/badge.svg" alt="CI: build and tests"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue" alt="License: Apache-2.0"></a>
    <img src="https://img.shields.io/badge/context-up%20to%20262K-orange" alt="Context up to 262K">
  </p>
</div>

Samosa is a local model app and gateway. Start it with no model installed,
choose a model in Settings, and Samosa downloads, verifies, loads, and runs it
on your machine. The terminal offers the same model-management workflow.

## Models

| Model | Download | Best fit | Runtime | License |
|---|---:|---|---|---|
| [Qwen3.6 35B A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) | 24.0 GB | Samosa's fullest integration, image input, expert-streamed local chat | Samosa C engine | Apache-2.0 |
| [Bonsai 27B 1-bit](https://huggingface.co/prism-ml/Bonsai-27B-gguf) | 3.8 GB | A very compact 27B reasoning model | Prism llama.cpp | Apache-2.0 |
| [Ornith 1.0 9B](https://huggingface.co/deepreinforce-ai/Ornith-1.0-9B-GGUF) | 5.6 GB | Fast compact coding and reasoning | Prism llama.cpp | MIT |

Only one model is resident at a time. Switching models unloads the current
backend before starting the next one.

> **Credit.** The Qwen runtime began from
> [colibrì](https://github.com/JustVugg/colibri) by JustVugg and runs the model
> released by the [Qwen team](https://huggingface.co/Qwen). Bonsai is released
> by [PrismML](https://huggingface.co/prism-ml), and Ornith by
> [DeepReinforce](https://huggingface.co/deepreinforce-ai). The GGUF backends
> use PrismML's [llama.cpp fork](https://github.com/PrismML-Eng/llama.cpp),
> itself based on [llama.cpp](https://github.com/ggml-org/llama.cpp). Samosa is
> an independent project and is not affiliated with those teams.

## Install from a clone

The source install intentionally includes no model. On macOS, install OpenMP
first:

```sh
brew install libomp
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

The app opens even on a clean machine. Open **Settings → Model**, then click
**Download** beside Qwen, Bonsai, or Ornith. Progress stays in the app; a
completed first download is activated automatically.

On Debian/Ubuntu, install a C compiler and OpenMP before the same clone/build
flow:

```sh
sudo apt-get install build-essential libomp-dev
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

The development install and three-model runtime have been exercised on a
16 GB Apple M3. CI builds and tests macOS and Linux, but performance and
long-running behavior are not claimed for machines that have not been measured.
The older Qwen-only Docker workflow remains documented in
[docs/INSTALL.md](docs/INSTALL.md).

Everything lives under `~/.samosa`. The launcher is
`~/.samosa/bin/samosa`; add that directory to `PATH` if desired.

## Download models from the terminal

```sh
samosa models             # installed / missing status for all three
samosa pull bonsai
samosa pull ornith
samosa pull qwen
samosa pull all
```

Downloads:

- are pinned to an immutable Hugging Face revision;
- resume from a `.partial` file after interruption;
- check the exact byte count and SHA-256 before installation;
- become visible to the gateway only after verification;
- preflight free disk space and retain a 2 GB reserve;
- install the pinned Prism runtime automatically for Bonsai and Ornith.

The Prism runtime is a checksum-pinned prebuilt release for macOS arm64/x64 and
Linux x64/arm64. Samosa does not silently use an arbitrary executable from the
network.

Approximate storage:

| What you install | Space |
|---|---:|
| Bonsai only | 3.8 GB plus ~12–17 MB runtime archive |
| Ornith only | 5.6 GB plus ~12–17 MB runtime archive |
| Qwen only | 24.0 GB |
| All three | 33.4 GB plus runtime |

## Use the app

```sh
samosa app
```

The app runs at <http://127.0.0.1:8642>. In Settings you can:

- install and switch among all three models;
- leave total context on **Auto (recommended)** or set an explicit capacity;
- enable automatic compaction and select its threshold;
- compact the current conversation immediately;
- control thinking, output length, seed, and optional Internet sources.

The browser keeps the visible transcript. Samosa keeps model-facing
continuation state under `~/.samosa/chats`, so the same chat can continue after
a restart or compaction.

## Use the terminal

Direct terminal chat currently uses the Qwen backend:

```sh
samosa pull qwen
samosa "explain how DNS works"
samosa --continue "and where does DNSSEC fit?"
samosa --think "solve this logic puzzle"
samosa --think-code "review this algorithm"
samosa --context-tokens 65536 "remember this"
samosa doctor
```

Use the app or the local HTTP API for Bonsai and Ornith chat. See
[docs/USAGE.md](docs/USAGE.md) for every flag.

## Hardware-aware context

All three models support up to 262,144 tokens, but **Auto is not a fixed
24K/8K setting**.

- **Qwen:** the C engine measures available memory, model-resident memory, and
  per-token K/V cost, then selects a safe capacity. An explicit app or CLI
  setting overrides Auto up to the model limit.
- **Bonsai and Ornith:** Samosa leaves context unset and lets Prism fit the
  model and K/V allocation to current device memory. The gateway reads the
  resulting `n_ctx` back from the running server and uses that exact number for
  status, request budgeting, and compaction. Samosa reserves a 4 GiB device
  margin and bounds prompt batches to avoid turning theoretical context into a
  machine-killing allocation.

On the measured 16 GB M3, the real runtime selected **66,816 tokens for
Bonsai** and **94,464 for Ornith** and generated successfully with each. These
are observations, not hardcoded tiers: another machine or a different amount
of free memory can receive a different capacity.

Manual values are advanced controls. A capacity that technically initializes
can still cause heavy memory pressure when filled, so Auto is the default.

## Conversation compaction

Automatic compaction defaults to 80% of projected context use and can be set
from 50–90% through the API (70–90% presets in the app).

When compaction runs:

1. Samosa loads the durable model-facing conversation.
2. The active model summarizes older turns into dense continuation memory.
3. Recent turns remain verbatim.
4. Samosa verifies that the replacement is smaller.
5. It atomically replaces the durable ledger while keeping the same chat ID and
   visible browser transcript.
6. The next request rebuilds the backend's K/V state from that compacted
   context.

Qwen uses its sealed conversation snapshot; Bonsai and Ornith use per-model
durable ledgers and reconstruct their llama.cpp prompt. A failed or non-shrinking
summary does not replace the previous ledger.

## Local API

```sh
samosa serve
```

Useful gateway endpoints:

| Endpoint | Purpose |
|---|---|
| `GET /healthz` | active model, readiness, actual context, compaction status |
| `GET /v1/backends` | three-model catalog plus install/download state |
| `POST /v1/backends/install` | start a verified model download |
| `POST /v1/backends/select` | unload and switch active model |
| `POST /v1/settings` | context and compaction settings |
| `POST /v1/compact` | compact one durable conversation |
| `POST /v1/chat/completions` | OpenAI-compatible streaming chat |

Full request and response examples are in
[docs/SERVE_API.md](docs/SERVE_API.md).

## Build and test

```sh
make              # portable Qwen engine
make omp          # multithreaded engine
make test         # self-contained unit and integration suite
```

The normal suite uses tiny fixtures and does not download model weights. Real
model evidence—including durable compaction—is kept under
[docs/regressions](docs/regressions).

## Privacy and network behavior

- Inference, chat history, and model switching stay on this computer.
- The gateway binds to `127.0.0.1`.
- There is no telemetry or model account.
- Installing a model contacts only its pinned Hugging Face source and, for
  GGUF models, the pinned Prism GitHub release.
- Internet-source tools are opt-in and clearly separate from model downloads.

## Known limits

- The three-model app has measured real-model evidence on one 16 GB M3, not a
  broad hardware benchmark.
- Direct terminal chat is Qwen-only; model management works for all three.
- Qwen expert streaming is SSD-sensitive. Bonsai and Ornith use Metal on Apple
  Silicon through Prism and have different performance characteristics.
- Deleting a browser chat does not yet remove every durable backend ledger.
- The published Hugging Face release remains a Qwen-oriented distribution;
  the fresh three-model workflow documented here is the source-clone
  `make install` path until a new release bundle is published.

## Documentation

| Topic | Document |
|---|---|
| installation and model storage | [docs/INSTALL.md](docs/INSTALL.md) |
| app and terminal usage | [docs/USAGE.md](docs/USAGE.md) |
| model behavior and network boundaries | [docs/MODELS_AND_INTERNET.md](docs/MODELS_AND_INTERNET.md) |
| local gateway API | [docs/SERVE_API.md](docs/SERVE_API.md) |
| architecture | [docs/DESIGN.md](docs/DESIGN.md) |
| measured performance | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| regression evidence | [docs/regressions](docs/regressions) |
