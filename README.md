<div align="center">
  <img src="assets/samosa-chat_medium.png" alt="Samosa Chat mascot" width="210">
  <h1>Samosa Chat</h1>
  <p><strong>A capable local AI for the Mac you already own.</strong></p>
  <p>Qwen3.6-35B-A3B &nbsp;·&nbsp; 16 GB Apple Silicon &nbsp;·&nbsp; No cloud, account, telemetry, or GPU</p>
</div>

Samosa Chat runs **Qwen3.6-35B-A3B** (int4, text-only) locally on a **16 GB
Apple Silicon Mac**.

Samosa Chat is a dependency-free C inference engine and localhost app server.
The current group-32 development baseline keeps approximately 3.0 GB of dense
weights resident and streams its 20.94 GB mixture-of-experts store from SSD on
demand. Measured chat/server runs peak around 3.2–3.9 GB RSS on the reference
machine.

The CLI is usable today. The resident OpenAI-shaped server, bounded request
queue, SSE reasoning/content stream, cancellation, health telemetry, clean
shutdown, and `samosa app` launcher are implemented in the repository. The
full browser chat interface and in-RAM multi-conversation slot pool remain
under active development; see [docs/APP_TASKS.md](docs/APP_TASKS.md).

## Examples

Real output from this model running on the 16 GB test machine, commands shown verbatim.

### A landing page

```sh
samosa --fast --seed 11 "Write a complete, single-file landing page (HTML with embedded CSS, \
no JavaScript) for a small specialty coffee shop called Kaapi. Include a hero section, \
a menu with four items, and a footer. Clean, modern, dark theme. Keep the CSS compact."
```

<p align="center"><img src="assets/example-landing.png" alt="generated landing page, rendered" width="740"></p>

<details>
<summary>Full generated HTML (1,807 tokens, unedited)</summary>

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Kaapi | Specialty Coffee</title>
    <style>
        /* Reset & Base Styles */
        :root {
            --bg-color: #121212;
            --text-color: #E0E0E0;
            --accent-color: #D4A373; /* Warm Coffee Gold */
            --card-bg: #1E1E1E;
        }

        * { box-sizing: border-box; margin: 0; padding: 0; }

        body {
            background-color: var(--bg-color);
            color: var(--text-color);
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            line-height: 1.6;
        }

        a { color: inherit; text-decoration: none; }
        ul { list-style: none; }

        /* Layout Utilities */
        .container {
            max-width: 1100px;
            margin: 0 auto;
            padding: 0 20px;
        }

        /* Header */
        header {
            padding: 2rem 0;
            text-align: center;
            margin-bottom: 3rem;
        }

        h1 {
            font-size: 2.5rem;
            font-weight: 300;
            letter-spacing: -1px;
        }

        p.subtitle {
            color: var(--accent-color);
            text-transform: uppercase;
            letter-spacing: 2px;
            font-size: 0.875rem;
            margin-top: 0.5rem;
        }

        /* Hero Section */
        .hero {
            text-align: center;
            padding: 4rem 0;
            border-top: 1px solid #333;
            border-bottom: 1px solid #333;
            margin-bottom: 4rem;
        }

        h2 {
            font-size: 1.5rem;
            margin-bottom: 1rem;
        }

        .intro-text {
            max-width: 600px;
            margin: 0 auto;
            font-weight: 300;
            opacity: 0.8;
        }

        /* Menu Section */
        .menu-section {
            padding-bottom: 4rem;
        }

        .menu-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
        }

        .menu-item {
            background: var(--card-bg);
            padding: 25px;
            border-radius: 8px;
            border: 1px solid #333;
            transition: transform 0.2s ease;
        }

        .menu-item:hover {
            transform: translateY(-3px);
            border-color: var(--accent-color);
        }

        .item-header {
            display: flex;
            justify-content: space-between;
            align-items: baseline;
            margin-bottom: 10px;
        }

        .item-name {
            font-weight: 700;
            font-size: 1.1rem;
        }

        .item-price {
            color: var(--accent-color);
            font-weight: bold;
        }

        .item-desc {
            font-size: 0.9rem;
            opacity: 0.7;
            font-weight: 300;
        }

        /* Footer */
        footer {
            background: var(--card-bg);
            padding: 4rem 0;
            text-align: center;
            border-top: 1px solid #333;
        }

        .footer-info {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 2rem;
            margin-bottom: 3rem;
            text-align: left;
        }

        h4 { color: var(--accent-color); margin-bottom: 0.5rem; }

        .copyright {
            font-size: 0.8rem;
            opacity: 0.5;
        }
    </style>
