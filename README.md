<div align="center">
  <img src="assets/samosa-chat_medium.png" alt="Samosa Chat mascot" width="210">
  <h1>Samosa Chat</h1>
  <p><strong>Qwen3.6-35B-A3B, tested locally on a 16 GB Apple Silicon Mac.</strong></p>
  <p>Native Apple Silicon app &nbsp;·&nbsp; No cloud account &nbsp;·&nbsp; No telemetry</p>
</div>

> **Foundation and model credit.** Samosa Chat is built on
> [colibrì](https://github.com/JustVugg/colibri) by JustVugg. Its
> expert-streaming design, SIMD kernels, and core utility headers made this
> project possible. The model is the text portion of
> [Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B), created and
> released by the Qwen team. Samosa Chat is an independent, unofficial
> Apache-2.0 project and is not affiliated with or endorsed by either team.

Samosa Chat is a small C inference runtime for running Qwen's 35B-total,
3B-active Mixture-of-Experts model without loading all 35B parameters into
RAM. Dense weights stay resident; routed experts are read from SSD as the
model selects them. The current macOS build uses CPU SIMD and does **not** use
Metal or the Apple GPU. In other words: no dedicated GPU is required, even
though every Apple Silicon Mac physically includes an integrated GPU.

> **Platform scope:** CPU-only does **not** mean “any laptop with 16 GB RAM.”
> The current installer requires macOS on Apple Silicon (`arm64`) and rejects
> other platforms. The full product has been exercised on one 16 GB M3
> MacBook Air. Portable kernel branches exist, but Linux/x86 and Windows are
> not supported Samosa Chat products today.

This repository is deliberately text-only. Qwen3.6 is natively multimodal,
but Samosa's converted artifact omits the vision tower.

## What is available today

The published package and this repository are not yet at the same release
level. This distinction matters:

| surface | status | what it contains |
|---|---|---|
| [Hugging Face package](https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-int4) | usable CLI release | legacy whole-row int4 artifact, one-shot chat, thinking switch, exact session resume |
| GitHub `main` | source preview | interactive local chat app, revised thinking controls, groupwise-q4, resident server, cancellation, bounded queue, atomic-upgrade tooling, expanded tests |
| Browser app | implemented on `main` | `samosa app` opens a responsive local UI with SSE streaming, conversation history, thinking display, stop control, settings, and live telemetry |
| Group-32 artifact | local development baseline | converted and tested on one reference Mac; not published to Hugging Face |

The one-line installer therefore installs the **published CLI release**, not
the server/app preview. The next model upload must be treated as a separate,
verified release event.

## Install the published CLI release

```sh
curl -fsSL https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-int4/resolve/main/install.sh | sh
```

Requirements: an Apple Silicon Mac, 16 GB RAM, a C compiler from Apple's
Command Line Tools, and roughly 25 GB free disk space. The current download is
about 18 GB. The published installer resumes downloads, verifies SHA-256
checksums, compiles the engine locally, and runs a smoke test. It requires no
administrator privileges.

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"
samosa --think "solve this logic puzzle"
samosa --long "write a detailed explanation"
samosa --fast "summarize this design"
samosa --seed 11 "give me a deterministic sample"
samosa doctor
```

The published wrapper defaults to a 512-new-token ceiling; `--long` raises it
to 2,048. The model may stop earlier when it emits its end-of-turn token.

## What Samosa adds on top

The Qwen checkpoint and colibrì foundation are the starting point. Work added
in this repository includes:

- a Qwen3.6 text engine in C covering the 30 Gated DeltaNet layers, 10 gated
  attention layers, shared/routed MoE path, tokenizer behavior, and chat
  template;
- a shard-by-shard converter and manifest-based expert container;
- legacy row-q4 loading, group-32 symmetric q4, and an experimental mixed
  group-q4 gate/up plus row-q8 down-projection format;
- Apple NEON dot-product and portable AVX2 grouped-q4/q8-down paths;
- a byte-budget expert cache with LRU eviction, per-layer floors, reusable
  slabs, pressure monitoring, and structured I/O telemetry;
- geometry-bound, SHA-256-sealed `QWSESS01` sessions with atomic writes and
  byte-identical continuation from saved state;
- Qwen's published direct/general/precise-code sampling profiles;
- a bounded thinking-budget transition, separate natural/forced closure
  telemetry, and a repeated-token-cycle guard;
- a dependency-free C localhost server with JSON/SSE responses, a bounded
  FIFO, cooperative cancellation, health telemetry, and clean shutdown;
- a 32 KB framework-free local chat UI with no remote scripts, analytics, or
  external requests, packaged with the transparent Samosa mascot;
- an atomic, versioned installer design that verifies and smoke-tests an
  inactive release before switching it live;
- regression tooling for output structure, task-specific correctness,
  upstream controls, quantized kernels, route traces, installer rollback, and
  machine-pressure guardrails.

## Thinking controls on `main`

The source preview follows Qwen's published sampling recommendations:

| profile | temperature | top-p | top-k | presence penalty | internal thinking budget |
|---|---:|---:|---:|---:|---:|
| direct | 0.7 | 0.80 | 20 | 1.5 | disabled |
| general thinking | 1.0 | 0.95 | 20 | 1.5 | 1,024 |
| precise code/WebDev | 0.6 | 0.95 | 20 | 0.0 | 2,048 |

The source wrapper uses an 8,192-new-token **outer ceiling**, not a fixed answer
length. The model decides when to stop inside that ceiling. If thinking reaches
its internal budget, Samosa appends Qwen's trained natural-language early-stop
transition before `</think>`; it does not inject a bare closing token. The
transition is containment, not proof that the resulting answer is correct.

A six-run upstream-compatible FP8 control used 353–616 reasoning tokens on the
small arithmetic family. A matched local group-32 run closed naturally and
answered correctly after 933 generated tokens with a 1,024-token thinking
budget. That validates this path for one prompt family; it is **not** evidence
of broad benchmark parity or release-wide stability. See
[the upstream-control report](docs/UPSTREAM_CONTROL_2026-07-14.md) and
[regression ledger](docs/REGRESSION_LEDGER.md).

## Local app on `main`

The interactive app is implemented and bounded-real tested on `main`; it is
not in the current Hugging Face package yet.

```sh
samosa serve          # foreground server on 127.0.0.1:8642
samosa app            # background singleton + open the local chat UI
samosa serve --stop   # cooperative shutdown
```

Implemented endpoints:

- `GET /healthz`
- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/cancel`
- `POST /v1/shutdown`

Chat completions support JSON or SSE, separate reasoning/content deltas,
sampler controls, `thinking`, `thinking_budget`, `max_tokens`, and a sanitized
`conversation_id`. Requests are serialized through one model with a bounded
wait queue. The UI streams answers and reasoning separately, can stop
generation, stores its display transcript in browser-local storage, and shows
tokens/s, RSS, and thinking closure. Conversation snapshots are durable, but
the planned four-slot in-RAM LRU, server-side transcript index, and write
batching are not implemented yet. API details and measured acceptance are in
[docs/SERVE_API.md](docs/SERVE_API.md).

A real group-32 app-path check served the UI and logo, then streamed exactly
`Samosa app works locally.` through the browser endpoint. It stopped naturally
at Qwen's end-of-turn token, saved its session, decoded at 5.13 tok/s on two
threads, and peaked at 3.28 GB RSS. That check also caught and fixed an earlier
server bug that leaked special control tokens into visible output.

## Model layout and storage

Qwen describes the upstream language model as 35B parameters total with 3B
activated per token, 40 layers, 256 routed experts, and 8 routed plus 1 shared
expert active per MoE layer.

Samosa currently has two materially different artifacts:

| artifact | routed experts | resident weights | release status |
|---|---:|---:|---|
| published legacy row-q4 | 16.6 GB | 1.3 GB | available from Hugging Face |
| local group-32 q4 baseline | 20.94 GB | 3.02 GB | tested locally, not published |

Group-32 uses more scale data and larger resident row-q8 payloads. It reduced
measured weight reconstruction error relative to the original whole-row q4
format, but one successful reasoning control is not enough to call the artifact
release-stable. The mixed q4/q8-down format exists in code only; no full mixed
artifact was produced.

## Measured performance

All numbers below are from one fanless MacBook Air M3 with 16 GB RAM. They are
workload-specific observations, not cross-platform guarantees.

| artifact / workload | threads | result |
|---|---:|---:|
| published legacy, ordinary decode | 2 | typically 7–8 tok/s |
| published legacy, `--fast` decode | 4 | about 9.5 tok/s |
| published legacy, prefill | 2–4 | about 14–24 tok/s |
| group-32 direct control | 2 | 7.27 tok/s |
| group-32 933-token thinking control | 2 | 4.85 tok/s |
| selective-precision 5,000-token WebDev control | 4 | 6.47 tok/s |

Measured peak RSS was roughly 2.5–3 GB on legacy direct runs and 3.2–3.9 GB
on group-32 chat/server runs. Durable sessions write a roughly 63–70 MB sealed
snapshot at turn end. Model data itself is read-only.

The 933-token group-32 reasoning control reread 376.77 GB of expert data. That
read amplification is a real product constraint: longer thinking can be useful,
but it costs time and sustained SSD traffic even when memory and thermals stay
within bounds.

## Real output from the legacy row-q4 artifact

The following landing page was generated on the 16 GB reference machine from
the same legacy row-q4 model format as the published artifact. The current
published wrapper needs `--long` to leave enough room for the 1,807-token
answer. The rendered screenshot is unedited.

```sh
samosa --fast --long --seed 11 "Write a complete, single-file landing page (HTML with embedded CSS, \
no JavaScript) for a small specialty coffee shop called Kaapi. Include a hero section, \
a menu with four items, and a footer. Clean, modern, dark theme. Keep the CSS compact."
```

<p align="center"><img src="assets/example-landing.png" alt="Kaapi landing page generated by Samosa Chat" width="740"></p>

Exact-run stats: 1,807 generated tokens, 9.60 tok/s decode, 2.47 GB peak RSS.

The same published artifact generated the following Python utility:

```sh
samosa "Write a Python function merge_intervals(intervals) that merges overlapping \
intervals. Include type hints, a concise docstring, and three assert-based test cases."
```

```python
from typing import List, Tuple


def merge_intervals(intervals: List[Tuple[int, int]]) -> List[Tuple[int, int]]:
    """Merge overlapping intervals.

    Given a list of [start, end] pairs, merge all overlapping intervals.
    """

    if not intervals:
        return []

    # Sort by start time
    sorted_intervals = sorted(intervals, key=lambda x: x[0])

    merged = [sorted_intervals[0]]

    for current in sorted_intervals[1:]:
        last_start, last_end = merged[-1]
        new_start, new_end = current

        if new_start <= last_end:
            # Overlapping or adjacent, merge them
            merged[-1] = (last_start, max(last_end, new_end))
        else:
            merged.append(current)

    return merged
```

The function passed overlap, adjacency, empty-input, and unsorted-input tests.
Exact-run stats: 191 generated tokens, 11.19 tok/s decode, 2.53 GB peak RSS.

## Validation and release discipline

`make test` currently covers the expert cache, long-context KV math, repetition
guard, Qwen budget transition, grouped-q4/q8-down kernels, server components,
wrapper behavior, atomic installer rollback, output structure, route analysis,
and converter layouts. The OpenMP engine build and shell/Python syntax checks
also pass.

The earlier evaluation harness contained a serious false positive: substring
checks reported 14/15 passes even though 0/15 samples closed `</think>`.
Structural closure, natural-versus-forced termination, repetition, model stop,
and task correctness are now scored separately. The current evidence is still
too small to publish a general benchmark score. The intended evaluation ladder
is documented in [docs/BENCHMARK_PLAN.md](docs/BENCHMARK_PLAN.md).

## Build from source

```sh
make            # portable CPU build
make omp        # multithreaded build; requires libomp on macOS
make test       # bounded tests; does not run long real-model generation
```

The engine has no Python runtime dependency. Python is used by conversion,
analysis, and regression tooling. `tools/convert_qwen36.py` consumes the
original Qwen checkpoint; conversion is not part of a normal user install.

## Privacy and machine safety

- Inference is local. The engine contains no telemetry client and the server
  binds to IPv4 loopback only.
- The one-line installer contacts Hugging Face to download public release
  files; normal inference does not require a cloud account.
- The macOS build is CPU-only. It uses NEON/SDOT and optional OpenMP, not Metal.
- CPU-only describes the current compute backend, not cross-platform support.
- Two threads remain the cool default; `--fast` is explicit.
- The expert cache monitors pressure and can evict payloads before the OS is
  forced to swap.
- Cancellation is checked between generated tokens in server mode.
- Real-model regressions are bounded because a single long reasoning run can
  reread hundreds of gigabytes from the expert store.

## Known limitations

- The published artifact still uses coarse whole-row q4. It can produce
  isolated word-level defects such as `of ofof`; re-asking or changing the seed
  may avoid an instance but is not a complete fix.
- The group-32 baseline is promising, not broadly validated or published.
- In-RAM conversation slots, server-side transcript management, document chat,
  and web access remain roadmap items. Deleting a chat in the current UI
  removes its browser transcript but does not yet remove its server snapshot.
- Long-context generation coverage is still thin. A stack-overflow defect above
  4,096 tokens was fixed, but the bounded >4K/>8K release regression remains
  open.
- Only macOS on Apple Silicon has been exercised as a product. Linux paths are
  present but unvalidated; Windows is not supported by Samosa Chat.
- Metal acceleration is not implemented. Apple GPU work is a planned measured
  optimization; the current SSD-streaming path remains partly I/O-bound.
- Text only: images, video, audio, tool calling, and Qwen's vision tower are not
  supported.
- SSD speed and endurance matter because routed experts are streamed and can be
  reread many times during long answers.

## Roadmap and documentation

- [App task program](docs/APP_TASKS.md)
- [Resident server API and acceptance](docs/SERVE_API.md)
- [Thinking-mode diagnosis](docs/THINKING_DIAGNOSIS.md)
- [Group-32 baseline](docs/GROUP32_BASELINE.md)
- [Storage migration ledger](docs/STORAGE_MIGRATION_2026-07-14.md)
- [Upstream-compatible control](docs/UPSTREAM_CONTROL_2026-07-14.md)
- [Detailed work log](docs/WORK_LOG_2026-07-14.md)

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for the full attribution
and derivative-work notice.
