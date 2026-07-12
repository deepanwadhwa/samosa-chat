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

# Jugnu — Qwen3.6-35B-A3B for everyone (runs on a 16 GB MacBook)

**Jugnu** (जुगनू, *firefly*) is a tiny, self-contained engine that carries a
big light: a 35B-parameter model running privately on ordinary laptops.

This repo contains everything needed to run Qwen3.6-35B-A3B **fully locally
on any Apple Silicon Mac with 16 GB of RAM** — no cloud, no account, no
telemetry, no GPU: the int4 model, the pure-C inference engine (~4,000
lines, zero dependencies), and a one-command installer.

> ⚠️ This is an **unofficial, text-only int4 conversion** of
> [Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)
> (vision tower removed). All credit for the model goes to the Qwen team.
> Distributed under Apache-2.0, same as the original.

## Install (one command)

```sh
curl -fsSL https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main/install.sh | sh
```

Needs: an Apple Silicon Mac (M1 or newer), 16 GB RAM, ~25 GB free disk, and
~18 GB of download. The installer verifies every byte, resumes if
interrupted, compiles the engine on your machine in seconds, and finishes
with a hello-world. No admin password required; uninstall is
`rm -rf ~/.jugnu`.

## Use

```sh
jugnu "explain how a hash table handles collisions"
jugnu --continue "and which strategy does Python use?"   # instant follow-up
jugnu --think "tricky logic puzzle here"                 # step-by-step reasoning
jugnu --fast "..."                                       # all P-cores (runs warmer)
```

`--continue` resumes your conversation from a ~70 MB snapshot instead of
re-reading the whole history — follow-ups start generating immediately, even
after a reboot. That trick is possible because 30 of the model's 40 layers
are linear-attention (DeltaNet) layers whose state is a **fixed** 63 MB
regardless of conversation length.

## What to expect (measured on a fanless MacBook Air M3, 16 GB)

| | tokens/second |
|---|---|
| Chat decode (default, cool & quiet) | ~7–8 |
| Chat decode (`--fast`, 4 P-cores) | ~9.5 |
| Prompt reading (prefill) | ~14–24 |

RAM stays around 2–3 GB (the 17 GB of experts stream from SSD on demand);
swap stays at zero; the engine writes nothing to disk except your session
snapshot. Quality: the int4 engine is validated **bit-exact** against its
reference implementation, and scores within the tolerance gates of the
original bf16 model on a 100-prompt evaluation (chat, code, reasoning,
multilingual, document QA).

## How it works (the interesting bits)

- **35B parameters on 16 GB RAM**: only ~1.3 GB of dense weights stay
  resident; the 256-experts-per-layer MoE weights (16.6 GB, int4) live on
  SSD and stream in per token at ~3 GB/s. Apple's unified page cache does
  most of the caching for free.
- **Pure C, zero dependencies**: one `clang` command builds it. NEON int8
  dot-product kernels; OpenMP optional (installer uses it when available).
- **Sessions**: fixed-size DeltaNet state + tiny KV (only 10 full-attention
  layers) → whole conversations snapshot to ~70–100 MB.

The complete engine source lives in the `engine/` folder of this repo.

## Files

| file | size | what |
|---|---|---|
| `experts.bin` | 16.6 GB | int4 routed experts (streamed from disk) |
| `resident.safetensors` | 1.3 GB | dense weights kept in RAM |
| `tokenizer_qwen36.json` | 28 MB | tokenizer (248,320-token vocab) |
| `engine/` | ~250 KB | complete C source |
| `install.sh`, `jugnu` | — | installer and chat command |
| `checksums.txt` | — | SHA-256 of every file above |

## Lineage, license, and attribution

Jugnu is **inspired by and built on [colibrì](https://github.com/JustVugg/colibri)**
by JustVugg (Vincenzo) — the pure-C, stream-experts-from-SSD design that
proved a huge MoE can live on a small machine. Jugnu shares colibrì's SIMD
kernels (`engine/kernels.h`) and utility headers, and adds a new engine for
the Qwen3.6 architecture (Gated DeltaNet linear attention, gated GQA,
256-expert MoE), the converter, instant-resume sessions, and this
distribution. A firefly and a hummingbird: different wings, same idea —
small, free, and everywhere.

Model weights: Apache-2.0, converted from
[Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) (all
credit to the Qwen team). Engine: Apache-2.0. Not affiliated with or
endorsed by Alibaba/Qwen or the colibrì project.
