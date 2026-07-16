# Hardware task program — get the best from the user's machine, honestly

Read [ISSUE_TASKS.md](ISSUE_TASKS.md) first, including the Working agreement.

This program cuts across issues [#1](TASKS_LINUX.md) and [#2](TASKS_WINDOWS.md)
and also touches macOS. It is not a GitHub issue; it is the answer to a question
the project owner asked on 2026-07-15:

> *"How can we fix it such that a user gets the best out of their hardware
> without killing their hardware?"*

**The short answer: one of the two things we are trading off is not real.**
Samosa is not wearing out anyone's SSD. What streaming actually costs is time,
power, and heat. Meanwhile the single biggest performance lever — vectorization —
is *free*: it makes the machine **cooler**, not hotter, because it finishes the
same math sooner. There is much less tension here than the current
documentation implies.

## Verified ground truth (measured 2026-07-15)

| Fact | Evidence |
|---|---|
| The scalar path is **7.6× slower** than vectorized: NEON 17.09 GFLOP/s / 0.49 ms vs SCALAR 2.26 GFLOP/s / 3.70 ms (`matmul_q`, I=2048 O=2048 S=1, 300 reps, 1 thread, arm64). Checksums agree to 5 decimals — scalar is numerically fine, just slow. | [regressions/linux/x86-dispatch.md](regressions/linux/x86-dispatch.md) |
| **Every x86 build ships the scalar path.** `install.sh` and the `Dockerfile` compile with `-O3` and **no `-march`**; `gcc -O3` on x86-64 does not define `__AVX2__`; [kernels.h:66-78](../src/kernels.h#L66-L78) has no `#else`, so the scalar remainder does 100% of the work. | same |
| **There is zero thermal feedback in the engine.** grep finds no thermal state, `thermal_zone`, or throttle detection anywhere. | grep, 2026-07-15 |
| Storage dominates: **2.3+ GB/s** (ext4+O_DIRECT/NVMe) → 5–7 tok/s; **0.55–0.64 GB/s** (virtiofs) → **0.92–0.96 tok/s**. Same CPU, same binary, 6× from storage alone. | [st.h:94](../src/st.h#L94), [regressions/linux/docker-product-path.md](regressions/linux/docker-product-path.md) |
| A **larger engine cache is slower**: the 2026-07-12 production A/B measured matmul **+41%** from cold slabs scattered over 6 GB. This is why `EBUDGET_AUTO` is opt-in. | [qwen36b.c:2251-2254](../src/qwen36b.c#L2251-L2254) |
| Decode reads are **buffered** (`g_direct` defaults 0), so they flow through the OS page cache and scale with the user's RAM for free. | [qwen36b.c:1841](../src/qwen36b.c#L1841), [:2101](../src/qwen36b.c#L2101) |

## The shape of the answer

- **Free wins, no tradeoff:** H2 (vectorization). Same work, fewer instructions,
  finishes sooner, less energy per token. Strictly cooler *and* faster.
- **Already works, don't break it:** the OS page cache. Decode reads are buffered
  ([qwen36b.c:1841](../src/qwen36b.c#L1841) gates O_DIRECT behind `DIRECT`, which
  defaults to 0 at [:2101](../src/qwen36b.c#L2101)), so on a 32–64 GB machine the
  20.9 GB expert file gets largely cached and the SSD goes quiet after warmup.
  G9's fix (subtracting reclaimable `file` from the pressure ratio) is what lets
  this happen without tripping false evictions. **Do not "improve" this by
  growing the engine's own LRU** — that was measured 41% *slower*.
- **Real tradeoff, needs a policy:** H3 (threads vs heat). Currently a hardcoded
  guess from one fanless M3.
- **A false tradeoff to delete:** H1 (SSD endurance).
- **Left on the table, now first-class:** H5 (host-capability profile). A big
  machine should run bigger *without the user knowing the flags* — RAM via the
  page cache, cores via `--fast`/threads — but never by growing the LRU, and
  never claimed beyond what a tier was measured to do.

---

## H1 — Correct the SSD-endurance claim  ~0.5 day  **Owner decision required**

**Status (2026-07-15): REWRITTEN in-tree. Not published — that is still gated.**
[README.md](../README.md) and [dist/MODEL_CARD.md](../dist/MODEL_CARD.md) now
state that endurance is consumed by writes, cite TBW/JEDEC JESD218 and DWPD, keep
the 376 GB figure, and recommend direct mode for power/heat rather than for
hardware protection.

Corrected on review the same day:

- **The swap comparison was vestigial.** The rewrite deleted the wrong claim but
  left "swap is tiny here: 9 GB" with its point removed. The inversion is the
  interesting part and is now stated: **those 9 GB of *writes* consume more drive
  life than the 376 GB of reads do.** The scary number was the wrong number.
- **Provenance is now explicit.** E-H4's measurement path is **blocked on the
  reference machine** — `smartctl` is absent and Apple Silicon's internal NVMe
  (`APPLE SSD AP0512Z`) exposes no endurance counters; `system_profiler
  SPNVMeDataType` has no TBW/written field. Per this card's own fallback, the
  claim is now sourced from the **endurance-rating definition** and **says so**,
  with the `smartctl -A /dev/nvme0` check named for anyone on Linux who wants to
  verify it. **Do not let this become the same process error twice:** the previous
  wrong claim existed because it was reasoned and never labelled as such.
- **LaTeX removed** — `$10^4$` does not belong in a README.

**Still gated:** `dist/MODEL_CARD.md` is the source for the **published Hugging
Face model card**, which now disagrees with the repo. Updating it is outward-facing
and waits for the owner's explicit confirmation.

### The defect

[README.md:464-495](../README.md) — "SSD wear: the one thing to be deliberate
about" — states, in its own words, "plainly":

> "So the reads from expert streaming, not swap, are what actually wear the SSD"
> "SSD speed and **lifespan** genuinely matter here"

[dist/MODEL_CARD.md:243](../dist/MODEL_CARD.md) repeats it: "SSD speed and
**endurance** matter for long generations".

**NAND endurance is consumed by program/erase cycles — writes.** That is why
drives are rated in **TBW** (terabytes *written*) and **DWPD** (drive *writes*
per day). Reads do not consume endurance. **376 GB of reads costs approximately
zero drive life.**

The honest caveat, which must appear in the rewrite rather than be hidden:
*read disturb* is real — repeatedly reading a NAND block perturbs neighbouring
cells, and the controller refreshes the block after a threshold, which is a
write. But those thresholds are on the order of **10⁴–10⁶ reads of the same
block**. 376 GB spread over a 20.9 GB file is **~18 reads per byte**. It is
orders of magnitude away from mattering.

### The section refutes itself

It dismisses **9 GB of swap writes** as "tiny" beside 376 GB of reads. Those
9 GB of *writes* consume more endurance than the 376 GB of reads do. The one
thing it tells users not to worry about is the only thing on the page that
actually wears the drive.

### Why this costs product

[README.md:206-207](../README.md) and the wear section tell users to prefer
direct mode **to protect their SSD**. That is advice from a false premise, and it
discourages the model's strongest capability for no reason. [README.md:286-288](../README.md)
("It must not wear out the machine… careful with the SSD reads that cause the
real wear") encodes it as a design principle.

### What is true and must be kept

- The read volume is large and worth stating: **376 GB for one 933-token
  thinking answer**. Keep the number.
- It genuinely costs **time, power/battery, heat**, and it **evicts the page
  cache**, which slows the rest of the system.
- **SSD *speed* genuinely matters** — measured 6× from storage path alone. That
  half of the claim is correct and load-bearing.

So the corrected claim is: **"SSD speed matters enormously. SSD lifespan does
not."**

### Method

1. Run **E-H4** first. Do not rewrite on physics alone — this project's bar is
   measured claims, and the current wrong claim exists because it was reasoned,
   not measured.
2. Rewrite [README.md:464-495](../README.md): retitle away from "wear" (e.g.
   "SSD speed: the one thing to be deliberate about"), keep the 376 GB figure,
   state that reads cost time/power/heat and not endurance, note the read-disturb
   caveat honestly, and correct the swap comparison.
3. Fix [README.md:206-207](../README.md) — recommend direct mode because it is
   faster and uses less power, **not** to protect hardware.
4. Fix [README.md:286-288](../README.md) — the principle is "must not wear out
   the machine"; keep it, but point it at what is real (memory bounded, heat,
   battery), not at read endurance.
5. Fix [dist/MODEL_CARD.md:243](../dist/MODEL_CARD.md) — "endurance" → "speed".

**Acceptance:** no surviving claim that reads wear the SSD; E-H4's measurement
cited inline; the 376 GB figure retained; direct mode recommended for the right
reason. A reader who knows how NAND works finds nothing to object to.

**Owner decision:** this corrects a published README **and a published model
card** on Hugging Face. Outward-facing corrections wait for explicit
confirmation. Flag it; do not push it.

**Risk:** the section is currently one of the most trust-building parts of the
README — it volunteers an inconvenient truth. The rewrite must not read as
walking back candour. It is *more* candid: we measured, and the scary number
turned out to be the wrong scary number.

---

## H2 — Runtime SIMD dispatch on x86 (fixes G10)  ~3–4 days  **Biggest single win**

**Status: open. This is the highest-value work in the program.**

### The defect

See G10 in [TASKS_LINUX.md](TASKS_LINUX.md) and
[regressions/linux/x86-dispatch.md](regressions/linux/x86-dispatch.md).
Compile-time dispatch + no `-march` = every x86 user runs a scalar loop at
2.26 GFLOP/s while their CPU can do ~8× that. The AVX2 and AVX512-VNNI kernels
inherited from colibrì are **dead code in every shipped configuration**.

### Why `-march` is not the fix

- **`-march=native`** works for `install.sh` (it compiles on the user's machine)
  but is **wrong for the Docker image**, which is built once and run on many
  CPUs — `native` on the builder would `SIGILL` on an older user CPU. Since
  Docker is the entire Windows/Linux delivery path ([#2](TASKS_WINDOWS.md)),
  this alone rules it out.
- **`-march=x86-64-v3`** (requires AVX2; Intel Haswell 2013+/AMD Excavator 2015+)
  is a defensible floor for a 35B-model product, but it silently excludes older
  CPUs with a `SIGILL` rather than a message, and hardcodes a policy where a
  runtime check costs nothing.

### Design: per-function targets + `cpuid`, resolved once at startup

**ARM needs no dispatch** — NEON is mandatory in the aarch64 base ISA, so
`__ARM_NEON` is always defined. Only x86 needs this, which halves the work.

```c
#if defined(__x86_64__)
/* One definition per ISA. NOT `inline` — these are called through a pointer. */
__attribute__((target("avx2,fma")))
static void matmul_q_avx2(float *y, const float *x, const int8_t *q,
                          const float *scale, int S, int I, int O) { /* AVX2 body */ }

__attribute__((target("avx512vnni,avx512bw")))
static void matmul_q_vnni(/* … */) { /* VNNI body */ }

static void matmul_q_scalar(/* … */) { /* portable body */ }

static void (*g_matmul_q)(float*,const float*,const int8_t*,const float*,int,int,int)
    = matmul_q_scalar;

static void simd_init(void) {
    const char *force = getenv("SAMOSA_SIMD");        /* escape hatch */
    __builtin_cpu_init();
    if (force && !strcmp(force, "scalar")) return;
    if ((!force || !strcmp(force,"avx512")) &&
        __builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512bw"))
         g_matmul_q = matmul_q_vnni;
    else if (__builtin_cpu_supports("avx2"))
         g_matmul_q = matmul_q_avx2;
    fprintf(stderr, "[simd] path=%s\n", /* … */);
}
#endif
```

**Do not use GCC `target_clones`.** It is more elegant but relies on **IFUNC**,
which is glibc-only and breaks musl/Alpine (see E-L5 in
[TASKS_LINUX.md](TASKS_LINUX.md)). Manual function pointers work everywhere.

**Scope — six hot kernels** in [kernels.h](../src/kernels.h): `matmul_q` (~L62),
`matmul_i4` (~L82), `matmul_i4_grouped` (~L121), `matmul_i2` (~L146), and the
integer-dot kernels (~L184–L290). Restructuring `static inline` bodies into
per-ISA functions is the bulk of the work.

**Log the selected path at startup** (`[simd] path=avx2`) — it is the first thing
anyone debugging a slow install will need, and today there is no way to tell.

### Ordering trap — read this before starting

**H2 activates code that has never executed anywhere.** Today x86 is
slow-but-scalar and *probably* correct (simple C, checksums matched to 5
decimals). After H2, x86 runs AVX2/VNNI kernels that have **never produced a
token on any machine**.

**H2 makes E-L1 (x86 numerical parity) mandatory, not optional.** And E-L1
**cannot be run on the reference Mac**: an amd64 container there reports no AVX2,
no AVX512-VNNI, not even SSE4.2 — Docker Desktop's x86 emulation does not provide
them. **H2 requires access to real x86 hardware.** Do not ship it without.

### Acceptance

- **Correctness (the gate):** on real x86 hardware, greedy token sequences are
  **identical between `SAMOSA_SIMD=scalar` and the AVX2 path for ≥256 tokens
  across 5 seeds**. Bit-identical *logits* are **not** expected — FP
  reassociation differs by ISA — so judge on token identity, and measure the
  noise floor before calling any divergence a bug (see E-L1).
- Repeat independently for the **AVX512-VNNI** path. VNNI has its own int8
  accumulation semantics and its own chance of being wrong.
- **Speed:** AVX2 ≥ 4× scalar on the `matmul_q` microbench (NEON measured 7.6×;
  AVX2 is 256-bit vs NEON's 128-bit, so ≥4× is conservative).
- **Fallback:** a pre-AVX2 x86 CPU selects scalar and runs correctly — verified,
  not assumed.
- **No regression:** macOS/arm64 behaviour byte-identical and no measurable
  perf change (ARM does not go through the pointer).
- **musl/Alpine** builds and runs (proves no IFUNC crept in).
- **One Docker image** is correct on both an AVX-512 Xeon and a pre-AVX2 CPU.

### Risks

- Restructuring `kernels.h` diverges from colibrì upstream, which
  [NOTICE](../NOTICE) records as the origin of that file. Keep the diff
  mechanical and reviewable.
- `__attribute__((target))` + function pointers can defeat inlining that the
  compile-time version got for free. Measure the *dispatched* AVX2 path against a
  `-march=x86-64-v3` compile-time build; if dispatch costs more than a few
  percent, hoist the pointer out of the inner loop.

---

## H3 — Thermal-adaptive thread policy  ~4–5 days

**Status (2026-07-15): IMPLEMENTED, with E-H3 still unrun.** `adjust_threads()`
([qwen36b.c:3136](../src/qwen36b.c#L3136)) is in-tree and sampled from
`generate()`. Reviewed and corrected the same day.

**What holds up (verified, not taken on report):**

- **The default is untouched.** The controller returns early unless
  `SAMOSA_FAST=1`; a plain run emits no `[threads]` line. Confirmed by running it.
- **`OMP_NUM_THREADS` always wins** — first check in the function.
- **`SAMOSA_FAST=1` reports `cool=2 max=4`** on the M3: starts at the owner's
  comfort default and ramps from there, exactly as this card required.
- **The SMT bug is fixed properly** — reads `/sys/devices/system/cpu/smt/active`
  instead of assuming `logical/2`.
- **Mid-generation adaptation is numerically safe. The risk this card called "the
  sharpest" was wrong.** It predicted that changing OpenMP team size mid-generation
  would perturb reduction order and change tokens. Tested on the real model:
  `OMP_NUM_THREADS=2` vs `4`, same seed, same prompt → **byte-identical output**.
  There are no `reduction()` clauses anywhere in `src/`; every `parallel for`
  writes distinct output indices, so team size cannot change the arithmetic.
  **Retired — do not re-raise it.**

**Corrected on review (2026-07-15):**

- **Containers ran `--fast` at max threads with no thermal signal.** The original
  implementation set `current_threads = max_threads` when it detected a container.
  That is backwards and it is the dangerous direction: Windows/Linux delivery *is*
  Docker ([TASKS_WINDOWS.md](TASKS_WINDOWS.md)), so that path routinely lands on
  thin laptops, where "all cores, no feedback" is precisely what `--fast` should
  protect against. Now holds `cool_default` and names `OMP_NUM_THREADS` as the
  override for a cooled machine.

**Still open — do not call H3 done:**

- **E-H3 has not been run**, so the controller is **not tuned against any
  measurement**. The 85 °C / 75 °C thresholds and the 4-sample / 16-token cadence
  are reasoned defaults. They are safe by construction — the loop can only move
  between `cool_default` and `max_threads`, so a wrong threshold costs throughput,
  never safety — but **the loop must not be described as tuned.** Marked as
  UNVALIDATED in the code at [qwen36b.c:3293](../src/qwen36b.c#L3293).
- **Re-run E-H3 after H2.** 7.6× less CPU time per token will move the curve; 4
  threads post-H2 may be cooler than 2 threads is today.
- macOS `notify(3)` thermal path and the Linux `thermal_zone` path have **not been
  exercised under real thermal pressure** — only the initialization was observed.

### The defect

[qwen36b.c:4490-4562](../src/qwen36b.c#L4490-L4562) picks a thread count once, at
startup, from a hardcoded heuristic, and never revisits it:

- **macOS:** `hw.perflevel0.physicalcpu / 2` — 2 of 4 P-cores on the M3. The
  comment ([:4493-4498](../src/qwen36b.c#L4493-L4498)) is explicit that this is
  **the owner's comfort preference for a fanless chassis** (~7.3 vs ~9.5 tok/s),
  and that "la pressione termica OS resta a zero in entrambi i casi" — **OS
  thermal pressure was zero at both settings.** Someone measured thermal state by
  hand and never wired it into the engine.
- **Linux:** cgroup `cpu.max` → else Intel hybrid P-cores/2 → else
  `sysconf(_SC_NPROCESSORS_ONLN) / 2 / 2`.

**One fanless laptop's comfort preference is now the universal default.** On a
desktop with a tower cooler it leaves most of the machine idle. On a thin PC
laptop it may still be too much. Neither case has any feedback.

**Bug in the Linux fallback:** `physical = logical / 2` **assumes SMT**. On
non-SMT hardware — Ampere Altra, Graviton, most ARM servers, Intel E-cores — it
halves a number that was already physical, then halves again, ending at a
**quarter** of the real core count. A 16-core non-SMT ARM server gets 4 threads.

### Design: a closed loop, with the owner's preference preserved

The rule that must not be broken: **the default stays exactly as it is today on
the reference Mac.** The 2-thread cool default is a deliberate product decision,
not a bug to optimise away. An adaptive policy that ramps an M3 Air to 4 threads
would *violate* the stated preference.

So adapt where there is no preference to violate:

1. **`OMP_NUM_THREADS` always wins.** Never override an explicit user setting.
2. **Default (no flag):** unchanged on macOS. On Linux, fix the SMT assumption
   and derive from real topology.
3. **`--fast` becomes adaptive.** Today it means "all performance cores (runs
   warmer)" ([dist/samosa:8](../dist/samosa#L8), implemented at
   [:289](../dist/samosa#L289)). It should mean **"as fast
   as this machine can sustain"**: start at the cool default, sample thermal
   state every N tokens, step up while green, back off on pressure, hysteresis to
   avoid oscillation. On the M3 that lands near 4 threads (thermal pressure was
   measured zero there); on a desktop it lands much higher; on a thin laptop it
   backs itself off. That is exactly "best performance without killing the
   hardware", and it changes no default.
4. **Never exceed the cgroup `cpu.max` quota** regardless of thermal headroom.

### Signals

| Platform | Signal |
|---|---|
| macOS | `notify(3)` key `com.apple.system.thermalpressurelevel` via `notify_register_check` / `notify_get_state` — C-accessible, no Obj-C. This is the signal the 2026-07-12 comment refers to. |
| Linux | `/sys/class/thermal/thermal_zone*/temp`; plus throttle detection via `scaling_cur_freq` vs `cpuinfo_max_freq`, and `/sys/devices/system/cpu/cpu*/thermal_throttle/*` on Intel. |
| **Docker / WSL2** | **Blind.** A guest cannot see host thermals. |

**The Docker caveat is a first-class finding, not a footnote.** Windows and Linux
delivery is Docker ([#2](TASKS_WINDOWS.md)), and **thermal adaptation cannot work
there** — the VM does not see the host's sensors. In a container, `--fast` must
fall back to the static conservative policy **and say so** (`[threads] no thermal
visibility in container; using static policy`). Silently pretending to adapt
would be worse than not adapting.

### Acceptance

- **M3 Air, default:** thread count and behaviour **identical to today**. Verified
  by same-seed output and tok/s within noise.
- **M3 Air, `--fast`, 10-minute sustained generation:** ramps up; OS thermal
  pressure stays at zero (matching the 2026-07-12 hand measurement); the chassis
  stays within the envelope the owner already accepts at 4 threads; ≥ today's
  `--fast` throughput.
- **Induced thermal pressure** (external load or a warm ambient): threads step
  down within one sampling window and recover after. No oscillation.
- **Non-SMT host:** thread count matches real physical cores, not a quarter of
  them. Test on aarch64 in Docker.
- **Container:** falls back to static, logs the reason, never claims to adapt.
- **`OMP_NUM_THREADS=N`** wins in every case above.
- **cgroup `--cpus=2`:** never exceeds 2 threads even with thermal headroom.

### Risks

- Changing threads mid-generation reshapes the OpenMP team; verify it does not
  perturb numerics (`matmul_q` reduction order can change with team size →
  different rounding → possibly different tokens). **Test same-seed determinism
  across a thread-count change**, and if it breaks, only adapt *between* turns,
  never mid-generation. This is the sharpest risk in the card.
- Thermal sensors in VMs may return plausible-but-meaningless numbers rather than
  failing. Detect the container case explicitly (`/.dockerenv`,
  `/proc/self/cgroup`) rather than trusting a sensor read to fail.

---

## H4 — An honest hardware gate  ~1–2 days

**Status: open. `install.sh` performs no AVX2 check.**

### The defect

The installer's Linux preflight checks OS, arch (`x86_64`/`aarch64`), RAM ≥ 16 GB,
and a compiler — and **nothing about AVX2 or storage**. So an x86 user on a
pre-2013 CPU, or any x86 user at all (G10), silently gets the 2.26 GFLOP/s scalar
path and concludes Samosa is slow. A user on a SATA SSD gets ~6× less throughput
than the published numbers, with no warning.

[README.md:131](../README.md) is otherwise **exemplary** — "'Runs on the CPU' does
**not** mean it runs on any 16 GB laptop" — but ends with "the Docker image can
package the POSIX server for **any platform**", which G10 and the storage
measurements contradict. [README.md:588](../README.md) is headed "Run on any
machine with 16 GB of RAM".

### The honest gate

- **CPU:** x86-64 **with AVX2** (Intel Haswell 2013+ / AMD Excavator 2015+), or
  arm64. Below that: the scalar path, ~7.6× slower — installable, but the user
  must be told before they download 24 GB.
- **RAM:** 16 GB+; **≥ 6 GB to the Docker VM** (default ~2 GB cannot load the
  model at all).
- **Storage: NVMe SSD.** SATA SSD is degraded; **HDD is unusable** (random 16 KB
  reads at ~100 IOPS ≈ 1.6 MB/s against hundreds of MB per token = minutes per
  token); network storage is unusable.
- **Model on a named Docker volume**, never a host bind mount (measured 6×).

### Method

1. **`install.sh` preflight:** on x86, `grep -qw avx2 /proc/cpuinfo` (Linux) /
   `sysctl -n machdep.cpu.features` (Intel macOS). If absent, warn loudly with
   the measured number and require an explicit `SAMOSA_ALLOW_SLOW_CPU=1` to
   proceed. **Warn, do not refuse** — a slow Samosa is still Samosa, and refusing
   hardware that works is its own overclaim.
2. **Storage detection:** resolve the model path's device; on Linux check
   `/sys/block/<dev>/queue/rotational` (1 = HDD → refuse; it is genuinely
   unusable) and identify NVMe vs SATA for the warning. Extend **D-4**'s doctor
   ([TASKS_WINDOWS.md](TASKS_WINDOWS.md)) rather than duplicating.
3. **Fix D-4's virtiofs detection — it does not fire.** Verified 2026-07-15: with
   `/model` a host bind mount, no warning appeared; `stat -f` reports the fstype
   as `UNKNOWN (0x6a656a63)`, so a name-based check misses Docker Desktop's mount
   entirely. Detect by **magic number**, or empirically (time a 100 MB read at
   startup and warn below a threshold) — the latter is more honest and catches
   slow storage generally, not just virtiofs. **A warning that silently fails is
   worse than no warning**, because it certifies a broken setup as fine.
4. **README/model card:** state the gate where a user decides, not in a footnote.
   Fix "any platform" at [README.md:131](../README.md) and the heading at
   [README.md:588](../README.md).

**Acceptance:** a pre-AVX2 x86 install warns with the measured factor and needs an
explicit override; an HDD is refused with a clear message; a bind-mounted model
warns (verified by actually bind-mounting one — the current check fails this
test); the README's requirements match what was measured.

---

## H5 — Host-capability profile: run to the machine, honestly  ~2–3 days (design)  **Cross-cutting; consolidates H2/H3/H4 detection**

**Status: open, design. New 2026-07-16, from the owner:**

> *"Samosa should be intelligent about where it's running. Someone with 64 GB
> should use relatively more and be faster than someone with 16 GB. Why hardcode
> two threads — a 128 GB box could run 12 and still have plenty left."*

Right instinct, and today Samosa does the opposite: every resource decision is
either a **hardcoded guess from the one fanless 16 GB M3 Air** (the 2-thread
default) or an **opt-in flag the user has to know** (`--fast`, `OMP_NUM_THREADS`,
`EBUDGET_AUTO`, `DIRECT`). Nothing asks *"what machine is this?"* and sizes the
run to it. Capability is left on the table on big machines; the burden is on the
user to discover the flags. H5 adds the missing layer: one `host_profile()`
resolved at startup that the other levers read.

### What "use more resources" does — and does not — mean (honesty first)

The answer is already in this card's Verified ground truth:

- **RAM → faster automatically, via the OS page cache — not the engine LRU.**
  Decode reads are buffered ([qwen36b.c:1841](../src/qwen36b.c#L1841); `g_direct`
  defaults 0 at [:2101](../src/qwen36b.c#L2101)), so on 64 GB the 20.9 GB expert
  file largely caches and the SSD goes quiet after warmup — for free, once E-H2
  confirms it. H5 routes "use my RAM" **to the page cache**. It must **not** grow
  the engine's cache: measured **41 % slower**
  ([qwen36b.c:2251-2254](../src/qwen36b.c#L2251-L2254)), a standing non-goal.
- **Cores → more threads — bounded by thermal reality (H3) and the cgroup quota.**
  A win on a cooled desktop; on a fanless laptop it is exactly what the 2-thread
  default protects against.
- **SIMD → already host-adaptive once H2 lands** (`cpuid` dispatch). H5 surfaces
  the selected path in the profile.
- **Storage class (H4) sets a floor, not a throttle** — HDD refused, SATA warned,
  NVMe expected.

So "a 128 GB box runs 12 threads" is only half right: the *threads* come from
cores + cooling (H3); the *speed from RAM* comes from the page cache (H5 → cache),
**not** from spending 128 GB on a bigger LRU.

### The genuinely new decision: should the *default* scale, or only `--fast`?

Today the conservative 2-thread default is universal, and "changing the macOS
default" is a non-goal **because it encodes the owner's comfort preference on a
fanless chassis**. But that reason is chassis-specific: a Mac Studio, or a
plugged-in Linux tower with a real cooler, has no fanless-comfort reason to cap
at 2.

H5's proposal: the default becomes a **function of a detected capability tier**,
with one hard rule — **the reference fanless M3 Air tier reproduces today's
behavior byte-for-byte** (the owner's preference preserved exactly, verified by
same-seed output and tok/s within noise). A machine demonstrably *not* that class
may default higher — but only for a tier that has been **measured** (E-H5).
Unknown/ambiguous class → the conservative profile, and say so. That is the
opposite of a hardcoded universal 2, without overriding the one preference that
is real.

### Design: `host_profile()`, resolved once, logged

Consolidate the detection currently duplicated across H2/H3/H4 into one struct + a
`[host]` line:

```
[host] tier=desktop-cooled ram=64GB pcores=12 smt=on ac=yes storage=nvme isa=avx2 thermal=visible container=no
```

Fields: `ram_gb`; `phys_perf_cores` (**fix the non-SMT bug once, here** — H3's
`logical/2` assumption that hands a 16-core non-SMT ARM server 4 threads — and let
H3/H4 share the corrected value); `smt`; `on_ac`; `storage_class` (H4);
`container` (`/.dockerenv`, `/proc/self/cgroup`); `cpu_isa` (H2);
`thermal_visibility` (bare-metal vs container-blind, H3). These resolve to a
**named tier from a small closed set** (`reference-fanless`, `desktop-cooled`,
`container-blind`, `constrained`, `unknown`) that parameterizes the default thread
count, the `--fast` ceiling, whether thermal adaptation is trusted, and the H4
gate messaging. **A small set of measured tiers, not a continuous optimizer** —
per-CPU autotuning stays a non-goal, for the same unboundedness reason.

Every downstream consumer reads the profile instead of re-detecting: H2 (already
automatic), H3 (its default derives from the tier; `reference-fanless` == today),
H4 (the gate messages off the same storage/ISA facts), and Samosa Jobs' resource
gate ([TASKS_JOBS.md](TASKS_JOBS.md) HR-6/J1.13, which today derives its thread
budget ad hoc — H5 is the shared source it should read).

### Honesty rules (non-negotiable)

- `reference-fanless` == today, exactly. Anything else is a bug.
- **No tier above the measured floor gets a throughput claim** until E-H5/E-H2/
  E-H3 measure it on that class. "Adapts upward" is sayable; "fast on 64 GB" is
  not, until measured.
- Unknown/ambiguous host → conservative, logged reason.
- Container → thermally blind (H3) → static conservative, logged.
- `OMP_NUM_THREADS` always wins (H3's rule, preserved).
- Never grow the engine LRU to "use RAM."

### Acceptance

- **M3 Air:** `tier=reference-fanless`; behavior **byte-identical to today**
  (same-seed output; tok/s within noise). **The gate.**
- **Big-RAM many-core box:** profile detected correctly; `[host]` line accurate;
  default threads > 2 **only** if that tier was measured (E-H5), else conservative
  + logged; the RAM win appears as a `bytes_read` drop via the page cache (E-H2),
  **not** LRU growth.
- **Non-SMT host:** `phys_perf_cores` correct — the value H3/H4 also consume.
- **Battery / container / unknown:** conservative, reason logged.
- **`OMP_NUM_THREADS=N`** wins in every case above.

### Risks

- Scaling the default risks violating the owner's preference — mitigated by the
  byte-identical `reference-fanless` gate and the measure-before-shipping rule.
- Detection lies (VMs misreport cores/thermals) — ambiguous → conservative, never
  optimistic; detect the container case explicitly rather than trusting a sensor
  read to fail (H3's rule).
- "Intelligent about the host" invites scope creep into autotuning — bounded to
  the named tiers with measured parameters.

---

## Experiments

### E-H1 — Does vectorization actually cool the machine?  ~1 day  **Gates H1+H2's framing**

The claim "vectorization makes it cooler" is **physics, not measurement**. Same
FLOPs in fewer instructions over less time ⇒ less energy per token. Plausible,
and unverified. If SIMD units draw enough extra power to offset the shorter
runtime, the claim is wrong and H1's rewrite must not use it.

**Method (macOS, NEON vs scalar via `-U__ARM_NEON`, identical prompt/seed):**
`sudo powermetrics --samplers cpu_power,thermal -i 1000` during a bounded
generation. Record joules/token, package power, thermal pressure, and wall clock.

**Acceptance:** a joules-per-token figure for both paths. **Report whichever way
it comes out.** If vectorized wins on energy, H1 and H2 may say so with a number.
If not, delete the claim.

### E-H2 — Does the page cache scale on a big-RAM machine?  ~1 day

The theory: decode reads are buffered, so on 32–64 GB the OS caches most of the
20.9 GB expert file and disk reads collapse after warmup. **Never measured.** The
reference machine has 16 GB and cannot test it.

**Method:** Docker with `--memory=` at 6/12/24/48 GB (needs a Docker VM ≥ 50 GB,
i.e. **not the reference Mac** — this needs a bigger host). Same prompt ×5 in one
container; record `expert_disk`, `bytes_read`, `expert_hit`, tok/s per run.

**Acceptance:** a memory → steady-state `bytes_read` and tok/s table. Confirms or
kills "more RAM ⇒ quiet SSD, automatically", which is H1's main consolation and
the honest answer to "how do I make it faster without hurting my machine".

**Do not respond to a bad result by enabling `EBUDGET_AUTO`** — that path was
measured **41% slower** ([qwen36b.c:2251-2254](../src/qwen36b.c#L2251-L2254)).
The page cache and the engine's LRU are different mechanisms; only one of them
scales.

### E-H3 — Thermal headroom curve  ~1 day  **Gates H3**

**Method:** on the M3 Air, sustained 10-minute generation at 1/2/3/4/6/8 threads.
Record tok/s, OS thermal pressure, package power, and chassis temperature if
available. Repeat on any other machine available.

**Acceptance:** a threads → sustained tok/s → thermal pressure curve. This is what
H3's controller is tuned against, and it will show whether the 2-thread default
still reflects the owner's preference once vectorization (H2) changes the shape.
**H2 may move this curve substantially** — 7.6× less CPU time per token could mean
4 threads is now cooler than 2 threads is today. **Run E-H3 after H2, or run it
twice.**

### E-H4 — Prove the SSD claim with SMART  ~0.5 day  **Gates H1**

Do not correct a measured-sounding claim with an argument. Measure it.

**Method:** read SMART before and after a long thinking generation (the kind that
reads ~376 GB):

```sh
# Linux (definitive)
smartctl -A /dev/nvme0 | grep -E "Data Units (Read|Written)|Percentage Used"
```

Expect `Data Units Read` to jump by ~376 GB, `Data Units Written` to barely move,
and `Percentage Used` to be unchanged.

**Known risk — this may not be runnable on the reference machine.** Apple Silicon
internal NVMe generally does not expose SMART to `smartctl`. If so: run it on a
Linux box (which the project does not have — see E-L1), or state the claim from
the TBW/DWPD rating definition, which is citable, uncontroversial, and
verifiable from any drive's own datasheet. **Say which of the two you did.**
Physics plus a vendor rating is still stronger than the current claim, which has
neither.

**Acceptance:** either a before/after SMART table, or an explicit, sourced
statement that endurance ratings are write-based with the caveat about read
disturb. H1 cites whichever.

### E-H5 — Measure a non-reference capability tier  ~1 day  **Gates any default above the fanless floor**

H5 may not ship a higher default for any tier it has not measured. On real
non-reference hardware — a cooled desktop or a big-RAM cloud box (the **same
hardware gap** as E-H2/E-L1; see Open questions) — run E-H3's sustained-generation
protocol at the tier's candidate thread count, plus E-H2's memory → `bytes_read`
sweep, and record whether the tier sustains its proposed default without thermal
pressure and whether more RAM actually quiets the SSD.

**Acceptance:** a per-tier row — thread count, sustained tok/s, thermal pressure,
steady-state `bytes_read` — that either justifies the tier's default or sends it
back to conservative. A tier with no measurement ships conservative. **Same rule
as everywhere: an unmeasured tier is not "fast."**

---

## Non-goals

- **GPU/Metal.** The separate, already-planned post-release performance track.
  H2 is CPU dispatch only.
- **Changing the *reference fanless* (M3 Air) default thread count.** It is the
  owner's explicit comfort preference and stays byte-identical. H3 adapts
  `--fast` and fixes non-macOS derivation; **H5** may raise the *default* for
  other, **measured** capability tiers (a cooled desktop), but never for the
  fanless reference class.
- **Growing the engine's expert cache.** Measured 41% slower. Settled.
- **Undervolting, fan control, or any OS power settings.** Not our business, and
  not something a local chat app should touch.
- **Per-CPU autotuning of the cache budget.** Interesting, unbounded; revisit only
  if E-H2 shows RAM scaling is real and the budget is what blocks it.

## Open questions

- **Where does E-L1 / E-H2 hardware come from?** H2 cannot ship without real x86;
  E-H2 needs >32 GB. Neither exists in the current setup, and both are now on the
  critical path. A cheap cloud box for an afternoon would unblock both — but note
  it will have **unknown storage**, so throughput numbers from it are not
  comparable to the reference Mac's.
- **Does the 16 GB floor still hold after H2?** Vectorization does not reduce
  memory, but it changes the compute/IO balance: at 7.6× faster matmul the engine
  spends proportionally *more* time waiting on the SSD, so the storage gate gets
  *stricter*, not looser. E-H2 and E-L4 should re-examine the floor together.
- **Should `--fast` be renamed?** If H3 makes it "as fast as your machine
  sustains", "fast" undersells it and "runs warmer" (the current README wording)
  becomes wrong on a well-cooled desktop where it runs *cool and fast*.
