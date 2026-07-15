---
license: apache-2.0
base_model: Qwen/Qwen3.6-35B-A3B
tags:
- qwen3.6
- moe
- int4
- apple-silicon
- local-inference
- c
pipeline_tag: text-generation
---

<div align="center">
  <img src="samosa-chat.png" alt="Samosa Chat mascot" width="190">
  <h1>Samosa Chat</h1>
  <p><strong>Run Qwen3.6-35B-A3B locally on a 16 GB Apple Silicon Mac.</strong></p>
  <p>In your terminal, or in your browser · Runs on the CPU · No cloud account · No telemetry</p>
</div>

> **Foundation and model credit.** Samosa Chat is built on
> [colibrì](https://github.com/JustVugg/colibri) by JustVugg; its
> expert-streaming design, SIMD kernels, and utility headers made this project
> possible. The converted checkpoint comes from
> [Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) by the
> Qwen team. This is an independent, unofficial Apache-2.0 text-only
> conversion. It is not affiliated with or endorsed by either upstream team.
>
> **What Samosa adds:** its own Qwen3.6 engine in C, the group-32 format and
> its converter, the expert cache that fits 35B parameters into 16 GB, sealed
> resumable conversations, a local server and app, and an atomic installer.
> [Details](#what-samosa-adds).

> **This repository hosts the model that powers Samosa Chat.** These are the
> group-32 model files used by [Samosa Chat](https://github.com/deepanwadhwa/samosa-chat),
> a free, open-source project that runs Qwen3.6-35B-A3B fully locally on a
> 16 GB Apple Silicon Mac. The complete source, documentation, and issue
> tracker live on GitHub:
> **[github.com/deepanwadhwa/samosa-chat](https://github.com/deepanwadhwa/samosa-chat)**

Samosa Chat packages the group-32 int4 model and a dependency-free C inference
engine. You use it from the terminal (`samosa "your question"`), which is the
normal way, or through a local browser app (`samosa app`), which is currently a
demo. Everything stays on the Mac: no account, no telemetry, and no remote
request during inference. The server binds only to `127.0.0.1`.

## What Samosa adds

Qwen's checkpoint and the colibrì runtime are the starting point. Samosa adds:

- **A Qwen3.6 engine in C**, covering the 30 Gated DeltaNet layers, the 10
  gated attention layers, the shared/routed MoE path, the tokenizer, and the
  chat template.
- **The group-32 format and its converter** — a shard-by-shard conversion of
  the original checkpoint into a manifest-based expert container. Group-32
  gives every 32 weights their own int4 scale instead of one scale per whole
  matrix row, which is what makes the output quality hold up.
- **A byte-budgeted expert cache** with LRU eviction, per-layer floors,
  reusable slabs, memory-pressure monitoring, and I/O telemetry — the piece
  that lets a 35B model run in 16 GB by streaming experts from SSD.
- **Sealed, resumable conversations** (`QWSESS01`): geometry-checked,
  SHA-256-sealed, atomically written, and byte-exact on resume.
- **A dependency-free local server and browser app** in C: JSON/SSE, a bounded
  queue, cancellation, health telemetry, and clean shutdown.
- **An atomic installer** that verifies every byte, compiles and smoke-tests an
  inactive release, and switches only on success — rolling back otherwise.
- **The tests around all of it**: quantized kernels, KV math, sessions, server
  behaviour, stop conditions, the wrapper, installer rollback, and PATH setup.

Full source and detail:
[github.com/deepanwadhwa/samosa-chat](https://github.com/deepanwadhwa/samosa-chat)

## Supported hardware

Version 1 supports **macOS on Apple Silicon (`arm64`) with at least 16 GB of
RAM**. The installer rejects other operating systems and architectures.

“CPU-only” describes the current inference backend: it uses Apple NEON/SDOT
and optional OpenMP, not Metal. It does **not** mean that an arbitrary 16 GB
laptop can run this release. Product testing to date is on one fanless 16 GB
M3 MacBook Air; M1/M2 and additional machines still need independent release
validation.

## Install

```sh
curl -fsSL https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main/install.sh | sh
```

Then **open a new terminal** and ask it something:

```sh
samosa "explain how DNS works"
```

Allow roughly 30 GB free. The installer downloads into an inactive versioned
release, verifies every byte size and SHA-256 digest, compiles the C engine,
and smoke-tests it before atomically switching the live release. A corrupt or
interrupted upgrade leaves the prior release untouched. No administrator
rights are required.

Everything installs under `~/.samosa` — the `samosa` command at
`~/.samosa/bin/samosa`, the active release at `~/.samosa/current`, and your
conversations at `~/.samosa/chats`. Nothing is installed system-wide. The
installer adds `~/.samosa/bin` to `PATH` via one line in your shell rc file,
which **only affects terminals opened afterwards** — hence the new terminal
above. If `samosa` is still not found, either
`export PATH="$HOME/.samosa/bin:$PATH"` in the current shell, or run it
directly as `~/.samosa/bin/samosa "how are you"`. To uninstall, delete
`~/.samosa` and remove that line.

## Two ways to use it

**The terminal is the normal way to use Samosa.**

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"
samosa --think "solve this logic puzzle"
samosa --think-code "build a responsive settings page"
samosa --fast "summarize this design"
samosa --max-tokens 2048 "write a long explanation"
samosa doctor
```

**The browser app is a demo** at this point. It works, and it is the nicest way
to watch answers stream and see the model's reasoning, but it exists to show the
engine off rather than as a polished interface. `samosa app` starts one resident
local model process and opens it; `samosa serve --stop` stops it. It provides:

- token-by-token answer streaming;
- a separate collapsible thinking view;
- direct, general-thinking, and precise-code profiles;
- stop/cancel, seed, and output-ceiling controls;
- browser-local conversation display history plus sealed server snapshots;
- tokens/s, RSS, and thinking-closure telemetry;
- responsive light/dark presentation with no framework, CDN, analytics, or
  external script.

The complete UI and logo total 181,552 bytes in this release.

## Local API

The resident server exposes local JSON/SSE chat completions at
`POST /v1/chat/completions`, plus health, model discovery, cancellation, and
clean shutdown endpoints. Requests are serialized through a bounded queue;
the model is never mutated by concurrent generations.

Generation stops when Qwen emits its end-of-turn token. The outer ceiling is
8,192 new tokens. General reasoning defaults to a 1,024-token internal budget
and precise code to 2,048. On budget exhaustion Samosa appends Qwen's trained
early-stop transition before `</think>` instead of injecting a bare control
token. Natural closure, budget transition, repetition stop, cancellation, and
length stop remain distinct in telemetry.

Saved history, the newly tokenized turn, and the requested completion ceiling
must fit a 24,576-token total context cap. Samosa rejects an oversized request
before queueing or allocating KV state. One conversation state is resident at
a time; other conversations remain as sealed snapshots on disk.

Stopping a generation is safe for the saved conversation: a cancelled turn is
persisted only up to its last complete sentence, so an interrupted answer does
not poison later turns. If it produced no complete sentence, the previous
snapshot is kept and `session_saved` reports false.

## Model layout

Qwen describes the upstream language model as 35B parameters total with 3B
activated per token, 40 layers, 256 routed experts, and 8 routed plus 1 shared
expert active per MoE layer. Samosa keeps dense weights resident and streams
only selected routed experts from SSD.

| file | bytes | purpose |
|---|---:|---|
| `experts.bin` | 20,942,159,872 | group-32 symmetric-q4 routed experts |
| `resident.safetensors` | 3,015,056,192 | resident dense row-q8 weights |
| `tokenizer_qwen36.json` | 28,142,621 | 248,320-token vocabulary |
| `app.html` | ~32 KB | complete local chat UI |
| `samosa-chat.png` | ~149 KB | transparent app mascot |
| `engine/` | ~300 KB | complete C source |
| `release-manifest.tsv` | — | SHA-256, size, and install path for every file |

The previous whole-row q4 artifact had materially higher measured weight
reconstruction error. This release uses group size 32. The format is custom to
Samosa and is not a GGUF or Transformers-loadable checkpoint.

## Measured behavior

All measurements below are from the 16 GB M3 MacBook Air reference machine.
They are workload-specific observations, not guarantees for every Mac.

| workload | threads | measured result |
|---|---:|---:|
| group-32 direct control | 2 | 7.27 tok/s |
| live 132-token app turn | 2 | 7.11 tok/s |
| 933-token general-thinking control | 2 | 4.85 tok/s |
| selective-precision 5,000-token WebDev control | 4 | 6.47 tok/s |

A bounded release-path check served the real UI and logo, streamed exactly
`Samosa app works locally.`, stopped naturally on Qwen's end-of-turn token,
saved the conversation snapshot, decoded at 5.13 tok/s, and peaked at 3.28 GB
RSS. The full bounded test suite covers grouped quantization, sessions, server
behavior, special-token stopping, cancellation, the wrapper, and corrupt
atomic-upgrade rollback.

For the resident app, macOS physical footprint measured 2.51 GiB after model
load and 4.07 GiB after a real two-turn continuation; the independent
`footprint` tool reported 2,566 MiB and 4,170 MiB. A live Activity Monitor
comparison also matched the value displayed by Samosa. GQA KV grows by about
40 KiB per context token, so the 24,576-token cap bounds that variable
component to about 960 MiB. The allocator may retain its high-water mark, but
chat history cannot grow KV memory beyond the cap.

Long thinking is expensive: one 933-token group-32 control reread 376.77 GB of
expert data. More output tokens can be useful, but they increase time, power
draw, and heat even when RAM stays bounded. SSD reads do not consume drive
endurance (TBW is write-only); the genuine cost is speed and energy.

## Privacy and storage

- Normal inference does not contact Hugging Face or any other remote service.
- The installer contacts Hugging Face only to download public release files.
- The listener is hard-bound to IPv4 loopback.
- Model files are read-only during inference.
- A continuing conversation atomically replaces a roughly 63–70 MB sealed
  session snapshot at turn end.
- Display transcripts are currently stored in the browser's local storage.

## Known limitations

- Only Apple Silicon macOS is supported; only one M3 16 GB machine has
  completed the full test program so far.
- Metal acceleration is not implemented. The CPU path is partly constrained
  by repeated SSD expert reads; a future Metal backend must be measured
  end-to-end rather than assumed faster.
- Group-32 improves the original quantization error, but broad benchmark and
  upstream-parity coverage remains incomplete.
- Deleting a conversation in the UI removes its browser transcript but does
  not yet remove the sealed server snapshot.
- In-RAM conversation slots, document chat, and web access remain roadmap
  work.
- Text only: the Qwen vision tower, image/audio/video input, and tool calling
  are not included.
- SSD speed is the primary performance bottleneck. Routed experts are streamed
  from storage on every token; NVMe bandwidth directly determines tok/s. Measured:
  2.3+ GB/s (native NVMe) gives 5-7 tok/s; ~0.5 GB/s (a Docker host bind mount
  through virtiofs) gives ~0.9 tok/s.
- SSD reads do **not** consume drive endurance. Flash endurance is rated in TBW
  (Terabytes *Written*, JEDEC JESD218) and DWPD (Drive *Writes* Per Day); program/
  erase cycles wear NAND, reads do not. Read disturb is real but its thresholds
  are orders of magnitude beyond this workload. Stated from the endurance rating
  definition, not from a SMART measurement on the reference machine — Apple
  Silicon's internal NVMe does not expose endurance counters. The genuine costs of
  streaming are speed, power, and heat.

Source and detailed evidence:
[github.com/deepanwadhwa/samosa-chat](https://github.com/deepanwadhwa/samosa-chat)

## License

Apache-2.0. The source repository includes `LICENSE` and `NOTICE` with the
complete derivative-work attribution.
