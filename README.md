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

Samosa is a local model application and HTTP gateway. Models can be downloaded, verified, and loaded directly through the app interface or terminal commands.

## Models

| Model | Download | Description | Runtime | License |
|---|---:|---|---|---|
| [Qwen3.6 35B A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) | 24.0 GB | Expert-streamed local chat with image support | Samosa C engine | Apache-2.0 |
| [Bonsai 27B 1-bit](https://huggingface.co/prism-ml/Bonsai-27B-gguf) | 3.8 GB | 1-bit GGUF reasoning model | Prism llama.cpp | Apache-2.0 |
| [Ornith 1.0 9B](https://huggingface.co/deepreinforce-ai/Ornith-1.0-9B-GGUF) | 5.6 GB | Q4_K_M GGUF coding and reasoning model | Prism llama.cpp | MIT |

Only one model is loaded in memory at a time. Selecting a new model unloads the active backend before launching the next.

> **Credits.** The Qwen runtime is based on [colibrì](https://github.com/JustVugg/colibri) by JustVugg and runs the model released by [Qwen](https://huggingface.co/Qwen). Bonsai is released by [PrismML](https://huggingface.co/prism-ml), and Ornith by [DeepReinforce](https://huggingface.co/deepreinforce-ai). GGUF execution uses PrismML's [llama.cpp fork](https://github.com/PrismML-Eng/llama.cpp). Samosa is an independent project.

## Installation

Building from source does not include pre-packaged model weights.

### macOS

Install OpenMP:

```sh
brew install libomp
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

Launch the app, open **Settings → Model**, and click **Download** for the desired model. First downloads are activated automatically upon verification.

### Linux (Debian/Ubuntu)

Install build dependencies and OpenMP:

```sh
sudo apt-get install build-essential libomp-dev
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

Files and binaries are placed in `~/.samosa`. Add `~/.samosa/bin` to `PATH` for direct command access.

For legacy Docker instructions, see [docs/INSTALL.md](docs/INSTALL.md).

## Terminal Model Management

```sh
samosa models             # Display installation status for all models
samosa pull bonsai
samosa pull ornith
samosa pull qwen
samosa pull all
```

Model downloads:
- Pin to specific Hugging Face commit revisions.
- Support resumable downloads via `.partial` files.
- Verify byte counts and SHA-256 hashes prior to installation.
- Preflight disk space with a 2 GB safety margin.
- Automatically provision the checksum-pinned Prism runtime binaries for GGUF models (macOS arm64/x64, Linux x64/arm64).

### Storage Requirements

| Installation | Disk Space |
|---|---:|
| Bonsai | 3.8 GB (+17 MB runtime) |
| Ornith | 5.6 GB (+17 MB runtime) |
| Qwen | 24.0 GB |
| All Models | 33.4 GB (+17 MB runtime) |

## App Usage

```sh
samosa app
```

Access the interface at <http://127.0.0.1:8642>. Available settings include:

- Model selection and downloading.
- Context size configuration (`auto` or explicit limits).
- Automatic and manual conversation compaction.
- Inference parameters (thinking budget, max tokens, seed) and optional web context tools.

State and conversation ledgers are persisted under `~/.samosa/chats`.

## Terminal Interface

Terminal interaction currently supports the Qwen backend:

```sh
samosa pull qwen
samosa "explain how DNS works"
samosa --continue "and where does DNSSEC fit?"
samosa --think "solve this logic puzzle"
samosa --think-code "review this algorithm"
samosa --context-tokens 65536 "remember this"
samosa doctor
```

See [docs/USAGE.md](docs/USAGE.md) for full CLI documentation.

## Hardware-Aware Context

Models support up to 262,144 context tokens. Context size allocation:

- **Qwen:** Calculates available system memory, resident weight size, and KV cache usage per token to determine context size.
- **Bonsai & Ornith:** Leverages Prism dynamic memory fitting with a 4 GiB target device memory margin. The gateway reads the fitted context length (`n_ctx`) directly from the backend.

On a 16 GB Apple M3 system, dynamic fitting selects **66,816 tokens for Bonsai** and **94,464 tokens for Ornith**.

## Conversation Compaction

Automatic compaction triggers when estimated context usage reaches a configurable threshold (50–90%, default 80%).

Execution steps:
1. Loads current conversation state.
2. Generates a condensed memory summary using the active model.
3. Retains recent messages verbatim.
4. Verifies that the summary reduces overall token context.
5. Atomically updates the conversation ledger.

If summary generation fails or does not reduce context size, the existing ledger remains unchanged.

## Local Gateway API

Start the background server:

```sh
samosa serve
```

Endpoints:

| Endpoint | Function |
|---|---|
| `GET /healthz` | System readiness, active model, context limits, and compaction status |
| `GET /v1/backends` | Model catalog and installation status |
| `POST /v1/backends/install` | Trigger model download |
| `POST /v1/backends/select` | Switch active model backend |
| `POST /v1/settings` | Update context and compaction settings |
| `POST /v1/compact` | Trigger conversation compaction |
| `POST /v1/chat/completions` | OpenAI-compatible chat completions |

For complete documentation, see [docs/SERVE_API.md](docs/SERVE_API.md).

## Build & Test

```sh
make              # Build Qwen C engine
make omp          # Build multithreaded OpenMP engine
make test         # Run unit and integration test suite
```

Test logs and regression benchmarks are located in [docs/regressions](docs/regressions).

## Privacy & Security

- All inference and chat data remain local to the host machine.
- The gateway server binds exclusively to `127.0.0.1`.
- No telemetry or external accounts are used.
- Downloads are limited to pinned Hugging Face repository assets and Prism GitHub release binaries.
- External web search tools are opt-in.

## Limitations

- Direct terminal chat is restricted to the Qwen backend.
- Qwen performance is dependent on SSD throughput due to expert streaming.
- Source build (`make install`) is currently required for multi-model functionality.

## Documentation

| Topic | Link |
|---|---|
| Installation & Model Storage | [docs/INSTALL.md](docs/INSTALL.md) |
| App & Terminal Usage | [docs/USAGE.md](docs/USAGE.md) |
| Model Specifications & Network | [docs/MODELS_AND_INTERNET.md](docs/MODELS_AND_INTERNET.md) |
| Gateway API | [docs/SERVE_API.md](docs/SERVE_API.md) |
| Architecture | [docs/DESIGN.md](docs/DESIGN.md) |
| Performance Benchmarks | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| Regression Logs | [docs/regressions](docs/regressions) |
