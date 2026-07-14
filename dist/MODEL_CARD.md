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

# Samosa Chat — Qwen3.6-35B-A3B int4 for 16 GB Macs

Run Qwen3.6-35B-A3B (int4, text-only) locally on an Apple Silicon Mac with
16 GB of RAM. No cloud, no account, no telemetry, no GPU. This repo contains
the quantized model, the dependency-free C inference engine, a loopback app
server, and a one-command installer.

> ⚠️ Unofficial, text-only int4 conversion of
> [Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)
> (vision tower removed). Credit for the model goes to the Qwen team.
> Apache-2.0, same as the original.

## Install

```sh
curl -fsSL https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main/install.sh | sh
```

Requirements: Apple Silicon Mac (M1 or newer) and 16 GB RAM. The installer
reads exact release sizes before downloading; allow roughly 30 GB free for the
groupwise build. It downloads into an inactive versioned release, verifies
every byte size and SHA-256 digest, compiles the engine, and smoke-tests it
before atomically switching the active release pointer. A failed upgrade leaves
the working release untouched. No admin rights. Uninstall: `rm -rf ~/.samosa`.

## Use

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"  # resumes last conversation
samosa --think "tricky logic puzzle"                     # general reasoning profile
samosa --think-code "build a responsive settings page"   # precise coding/WebDev
samosa --fast "..."                                      # all P-cores
samosa serve                                             # foreground localhost API
samosa app                                               # start server + open browser
samosa serve --stop                                      # clean server shutdown
```

The resident server binds only to `127.0.0.1:8642`. It provides OpenAI-shaped
JSON/SSE completions, separate reasoning and answer deltas, a bounded request
queue, cancellation, health telemetry, and optional sealed conversation
snapshots. The server is a developer preview: the full browser chat interface
and four-slot in-RAM conversation pool are not part of this artifact yet.

The two thinking commands use Qwen3.6's published task-specific sampling
profiles. Precise coding/WebDev additionally keeps the routed/shared MoE
down-projection input in float. This selective precision boundary crossed the
fully accelerated path's long-output failure point without a repetition
attractor, while retaining 6.47 tok/s in the 5,000-token four-thread control.

Generation stops early when the model emits end-of-turn. Every profile
defaults to an 8,192-new-token outer ceiling. General reasoning also has a
1,024-token internal thinking budget and precise code has 2,048;
`--max-tokens N` and `--thinking-budget N` override those bounds. On budget
exhaustion Samosa appends Qwen's published natural-language early-stop
transition before `</think>` rather than forcing a bare control token. A visible
notice distinguishes a ceiling stop from a model-completed answer, and a
repeated-token-cycle guard stops runaway loops.

A six-run upstream FP8 arithmetic pilot naturally used 353--616 reasoning
tokens. The matched local group-32 control closed naturally and answered
correctly after 933 total tokens with the 1,024 budget. This validates the
configured thinking path for that prompt family; broader release claims still
require upstream-calibrated parity across task families.

`--continue` restores the previous conversation from a ~70 MB snapshot
instead of re-processing the history, including across reboots; continuation
output is byte-identical to an uninterrupted session. This works because 30
of the model's 40 layers are DeltaNet linear-attention layers with a fixed
63 MB state, and only 10 layers keep a KV cache (~40 KB/token).

## Performance (measured, MacBook Air M3 16 GB, fanless)

| workload | tokens/s |
|---|---|
| decode, direct default (2 threads) | 7–8 |
| decode, direct `--fast` (4 threads) | ~9.5 |
| 933-token general-thinking control (2 threads) | 4.85 |
| selective-precision 5,000-token WebDev control (4 threads) | 6.47 |
| prefill | 14–24 |

Measured peak RSS ranges from about 2.5 GB on the legacy direct path to
3.2–3.9 GB on the group-32 chat/server path. Experts stream from SSD on demand;
there are no model-data writes. Durable conversations atomically replace a
roughly 63–70 MB sealed snapshot at turn end. The 5,000-token stability control
peaked at 2.48 GB RSS, left 69% memory free, held swap at 5 MB, and caused no
macOS thermal or performance warning. Quantization-aware teacher forcing is
supplemented by structural generation gates for thinking closure, repetition,
and task-specific completion.

## Files

| file | size | what |
|---|---|---|
| `experts.bin` | 20.94 GB | group-32 int4 routed experts (streamed from disk) |
| `resident.safetensors` | 3.02 GB | dense row-q8 weights kept in RAM |
| `tokenizer_qwen36.json` | 28 MB | tokenizer (248,320-token vocab) |
| `engine/` | ~250 KB | complete C source |
| `install.sh`, `samosa` | — | installer and chat command |
| `checksums.txt` | — | SHA-256 of every file above |

Source repository: https://github.com/deepanwadhwa/samosa-chat

## Known limitations

- The int4 conversion can still produce isolated word-level defects such as
  `of ofof`, including with full float activations. This is separate from the
  catastrophic fast-path attractor addressed by selective precision.
- Text-only: the upstream vision tower is not included.

## Credits and license

Built on [colibrì](https://github.com/JustVugg/colibri) by JustVugg: the
expert-streaming design, SIMD kernels, and utility headers originate there.
Samosa Chat adds the Qwen3.6 engine (DeltaNet linear attention, gated GQA,
256-expert MoE), the converter, sessions, and the distribution.

Apache-2.0. Weights converted from Qwen/Qwen3.6-35B-A3B — credit to the Qwen
team. Not affiliated with or endorsed by Alibaba/Qwen or the colibrì project.
