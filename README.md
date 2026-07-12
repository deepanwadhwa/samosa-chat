# Jugnu 🪔

**जुगनू — firefly.** A tiny, self-lit engine carrying a big light: run a
**35-billion-parameter** language model privately on a **16 GB MacBook** —
no cloud, no account, no telemetry, no GPU.

Jugnu is ~4,000 lines of dependency-free C that runs
[Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) (int4,
text-only) by keeping 1.3 GB of dense weights in RAM and streaming the
16.6 GB of mixture-of-experts weights from SSD on demand.

## Install (one command)

```sh
curl -fsSL https://huggingface.co/deepanwa/Jugnu-Qwen3.6-35B-A3B-int4/resolve/main/install.sh | sh
```

Requirements: Apple Silicon Mac (M1 or newer), 16 GB RAM, ~25 GB free disk.
The installer verifies every byte, resumes interrupted downloads, compiles
the engine on your machine in seconds, and ends with a hello-world.
Uninstall: `rm -rf ~/.jugnu`.

## Use

```sh
jugnu "explain how a hash table handles collisions"
jugnu --continue "and which strategy does Python use?"  # instant follow-up
jugnu --think "tricky logic puzzle"                     # step-by-step reasoning
jugnu --fast "..."                                      # all P-cores (runs warmer)
jugnu doctor                                            # check the install
```

`--continue` resumes a conversation from a ~70 MB snapshot instead of
re-reading the history — even after a reboot. This is possible because 30 of
the model's 40 layers are linear-attention (DeltaNet) layers whose state is a
fixed 63 MB regardless of conversation length.

## Measured performance (fanless MacBook Air M3, 16 GB)

| | tokens/s |
|---|---|
| Chat decode (default — cool and quiet) | ~7–8 |
| Chat decode (`--fast`) | ~9.5 |
| Prompt reading (prefill) | ~14–24 |

RAM stays at 2–3 GB, swap at zero, and the engine writes nothing except your
session snapshot. Output is validated **bit-exact** against the reference
implementation of its quantized weights.

## Build from source

```sh
clang -O3 src/qwen36b.c src/expert_cache.c -o qwen36b -lm
# optional, ~2x faster: brew install libomp, then add:
#   -Xclang -fopenmp -I/opt/homebrew/opt/libomp/include -L/opt/homebrew/opt/libomp/lib -lomp
```

Standalone tests: `clang -O1 tests/test_expert_cache.c src/expert_cache.c -o t && ./t`

`tools/convert_qwen36.py` reproduces the int4 container from the original
checkpoint, shard by shard, in under 25 GB of working disk space.

## Platform support

- **macOS, Apple Silicon — tested.** Everything above.
- **Linux (x86_64/ARM)** — expected close: the code is POSIX and `kernels.h`
  carries an AVX2 path, but nobody has validated it yet. Reports and PRs
  welcome. A fast NVMe (~3 GB/s) matters more than the CPU.
- **Windows** — not natively (POSIX I/O throughout). WSL2 may work;
  unmeasured.

## Lineage

Jugnu is **inspired by and built on
[colibrì](https://github.com/JustVugg/colibri)** by JustVugg — the pure-C,
stream-the-experts-from-SSD design that proved a huge MoE can live on a small
machine. Jugnu shares colibrì's SIMD kernels and utility headers, and adds a
new engine for the Qwen3.6 architecture (Gated DeltaNet linear attention,
gated GQA, 256-expert MoE), a memory-bounded converter, instant-resume
sessions, and this distribution. *A firefly and a hummingbird: different
wings, same idea — small, free, and everywhere.*

## License

Apache-2.0 (see `LICENSE` and `NOTICE`). Model weights are Apache-2.0,
converted from Qwen3.6-35B-A3B — all credit to the Qwen team. Not affiliated
with or endorsed by Alibaba/Qwen or the colibrì project.
