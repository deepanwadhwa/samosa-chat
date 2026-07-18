# E-X10 M1 — CPU + Metal + SSD system experiment

Date: 2026-07-18. Branch: `experiments/e-x1-phase-baseline`.

## Verdict

**No tested Metal configuration reliably beats the four-thread CPU engine on
real decode. Do not promote the backend.**

This experiment integrated the real engine rather than timing an isolated
matrix:

```text
SSD predictor for layer L+1
            |
            v
bounded expert slabs --no-copy--> Metal: N routed experts
            |                     CPU: 8-N routed + shared expert
            +-------- LRU/cache ownership waits for both --------+
```

The best split was `N=1`. Its GPU work was fully covered by CPU work, but
submit/wait overhead and unified-memory contention consumed the saved CPU
arithmetic. Three alternating matched trials:

| Mode | Trial 1 | Trial 2 | Trial 3 | Median |
|---|---:|---:|---:|---:|
| CPU 4T | 8.08 | 8.08 | 8.05 tok/s | **8.08** |
| GPU 1 + CPU 7 | 8.02 | 7.97 | 8.09 tok/s | **8.02** |

Generated output was byte-for-byte identical in every comparison.

A longer 80-token run reached 8.77 tok/s on CPU and 8.76 tok/s with the
GPU-1 split. Decode-window power was 9.025 W and 9.106 W respectively; both
stayed at thermal pressure `Nominal`. The near-tie therefore does not buy an
energy win either.

## What was implemented

- `src/metal_expert.m`: native Metal grouped-q4 gate/up/down backend.
- Eight independent LRU slabs are bound through a Metal argument buffer; no
  per-layer expert join or format conversion occurs.
- Expert slabs use 16 KiB alignment and retain a no-copy `MTLBuffer` view
  through scratch/cache ownership swaps.
- `SAMOSA_METAL_EXPERTS=N` splits the selected eight experts between GPU and
  CPU. CPU routed work and the shared expert execute while Metal is in flight.
- Cache admission/eviction occurs only after GPU completion.
- Optional concurrent `MTLIOCommandQueue` predictor loads are available with
  `SAMOSA_MTLIO=1`.
- `make metal-omp` produces the isolated `qwen36b-metal` experiment binary.
  The normal `qwen36b` build and installer remain CPU-only.

Runtime remains doubly opt-in:

```sh
make metal-omp
SAMOSA_METAL=1 SAMOSA_METAL_EXPERTS=1 ./qwen36b-metal ...
```

## Why M0 was too optimistic

M0's 0.4 ms/layer fused result reused one synthetic 15 MB top-8 buffer for
forty emulated layers. That is a useful warm arithmetic lower bound, but it
does not model the real engine. Production decode touches a different set of
expert pages at every transformer layer.

With real no-copy LRU slabs, all eight routed experts require about
1.67 ms of measured GPU execution per layer. The failure is not just the
roughly 0.1 ms CPU/GPU handoff: cold per-layer unified-memory traffic is the
dominant difference from M0.

## Routed split sweep

Matched prompt, 17 timed decode tokens, four OpenMP threads, predictor enabled:

| GPU experts | CPU experts | GPU ms/layer | CPU overlap ms/layer | Wait tail ms/layer | Decode |
|---:|---:|---:|---:|---:|---:|
| 0 | 8 | — | — | — | **7.85 tok/s** |
| 1 | 7 | 0.347 | 0.743 | 0.105 | 7.56 tok/s |
| 2 | 6 | 0.520 | 0.650 | 0.319 | 7.23 tok/s |
| 4 | 4 | 0.927 | 0.488 | 0.925 | 6.44 tok/s |
| 6 | 2 | 1.300 | 0.323 | 1.469 | 5.82 tok/s |
| 8 | 0 | 1.674 | 0.131 | 2.043 | 5.41 tok/s |

The GPU-1 row is the only sensible split. With predictor disabled it became a
repeatable statistical tie rather than a regression, but did not cross the
CPU median.

## SSD findings

The true demand-only CPU baseline spent 53.7 ms/token blocked in expert reads.
Kernel readahead reduced that to 51.8 ms/token and moved decode from 7.88 to
8.00 tok/s in the representative pair.

The trained layer-L to layer-L+1 predictor can reduce the visible disk bucket,
but its loader and memory traffic slow concurrent CPU math enough to erase the
gain on this machine. MTLIO made the trade worse:

| Configuration | Expert disk | Decode |
|---|---:|---:|
| CPU demand + kernel readahead | 51.8 ms/token | **8.00 tok/s** |
| GPU 1 / CPU 7, predictor off | 48.0 ms/token | 7.98 tok/s |
| GPU 1 / CPU 7, pread predictor | 46.5 ms/token | 7.56 tok/s |
| GPU 1 / CPU 7, MTLIO predictor | 57.3 ms/token | 7.08 tok/s |

MTLIO consumed more predicted entries, but its direct concurrent traffic
competed with demand reads and did not provide useful page-cache warming.

## Unified-memory cache sweep

The default 16-slot/layer policy holds about 1.29 GB of expert payload. Explicit
larger byte budgets increased hit rate but slowed the scattered CPU matmuls and
eventually caused memory pressure:

| Cache budget | Hit rate | Physical RSS | Decode |
|---:|---:|---:|---:|
| default (~1.29 GB payload) | 26.9% | 4.33 GB | **8.00 tok/s** |
| 2 GB | 44.2% | 4.99 GB | 7.79 tok/s |
| 4 GB | 51.6% | 6.86 GB | 7.05 tok/s |
| 6 GB | 55.1% | 8.61 GB | 4.38 tok/s |

The 6 GB run recorded a critical memory-pressure event. More residency is not
automatically faster on a 16 GB unified-memory machine.

## Power and thermal

The existing privileged `powermetrics` collector sampled one-second decode
windows. The first four seconds (initialization/prefill) and final second were
excluded.

| Mode | Decode | CPU W | GPU W | CPU+GPU W | Thermal |
|---|---:|---:|---:|---:|---|
| CPU 4T | 8.77 tok/s | 8.880 | 0.145 | **9.025** | Nominal |
| GPU 1 + CPU 7 | 8.76 tok/s | 8.783 | 0.323 | **9.106** | Nominal |

Unlike M0's repeated warm arithmetic loop, the real hybrid path does not
improve joules per decoded token.

## Decision

- **Custom-format correctness:** PASS.
- **No-copy bounded cache ownership:** PASS.
- **All-routed Metal decode:** NO-GO.
- **Split CPU/GPU routed decode:** NO-GO for promotion; N=1 is a tie.
- **MTLIO predictor:** NO-GO.
- **Larger unified-memory expert cache:** NO-GO.
- **Reliable 9–10 tok/s from this architecture:** not demonstrated.

The next Metal work should not be another setting sweep. It would need a
different algorithmic unit of work that reuses weights or batches tokens,
such as speculative verification/prefill. For ordinary S=1 autoregressive
decode, the CPU dependency at every layer and cold expert working set leave no
profitable GPU slice on this M3.

## Verification

```text
make metal-omp       PASS
make test            PASS
NumPy converter tests: 5/5 PASS via .venv/bin/python (NumPy 2.5.1)
matched generated output: PASS for every CPU/GPU split
thermal pressure: Nominal except the intentional 6 GB cache stress run
```
