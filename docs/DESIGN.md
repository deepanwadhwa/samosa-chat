# Design: what Samosa is, and what it adds

The architecture, the group-32 format, what was tried and rejected, and the
principles behind the decisions. The [README](../README.md#what-this-is) has the
short version.

## What this is

Samosa Chat runs Qwen's 35-billion-parameter model on a Mac that has only 16 GB
of RAM.

The model is a Mixture of Experts. It has 35B parameters in total, but it only
uses about 3B of them for each token. Samosa never loads all 35B into memory.
The shared ("dense") weights stay in RAM. The expert weights are read from the
SSD as the model chooses which experts each token needs.

Samosa runs entirely on the CPU. It does not use Metal or the Apple GPU. You do
not need a dedicated GPU.

The model is text only. Qwen3.6 can also read images, but Samosa's converted
model leaves the image part out.

## The three principles

Every decision follows these three goals, in this order:

1. **It must be stable on a machine like this one** — a 16 GB Apple Silicon Mac.
   Memory stays bounded. It does not grow without limit. It stops at clear
   limits instead of crashing.
2. **It must be actually useful.** Real answers, real code, real multi-turn
   conversations. Not a demo that only loads.
3. **It must not wear out the machine.** Keep memory bounded so the system does
   not swap heavily. Use two threads by default so the Mac stays cool. Be
   deliberate with the heavy SSD read passes that drain battery/power and generate heat (explained below).

A feature is only called "released" once it meets all three. Until then it
stays in this repository as source.

## What Samosa adds on top

The Qwen model and the colibrì runtime are the starting point. This repository
adds:

- A Qwen3.6 text engine written in C. It covers the 30 Gated DeltaNet layers,
  the 10 gated attention layers, the shared and routed expert path, the
  tokenizer, and the chat template.
- A converter that turns the original Qwen checkpoint into Samosa's format,
  shard by shard, with a manifest-based container for the expert weights.
- Three weight formats: the older whole-row int4, the newer group-32 int4, and
  an experimental mixed format (group int4 for gate/up, row int8 for the
  down-projection).
- CPU math for those formats: Apple NEON dot-product on Apple Silicon, and a
  portable AVX2 path for other CPUs.
- An expert cache that keeps a fixed byte budget in RAM, drops the
  least-recently-used experts first, keeps a floor per layer, reuses freed
  memory, watches system memory pressure, and reports its I/O.
- Saved conversations (`QWSESS01` files) that are checked against the model
  geometry, sealed with a SHA-256 hash, written atomically, and can be resumed
  exactly.
- Qwen's published sampling settings for direct, general-thinking, and
  precise-code modes.
- A thinking-budget limit with a clean hand-off to the answer, separate counts
  for natural versus forced endings, and a guard that stops a repeating token
  loop.
- A local HTTP server in C with no dependencies: JSON or streaming replies, a
  bounded request queue, cancellation, health reporting, and clean shutdown.
- A 32 KB browser chat page with no external scripts, no analytics, and no
  outside requests, shipped with the Samosa logo.
- An installer that verifies and tests a new version in place before switching
  to it, and rolls back if the new version is bad.
- Test tooling for output structure, task correctness, upstream comparisons,
  the quantized math, route traces, installer rollback, and memory-pressure
  limits.

## What group-32 is

Group-32 is the model format Samosa Chat uses. This section explains what it
means, because it is the heart of the product.

Qwen describes the model as 35B parameters in total, about 3B used per token,
40 layers, 256 routed experts, and 8 routed plus 1 shared expert active in each
Mixture-of-Experts layer. To fit that on a 16 GB Mac, Samosa stores most weights
in int4 — 4 bits each instead of 16. Four bits cannot hold a real number on
their own, so each weight also needs a **scale**: a full-precision number that
the 4-bit value is multiplied by to reconstruct the original weight.

The question is how many weights share one scale.

- **Group-32** gives every block of **32 weights** its own scale. A scale only
  has to cover 32 nearby numbers, so it fits them closely. The reconstructed
  weights are close to the originals, and the model's output quality is good.
- The older approach (see below) gave a whole matrix **row** — hundreds or
  thousands of weights — a single scale. One scale cannot fit that many
  different numbers well, so the reconstructed weights drift from the originals
  and the output shows visible defects.

Finer scales cost more storage, which is why group-32 is larger on disk:

| part | group-32 (the product) | older whole-row |
|---|---:|---:|
| expert weights | 20.94 GB | 16.6 GB |
| shared weights | 3.02 GB | 1.3 GB |

Group-32 also keeps the down-projection weights at int8 (8 bits) rather than
int4, which is the main reason its shared weights are larger. The result is a
model that reconstructs the original Qwen weights with measurably less error
than the older format. It is what the app runs, and it is the published release:
[deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32](https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32).
Its quality is measured on one reference machine and one reasoning control, not
across a broad benchmark suite — see [Known limitations](#known-limitations).

## What we tried that did not work

**Whole-row int4 (the first format).** This was the first quantization attempt:
one int4 scale for an entire weight row. It is too coarse. A single scale has to
cover a whole row of weights with very different sizes, so many weights are
reconstructed inaccurately. In use this shows up as word-level defects such as
`of ofof`. Re-asking or changing the seed can avoid a given case but does not fix
the cause. This is exactly why group-32 was built, and group-32 replaced it as
the product. The old format still exists in an
[earlier Hugging Face repo](https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-int4),
kept only so existing installs keep working. Do not start there.

**Mixed int4/int8 format.** An attempt to use int4 for the gate and up
projections and int8 for the down projection across the whole model. The code
for it exists and its math is tested, but no full model was ever produced in
this format. It is not used by the product.

## Example output from the group-32 model

Everything here was generated by the **group-32 model** — the product — through
the app's chat endpoint in direct mode (no thinking, seed 11) on the 16 GB
reference Mac. The screenshot is not edited.

Asked to build its own landing page — a single HTML file with embedded CSS, no
JavaScript, dark theme, given the facts about Samosa Chat — the model produced
this:

<p align="center"><img src="assets/example-landing.png" alt="Landing page for Samosa Chat, generated by the group-32 model" width="740"></p>

This run: 2,528 tokens, 5.15 tokens/sec, 4.33 GB memory. The exact HTML it wrote
is saved at [assets/example-landing.html](assets/example-landing.html). (The
`brew install` line in the page is the model's own copy; the real install
command is the one above.)

Asked for a Python function, the group-32 model wrote this, and it passes its
own tests when run:

```python
from typing import List


def merge_intervals(intervals: List[List[int]]) -> List[List[int]]:
    """Merge overlapping intervals.

    Args:
        intervals: A list of intervals, each represented as [start, end].

    Returns:
        A list of non-overlapping intervals that cover all input intervals.
    """
    if not intervals:
        return []

    # Sort by start time
    sorted_intervals = sorted(intervals, key=lambda x: x[0])
    merged = [sorted_intervals[0]]

    for current in sorted_intervals[1:]:
        last = merged[-1]
        if current[0] <= last[1]:
            # Overlapping intervals, merge them
            last[1] = max(last[1], current[1])
        else:
            merged.append(current)

    return merged


# Test cases
assert merge_intervals([[1, 3], [2, 6], [8, 10], [15, 18]]) == [[1, 6], [8, 10], [15, 18]]
assert merge_intervals([[1, 4], [4, 5]]) == [[1, 5]]
assert merge_intervals([]) == []
```

This run: 279 tokens, 7.19 tokens/sec, 4.09 GB memory.
