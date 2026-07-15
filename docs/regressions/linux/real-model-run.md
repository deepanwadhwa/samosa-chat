# E-L2 (partial): first real-model run on Linux — and a G2 defect

Ran on: 2026-07-15
Spec: [../../TASKS_LINUX.md](../../TASKS_LINUX.md) E-L2 / E-L3
Tree: `issue-1-linux` @ `61f69c3` (G8.1 + G8.2 fixes applied)

## Setup

Docker Desktop on a 16 GB M3 MacBook Air, VM raised to 8.32 GB / 6 CPU.

```
container:  debian:bookworm-slim, aarch64, gcc 12.2.0, --memory=7g
build:      gcc -O3 -Wno-unused-function -pthread -fopenmp ... -lm     -> ok
model:      bind-mounted READ-ONLY from ~/Documents/samosa-models/qwen36_group32_i8
            (virtiofs; not copied, not hard-linked)
cgroup:     memory.max = 7516192768 (7.52 GB), cpu.max = "max 100000"
run:        OMP_NUM_THREADS=2 SNAP=/model qwen36b --chat "Reply with exactly: hello" \
              --no-thinking --tokens 16 --seed 11 --tokenizer /tok/tokenizer_qwen36.json
```

**This is the first time the real model has ever produced a token on Linux.**
Docker on the existing Mac is a Linux box — no separate hardware was needed.
E-L0 previously said "if a 32 GB+ Linux box exists", which is why this was never
attempted. That framing was wrong.

## Result: the port works

```
output:  hello
[stats] prompt=17 generated=2 stop=model  peak_rss=3.90 GB
```

Correct output, correct EOS stop. **`peak_rss=3.90 GB` sits inside the macOS
plateau band (3.91–4.2 GiB)** — the memory shape carries over.

Three of the highest-risk Linux fixes are now verified against real behavior, not
just compilation:

| Gap | Evidence from the run |
|---|---|
| **G1** `/proc/self/statm` | `peak_rss=3.90 GB` — a plausible current-RSS figure in the macOS band. Previously would have reported peak-only. |
| **G2** cgroup pressure | `[ecache] memory pressure WARN: released 323.1 MB`, `pressure_critical=1` — **the Linux path fires.** It did not exist before. But see the defect below. |
| **G3** `malloc_trim` | `[memory] freed_pool=1.9 MB allocator_relief=trimmed` |
| **G4** `cpu.max` | cgroup limits read at startup |

`budget=2.07 GB (default-16-slot)` is **correct, not a bug** — the 16-slot budget
is the intended default on every platform; AUTO (`mem_available_gb()`) is opt-in.
`mem_available_gb()` correctly takes `min(cgroup, host)` at
[qwen36b.c:1988-1991](../../../src/qwen36b.c#L1988-L1991).

## DEFECT G9 — the cgroup pressure signal counts page cache and over-triggers

`linux_memory_pressure_level()` ([qwen36b.c:1944-1948](../../../src/qwen36b.c#L1944-L1948))
computes `ratio = memory.current / memory.max`, returning CRITICAL above 0.90 and
WARN above 0.80.

**cgroup v2's `memory.current` includes the page cache.** The engine streams
`experts.bin` through it, so the ratio tracks file cache, not memory pressure.

Sampled `/sys/fs/cgroup/memory.{current,stat}` at 2 s intervals during the run:

```
PEAK:  cur=6400155648 (6.40 GB)   anon=4189786112 (4.19 GB)   file=2113630208 (2.11 GB)
       ratio = 6.40 / 7.52 = 0.85   ->  WARN fires (>0.80)

after process exit:
       cur=3331338240   anon=847872 (~0)   file=2143748096 (2.14 GB)
       ^ page cache outlives the process: it is reclaimable, and not the engine's
```

**2.11 GB of the 6.40 GB is reclaimable page cache.** The engine's own memory is
`anon` = 4.19 GB, and `4.19 / 7.52 = 0.56` — far below the 0.80 threshold. **No
pressure existed.** The engine evicted 323 MB of its own expert cache because the
kernel's free file cache filled up.

Cost in this run: `evictions=1803`, `expert_hit=50/2509` — a **2% hit rate**.

**Why this is wrong, not merely conservative.** The kernel reclaims page cache
before it OOM-kills anything; hitting 0.85 with 2.11 GB of file cache is
completely safe. The engine is discarding useful anonymous cache to relieve
pressure that does not exist. macOS does not have this problem because
`kern.memorystatus_vm_pressure_level` is a kernel-computed signal that already
accounts for reclaimable memory — the Linux port reimplemented it as a naive
ratio, and that is where the semantics diverged.

**Impact.** This run generated 2 tokens and already tripped `pressure_critical=1`.
A real conversation streams far more of the 20.9 GB through page cache, so the
signal would fire on its 10 s cooldown indefinitely and continuously dump the
expert cache. **This is a Linux/container performance regression, and it lives in
G2 — the highest-risk change in the port.**

**Fix directions** (Resolved 2026-07-15):

Option 1 was implemented. A helper `read_cgroup_stat("file")` was added to read the reclaimable page cache size from `/sys/fs/cgroup/memory.stat`. This file cache value is now subtracted from `memory.current` in both `cgroup_mem_available_gb()` and `linux_memory_pressure_level()`. 

This guarantees that the computed ratios and available space reflect actual anonymous memory pressure (matching XNU's `phys_footprint` page exclusion semantics), preventing page-cache-induced false cache dumps.

## Not run — do not infer these

- **E-L1 x86 parity — still not run, and Docker on Apple Silicon cannot do it.**
  The container is aarch64, so it exercises the **same NEON kernels as macOS**.
  The AVX2 and AVX512-VNNI paths still have never executed. QEMU amd64 emulation
  could run them (~10–50× slower — a 16-token greedy check is feasible; a
  throughput number is not).
- **Throughput numbers here are invalid.** `prefill 1.61–1.77 tok/s, decode
  0.44–0.76 tok/s` versus ~14 / ~5–7 native. The model is bind-mounted through
  virtiofs — `expert_disk` read 4.83 GB in 7.5–8.8 s ≈ **0.55–0.64 GB/s**, against
  the 2.3+ GB/s that [st.h:94](../../../src/st.h#L94) measured for O_DIRECT on
  ext4. **This measures virtiofs, not the engine.** A real throughput number needs
  the model inside a Docker volume or on native Linux (E-L4).
- **E-L2 proper — not run.** This was a 2-token generation. The 8-turn repeated
  plateau (the macOS release gate that caught a 210 MB/turn leak) still needs
  running, and G9 must be fixed first or it will pollute the result.
- **E-L4, E-L5 — not run.**
