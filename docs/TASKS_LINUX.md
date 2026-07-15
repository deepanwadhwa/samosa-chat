# Issue #1 — Optimize samosa for Linux

Read [ISSUE_TASKS.md](ISSUE_TASKS.md) first for shared ground truth and the
accuracy rules this program runs under.

## Framing: this is a restoration, not a port

The engine has Linux/x86 ancestry and macOS was the port, not the origin:

- [compat.h:1-4](../src/compat.h#L1-L4) describes itself as a shim "per
  piattaforme non-Linux (oggi: macOS / Apple Silicon)" and states plainly:
  "Su Linux questo header e' un NO-OP totale: nessun simbolo definito o
  ridefinito, zero impatto sul percorso x86 esistente."
- [st.h:94](../src/st.h#L94) cites a *measured* O_DIRECT benchmark on
  **ext4-in-VHDX** — 0.8 GB/s buffered vs 2.3+ GB/s direct. That is a WSL2 disk.
  Someone ran this engine on Linux and profiled its I/O.
- [kernels.h](../src/kernels.h) already carries `__AVX2__` and
  `__AVX512VNNI__ + __AVX512BW__` paths for every hot kernel (lines 11, 66, 87,
  151, 184, 207, 217, 227, 256, 272), alongside the `__ARM_NEON` ones.
- [qwen36b.c:1877-1882](../src/qwen36b.c#L1877-L1882) `mem_available_gb()`
  already reads `/proc/meminfo` `MemAvailable` on non-Apple.
- [qwen36b.c:54-60](../src/qwen36b.c#L54-L60) `peak_rss_gb()` already handles the
  Linux `ru_maxrss`-in-KB vs macOS-in-bytes difference.

So the job is to find what rotted, not to invent a platform layer. **Do not
start by writing an abstraction.** Start by compiling it (L0) and let the errors
scope the work.

### One thing that is already right, verified 2026-07-15

O_DIRECT on Linux requires the memory buffer, the file offset, **and** the
transfer length all be aligned. All three are satisfied by design:

- `ALIGNMENT_BYTES = 16 * 1024` in
  [convert_qwen36.py:16](../tools/convert_qwen36.py#L16); every expert blob is
  padded to a 16 KB multiple ([:349-350](../tools/convert_qwen36.py#L349-L350))
  and the packer asserts the invariant ([:883-884](../tools/convert_qwen36.py#L883-L884)).
- Confirmed against the shipped `manifest.json`: **all 10,496 expert entries have
  16 KB-aligned offsets and 16 KB-aligned sizes. Zero exceptions.**
- The read buffer is `posix_memalign(&m->seq_buf, 16384, span)`
  ([qwen36b.c:2622](../src/qwen36b.c#L2622)) — matching `ALIGNMENT_BYTES` exactly.

This was checked because the sequential prefill path at
[qwen36b.c:2628](../src/qwen36b.c#L2628) uses the O_DIRECT fd **unconditionally**
(unlike [:1828](../src/qwen36b.c#L1828), which gates on the `DIRECT` env var), so
on Linux it engages by default. It is fine. Filesystems that do not support
O_DIRECT at all (tmpfs, overlayfs, ZFS before OpenZFS 2.3) fail the `open()`,
yield `fd = -1`, and fall back to buffered reads — handled at
[st.h:87](../src/st.h#L87) and [qwen36b.c:2007](../src/qwen36b.c#L2007).

## The real gaps

Verified by inspection 2026-07-15. Each is a genuine defect on Linux today.

### G1 — `rss_gb()` reports peak, not current  **(correctness of every memory claim)**

[qwen36b.c:62-69](../src/qwen36b.c#L62-L69): the `TASK_VM_INFO` phys_footprint
read is `#ifdef __APPLE__`; the fallback is `peak_rss_gb()`, which is
`ru_maxrss` — a **high-water mark that never decreases**.

`/healthz`, the UI telemetry, and every memory number this project publishes
flow through this function. On Linux they would be monotonically
non-decreasing, and the entire verified memory story (2.51 GiB fresh → 3.91 GiB
plateau) would be unmeasurable — worse, a leak would be indistinguishable from
correct behavior.

Fix: read `/proc/self/statm` field 2 (resident pages × page size) or
`/proc/self/status` `VmRSS`. **Then document that Linux RSS and macOS
phys_footprint are not the same metric** — phys_footprint includes compressed
memory and accounts file-backed pages differently. Do not present them as
comparable numbers in the same table without saying so.

### G2 — No memory-pressure handling at all  **(biggest risk)**

[qwen36b.c:1927-1955](../src/qwen36b.c#L1927-L1955)
`ecache_service_pressure()` — the **entire body** is inside `#ifdef __APPLE__`.
On Linux the function does nothing.

This is not cosmetic. The byte-budgeted expert cache sizes itself against
available RAM and relies on this callback to shrink under pressure. On a 16 GB
Linux box with a 24 GB model streaming experts, the cache would grow and never
yield — straight into the OOM killer, which is less forgiving than macOS's
memory compressor.

Linux signals, in order of preference:

1. **cgroup v2** `memory.pressure`, `memory.current`, `memory.high`,
   `memory.max` — the correct answer inside containers, and the only one that
   sees a container's real limit.
2. **PSI** `/proc/pressure/memory` (`some avg10` / `full avg10`) — kernel ≥ 4.20
   with `CONFIG_PSI=y`. Not universally enabled; detect, don't assume.
3. **Fallback:** poll `/proc/meminfo` `MemAvailable` — the reader already exists
   at [qwen36b.c:1877](../src/qwen36b.c#L1877).

Keep the existing structure: the decimator (poll 1-in-16) and the 10-second
anti-thrash cooldown at [:1929-1939](../src/qwen36b.c#L1929-L1939) are sound and
platform-independent. Only the *signal* is Darwin-specific.

### G3 — Freed pages never return to the OS

[qwen36b.c:3900-3911](../src/qwen36b.c#L3900-L3911) is `#ifdef __APPLE__` and
calls `malloc_zone_pressure_relief`. Linux/glibc equivalent: `malloc_trim(0)`.

Without it, freed KV/scratch arenas stay in glibc's heap and RSS never drops
between turns. **musl has no `malloc_trim`** — on Alpine this must degrade
cleanly, not fail to compile.

Note that `eslot_pool_trim(m, 0)` on the line above it is portable and does the
load-bearing work; only the allocator hint is Darwin-specific.

### G4 — Thread defaults are Apple-only

[qwen36b.c:4343-4357](../src/qwen36b.c#L4343-L4357): the "half the P-cores" cool
default is `#if defined(_OPENMP) && defined(__APPLE__)`, keyed off
`hw.perflevel0.physicalcpu`.

On Linux, OpenMP defaults to *every* core. On a laptop that means heat and
throttling; on a 64-core server it means 64 threads thrashing a cache tuned for
2. Neither is a sensible default.

Linux equivalents: ARM big.LITTLE via
`/sys/devices/system/cpu/cpu*/cpu_capacity`; Intel hybrid via
`/sys/devices/cpu_core/cpus` vs `/sys/devices/cpu_atom/cpus`. **And respect
cgroup CPU quota** (`/sys/fs/cgroup/cpu.max`) — `sysconf(_SC_NPROCESSORS_ONLN)`
lies inside a container, and getting this wrong is the classic containerized-
inference bug.

### G5 — The installer is macOS-gated and has a portability bug

[dist/install.sh](../dist/install.sh):

| Line | Issue |
|---|---|
| [16-17](../dist/install.sh#L16-L17) | Hard `Darwin` + `arm64` gate — fails immediately |
| [18](../dist/install.sh#L18) | `sysctl -n hw.memsize` → `/proc/meminfo MemTotal` |
| [20-25](../dist/install.sh#L20-L25) | `xcode-select` preflight → per-distro cc + libomp check |
| [43](../dist/install.sh#L43), [94](../dist/install.sh#L94) | `shasum -a 256` → `sha256sum` (`shasum` needs perl) |
| [75](../dist/install.sh#L75) | `df -k … NR==2 {print $4}` — **long device names wrap to a second line on Linux, making `$4` wrong. Use `df -Pk`** (POSIX output guarantees one line per fs) |
| [113-118](../dist/install.sh#L113-L118) | Homebrew `libomp.dylib` paths → `libgomp` / `libomp.so` |
| [120](../dist/install.sh#L120) | `clang -Xclang -fopenmp` → gcc wants plain `-fopenmp`; detect the compiler |
| **[174](../dist/install.sh#L174)** | **`mv -fh` — `-h` is BSD-only. GNU `mv` has no `-h`.** This is the atomic symlink swap; the atomicity guarantee silently breaks on Linux |
| [157](../dist/install.sh#L157) | Smoke test greps for `'Your model. Your Mac.'` — a Mac-specific UI string |

The `mv -fh` one deserves emphasis: it is the atomic activation step the whole
versioned-release design rests on. The GNU idiom is
`ln -s target tmp && mv -T tmp link`. Plain `ln -sfn` is **not** atomic. Do not
"fix" this by making activation non-atomic on Linux.

### G6 — The launcher is macOS-gated

[dist/samosa](../dist/samosa): `OPEN="${SAMOSA_OPEN:-open}"`
([:41](../dist/samosa#L41)) → `xdg-open`; `sysctl -n hw.memsize` in `doctor`
([:139](../dist/samosa#L139)); `--fast` reads
`sysctl -n hw.perflevel0.physicalcpu` ([:169](../dist/samosa#L169)). `nohup`,
`kill -0`, and the PID file are already portable.

### G8 — Toolchain and userland variance  **VERIFIED DEFECTS, both reproduced 2026-07-15**

**Full evidence, with commands and output:
[../regressions/linux/report.md](regressions/linux/report.md).** Both defects are
**open** — neither fix is applied in the tree.

An earlier version of this document missed this whole class. It covered
BSD-vs-GNU differences (`mv -fh`) but not **implicit-linkage differences** or
**GNU-vs-GNU userland variance**. Both bit immediately.

**G8.1 — `Makefile:17` is missing `-lm`.** `test_kv_cache` does not link on
Linux: `undefined reference to sinf, expf, nextafterf, exp2f, sqrt`. macOS's
libSystem provides libm implicitly; glibc requires `-lm` explicitly. Lines 20
and 21 already pass `-lm`; line 17 does not. **`make test` exits 2 on Debian and
Ubuntu, aarch64 and x86_64.** Reproduced in `debian:bookworm-slim` and
`ubuntu:latest`.

**G8.2 — `install.sh:39` uses an awk interval expression mawk may not support.**
The manifest validator matches `/^[0-9a-f]{64}$/`. Debian bookworm's default awk
is **mawk 1.3.4 20200120, which does not implement interval expressions** and
treats the braces literally — proven: `"aaaa" ~ /^a{4}$/` does **not** match,
while `"a{4}" ~ /^a{4}$/` **does**. The validator therefore rejects every valid
manifest and the installer aborts at its first step with "release manifest is
malformed or unsafe". **The Linux installer cannot install on Debian stable.**
`tests/test_atomic_install.sh` fails as a result.

**And this is the trap: `ubuntu-latest` does not catch it.** Ubuntu 26.04 ships
mawk 1.3.4 **20260129**, which *does* support intervals and accepts the
manifest. A green CI leg would report success while Debian users are broken.

POSIX-safe rewrite, verified working under both mawk versions and gawk:

```awk
NF != 3 || length($1) != 64 || $1 !~ /^[0-9a-f]+$/ || $2 !~ /^[0-9]+$/ || ...
```

With G8.1 and G8.2 patched, the **full `make test` suite passes on Linux
(exit 0)** — verified in `debian:bookworm-slim`, aarch64. Nothing else is hiding
behind them.

**The generalization for whoever picks this up:** macOS's userland and libc are
one implementation. Linux is many. Differences to check deliberately, none of
which "does it compile on Ubuntu" will find:

- **Implicit linkage** — libm, and anything else libSystem folds in for free.
- **awk** — mawk (Debian/Ubuntu default) vs gawk (Fedora/Arch) vs busybox awk
  (Alpine), *and across mawk versions*. Intervals, `length()`, `-F '\t'`,
  `gensub` are all variance points.
- **shell** — `/bin/sh` is dash on Debian/Ubuntu, bash on Fedora, busybox ash on
  Alpine. `install.sh` and `dist/samosa` are `#!/bin/sh`; no bashisms.
- **coreutils vs busybox** — `df -P`, `stat`, `readlink -f`, `mv -T` flags all
  differ under busybox.
- **libc** — glibc vs musl (G3, E-L5).

### G9 — cgroup pressure signal counts page cache and over-triggers  **VERIFIED DEFECT, open**

**Full evidence: [../regressions/linux/real-model-run.md](regressions/linux/real-model-run.md).**
Found on the first real-model run on Linux (2026-07-15). Lives inside G2 — the
highest-risk change in the port.

`linux_memory_pressure_level()` ([qwen36b.c:1944-1948](../src/qwen36b.c#L1944-L1948))
uses `ratio = memory.current / memory.max` (CRITICAL >0.90, WARN >0.80). **cgroup
v2's `memory.current` includes the page cache**, which the engine fills by
streaming `experts.bin`. Measured at peak during a **2-token** generation:

```
cur=6.40 GB   anon=4.19 GB   file=2.11 GB (page cache)   limit=7.52 GB
ratio = 6.40/7.52 = 0.85  -> WARN fires
anon  = 4.19/7.52 = 0.56  -> no pressure actually exists
```

The engine evicted 323 MB of its own expert cache to relieve pressure caused by
the kernel's own reclaimable file cache. Result: `evictions=1803`,
`expert_hit=50/2509` (**2%**). The kernel reclaims page cache long before it
OOM-kills; 0.85-with-2.11 GB-of-file-cache is entirely safe.

macOS is unaffected: `kern.memorystatus_vm_pressure_level` is a kernel-computed
signal that already discounts reclaimable memory. The port replaced it with a
naive ratio, and that is where the semantics diverged.

Two tokens already tripped `pressure_critical=1`. A real conversation would fire
it on its 10 s cooldown indefinitely and continuously dump the expert cache.

**Fix — E-L3 decides:** either discount reclaimable cache (`anon` from
`memory.stat`, or `(current - file)/limit`), or **use PSI**
(`/sys/fs/cgroup/memory.pressure`), which measures real stall time rather than a
ratio and is closest in spirit to the macOS signal. PSI is already E-L3's leading
candidate. **G9 must be fixed before E-L2's plateau result means anything.**

### G10 — the AVX2/AVX512 kernels are dead code in every x86 build  **VERIFIED DEFECT, open**

**Full evidence: [../regressions/linux/x86-dispatch.md](regressions/linux/x86-dispatch.md).
Fix spec: [TASKS_HARDWARE.md](TASKS_HARDWARE.md) H2.** Open — no fix applied.

`install.sh` and the `Dockerfile` compile with `-O3` and **no `-march`**. Verified:
`gcc -O3` on x86-64 does **not** define `__AVX2__`. [kernels.h:66-78](../src/kernels.h#L66-L78)
dispatches at compile time and has **no `#else`**, so on x86 neither the AVX2 nor
the NEON branch exists, `i` stays 0, and the scalar remainder loop does **100% of
the work**.

**Measured cost — 7.6×** (`matmul_q`, I=2048 O=2048 S=1, arm64, scalar forced via
`-U__ARM_NEON`):

| Path | ms/call | GFLOP/s |
|---|---|---|
| NEON | 0.49 | **17.09** |
| SCALAR | 3.70 | **2.26** |

Checksums matched to five decimals — the scalar path is numerically fine, just
slow. NEON-vs-scalar is a **proxy** for AVX2-vs-scalar; AVX2 is 256-bit against
NEON's 128-bit, so the real x86 gap is plausibly larger.

**This corrects this document's own framing.** The "restoration, not a port"
argument above leans partly on "`kernels.h` already carries `__AVX2__` and
`__AVX512VNNI__` paths for every hot kernel". The code is there; **the shipped
configuration never reaches it.** Inherited kernels that never compile provide no
coverage and no confidence. The x86 hot path is not "already written" — it is
unreachable.

**`-march=native` is not the fix.** The Docker image ([#2](TASKS_WINDOWS.md)) is
built once for many CPUs, so `native` would `SIGILL` on older user hardware. And
Docker is the entire Windows/Linux delivery path. Runtime `cpuid` dispatch is
required — see [TASKS_HARDWARE.md](TASKS_HARDWARE.md) H2.

**Ordering trap.** Today x86 is slow-but-scalar and probably correct. Fixing G10
**activates AVX2/VNNI kernels that have never produced a token anywhere**, which
makes **E-L1 mandatory rather than optional**. And E-L1 cannot run on the
reference Mac: an amd64 container there reports no AVX2, no AVX512-VNNI, not even
SSE4.2. **G10 needs real x86 hardware to close.**

### G7 — CI is macOS-only

[.github/workflows/ci.yml](../.github/workflows/ci.yml) is `runs-on:
macos-latest` and asserts `uname -m == arm64`. Linux support that is not in CI
will rot again within a release. This is how the current situation happened.

## Experiments

### E-L0 — Does it compile and run today?  ~0.5 day  **RUN THIS FIRST**

Cheap, and it scopes everything else. Do not plan around predicted errors; go
get the real ones. **G8.1 and G8.2 were both found by running this for one hour
in Docker, after the port was written. Run it before.**

**A distro matrix, not a distro.** One distro is not Linux — G8.2 fails on
Debian and passes on Ubuntu, so "it works on Ubuntu" would have shipped a broken
Debian installer. Minimum matrix:

| Target | Why it is in the matrix |
|---|---|
| `debian:bookworm-slim` | mawk 1.3.4 20200120 (no intervals), dash `/bin/sh` — found G8.2 |
| `ubuntu:latest` | the CI runner; newer mawk — proves CI's blind spot |
| `alpine` | musl + busybox awk/coreutils (G3, E-L5) |
| `fedora` (optional) | gawk, different defaults |

× `linux/amd64` and `linux/arm64`. Docker is enough for all of it; no VM needed.

**Run all three, not just the first:**

1. `make` and `make omp` — and also `-march=x86-64-v3` (AVX2) and
   `-march=sapphirerapids` (AVX512-VNNI) on amd64, since those kernel paths
   compile separately.
2. **`make test`** — the suite is self-contained (stubs the engine and network,
   tiny fixtures, no 24 GB download per
   [ci.yml:37-39](../.github/workflows/ci.yml#L37-L39)), so it runs on a bare
   container. **This is the step that was skipped, and it is where both G8
   defects surface.** Compiling clean proves nothing about it.
3. `sh tests/test_atomic_install.sh` explicitly per distro — it exercises the
   real `install.sh` against a synthetic release and is the only thing that
   catches installer-level userland variance.

### The real model runs on Linux **today**, in Docker — no extra hardware

An earlier version of this card said "a real-model run **if a 32 GB+ Linux box
with 24 GB free exists**". That was wrong, and it is why no model ran on Linux
while the port was being written. **Docker on the existing Mac is an arm64 Linux
box**, and the model bind-mounts read-only — no copy, no hard-link, no second
machine. Verified 2026-07-15: it loads, generates correct tokens, and reports
`peak_rss=3.90 GB`, inside the macOS plateau band. It also **found G9**, which no
amount of `make test` would have.

```sh
docker run --rm --platform linux/arm64 --memory=7g \
  -v "$PWD":/src:ro \
  -v "$HOME/Documents/samosa-models/qwen36_group32_i8":/model:ro \
  -v "$HOME/.samosa/current":/tok:ro \
  debian:bookworm-slim bash -c '
    apt-get update -qq && apt-get install -y -qq gcc libgomp1
    cp -r /src /work && cd /work
    gcc -O3 -Wno-unused-function -pthread -fopenmp src/qwen36b.c src/expert_cache.c -o /tmp/q -lm
    OMP_NUM_THREADS=2 SNAP=/model /tmp/q --chat "Reply with exactly: hello" \
      --no-thinking --tokens 16 --seed 11 --tokenizer /tok/tokenizer_qwen36.json'
```

Requires Docker Desktop's VM at **≥ 6 GB** — it defaults to ~2 GB, which cannot
even load the model. That settings toggle was the entire blocker.

**For G2/G4, Docker is not a workaround — it is the only option.** cgroup
pressure and `cpu.max` handling cannot be exercised on bare macOS at all, because
macOS has no cgroups. `--memory=6g` is how you test them, and it is how G9 was
found.

**What Docker-on-Apple-Silicon still cannot do — do not imply otherwise:**

- **E-L1 x86 parity.** The container is aarch64, running the **same NEON kernels
  as macOS**. AVX2/AVX512-VNNI still never execute. QEMU amd64 can run them
  ~10–50× slower: enough for a bounded greedy parity check, useless for
  throughput.
- **Any throughput number.** The bind mount is virtiofs: measured **0.55–0.64
  GB/s** expert reads against the 2.3+ GB/s [st.h:94](../src/st.h#L94) recorded
  for O_DIRECT on ext4; decode fell to 0.44–0.76 tok/s. **That measures virtiofs,
  not the engine.** For real numbers, put the model in a Docker volume or use
  native Linux (E-L4).

If you skip the real-model run, say so — do not imply one ran.

**Deliverable:** a per-distro × per-arch table of build / `make test` /
`test_atomic_install.sh` results, with the actual output committed under
`docs/regressions/linux/`. That table, not this document, is the task breakdown
for L1 — and it is the evidence for any Linux claim in the README.

### E-L1 — x86 numerical parity  ~1–2 days  **Highest-information experiment**

The AVX2/VNNI kernels exist. **They have never been run against this model's
quantization** — group-32 symmetric q4 experts plus row-wise int8/int4 resident
weights (see [ISSUE_TASKS.md](ISSUE_TASKS.md)). The group-32 container postdates
the macOS port, and `tests/test_groupwise_q4.c` has only ever run on ARM. Both
schemes need separate parity checks: the group-32 expert path and the row-wise
resident path exercise different kernel entry points.

**Method.** Run `test_groupwise_q4` on x86 first. Then greedy-decode 256 tokens
from the real model on identical seeds, x86 vs the ARM reference, and diff.

**Set the acceptance correctly, or this experiment will lie to you.** NEON and
AVX2 reduce FP accumulations in a different order, so **bit-identical logits are
not expected and their absence is not a bug.** The meaningful criteria:

- Greedy token sequences identical for ≥ 256 tokens on 5 seeds.
- Per-token top-1 logit gap ≥ the observed cross-ISA numerical noise floor —
  measure the noise floor first, then judge divergence against it.
- If sequences diverge: find the first divergent token, dump both logit vectors,
  and determine whether it is reassociation noise near a near-tie (acceptable —
  document it) or a genuine kernel bug (not acceptable).

Run the AVX2 and the AVX512-VNNI paths separately. VNNI has its own int8
accumulation semantics and its own chance of being wrong.

### E-L2 — The 8-turn RSS plateau, on Linux  ~1 day  **The real gate**

This is the macOS release gate ported: an 8-turn repeated conversation loaded
fresh at 2.51 GiB, warmed to 3.91 GiB, and plateaued at 3.91–3.92 GiB. Before
the eslot-pool fix the same test grew ~210 MB/turn.

Run it on Linux, with and without the G3 `malloc_trim` fix, measuring via the G1
fix (`/proc/self/statm`), plus `smaps_rollup` Rss/Pss for cross-checking.

**Acceptance:** plateau within 10% of the macOS figure, or a documented
explanation of why glibc's arena behavior makes it differ. A different number
with a real explanation is a pass; an unexplained number is not.

### E-L3 — Which pressure signal fires in time?  ~1 day  **Gates G2's design**

Drive the machine into memory pressure with the model resident. Compare cgroup
v2 `memory.pressure`, PSI `/proc/pressure/memory`, and `MemAvailable` polling on:
latency from real pressure to signal, and false-positive rate.

The constraint the design must satisfy: at 5–7 tok/s with a 1-in-16 poll
decimator, the poll fires roughly every 2–3 seconds. **A signal slower than the
OOM killer is useless.** If none of the three is fast enough, that is a finding —
report it, and consider a proactive `MemAvailable` floor instead of a reactive
signal.

### E-L4 — Expert streaming across filesystems  ~1–2 days

The design is SSD-bound; ~5–7 tok/s assumes APFS on Apple NVMe. Measure decode
tok/s and expert read bandwidth on ext4, btrfs, xfs, and zfs; NVMe vs SATA SSD;
and with O_DIRECT forced off (`DIRECT=0`) vs the default-on path at
[:2628](../src/qwen36b.c#L2628).

btrfs and zfs with compression enabled are the interesting cases: `experts.bin`
is quantized and near-incompressible, so CoW+compression may cost real bandwidth
for nothing.

**Deliverable:** a filesystem → tok/s table for the README. This is what
"supported on Linux" has to mean — a measured configuration, not an aspiration.

### E-L5 — glibc vs musl  ~0.5 day

Alpine has no `malloc_trim` and a very different allocator. Either support it
with a measured plateau number or state that musl is unsupported. Do not leave it
ambiguous.

## Tasks

### L1 — Compile and run clean  ~1–2 days  (scoped by E-L0)

Fix what E-L0 found, **including the two known-open defects G8.1 (`Makefile:17`
missing `-lm`) and G8.2 (mawk interval expression in `install.sh:39`)**. Both
are reproduced, both have verified fixes in G8, neither is applied as of
2026-07-15.

Keep every platform difference in [compat.h](../src/compat.h) — that is the
file's stated contract ("ogni differenza di piattaforma vive QUI; i .c restano
puliti") and it is a good one. Resist scattering `#ifdef __linux__` through
`qwen36b.c`.

**Acceptance — evidence required per the working agreement, not assertion:**
`make`, `make omp`, and **`make test`** exit 0 on **every distro in E-L0's
matrix** × amd64 and arm64, with the output committed under
`docs/regressions/linux/`. No new warnings on macOS, and the macOS build
verified still green (it is currently the only shipping platform — breaking it
is worse than not shipping Linux).

### L2 — Telemetry and memory management  ~2 days  (G1, G2, G3)

**Acceptance:** E-L2 plateau passes; E-L3's chosen signal demonstrably reclaims
under induced pressure; `/healthz` reports a current-RSS number that tracks
`smaps_rollup` within 5%.

### L3 — Thread defaults  ~1 day  (G4)

**Acceptance:** on a hybrid laptop, default thread count matches the P-core-half
policy's intent; inside a container with `cpu.max` set to 2, exactly 2 threads
are used. `OMP_NUM_THREADS` still overrides everything.

### L4 — Installer and launcher  ~2 days  (G5, G6)

**Acceptance:** `tests/test_install_path.sh` and `tests/test_atomic_install.sh`
pass on Linux. Add a test that the activation swap is atomic under GNU coreutils
— a concurrent reader must never observe a missing or partial `current` symlink.

### L5 — CI  ~0.5 day  (G7)

Add `ubuntu-latest` (x86_64) to the matrix. Add aarch64 if a runner is available.

**`ubuntu-latest` alone is not sufficient and must not be treated as the support
claim** — G8.2 passes on Ubuntu and fails on Debian. Add a container-based leg
running E-L0's distro matrix (`debian:bookworm-slim`, `alpine`) via
`docker run`; it needs no extra runners and costs minutes.

**Acceptance:** the matrix is green; a macOS-only regression fails the Linux leg
(test this by deliberately breaking one, once); **and a Debian-only regression
fails the container leg — verify with G8.2 itself before fixing it.** The README
names which configurations CI covers and which it does not.

### L6 — Honest documentation  ~0.5 day

README and model card get exactly what was measured: distro, kernel, arch, libc,
filesystem, and the E-L4 tok/s table. The current claim is macOS/Apple Silicon,
one tested M3. The Linux claim must be equally specific.

**"Runs on Linux" is not an acceptable sentence.** "Verified on Ubuntu 24.04,
kernel 6.8, x86_64 + AVX2, glibc, ext4 on NVMe: 6.2 tok/s decode, 3.9 GiB
plateau" is.

## Non-goals

- Distro packages (.deb/.rpm/AUR). The `curl | sh` installer is the shipping
  channel; adding packaging before the engine is verified is premature.
- GPU/CUDA. Out of scope — the design is CPU + SSD streaming. The Metal
  prototype is the separate, already-planned performance track.
- 32-bit anything.

## Open questions

- **Which arch is primary?** The AVX2 path implies x86_64 was the original
  target, and x86_64 is most Linux desktops. aarch64 shares the NEON kernels with
  macOS and is therefore *cheaper* to validate. Recommend x86_64 primary (it
  exercises untested code, so it finds more), aarch64 secondary.
- **What is the minimum RAM claim on Linux?** macOS's 16 GB floor leans on the
  memory compressor, which Linux does not have (zram/zswap are opt-in). The
  honest Linux floor may be higher. E-L2 and E-L3 should answer this, and it must
  land in the installer's preflight — the current gate is
  [install.sh:18-19](../dist/install.sh#L18-L19).
