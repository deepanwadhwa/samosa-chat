<div align="center">
  <img src="assets/samosa-chat_medium.png" alt="Samosa Chat mascot" width="210">
  <h1>Samosa Chat</h1>
  <p><strong>Run Qwen3.6-35B-A3B locally on a 16 GB machine.</strong></p>
  <p>Fast on Apple Silicon &nbsp;·&nbsp; Slower on Linux &amp; Windows via Docker &nbsp;·&nbsp; Runs on the CPU &nbsp;·&nbsp; No cloud account &nbsp;·&nbsp; No telemetry</p>

  <p>
    <a href="https://github.com/deepanwadhwa/samosa-chat/actions/workflows/ci.yml"><img src="https://github.com/deepanwadhwa/samosa-chat/actions/workflows/ci.yml/badge.svg" alt="CI: build and tests"></a>
    <a href="https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32"><img src="https://img.shields.io/badge/%F0%9F%A4%97%20Hugging%20Face-model-FFD21E" alt="Hugging Face model"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue" alt="License: Apache-2.0"></a>
  </p>
  <p>
    <img src="https://img.shields.io/badge/macOS-Apple%20Silicon-000000?logo=apple&logoColor=white" alt="macOS on Apple Silicon">
    <img src="https://img.shields.io/badge/Linux%20%26%20Windows-via%20Docker-2496ED?logo=docker&logoColor=white" alt="Linux and Windows via Docker">
    <img src="https://img.shields.io/badge/RAM-16%20GB-orange" alt="16 GB RAM">
    <img src="https://img.shields.io/badge/GPU-not%20required-success" alt="No GPU required">
    <img src="https://img.shields.io/badge/engine-C-555555?logo=c&logoColor=white" alt="Written in C">
    <img src="https://img.shields.io/badge/model-35B%20total%20%2F%203B%20active-8A2BE2" alt="35B total, 3B active">
  </p>
</div>

