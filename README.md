<div align="center">
  <img src="assets/samosa-chat_medium.png" alt="Samosa Chat mascot" width="210">
  <h1>Samosa Chat</h1>
  <p><strong>Private chat with Qwen on your Mac.</strong></p>
  <p>No cloud account &nbsp;·&nbsp; No telemetry &nbsp;·&nbsp; Hardware-aware context &nbsp;·&nbsp; Durable compaction</p>

  <p>
    <a href="https://github.com/deepanwadhwa/samosa-chat/actions/workflows/ci.yml"><img src="https://github.com/deepanwadhwa/samosa-chat/actions/workflows/ci.yml/badge.svg" alt="CI: build and tests"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue" alt="License: Apache-2.0"></a>
    <img src="https://img.shields.io/badge/context-up%20to%20262K-orange" alt="Context up to 262K">
  </p>
</div>

Samosa is a local chat app and HTTP gateway. It downloads, verifies and runs Qwen on your Mac.

## Features

- Uses a local model application and HTTP gateway.
- Starts with zero installed models.
- Includes a browser interface for downloading Qwen from a validated catalog.
- Supports resumable and verified model downloads.
- Automatically routes chat images and PDFs through local text/OCR and an
  on-demand visual specialist; oversized inputs are bounded from live
  memory/thermal conditions and multi-page work has no arbitrary page cap.
- Provides an internet-search feature that does not require an API key.
- Integrates portable Chutni folder memory through the bundled local `chutni-mcp` service.

## Models

| Model | Download | Description | Runtime | License |
|---|---:|---|---|---|
| [Qwen3.6 35B A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) | 24.0 GB | Expert-streamed local chat with image support | Samosa C engine | Apache-2.0 |
| [VisionPsy-Nano 460M](https://huggingface.co/KaedeTai/VisionPsy-Nano-460M-MLX) | 1.01 GB | Auxiliary vision specialist for chat attachments (macOS arm64) | Samosa MLX Metal C++ helper | Apache-2.0 |

Qwen handles all chat responses. VisionPsy can supply image and PDF evidence
on demand, and the separate speech tools handle recordings and read-aloud.

> The Qwen runtime is based on [colibrì](https://github.com/JustVugg/colibri) by JustVugg and runs the model released by [Qwen](https://huggingface.co/Qwen).

## Installation

Building from source does not include pre-packaged model weights.

### macOS

Install OpenMP and Node.js for the native build and catalog assembly.

```sh
brew install libomp node
git clone --recurse-submodules https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

Launch the app, open **Settings → Model**, and choose **Download** for Qwen. The app activates the model after verifying its files.

### Linux (Debian/Ubuntu)

Install build dependencies and OpenMP:

```sh
sudo apt-get install build-essential libomp-dev nodejs
git clone --recurse-submodules https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

Files and binaries are placed in `~/.samosa`. Add `~/.samosa/bin` to `PATH` for direct command access.

For legacy Docker instructions, see [docs/INSTALL.md](docs/INSTALL.md).

## Terminal Model Management

```sh
samosa doctor
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
| VisionPsy (macOS arm64 only) | 1.01 GB |
| Molmo2 native Q4 package (optional, macOS arm64) | ≤4.0 GiB |
| All Models | 34.4 GB (+17 MB runtime) |

## App Usage

```sh
samosa app
```

Access the interface at <http://127.0.0.1:8642>. Available settings include:

`samosa app` is browser-owned: closing the final Samosa tab stops the gateway,
the loaded model, and Samosa helper processes after a short refresh-safe grace
period. Use `samosa serve` only when you intentionally want the service to
remain running without an open browser tab. The Privacy settings page also has
a **Stop local processes** action for immediate authenticated shutdown.

- Model selection and downloading.
- Grouped Text LLM, Vision, OCR, and Voice model settings with live readiness.
- Automatic chat-attachment routing, hardware-adaptive image resizing, and
  honest text/OCR-only partial answers if visual analysis fails.
- Streamed MP4/MOV/M4V attachments with bounded overview, temporal, or
  exhaustive Molmo2 coverage and explicit unprocessed intervals.
- Context size configuration (`auto` or explicit limits).
- Automatic and manual conversation compaction.
- Inference parameters (thinking budget, max tokens, seed) and optional web context tools.
- Opt-in Developer mode with one correlated local trace across routing, OCR,
  VisionPsy, evidence assembly, and the final model response.

State and conversation ledgers are persisted under `~/.samosa/chats`.
Developer traces are off by default because they contain full prompts and
document/model contents; see [docs/DEVELOPER_MODE.md](docs/DEVELOPER_MODE.md).

To use the Mac's models from a phone or another computer on the same Wi-Fi:

```sh
samosa app --lan
```

Samosa opens the normal local app and prints a clean `Other devices` link.
Open it on the other device and enter the LAN password (`password1234` for
now). Keep the Mac awake, allow incoming connections if macOS asks, and stop
sharing with `samosa serve --stop`. See the full
[LAN access guide](docs/LAN_ACCESS.md) for the trust boundary, password
sessions, shared-model concurrency, model switching, and troubleshooting.

### Folder memory with Chutni

Samosa bundles the independent [Chutni protocol](https://github.com/deepanwadhwa/chutni-protocol)
service; there is no separate Chutni installation.

1. Open **Chutni** in the app and choose a folder.
2. Review the proposed adjacent `Folder.chutni` path and scan policy.
3. Click **Build memory**.
4. On the ready folder, click **Use in this chat**.

Samosa creates or reuses that portable store. For a chat bound to the folder,
the gateway searches before each local-model turn and injects only bounded,
source-labelled excerpts as untrusted file data. Another Chutni-capable
application can use the same store without conversion.
The current scanner extracts text-like files and records metadata for other
files; PDF page extraction, OCR, images, spreadsheets, and audio are not on the
protocol path yet. See [docs/CHUTNI_PROTOCOL.md](docs/CHUTNI_PROTOCOL.md).

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
make chutni-gateway-test  # Build and test the bundled Chutni handoff
make test         # Run unit and integration test suite
```

Test logs and regression benchmarks are located in [docs/regressions](docs/regressions).

## Privacy & Security

- All inference and chat data remain local to the host machine.
- The gateway binds exclusively to `127.0.0.1` unless the user explicitly
  starts password-protected LAN access with `--lan`.
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
| Password-protected LAN access | [docs/LAN_ACCESS.md](docs/LAN_ACCESS.md) |
| Model Specifications & Network | [docs/MODELS_AND_INTERNET.md](docs/MODELS_AND_INTERNET.md) |
| Gateway API | [docs/SERVE_API.md](docs/SERVE_API.md) |
| Conversation context & document reuse | [docs/CONVERSATION_CONTEXT.md](docs/CONVERSATION_CONTEXT.md) |
| Chutni folder memory | [docs/CHUTNI_PROTOCOL.md](docs/CHUTNI_PROTOCOL.md) |
| Architecture | [docs/DESIGN.md](docs/DESIGN.md) |
| Performance Benchmarks | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| Regression Logs | [docs/regressions](docs/regressions) |