</head>
<body>

    <div class="container">
        <header>
            <h1>Kaapi</h1>
            <p class="subtitle">Specialty Coffee Roasters</p>
        </header>

        <section class="hero">
            <h2>Pure. Roasted. Served.</h2>
            <p class="intro-text">
                We source single-origin beans from sustainable micro-lots. 
                Roasting in small batches to unlock flavors you haven't tasted yet.
                No sugar coating, just great coffee.
            </p>
        </section>

        <section class="menu-section">
            <ul class="menu-grid">
                <li class="menu-item">
                    <div class="item-header">
                        <span class="item-name">Etiopya</span>
                        <span class="item-price">$6.50</span>
                    </div>
                    <p class="item-desc">Yirgacheff Washed<br>Chamomile, Lemon Zest, Clean Finish</p>
                </li>
                <li class="menu-item">
                    <div class="item-header">
                        <span class="item-name">Tumba</span>
                        <span class="item-price">$6.75</span>
                    </div>
                    <p class="item-desc">Tumaco Supremo<br>Caramel Sweetness, Cream Body</p>
                </li>
                <li class="menu-item">
                    <div class="item-header">
                        <span class="item-name">Pacafist</span>
                        <span class="item-price">$7.00</span>
                    </div>
                    <p class="item-desc">Nariño Honey<br>Tropical Fruit, Peach Flesh</p>
                </li>
                <li class="menu-item">
                    <div class="item-header">
                        <span class="item-name">Mandev</span>
                        <span class="item-price">$6.25</span>
                    </div>
                    <p class="item-desc">Sud Valley Washed<br>Pumpkin Spiced, Milk Chocolate</p>
                </li>
            </ul>
        </section>
    </div>

    <footer>
        <div class="container">
            <div class="footer-info">
                <div>
                    <h4>Find Us</h4>
                    <p>4234 24th St<br>Metro, CA 94127</p>
                </div>
                <div>
                    <h4>Cafe</h4>
                    <p>Mon - Fri: 7am - 4pm<br>Sat - Sun: 8am - 4pm</p>
                </div>
                <div>
                    <h4>Keep in Touch</h4>
                    <p>hello@kaapi.com<br>@kaapi.coffee</p>
                </div>
            </div>
            <p class="copyright">&copy; 2024 Kaapi. All rights reserved.</p>
        </div>
    </footer>

</body>
</html>
```

</details>

Engine stats for this exact run (seed 11, 4 threads):
`generated=1807 prefill 16.3 tok/s decode 9.60 tok/s peak_rss=2.47 GB`

### A Python utility

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

Output verified: the function passes overlap, adjacency, empty-input, and
unsorted-input tests. Stats: `generated=191 decode 11.19 tok/s peak_rss=2.53 GB`.

## Install

```sh
curl -fsSL https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-int4/resolve/main/install.sh | sh
```

Requirements: Apple Silicon Mac (M1 or newer) and 16 GB RAM. Disk need is
release-specific; the installer reads exact artifact sizes before downloading
and preserves a 2 GB completion margin. Allow roughly 30 GB free for the local
groupwise build. Downloads are resumable and staged as an inactive versioned
release. Every byte size and SHA-256 digest, the compiled engine, and a smoke
test must pass before one atomic `current` pointer switches. A failed or corrupt
upgrade leaves the live release untouched. No admin rights needed. Uninstall:
`rm -rf ~/.samosa`.

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
samosa doctor                                            # verify the install
```

The resident server binds only to `127.0.0.1:8642` and exposes
`GET /healthz`, `GET /v1/models`, `POST /v1/chat/completions`,
`POST /v1/cancel`, and `POST /v1/shutdown`. Chat completions support ordinary
JSON and SSE streaming, `thinking: "off" | "general" | "code"`,
`thinking_budget`, `max_tokens`, sampler controls, and an optional sanitized
`conversation_id` backed by sealed session snapshots.

Thinking modes use Qwen3.6's task-specific sampling profiles. General
reasoning uses temperature 1.0 with presence penalty 1.5; precise coding uses
temperature 0.6 without a presence penalty. The distinction matters: applying
the general temperature to WebDev, or omitting the general presence penalty,
can send otherwise valid reasoning into a repetition loop. Precise coding also
uses float activations specifically for the routed/shared expert down
projections; this avoids the long-output attractor reproduced by the fully
W4A8 path without imposing the roughly 4 tok/s cost of full float activations.
See the controlled same-seed ledger in
[docs/THINKING_DIAGNOSIS.md](docs/THINKING_DIAGNOSIS.md).

The token setting is a ceiling, not a fixed answer size. The model normally
stops early on its end-of-turn token. Every profile allows up to 8,192 new
tokens by default; `--max-tokens N` overrides the outer ceiling. General
reasoning has a 1,024-token internal thinking budget and precise code has
2,048. The model may finish reasoning earlier; otherwise Samosa appends Qwen's
published natural-language early-stop transition before `</think>`, then
preserves the remaining budget for the requested answer. A bare control-token
injection is deliberately not used because it does not match Qwen's trained
budget protocol.
`--thinking-budget N` overrides that safety bound. Samosa prints an explicit
notice if the outer ceiling is reached, because that answer may be incomplete.
A repeated-token-cycle guard stops pathological loops early.

