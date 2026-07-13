# Samosa Chat

Run **Qwen3.6-35B-A3B** (int4, text-only) locally on a **16 GB Apple Silicon
Mac**. No cloud, no account, no telemetry, no GPU.

Samosa Chat is ~4,000 lines of dependency-free C. It keeps 1.3 GB of dense weights
resident in RAM and streams the 16.6 GB of mixture-of-experts weights from
SSD on demand, so a 35B-parameter model fits in a 2–3 GB memory footprint.

## Examples

Real output from this model running on the 16 GB test machine, commands shown verbatim.

### A landing page

```sh
samosa --fast --think "Write a complete, single-file landing page (HTML with embedded CSS, \
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

Requirements: Apple Silicon Mac (M1 or newer), 16 GB RAM, ~25 GB free disk.
The installer checks the machine, downloads ~18 GB (resumable, every file
SHA-256-verified), compiles the engine locally, and runs a smoke test.
No admin rights needed. Uninstall: `rm -rf ~/.samosa`.

## Use

```sh
samosa "explain how a hash table handles collisions"
samosa --continue "and which strategy does Python use?"  # resumes last conversation
samosa --think "tricky logic puzzle"                     # chain-of-thought mode
samosa --fast "..."                                      # all P-cores
samosa doctor                                            # verify the install
```

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
| prefill, default | ~14 |
| prefill, `--fast` | ~24 |

Peak RSS 2–3 GB; zero swap; the engine performs no disk writes except the
session snapshot. Engine output is validated bit-exact against a
quantization-aware reference implementation (teacher-forcing and generation),
and the release configuration passed a gated benchmark suite (100-prompt
corpus, 15-minute soak, swap/write/thermal guardrails).

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

- int4 experts (per-row scales), int8 activations with SDOT/AVX2 integer-dot
  kernels; `IDOT=0` switches to exact f32 kernels for validation.
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

- The int4 conversion occasionally doubles a short function word during
  generation ("of of"), roughly once per ~10 longer answers. It is a
  quantization artifact of generation-time states, not present under
  teacher forcing; analysis notes live in the source history. Re-asking or a
  different seed avoids it. `--presence-penalty` and `--no-doubling` exist as
  experimental sampler knobs, but the artifact can bypass token-id-level
  penalties, so neither is a complete fix.
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
