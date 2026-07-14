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
the quantized model, the dependency-free C inference engine (~4,000 lines),
and a one-command installer.

> ⚠️ Unofficial, text-only int4 conversion of
> [Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)
> (vision tower removed). Credit for the model goes to the Qwen team.
> Apache-2.0, same as the original.

## Install

```sh
curl -fsSL https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main/install.sh | sh
```

Requirements: Apple Silicon Mac (M1 or newer), 16 GB RAM, ~25 GB free disk,
~18 GB download. The installer verifies every file by SHA-256, resumes
interrupted downloads, compiles the engine locally, and runs a smoke test.
No admin rights. Uninstall: `rm -rf ~/.samosa`.

## Use

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"  # resumes last conversation
samosa --think "tricky logic puzzle"                     # chain-of-thought mode
samosa --fast "..."                                      # all P-cores
```

`--continue` restores the previous conversation from a ~70 MB snapshot
instead of re-processing the history, including across reboots; continuation
output is byte-identical to an uninterrupted session. This works because 30
of the model's 40 layers are DeltaNet linear-attention layers with a fixed
63 MB state, and only 10 layers keep a KV cache (~40 KB/token).

## Performance (measured, MacBook Air M3 16 GB, fanless)

| workload | tokens/s |
|---|---|
| decode, default (2 threads) | 7–8 |
| decode, `--fast` (4 threads) | ~9.5 |
| prefill | 14–24 |

Peak RSS 2–3 GB (the 16.6 GB of experts stream from SSD on demand); zero
swap; no disk writes except the session snapshot. Engine output is validated
bit-exact against a quantization-aware reference, and the release
configuration passed a gated benchmark suite (100-prompt corpus, 15-minute
soak, swap/write/thermal guardrails).

## Files

| file | size | what |
|---|---|---|
| `experts.bin` | 16.6 GB | int4 routed experts (streamed from disk) |
| `resident.safetensors` | 1.3 GB | dense weights kept in RAM |
| `tokenizer_qwen36.json` | 28 MB | tokenizer (248,320-token vocab) |
| `engine/` | ~250 KB | complete C source |
| `install.sh`, `samosa` | — | installer and chat command |
| `checksums.txt` | — | SHA-256 of every file above |

Source repository: https://github.com/deepanwadhwa/samosa-chat

## Known limitations

- Thinking mode can deliberate to the token ceiling without answering on
  open-ended tasks; direct mode is recommended for code/writing generation.
- The int4 conversion occasionally doubles a short function word during
  generation ("of of"), roughly once per ~10 longer answers — a quantization
  artifact of generation-time states, absent under teacher forcing.
  Re-asking or a different seed avoids it.
- Text-only: the upstream vision tower is not included.

## Credits and license

Built on [colibrì](https://github.com/JustVugg/colibri) by JustVugg: the
expert-streaming design, SIMD kernels, and utility headers originate there.
Samosa Chat adds the Qwen3.6 engine (DeltaNet linear attention, gated GQA,
256-expert MoE), the converter, sessions, and the distribution.

Apache-2.0. Weights converted from Qwen/Qwen3.6-35B-A3B — credit to the Qwen
team. Not affiliated with or endorsed by Alibaba/Qwen or the colibrì project.