> **Credit.** Samosa Chat is built on [colibrì](https://github.com/JustVugg/colibri)
> by JustVugg. Its expert-streaming design, SIMD kernels, and core utility
> headers made this project possible. The model is the text part of
> [Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B), created and
> released by the Qwen team. Samosa Chat is an independent, unofficial,
> Apache-2.0 project. It is not affiliated with or endorsed by either team.
>
> **What Samosa adds:** its own Qwen3.6 inference engine in C — the 30 Gated
> DeltaNet layers, the 10 attention layers, and the routed-expert path — the
> group-32 quantization format and its converter, the byte-budgeted expert
> cache that fits 35B parameters into 16 GB, sealed conversations that resume
> exactly, a local server and browser app, an atomic installer that verifies
> and rolls back, and the tests around all of it.
> [The full list](docs/DESIGN.md#what-samosa-adds-on-top).

## What it looks like

A real, unedited recording on the 16 GB M3 MacBook Air — a question in, an
answer out, no cloud:

<p align="center"><img src="assets/demo-terminal.gif" alt="Samosa Chat answering a question in the terminal" width="900"></p>

Real time, played at normal speed. The pause before the answer is the model
loading; after that it writes at about 5–9 tokens per second.

## Install

**Find your machine, run that.** Full detail, troubleshooting, and the Windows
walkthrough: **[docs/INSTALL.md](docs/INSTALL.md)**.

| Your machine | How | Speed |
|---|---|---|
| **macOS, Apple Silicon** (M1+, 16 GB) | the command below | **5–7 tok/s** |
| **Windows** | [Docker in WSL2](docs/INSTALL.md#windows) | **~1.3 tok/s** |
| **Linux, x86_64 / arm64** | [Docker](docs/INSTALL.md#linux) | ~1–2 tok/s |
| Intel Mac, or under 16 GB RAM | not supported | — |

**macOS:**

```sh
curl -fsSL https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32/resolve/main/install.sh | sh
```

Then **open a new terminal** (the installer adds to `PATH`, which only affects
new shells) and ask it something:

```sh
samosa "explain how DNS works"
```

**Linux and Windows** run Samosa as a Linux container. On Windows this lives
inside WSL2 — you do *not* need Docker Desktop, and
[docs/INSTALL.md](docs/INSTALL.md#windows) walks through it from `wsl --install`
onward:

```sh
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
docker build -t samosa .
docker volume create samosa-model
docker run --rm -v samosa-model:/model samosa pull
docker run -d --name samosa -p 127.0.0.1:8642:8642 -v samosa-model:/model --memory=6g samosa serve
```

Then open <http://127.0.0.1:8642>.

Everything needs **16 GB RAM** (≥6 GB of it given to the Docker VM), **~30 GB
free disk**, and **an NVMe SSD** — expert weights stream from disk on every
token, so storage is the main driver of speed. Everything installs under
`~/.samosa`; `samosa doctor` checks it; deleting that directory uninstalls it.

## Two ways to use it

Both come from the same install. Full reference: **[docs/USAGE.md](docs/USAGE.md)**.

| | Terminal | Web app |
|---|---|---|
| | `samosa "your question"` | `samosa app` → <http://127.0.0.1:8642> |
| | **the normal way to use it** | **a demo** — streams tokens, shows the model's reasoning |

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"   # resumes from a snapshot, no re-reading
samosa --think "solve this logic puzzle"                  # reasoning first, then the answer
samosa --fast "summarize this design"                     # more threads, runs warmer
samosa doctor
```

Conversations are sealed to disk and resume byte-exactly, so a follow-up never
re-reads the history. A conversation is capped at 24,576 tokens total.
[Thinking modes](docs/USAGE.md#thinking-modes) explains `--think` and
`--think-code`.

## What this is

Samosa Chat runs Qwen's 35-billion-parameter model on a machine with 16 GB of
RAM.

The model is a Mixture of Experts: 35B parameters in total, but only ~3B are
used per token. Samosa never loads all 35B. The shared weights stay in RAM; the
expert weights are **read from the SSD as the model chooses them**, token by
token. That one decision is what makes it fit, and it is why storage speed
matters more than anything else here.

It runs entirely on the CPU — no Metal, no CUDA, no GPU required. It is text
only today.

The architecture, the group-32 quantization format, what was tried and rejected,
and real example output: **[docs/DESIGN.md](docs/DESIGN.md)**.

## Where it runs, and how fast

Every number is measured, on the machine named beside it. Nothing is
extrapolated. Full detail: **[docs/PERFORMANCE.md](docs/PERFORMANCE.md)**.

| Platform | Measured decode | Verified on |
|---|---|---|
| macOS, Apple Silicon | **5–7 tok/s** | one 16 GB M3 MacBook Air (fanless), 2-thread default |
| Windows, x86_64 (Docker/WSL2) | **1.26 tok/s** | one ASUS Zenbook, i7-1260P, 16 GB |
| Linux, x86_64 (Docker) | *not yet measured* | build + tests green on Debian 12, Ubuntu 26.04 |

**macOS is the fast path; x86 is currently ~4–5x slower.** The build passes no
`-march`, so the AVX2 kernels are compiled out on x86 and the engine falls back
to a scalar loop — 7.6x slower, measured. Runtime CPU dispatch fixes it, and is
the next thing on the roadmap ([G10/H2](docs/TASKS_HARDWARE.md)).

Behaviour is identical on every platform: the same prompt and seed returns the
same tokens on macOS/NEON, arm64 Linux, and x86_64 Linux, at the same ~3.84 GB
footprint. Only speed differs.

**Memory:** ~2.5 GiB fresh, ~3.9–4.2 GiB warmed. Bounded — it does not grow with
conversation length.

**Storage is the bottleneck, not the CPU.** On the M3, **70% of decode is spent
waiting on the SSD and 30% on maths.** That is why an NVMe drive matters, why a
host bind mount instead of a named Docker volume costs ~6x, and why a GPU would
buy at most ~1.4x here. Reads do not wear out your SSD — endurance is spent by
writes — so the real costs are time, power, and heat:
[the details](docs/PERFORMANCE.md#ssd-speed-the-one-thing-to-be-deliberate-about).

## Build from source

```sh
make          # portable build
make omp      # multithreaded (macOS: brew install libomp first)
make test     # the full suite — no model download needed
```

The suite is self-contained — it stubs the engine and the network and uses tiny
fixtures, so it runs on a clean machine with no 24 GB download. It covers the
expert cache, long-context KV math, the repetition guard, the thinking
wind-down, quantized math, the server, the CLI wrapper, installer rollback,
output structure, route analysis, and the converter layout.

CI runs it on macOS and Ubuntu, plus a Debian container leg — Debian and Ubuntu
ship different `awk` and libc behaviour, and the container leg catches what the
Ubuntu runner cannot see.

Answer quality is scored on structure, stop reason, repetition, and correctness
separately, rather than by matching substrings. There is not yet enough evidence
to publish a general benchmark score; the plan for getting there is in
[docs/BENCHMARK_PLAN.md](docs/BENCHMARK_PLAN.md).

## Privacy and machine safety

- The model runs on your machine. The engine has no telemetry. The server
  listens on local loopback only.
- The installer contacts Hugging Face only to download the public release files.
  Running the model needs no cloud account.
- Two threads is the cool default. `--fast` is a deliberate choice.
- The expert cache watches memory pressure and drops cached experts before the
  system is forced to swap.
- A generation can be cancelled between tokens.
- Real-model test runs are kept short on purpose: one long run can read hundreds
  of gigabytes from the SSD.

## Roadmap

Full detail and reasoning: **[docs/ROADMAP.md](docs/ROADMAP.md)**.

- **Make x86 fast.** Linux and Windows now work; what is left is the scalar-path
  penalty. Runtime CPU dispatch should be worth ~3x
  ([G10/H2](docs/TASKS_HARDWARE.md)).
- **Vision.** Qwen3.6 is multimodal, and **the vision tower already ships inside
  every install** — all 27 blocks, validated at mean cosine 0.9976 against the
  reference weights. The weights are on your disk and usable today; what is
  missing is the runtime: an image decoder, the encoder in C, and splicing image
  embeddings into the language model ([docs/TASKS_VISION.md](docs/TASKS_VISION.md)).
- **Documents and internet access** ([#5](docs/TASKS_DOCUMENTS.md),
  [#4](docs/TASKS_INTERNET.md)).
- **A Metal backend**, eventually — though the 70/30 split above caps it at ~1.4x.

## Known limitations

- **x86 is ~4–5x slower than macOS** — 1.26 tok/s on an i7-1260P against 5–7 on
  the M3, because the AVX2 kernels are not compiled in yet
  ([G10/H2](docs/TASKS_HARDWARE.md)).
- **Linux and Windows speed is measured on one machine each.** Sustained and
  long-running behaviour on those platforms has not been measured.
- **Text only.** No images, video, audio, or tool calling — yet.
- **No GPU acceleration.** Decode is 70% SSD wait, which caps any GPU near
  1.4x, and 24 GB of experts do not fit in a typical laptop GPU.
- **Quality is measured on one machine and one reasoning control**, not across
  many machines or task types.
- Deleting a chat in the app removes it from the browser but not yet from disk.

## More documentation

**Start here depending on what you want:**

| I want to… | Read |
|---|---|
| install it, or fix an install | [docs/INSTALL.md](docs/INSTALL.md) |
| use the CLI, the app, thinking modes | [docs/USAGE.md](docs/USAGE.md) |
| know how fast it is, and why | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| understand how it works | [docs/DESIGN.md](docs/DESIGN.md) |
| know what is next | [docs/ROADMAP.md](docs/ROADMAP.md) |
| use the local HTTP API | [docs/SERVE_API.md](docs/SERVE_API.md) |
| contribute, or pick up a task | [docs/ISSUE_TASKS.md](docs/ISSUE_TASKS.md) and [CLAUDE.md](CLAUDE.md) |

**Engineering detail:** [hardware and performance work](docs/TASKS_HARDWARE.md) ·
[Linux](docs/TASKS_LINUX.md) · [Windows/Docker](docs/TASKS_WINDOWS.md) ·
[vision](docs/TASKS_VISION.md) · [documents](docs/TASKS_DOCUMENTS.md) ·
[internet](docs/TASKS_INTERNET.md) · [app program](docs/APP_TASKS.md)

**Evidence and measurements:** [regression ledger](docs/REGRESSION_LEDGER.md) ·
[group-32 baseline](docs/GROUP32_BASELINE.md) · [benchmark plan](docs/BENCHMARK_PLAN.md) ·
[thinking diagnosis](docs/THINKING_DIAGNOSIS.md) ·
[upstream comparison](docs/UPSTREAM_CONTROL_2026-07-14.md) ·
[measured runs](docs/regressions/) · [work log](docs/WORK_LOG_2026-07-14.md)

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for the full attribution
and derivative-work notice.
