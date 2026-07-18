# E-X10 M0 — native Metal grouped-q4 feasibility spike

Date: 2026-07-18. Branch: `experiments/e-x1-phase-baseline`.

## Verdict

**The Metal track survives M0, but expert-only decode is not yet a speed
win.** The custom group-32 bytes work directly on the M3 GPU, the fused GPU
kernel is faster and more energy-efficient than the CPU arithmetic when run
without inter-layer handoffs, and a bounded no-copy cache is viable. However,
the required CPU/GPU dependency at every transformer layer reverses the raw
kernel win:

| Warm S=1 routed-expert path, 40 layers | Median |
|---|---:|
| CPU, production grouped-q4 reference, 4 threads | 20.99–21.68 ms/token |
| GPU, one command buffer, no CPU dependencies | 15.87–16.02 ms/token |
| GPU, one command buffer, 40 real CPU/GPU event handoffs | 25.14–25.85 ms/token |
| GPU, same handoffs with CPU busy-polling | 23.30–23.53 ms/token |

The no-event row is an intentionally unrealizable lower bound: the CPU must
consume each layer's expert output before it can execute the next layer's
attention and router. Busy-polling still loses to the four-thread CPU and
burns a core, so it is not a product design.

**Decision by sub-track:**

- **M0 correctness / custom format: GO.**
- **Bounded Metal-backed expert cache: GO.**
- **Whole-file single-buffer mmap: NO-GO on this machine.**
- **M1 prefill with this first generic kernel: NO-GO; it loses to four-thread
  NEON at every tested S. A genuinely batch-tiled kernel is required before
  reconsidering M1.**
- **M2 expert-only engine integration: HOLD, not funded yet.** First measure
  whether CPU shared-expert work and pilot prefetch can occupy the GPU window
  enough to recover the 3–5 ms routed-expert latency deficit.
- **Energy hypothesis: GO.** GPU J/GFLOP is materially lower, with thermal
  pressure Nominal throughout.

No changes were made to `qwen36b` or its link line. The spike is a separate
binary and target.

## Scope added

- `tools/metal_spike.m`: standalone Objective-C/Metal probe with embedded MSL.
- `make metal-spike`: macOS-only experiment build, linked against Metal,
  Foundation, and Homebrew OpenMP.
- The normal `samosa-engine` and `omp` recipes are unchanged.

Link isolation was verified:

```text
$ otool -L metal-spike | head -7
metal-spike:
    .../Foundation.framework/.../Foundation
    .../Metal.framework/.../Metal
    /usr/lib/libSystem.B.dylib
    /opt/homebrew/opt/libomp/lib/libomp.dylib
    /usr/lib/libobjc.A.dylib

$ otool -L qwen36b
qwen36b:
    /usr/lib/libSystem.B.dylib
```

## Reference machine

```text
macOS 26.5.1 (25F80)
Darwin 25.5.0 arm64, Mac15,12
Apple M3, 10-core GPU, 16 GB unified memory
Apple clang 21.0.0
Metal device family: Apple9
```

Runtime device query:

```text
[device] name=Apple M3 unified=true
         max_buffer_bytes=9534832640
         recommended_working_set_bytes=12713115648
```

`experts.bin` is 20,942,159,872 bytes, so it cannot be represented by one
`MTLBuffer` on this device. Splitting it into several resources would not
resolve the physical-working-set risk and was deliberately not attempted.

## Correctness

The exact production CPU reference is
`matmul_i4_grouped_idot` from `src/kernels.h`: int8 activations, packed signed
q4 weights, group-32 F32 scales. Metal fast math was disabled.

```text
[correctness] source=synthetic S=3 I=2048 O=512
              max_abs=0 max_rel=0 verdict=PASS
[correctness] source=real key=model.layers.0.mlp.experts.0
              S=2 I=2048 O=512 max_abs=0 max_rel=0 verdict=PASS
[full-layer-correctness] experts=8
              max_abs=8.94069672e-08 max_rel=0.0329094259 verdict=PASS
```

The fused top-8 shader uses a different F32 reduction order to remove dozens
of SIMD reductions per row. Its maximum relative error occurs at outputs near
zero; the maximum absolute error is below `9e-8`. This is a class-2 numerical
path, not a bit-exact promise.

The fused S=1 GPU path uses three dispatches per layer:

1. eight experts' gate + up projections, fused with SiLU;
2. per-expert hidden activation quantization to int8;
3. eight down projections fused with router-weighted reduction.

It reads the production-sized 15,728,640 bytes selected per layer.

## Throughput

Command:

```sh
make metal-spike
./metal-spike
```

The generic exact matrix shader commits one command buffer per matrix. These
results decide whether the initial shader is suitable for M1; they are not the
fused M2 design:

| Shape | S | GPU GFLOP/s | CPU 1T | CPU 4T |
|---|---:|---:|---:|---:|
| gate/up, 2048→512 | 1 | 7.27 | 52.66 | 90.89 |
| gate/up | 8 | 40.38 | 51.72 | 159.20 |
| gate/up | 32 | 59.79 | 52.20 | 183.57 |
| gate/up | 128 | 73.72 | 51.38 | 186.11 |
| down, 512→2048 | 1 | 9.89 | 53.28 | 95.17 |
| down | 8 | 40.11 | 52.18 | 158.42 |
| down | 32 | 59.86 | 52.44 | 182.77 |
| down | 128 | 68.17 | 52.33 | 187.96 |