For example, `samosa --think --thinking-budget 2048 "your prompt"` permits a
longer reasoning trajectory. A higher budget does not change the arithmetic
performed per token or force the model to consume the whole allowance; it can
increase total response time and expert-cache traffic when the model chooses
to think longer. The current 1,024 general default remains the cooler bounded
choice pending upstream-calibrated results from more than the arithmetic pilot.

`--continue` restores the previous conversation from a ~70 MB snapshot
instead of re-processing the history, including across reboots. 30 of the
model's 40 layers are DeltaNet linear-attention layers with a fixed 63 MB
state, and only 10 layers keep a KV cache (~40 KB/token), which is what makes
the snapshot small and the resume exact: continuation output is byte-identical
to an uninterrupted session.

## Performance (measured, MacBook Air M3 16 GB, fanless)

| workload | tokens/s |
|---|---|
| decode, default (2 threads) | 7–8 |
| decode, `--fast` (4 threads) | ~9.5 |
| recalibrated 933-token thinking control (2 threads) | 4.85 |
| 5,000-token WebDev, selective-precision control (4 threads) | 6.47 |
| 5,000-token WebDev, full float-activation validation (4 threads) | 4.19 |
| prefill, default | ~14 |
| prefill, `--fast` | ~24 |

Measured peak RSS ranges from about 2.5 GB on the legacy direct path to
3.2–3.9 GB on the group-32 chat/server path. The engine performs no model-data
writes; durable conversations atomically replace a roughly 63–70 MB sealed
session snapshot at turn end. The 5,000-token stability control peaked at
2.48 GB RSS, left 69% system memory free, held swap at 5 MB, and triggered no
macOS thermal or performance warning. The engine has quantization-aware
teacher-forcing checks, but release quality also requires structural generation
checks; the older substring-only suite did not detect unfinished thinking.

## Evaluation

There is no single benchmark suite run identically by every model company.
Samosa's reproducible evaluation ladder covers runtime stability first, then
public knowledge/reasoning, coding, instruction-following, long-context, and
agent benchmarks with pinned prompts and dataset revisions. See
[docs/BENCHMARK_PLAN.md](docs/BENCHMARK_PLAN.md). Results are not compared with
upstream unless the template, sampler, tool scaffold, context limit, and
scorer match.

## Build from source

```sh
make            # portable single-threaded build
make omp        # multithreaded (brew install libomp first)
make test       # standalone cache test suites
```

`tools/convert_qwen36.py` reproduces the int4 container from the original
checkpoint shard-by-shard in under 25 GB of working disk.

## Platform support

- **macOS, Apple Silicon** — tested.
- **Linux (x86_64/ARM)** — untested; the code is POSIX and `kernels.h` has an
  AVX2 path. A fast NVMe (~3 GB/s) matters more than the CPU. PRs welcome.
- **Windows** — not supported natively (POSIX I/O). WSL2 untested.

## Architecture notes

- legacy int4 experts use per-row scales; the replacement groupwise-q4 format
  and kernels are implemented and awaiting a reconverted release artifact.
  Quantized weights normally use int8 activations and SDOT/AVX2 integer-dot
  kernels. Precise coding keeps the MoE down-projection input in float;
  `IDOT=0` switches every quantized matmul to float activations for validation.
- Expert blobs are 16 KB-aligned and streamed with F_NOCACHE + F_RDADVISE;
  a byte-budget LRU cache with per-layer floors handles residency, and a
  kernel memory-pressure poll evicts under system pressure (zero-swap
  verified under a forced WARN storm).
- Gated DeltaNet recurrence and causal-conv are parallelized per-head /
  per-channel with the per-chain float operation order preserved
  (bit-identical to the sequential implementation).
- Sessions (`QWSESS01`): geometry-bound, SHA-256-sealed snapshots written
  atomically (tmp + fsync + rename).

## Roadmap

A local chat app on this engine — ChatGPT-style UI in the browser, document
chat built on instant-resume sessions, and user-initiated web access — is
specified with measured acceptance gates in
[docs/APP_TASKS.md](docs/APP_TASKS.md).

## Known limitations

- The int4 conversion can still produce isolated word-level defects such as
  `of ofof`, even on the full float-activation validation path. This is
  distinct from the catastrophic W4A8 repetition attractor fixed by the
  selective thinking path. Re-asking or a different seed can avoid a local
  defect; token-level penalties are not a complete fix.
- Text-only: the vision tower of the upstream model is not included.

## Credits and license

Built on [colibrì](https://github.com/JustVugg/colibri) by JustVugg: the
expert-streaming design and the SIMD kernels (`src/kernels.h`) and utility
headers originate there. Samosa Chat adds the Qwen3.6 engine (DeltaNet, gated GQA,
256-expert MoE), the converter, sessions, and the distribution.

Weights converted from
[Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) — credit
to the Qwen team.

Apache-2.0 (`LICENSE`, `NOTICE`). Not affiliated with or endorsed by
Alibaba/Qwen or the colibrì project.