The fused top-8 S=1 path was repeated three times after an explicit GPU warmup:

```text
no-event GPU:    16.020, 15.868, 15.889 ms/token
event GPU:       25.571, 25.141, 25.848 ms/token
busy-poll GPU:   23.304, 23.533, 23.384 ms/token
CPU 4T:          21.684, 20.991, 20.988 ms/token
```

The first cold-frequency attempts were much noisier (event-pipeline medians
around 40 ms/token). The table and decision use the pre-registered warm decode
condition, while retaining the cold observation as a time-to-first-token risk.

## Synchronization

The synchronization probe encodes 40 wait → empty dispatch → signal sequences
inside one command buffer. Notifications run on a dedicated
user-interactive-QoS serial queue.

Three warmed repeats:

```text
median 114.635 us/round-trip -> 4.585 ms/token
median 113.978 us/round-trip -> 4.559 ms/token
median 114.995 us/round-trip -> 4.600 ms/token
```

This is below the card's 200 µs redesign threshold but large enough to erase
most of the raw 5–6 ms GPU arithmetic advantage. “GPU takes layer pairs” is
not a valid mitigation while the CPU owns the attention and router between
those layers.

## No-copy arena and mmap

The process page size is 16 KiB. A 16 MiB `posix_memalign(16384)` allocation
was wrapped with `newBufferWithBytesNoCopy`; the GPU read the byte written by
the CPU:

```text
[nocopy] page_bytes=16384 arena_bytes=16777216
         create=PASS gpu_read=42 verdict=PASS
```

For the bounded mmap leg, the spike sampled manifest entries using `mincore`
and selected a fully nonresident, page-aligned 1,966,080-byte real expert.
A representative run:

```text
[mmap] resident_before_pct=0.00 resident_after_pct=100.00
       first_gpu_ms=2.319 footprint_delta_mb=0.131
       system_wired_delta_mb=1.966
       whole_file_one_buffer=IMPOSSIBLE
```

The system-wide wired counter was noisy across runs, including negative deltas
while other processes released memory. The process-footprint delta remained
small. This proves bounded mapped access works; it does not authorize mapping
the full file.

Metal I/O queue creation also passed:

```text
[metal-io] queue_create=PASS
```

No load-throughput claim is made. `MTLIOCommandQueue` still needs an A/B
against `pread` into the same Metal-backed slots.

## Energy and thermal

The owner-started privileged collector was already running:

```sh
sudo /usr/bin/powermetrics \
  --samplers cpu_power,gpu_power,thermal -i 1000 \
  -o /tmp/samosa-e-x10-m0-powermetrics.log
```

Workloads:

```sh
./metal-spike --sustain gpu  --seconds 15
./metal-spike --sustain cpu4 --seconds 15
./metal-spike --sustain cpu1 --seconds 15
```

Power below is raw CPU+GPU package power, with no idle-baseline subtraction.
The short confirmation throughput runs were 76.667 GPU, 82.490 CPU 4T, and
44.469 CPU 1T GFLOP/s.

| Mode | Samples | CPU W | GPU W | Combined W | J/GFLOP |
|---|---:|---:|---:|---:|---:|
| GPU | 15 | 1.025 | 6.661 | 7.686 | **0.100** |
| CPU 4T | 16 | 13.990 | 0.088 | 14.079 | **0.171** |
| CPU 1T confirmation | 5 | 5.873 | 0.165 | 6.037 | **0.136** |

Every captured sample reported `Current pressure level: Nominal`; `pmset -g
therm` reported no thermal or performance warning after the runs.

The GPU used about 26% less raw energy per GFLOP than one CPU thread and 41%
less than four CPU threads. This passes M0's energy gate even though the
event-pipelined latency gate does not yet beat four-thread CPU execution.

## What the spike does and does not establish

Established:

- Samosa's shipped group-32 bytes are directly GPU-readable and correct.
- A bounded Metal-backed cache does not require a second expert copy.
- The fused GPU arithmetic has useful throughput and substantially better
  energy efficiency.
- Per-layer CPU/GPU synchronization is the current decode blocker.

Not established:

- Faster end-to-end decode.
- A real engine cache allocator or GPU-safe eviction lifetime.
- Overlap with the CPU shared expert or active pilot-prefetch thread.
- MTLIO throughput versus `pread`.
- Prefill acceleration.
- Quality-suite behavior over generated tokens.

The next experiment should not yet modify the production engine. Build a
single-layer integration harness that executes, concurrently:

```text
GPU: routed top-8 expert
CPU: shared expert + pilot router
I/O: predicted L+1 expert loads
```

Fund M2 only if that measured layer makes the event-pipelined GPU wall time no
worse than the CPU routed+shared baseline at lower J/token. The current result
is close enough to justify that bounded follow-up, but not a speed claim.

## Repository validation

```text
$ make metal-spike
exit 0

$ make test
exit 0
expert cache tests: ok (16,807 exhaustive traces + 50,000 random transitions)
kv_cache: 57529 checks passed
repetition guard tests: ok
thinking budget transition: ok
groupwise q4 and mixed q8-down tests: ok
samosa serve components: ok
samosa wrapper: PASS
atomic installer: PASS
install PATH setup: PASS
Python test suites: 26 tests, all OK
converter quant tests: SKIP (NumPy environment unavailable)

$ git diff --check
exit 0
```
