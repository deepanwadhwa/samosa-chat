# Experiments program — Apple Silicon: 12–15 tok/s without hurting the machine

Read [ISSUE_TASKS.md](ISSUE_TASKS.md) first, including the Working agreement.
Per that agreement this card belongs on `main`; experiment code and evidence
belong on a branch cut from `main`.

This program answers a question the project owner asked on 2026-07-17:

> *"I want the model to use all the capabilities of this machine (M3 Mac,
> 16 GB unified memory) to the max level without killing it. My goal is
> 12–15 tokens/sec without overheating the machine or wearing the components."*

It is macOS/Apple-Silicon-specific and complements
[TASKS_HARDWARE.md](TASKS_HARDWARE.md), which owns the cross-platform work
(H2 x86 dispatch, H3 thermal policy, H5 host profile). Nothing here changes
any default without an owner decision; every experiment is opt-in behind an
env var or a separate build, measured on the reference machine, and honest
about negative results. **A negative result is a result** — an experiment
that kills an idea has done its job.

## Verified ground truth (2026-07-17, reference M3 Air, Apple clang 21.0.0)

Every row was verified on 2026-07-17 by the method stated. Do not re-derive;
do re-verify if the toolchain or model changes.

| Fact | Evidence |
|---|---|
| **NEON is real and dotprod is already on.** `cc -O3 -dM -E -x c /dev/null` defines `__ARM_FEATURE_DOTPROD`; decode takes the integer-dot path at S=1 ([kernels.h:194-198](../src/kernels.h#L194-L198), `vdotq_s32` at [:296](../src/kernels.h#L296)). There is **no arm64 sibling of G10** — the 7.6× "un-break the build" win does not exist here. | compile check, 2026-07-17 |
| **i8mm is NOT enabled.** `__ARM_FEATURE_MATMUL_INT8` is absent from the same dump. The M3 supports it (`sysctl hw.optional.arm.FEAT_I8MM`); `smmla` roughly doubles int8 GEMM throughput vs `sdot` — but only for batched shapes (S≥2), i.e. prefill and any future verify pass, not raw decode. | compile check, 2026-07-17 |
| **The KV cache is f32.** Allocated `sizeof(float)` at [qwen36b.c:4004-4006](../src/qwen36b.c#L4004-L4006); session save/load also f32 ([:3680](../src/qwen36b.c#L3680), [:3801](../src/qwen36b.c#L3801)). `__ARM_FEATURE_FP16_VECTOR_ARITHMETIC` and `FP16_FML` are already defined by default. | grep + compile check, 2026-07-17 |
| **Experts stream via buffered 16 KB `pread` into an engine LRU** ([qwen36b.c:2](../src/qwen36b.c#L2)); `DIRECT` defaults 0 ([:2164](../src/qwen36b.c#L2164)). Both the page cache and the engine LRU hold expert bytes — double residency on a 16 GB machine. Cold-start tradeoffs were measured once ([:292-294](../src/qwen36b.c#L292-L294)); steady-state residency never has been. | code read, 2026-07-17 |
| **A larger engine LRU is slower — settled.** +41% measured ([qwen36b.c:2251-2254](../src/qwen36b.c#L2251-L2254)); `EBUDGET_AUTO` is opt-in for this reason. No experiment here may respond to anything by growing the LRU. | [TASKS_HARDWARE.md](TASKS_HARDWARE.md) ground truth |
| **Per-turn instrumentation already exists.** `[stats]` prints prefill/decode tok/s, `expert_hit`, `expert_disk` (disk-wait seconds), `expert_mm`, `peak_rss` ([qwen36b.c:4473-4483](../src/qwen36b.c#L4473-L4483)); `[ecache]` prints budget/payload/evictions/`bytes_read`/`bytes_avoided`/pressure events ([:4484-4496](../src/qwen36b.c#L4484-L4496)); `[seqio]` prints sequential-prefill I/O ([:4497-4499](../src/qwen36b.c#L4497-L4499)). What's missing is only the compute-side split (attention vs expert matmul vs head). | code read, 2026-07-17 |
| **Adaptive top-k is already implemented, with telemetry.** `MOE_K` (fixed), `MOE_MASS` (cumulative router-mass cutoff), guarded by `MOE_MAX_ENTROPY` / `MOE_MIN_GAP` ([qwen36b.c:566-606](../src/qwen36b.c#L566-L606), env at [:5241-5243](../src/qwen36b.c#L5241-L5243), CLI at [:5325-5333](../src/qwen36b.c#L5325-L5333)). `[moe-policy]` reports decisions, `avg_k`, an effective-k histogram, and a bytes-saved proxy ([:643-668](../src/qwen36b.c#L643-L668)). Whether it has ever been *measured* for speed/quality is unknown — no regression doc found. | code read, 2026-07-17 |
| **Routing record/replay exists.** `ROUTE_TRACE` / `ROUTE_REPLAY` ([qwen36b.c:763-799](../src/qwen36b.c#L763-L799)). | code read, 2026-07-17 |
| **Prefetch admission and waste accounting exist in the cache.** `ECACHE_ADMIT_PREFETCH`, `wasted_prefetch_planes/bytes` ([expert_cache.h:72-75](../src/expert_cache.h#L72-L75), [:180-181](../src/expert_cache.h#L180-L181)); 2Q policy available ([:64-70](../src/expert_cache.h#L64-L70)). **The cache API is not thread-safe** ([:14](../src/expert_cache.h#L14)) — any prefetch thread may only do I/O; admissions stay on the engine thread. | code read, 2026-07-17 |
| **Threads:** default 2 on macOS — the owner's comfort preference on a fanless chassis, ~7.3 tok/s vs ~9.5 at 4 threads, OS thermal pressure zero at both ([qwen36b.c:5079-5083](../src/qwen36b.c#L5079-L5083)). `SAMOSA_FAST=1` enables the H3 adaptive controller ([:3246-3250](../src/qwen36b.c#L3246-L3250)); E-H3 tuning is still unrun ([:3411](../src/qwen36b.c#L3411)). Thread-count changes were measured **byte-identical** in output (no reductions anywhere in `src/`) — see H3. | code read + H3 card |
| Every hot kernel parallelizes with `#pragma omp parallel for schedule(static)` ([kernels.h](../src/kernels.h) throughout). On 4P+4E asymmetric cores a static split barriers on the slowest thread. | code read, 2026-07-17 |
| A GPU/CUDA backend seam already exists in the matmul dispatcher ([kernels.h:347-357](../src/kernels.h#L347-L357)); resident tensors are marked by `cuda_eligible` ([:41](../src/kernels.h#L41)). | code read, 2026-07-17 |

**Unverified, investigate before relying on:** the semantics of `REF`
([qwen36b.c:5228](../src/qwen36b.c#L5228)) and the `teacher_corpus` /
`teacher_output` machinery ([:5231-5232](../src/qwen36b.c#L5231-L5232)) —
both look like scoring/comparison hooks that E-X8 could reuse.

## The arithmetic of the target

- 12 tok/s = 83 ms/token; 15 tok/s = 67 ms/token.
- Today (hand-measured, [qwen36b.c:5083](../src/qwen36b.c#L5083)): ~7.3 tok/s
  at 2 threads (137 ms), ~9.5 at 4 threads (105 ms). The published 5–7 figure
  is the 2-thread default.
- So from the 4-thread envelope the owner already accepts, **12 tok/s needs
  −22 ms/token and 15 needs −38 ms/token.** Where those milliseconds are —
  expert-miss stalls, attention, dense matmul, dequant — is exactly what
  E-X1 measures. Every "expected gain" below is an estimate until E-X1
  replaces it with a measurement.
- Honest prediction recorded up front: E-X2–E-X7 plausibly reach ~10–12;
  the stretch to 15 most likely requires E-X8 (speculation), E-X9
  (adaptive top-k), or the Metal track (E-X10, reopened 2026-07-17) — all
  gated: E-X8 on a measured acceptance rate, E-X9 on an owner quality
  decision, E-X10 on its one-day M0 spike. The strongest path to 15+ is
  the *combination*: E-X10 M3 (CPU drafts, GPU verifies) multiplies E-X8's
  speculation by Metal's cheap batched verification, and M2 lowers J/token
  so whatever speed is reached actually *sustains* on a fanless chassis.
- **Owner decision (2026-07-17): the target is *generic felt speed*, not
  long-session speed.** "12–15" means what a user feels in an ordinary chat
  — the owner does not assume users will run very long sessions. Two
  consequences: (1) the gate is **W-DECODE at ~1k context**, with W-SESSION
  kept as a no-regression check rather than the headline; (2) felt speed
  also includes **time-to-first-token**, so the prefill experiments
  (E-X6/E-X7) are not secondary — a paste of a document or a long first
  message is felt entirely as prefill latency.

## Dependency order

Ordered for the felt-speed decision (owner, 2026-07-17): decode multipliers
and cheap measurements first; fp16 KV runs later, for footprint/safety more
than speed.

```
E-X1 (baseline + phase timers)  ── gates everything below
├── E-X4 Phase A (routing locality)      cheap, measurement-only; feeds E-X8
├── E-X5 (schedule/threads sweep)        cheap, one-line build variant
├── E-X8 (speculation go/no-go)          measurement → maybe a new card; the ×-multiplier candidate
├── E-X9 (adaptive top-k sweep)          measurement → owner decision; the other multiplier
├── E-X3 (residency & budget sweep)      cheap, measurement-only
├── E-X6 (i8mm dispatch)                 code, bit-exact gate; time-to-first-token
├── E-X7 (Accelerate/AMX prefill)        code, owner note (build change); time-to-first-token
└── E-X2 (fp16 KV)                       code; demoted to footprint/swap-safety — see its card
E-X10 (Metal M-track: M0 spike → M1 prefill → M2 decode → M3 draft/verify)
      — reopened by owner 2026-07-17; M0 runnable anytime and is the track's RUN-FIRST
E-X11 (MLX / llama.cpp yardstick) — independent, anytime; calibrates the target against Metal-native engines
```

---

## Common protocol — read once, apply to every experiment

### Workloads

Fixed, deterministic, committed under `tests/fixtures/experiments/` (created
by E-X1). All runs greedy with a fixed seed so outputs are comparable.
Check exact CLI flags against the argv parsing at
[qwen36b.c:5265](../src/qwen36b.c#L5265) before scripting — do not guess.

| ID | Shape | Measures |
|---|---|---|
| **W-DECODE** | realistic mid-chat state: ~1,000 tokens of warm context (a few turns in), generate 256 tokens, thinking off | decode tok/s — **the felt-speed gate: this is the number that must reach 12–15** (owner decision, 2026-07-17) |
| **W-PREFILL** | ~2,000-token document prompt, generate 32 tokens | prefill tok/s |
| **W-SESSION** | resumed session with ≥4,096 tokens of context, generate 128 | long-context decode (attention-heavy) |
| **W-SUSTAIN** | W-DECODE looped for 10 minutes | thermal/sustained behaviour (E-H3 protocol) |

### Run rules

- Reference machine, on AC power, lid open, no other workload, **never while
  the owner is chatting with the model** (standing machine-safety rule).
- One warm-up run (populates page cache + LRU), then **3 measured runs**;
  report all three and the median. Report cold-state runs separately and
  labelled — cold and warm are different experiments.
- Record the exact command line, env, git SHA, and the full `[stats]`,
  `[ecache]`, `[seqio]`, and (when active) `[moe-policy]` lines.
- Evidence goes to `docs/regressions/experiments/e-x<N>-<slug>/report.md`
  with raw logs beside it. Paste commands and output; "not run" where not run.

### Performance measurement

Primary: the engine's own `[stats]` line — `decode tok/s`, `prefill tok/s`,
`expert_hit`, `expert_disk` seconds, `peak_rss` — plus `[ecache] bytes_read`.
Wall-clock deltas below ~3% are noise on one machine; call anything <3%
"within noise", not a win.

### Machine-safety measurement (every experiment, not optional)

| Concern | How to measure | Bound |
|---|---|---|
| Heat / throttling | `sudo powermetrics --samplers cpu_power,thermal -i 1000` in a second terminal during the run; record package power and "pressure level". Separately `pmset -g therm` before/after (CPU_Speed_Limit=100 means no throttle). | Thermal pressure stays **Nominal** during W-DECODE/W-PREFILL. W-SUSTAIN may reach Moderate; sustained Heavy/Serious → abort, record, shorten the run. |
| Energy per token | mean package mW × decode_s ÷ tokens from the same powermetrics log. Joules/token is the honest efficiency metric on a fanless chassis (E-H1's metric). | Report it for every before/after. An optimization that raises J/token needs a reason. |
| Swap writes (the real SSD wear vector — H1) | `sysctl vm.swapusage` and `vm_stat` (Pageouts) before/after each run; delta. | Swap-used delta ≈ 0; pageout delta < 100 MB per run. Larger → abort, record, investigate footprint. |
| Footprint | `peak_rss` in `[stats]`; optionally `footprint <pid>`. | No experiment may push warmed peak_rss above ~4.5 GB (today: ~3.9–4.2). |
| SSD reads | Reads do not wear NAND (H1 — settled). Track `[ecache] bytes_read` for power/heat honesty, not for wear. | — |

### Quality measurement — three classes, three bars

1. **Bit-exact class** (pure integer rearrangement: E-X6): logits must be
   **bit-identical** to the baseline path. Integer dot products have no
   rounding freedom; any difference is a bug, not noise.
2. **Numerics-perturbing class** (E-X2 fp16 KV, E-X7 f32 GEMM): tokens may
   legitimately diverge. Protocol: same-seed greedy, 5 seeds × 256 tokens;
   record first-divergence position per seed; run the quality suite (below)
   side-by-side and review. Useful fact: thread count is byte-identical
   (H3), so the divergence noise floor is **zero** — any token change is
   attributable to the change under test.
3. **Policy class** (E-X8 if implemented, E-X9): the model legitimately
   computes something different. Strongest bar: the quality suite reviewed
   by the owner, divergence stats reported, and **no default changes without
   explicit owner sign-off** (accuracy bar is a non-negotiable).

**Quality suite** (created by E-X1, committed under
`tests/fixtures/experiments/prompts/`): 12 fixed prompts — factual QA ×3,
arithmetic/logic ×2, short code ×2, summarization of a committed fixture
document ×2, instruction-following ×2, one long-document QA reusing a Jobs
corpus fixture. Greedy, fixed seed, outputs archived in evidence so every
later experiment diffs against the same baseline.

---

## E-X1 — Per-token phase breakdown + baseline card  ~1 day  **Gates everything**

**Hypothesis:** none — this is the measurement the rest of the program hangs
off. At ~105 ms/token (4T) we do not know how many ms are attention, expert
matmul, expert-miss stalls, dense/resident matmul, lm_head, or dequant.

**What exists:** `expert_disk` and `expert_mm` timing and the tok/s split are
already in `[stats]` ([qwen36b.c:4473-4483](../src/qwen36b.c#L4473-L4483));
`now_s()` at [:58](../src/qwen36b.c#L58). Missing: attention, router,
resident-dense, head/sampler buckets.

**Method:**
1. Add an opt-in `SAMOSA_PHASE_STATS=1` that accumulates per-phase seconds in
   the forward pass: `t_attn`, `t_router`, `t_dense` (resident QKV/O etc.),
   `t_expert_mm` (exists), `t_edisk` (exists), `t_head` (lm_head + sampler),
   `t_other` (remainder — print it; if it's large the bucketing is wrong).
   Bracket the existing `t_edisk`/`t_emm` accounting sites with the same
   pattern. Emit one `[phase]` line next to `[stats]`, split prefill/decode.
2. Overhead guard: run W-DECODE ×3 with and without the flag; require <2%
   tok/s delta, else coarsen the buckets.
3. Create the workloads and quality suite fixtures (Common protocol).
4. Produce the **baseline card**: {2T, 4T} × {W-DECODE, W-PREFILL, W-SESSION},
   warm (plus one labelled cold run each), with full safety telemetry
   including joules/token.

**Files:** [src/qwen36b.c](../src/qwen36b.c) (forward pass + stats print)
only; `tests/fixtures/experiments/` (new).

**Acceptance:** a table *phase → ms/token* at 2T and 4T warm, summing to
within 5% of measured wall time; baseline tok/s and J/token recorded;
flag overhead <2%. This table assigns every later experiment its Amdahl
ceiling — e.g. if warm `expert_disk` is 2 ms/token, E-X4 Phase B is dead
before it starts, and that is a result.

**Safety:** opt-in flag, no behaviour change. Standard telemetry anyway.

---

## E-X2 — fp16 KV cache  ~2–3 days

**Hypothesis:** KV at f32 doubles attention bandwidth and footprint for no
accuracy the model needs. Storing K/V as IEEE fp16 halves both; at ≥4k
context (W-SESSION) the attention share of decode time drops measurably.
This defends the architecture's core promise — long sessions — where decode
must still be fast at token 20,000, and it *reduces* swap risk (the real
wear vector) by shrinking footprint.

**Priority note (owner decision, 2026-07-17):** the felt-speed target binds
at ~1k-token context, where the attention share — and therefore E-X2's speed
win — is modest. E-X2 stays in the program because (a) halving KV footprint
directly reduces swap risk, the machine's one real wear vector, and (b) even
ordinary multi-turn chats accumulate a few thousand tokens, so it protects
felt speed from *degrading* as a conversation grows. It is no longer a
headline speed lever; run it after the decode multipliers.

**What exists:** f32 alloc at [qwen36b.c:4004-4006](../src/qwen36b.c#L4004-L4006);
attention consumption around [:2486](../src/qwen36b.c#L2486); session
persistence writes f32 at [:3680](../src/qwen36b.c#L3680) and reads at
[:3801](../src/qwen36b.c#L3801). fp16 vector arithmetic + FML confirmed
available at default flags.

**Method:**
1. Opt-in `SAMOSA_KV=f16` (default `f32` until accepted). Guard the fp16
   path with `#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)`; other
   platforms keep f32.
2. Step 1 is **storage-only**: store `__fp16`, convert to f32 8-wide
   (`vcvt_f32_f16`) at the point of use; all arithmetic stays f32. The win
   is bandwidth, not FLOPs. (Optional step 2, only if step 1 accepts:
   fp16 FMLAL accumulate-to-f32 in the attention dot.)
3. **Session format unchanged:** convert to/from f32 at save/load so
   `session_save`/load and every existing session file stay compatible.
   State this in the report — session compat is user-facing.
4. Extend [tests/kv_cache.c](../tests/kv_cache.c) /
   [tests/test_kv_cache.c](../tests/test_kv_cache.c): fp16 round-trip error
   bounds and an attention-output parity check against f32 within a stated
   tolerance.

**Measure:** W-SESSION decode tok/s and `[phase] t_attn` vs baseline (the
headline number); W-DECODE (short context — expect ~neutral; confirm no
regression); `peak_rss` delta (KV bytes should halve — compute expected
bytes from the model config and check).

**Quality:** class 2 protocol. Additionally run W-SESSION's suite prompt —
long-context behaviour is where fp16 KV would bite if it bites.

**Acceptance:** at ≥4k context, `t_attn` reduction consistent with halved KV
traffic and a decode tok/s gain beyond noise; no suite regression on owner
review; footprint down. Negative result (attention share too small for the
target context lengths) closes the card — keep f32, record the numbers.

**Risk:** fp16 range is fine for K/V magnitudes in practice but verify: log
max |K|,|V| on the suite before trusting; overflow → saturation artifacts.

---

## E-X3 — Where do expert bytes actually live? Residency + LRU budget sweep  ~1–2 days

**Hypothesis:** on this 16 GB machine, expert bytes are held twice (engine
LRU + page cache). Growing the LRU is settled-slower (+41%); the *unasked*
question is whether the current budget is already past the sweet spot — a
**smaller** LRU frees RAM for the page cache, which may raise effective hit
rate and lower miss stalls. Also produces the first real memory map of the
machine at steady state.

**What exists:** buffered pread (page cache in play) — ground truth;
`EBUDGET_GB` / `ECACHE` env ([qwen36b.c:2295-2296](../src/qwen36b.c#L2295-L2296));
full `[ecache]` telemetry. **Record the current default budget** from the
code around [:2295-2330](../src/qwen36b.c#L2295-L2330) as step 0.

**Method:**
1. New standalone tool `tools/pagecache_residency.c` (~80 lines, no engine
   changes): `mmap(PROT_READ)` the 20.9 GB `experts.bin`, `mincore()` over
   it (16 KB pages ⇒ ~1.3 M-entry vector), report resident MB. Run it
   before/after each measured run.
2. Sweep `EBUDGET_GB` ∈ {default, −25%, −50%, minimum viable} × W-DECODE
   warm ×3. Record: decode tok/s, `expert_hit`, `expert_disk`,
   `bytes_read`, `bytes_avoided`, page-cache-resident MB, `peak_rss`,
   `vm.swapusage` delta.
3. One labelled `DIRECT=1` leg to isolate the page cache's contribution
   (expect slower; informative, not a candidate setting).
4. Draw the memory map: engine anon + LRU + experts-resident-in-page-cache
   + everything else, at steady state.

**Files:** `tools/pagecache_residency.c` (new). No engine changes.

**Acceptance:** a budget → {tok/s, hit rate, miss-stall ms, residency} table
with a stated recommendation. If the curve is flat, the double-caching
concern is closed with data. Any *default* budget change is an owner
decision (it touches every user).

**Safety:** measurement-only; `mincore` touches page tables, no I/O. Watch
`memory_pressure` during the small-budget legs — if the OS compressor
starts working, record it.

**Risk:** page-cache residency is fragile state — anything else running
perturbs it. Idle protocol is mandatory; check run-to-run stability before
trusting differences.

---

## E-X4 — Expert reuse locality, then prefetch overlap  ~2–4 days, hard gate between phases

**Hypothesis:** expert routing has temporal locality across consecutive
tokens; miss stalls (`expert_disk`) can be hidden behind attention compute
by prefetching predicted experts. Also produces the expert-union statistics
E-X8 needs for free.

**What exists:** `ROUTE_TRACE`/`ROUTE_REPLAY`
([qwen36b.c:763-799](../src/qwen36b.c#L763-L799) — verify the trace format
before scripting); prefetch admission + waste counters in the cache
([expert_cache.h:72-75](../src/expert_cache.h#L72-L75), [:180-181](../src/expert_cache.h#L180-L181));
`expert_disk` seconds in `[stats]`.

**Phase A — measurement only (~1 day):**
1. `ROUTE_TRACE` a W-DECODE and a W-SESSION run.
2. New `tools/route_locality.py`: per layer — expert reuse-distance
   distribution; next-token overlap (how much of token t+1's expert set is
   predicted by token t's); hot-set concentration (what fraction of
   (layer, expert) pairs serve 90% of references); and **expert-union size
   for windows of 4/6/8 consecutive tokens** (this is E-X8's cache-pressure
   input — compute it here once).
3. Combine with E-X1's warm `expert_disk` ms/token.

**Gate:** proceed to Phase B only if warm miss stalls ≥ ~10 ms/token *and*
next-token predictability is high enough that prefetch waste would stay
under ~20% of `bytes_read`. Otherwise close the card with the analysis —
that analysis is still the foundation of E-X8 and a pinning decision.

**Phase B — prototype (~2 days):**
1. One pthread prefetcher. **The cache API is not thread-safe**
   ([expert_cache.h:14](../src/expert_cache.h#L14)): the thread only
   `pread`s predicted experts into staging buffers; the engine thread
   admits them with `ECACHE_ADMIT_PREFETCH` at its next natural touch
   point. Predictor v1: token t's expert set predicts token t+1 (persistence
   — the simplest thing Phase A can validate).
2. Opt-in `SAMOSA_PREFETCH=1`. Print `wasted_prefetch_planes/bytes` in the
   `[ecache]` line (counters exist; they are not currently printed).
3. Measure W-DECODE/W-SESSION: decode tok/s, `expert_disk`, waste ratio.

**Quality:** none expected — prefetch changes *when* bytes are read, never
what is computed. Confirm same-seed byte-identity anyway (cheap).

**Acceptance:** decode tok/s up beyond noise with waste <20% and J/token not
worse than proportional to the gain; or a recorded negative. Reads cost
power, not SSD life (H1) — but they evict page cache, so re-run the E-X3
residency check with prefetch on.

---

## E-X5 — OpenMP schedule and core-class sweep  ~0.5–1 day

**Hypothesis:** `schedule(static)` on 4P+4E barriers every matmul on the
slowest thread; with ~150+ parallel regions per token the straggler tax may
be 5–20% at ≥4 threads. Dynamic or chunked scheduling recovers it for free.

**What exists:** every hot pragma is `schedule(static)`
([kernels.h](../src/kernels.h)); thread-count changes are byte-identical
(H3, retired risk — no reductions in `src/`).

**Method:**
1. Experiment build: change the hot kernels' pragmas to `schedule(runtime)`
   behind `-DSAMOSA_SCHED_RUNTIME` (Makefile experiment target; shipping
   build untouched). `OMP_SCHEDULE` then drives the policy without
   rebuilds. First check `schedule(runtime)` itself didn't cost anything:
   `OMP_SCHEDULE=static` must match the baseline build within noise.
2. Sweep {static, dynamic,16, guided} × threads {2, 4, 6, 8} × W-DECODE and
   W-PREFILL, warm, ×3.
3. Negative control: `taskpolicy -c utility` (forces efficiency-core class)
   to bound what an all-E-core run looks like.
4. Confirm same-seed byte-identity across all legs.

**Files:** [src/kernels.h](../src/kernels.h) (pragmas under the ifdef),
[Makefile](../Makefile) (experiment target).

**Measure:** tok/s and J/token per cell. 6/8-thread legs are expected
flat-to-worse (bandwidth-bound) and hotter — bound them to 5-minute runs
with powermetrics watched; abort on sustained pressure above Moderate.

**Acceptance:** a schedule × threads → tok/s + J/token table. Adopt a
schedule change only if ≥5% sustained with no J/token regression. Feed the
curve into H3's tuning (E-H3) and H5's tier table regardless of outcome.

---

## E-X6 — i8mm (`smmla`) runtime dispatch for batched integer paths  ~2–3 days

**Hypothesis:** the M3 has i8mm; the build doesn't use it. `smmla` computes
2×2 int8 dot tiles and roughly doubles throughput over `sdot` for S≥2 —
prefill and any future verify batch. Decode (S=1) is out of scope.

**What exists / constraints:** feature absent at default flags (ground
truth); the batched paths are `matmul_q_idot` / `matmul_i4_idot` /
`matmul_i4_grouped_idot` ([kernels.h:313-345](../src/kernels.h#L313-L345))
over `dot_i8i8`/`dot_i4i8`. H2's design rules apply: **runtime dispatch,
never a baked `-mcpu`** (one binary serves many Macs — M1 lacks i8mm), an
escape hatch (`SAMOSA_SIMD`), and a startup log line. Note H2's card says
"ARM needs no dispatch" — this experiment obsoletes that sentence; update
the card on `main` if E-X6 ships.

**Method:**
1. Step 0: confirm `sysctl hw.optional.arm.FEAT_I8MM` = 1 on the reference
   machine, and that Apple clang 21 accepts a per-function
   `__attribute__((target("arch=armv8.6-a+i8mm")))` with `vmmlaq_s32` in a
   scratch file. If the attribute path fails, fall back to one separate TU
   compiled with the extra `-march` in the experiment target.
2. Write `smmla` variants of the S≥2 inner loops (pair output rows × pair
   tokens per tile). Select via function pointer at startup:
   `[simd] arm=i8mm` / `arm=dotprod`; `SAMOSA_SIMD=dotprod` forces the old
   path.
3. Microbench in the style of
   [regressions/linux/x86-dispatch.md](regressions/linux/x86-dispatch.md):
   `matmul_q` I=2048 O=2048 S=8, GFLOP/s, both paths.
4. End-to-end: W-PREFILL at 2T/4T.

**Quality:** **bit-exact class.** Integer sums have no rounding freedom —
logits must be bit-identical between `sdot` and `smmla` paths on the full
suite. Any difference is a bug.

**Acceptance:** microbench ≥ +50% on S≥2 int8 GEMM; W-PREFILL tok/s ≥ +15%
end-to-end (Amdahl-bounded by E-X1's idot share — state the predicted bound
before running); bit-identical logits; J/token reported (expect improvement
— same math, fewer instructions, the E-H1 argument).

**Files:** [src/kernels.h](../src/kernels.h), startup log in
[src/qwen36b.c](../src/qwen36b.c), possibly [Makefile](../Makefile).

---

## E-X7 — Accelerate/AMX for resident dense prefill  ~3–5 days  **Owner note: adds an OS-framework link**

**Hypothesis:** the AMX matrix units (reachable only via the Accelerate
framework — private intrinsics are a hard non-goal) offer several times NEON's
GEMM throughput at lower power. Best case is **resident dense** prefill:
attention projections and lm_head see the full token batch. Experts do
*not*: with top-8-of-`n_experts` routing, each expert sees only a small
slice of the batch (verify the actual expert count from the snapshot config
at run time), so dequant-to-f32 barely amortizes there — experts stay on
the integer kernels.

**Method:**
1. Opt-in `SAMOSA_ACCEL=1`, `#ifdef __APPLE__`. In `matmul_qt_impl`
   ([kernels.h:347](../src/kernels.h#L347)): if S ≥ S_min (start 16) and
   fmt ∈ {int8, int4-row} and the tensor is resident (reuse the
   `cuda_eligible` marker, [kernels.h:41](../src/kernels.h#L41)), dequant a
   row block to an f32 scratch and `cblas_sgemm`. **Tile over output rows**
   (block ≈ 1024 rows × I=2048 × 4 B = 8 MB scratch) — never materialize
   lm_head at f32 whole (that would be >1 GB).
2. Link `-framework Accelerate` in the Makefile's Darwin branch. It ships
   with macOS — the zero-third-party-dependency claim survives — but it is
   a build change on every Mac, so it is flagged here for the owner.
3. Measure W-PREFILL at 2T/4T vs baseline, plus the E-X1 `[phase]` dense
   share before/after. State the Amdahl bound from E-X1 in the report
   *before* running.
4. Decode guard: S_min keeps decode on NEON; confirm W-DECODE unchanged.

**Quality:** class 2. Like-for-like numerical reference is the `IDOT=0`
float path ([qwen36b.c:2168](../src/qwen36b.c#L2168) forces it) — same
math, different reassociation; the default idot path additionally quantizes
activations, so compare against both and report divergence stats on the
suite.

**Acceptance:** prefill tok/s ≥ +20% end-to-end, or a recorded negative
with the microdata (dequant cost vs GEMM win per shape). J/token must not
regress — AMX should *improve* it; if it doesn't, say so. Prefill is the
project's stated binding constraint on documents/vision — a win here
changes those roadmaps' arithmetic; note it in the report.

---

## E-X8 — Speculative self-drafting: measure acceptance before building anything  ~2–3 days

**Hypothesis:** the MoE is its own draft model — `MOE_K=1` runs the full
architecture with top-1 experts at a fraction of the expert cost. If a
K=1 draft agrees with the full model often enough, drafting W tokens and
verifying them in one batched pass (prefill-shaped, where E-X6/E-X7 land)
multiplies decode throughput without changing outputs (greedy acceptance
keeps token identity). **This card authorizes only the measurement.** The
known risks that kill it: low acceptance; the verify pass needing the
expert *union* of W tokens per layer (working-set spike on 16 GB — E-X4
Phase A measures exactly this); KV rollback complexity.

**What exists:** `MOE_K` end-to-end with telemetry (ground truth) — the
draft model is runnable today with zero code. `ROUTE_TRACE` for union
stats. Possibly reusable scoring hooks: `REF` and `teacher_*`
([qwen36b.c:5228-5232](../src/qwen36b.c#L5228-L5232)) — **step 0 is to read
what they do**; if the engine already scores a token sequence under
teacher forcing, most of the harness exists.

**Method:**
1. Step 0: semantics of `REF`/`teacher_*`. If insufficient, add an opt-in
   score mode: teacher-force a provided token file, dump per-position
   argmax (and top-p mass) to TSV.
2. Draft speed: measure `MOE_K=1` decode tok/s directly (also record its
   `[moe-policy]` and hit-rate lines — the draft's own expert traffic
   matters). This single number bounds everything.
3. Acceptance: on 5 diverse ~512-token continuations (suite-adjacent:
   chat, code, summary), generate with `MOE_K=1` greedy, then teacher-force
   the full model over the same tokens and compute per-position agreement α
   and expected accepted-run length for W ∈ {4, 6, 8}.
4. Verify cost: time a batched forward of W tokens on a warmed engine (a
   W-token prefill continuation approximates it; note the approximation).
5. Cache pressure: expert-union sizes for W-token windows from E-X4 Phase A.
6. Model the speedup honestly:
   `tokens/step = E[accepted]+1`; `time/step = W·t_draft + t_verify(W)`;
   compare against measured `t_full`. Report the whole curve, not the best
   cell.

**Go/no-go:** modeled end-to-end speedup ≥1.4× at measured α and union
sizes → write a separate implementation card (KV rollback design,
sampling-mode acceptance, cache-pressure mitigation) for owner review.
Below that → close with the numbers; the target then rests on E-X9, the
Metal track, or a lower target. **Publish the full α/W curve either way**,
not just the verdict: E-X10 M3 replaces `t_verify` with a much cheaper
GPU batched pass, so an α that fails this CPU-only gate may still fund
CPU-draft/GPU-verify — the curve lets that cell be recomputed without
re-measuring.

**Quality:** the measurement phase has no quality surface. An eventual
implementation is class 3 at minimum during bring-up (greedy acceptance is
exactness-preserving *when correctly implemented* — prove it, don't assume
it).

**Files (measurement):** `tools/spec_accept.py` (new); possibly a small
score-mode addition in [src/qwen36b.c](../src/qwen36b.c).

---

## E-X9 — Adaptive top-k: measure the machinery that already exists  ~1–2 days + owner decision

**Hypothesis:** when router mass concentrates, computing fewer experts per
token cuts expert bandwidth/compute 20–40% at negligible quality cost. The
engine already implements the policy (`MOE_MASS` + entropy/gap guards) and
already reports `avg_k` and a bytes-saved proxy — what appears to be
missing is any *measurement* of speed and quality.

**Method:**
1. Step 0: search the repo and `docs/regressions/` for any prior
   measurement of `[moe-policy]` (grep `moe-policy`, `MOE_MASS`). If found,
   start from it; do not re-derive.
2. Sweep `MOE_MASS` ∈ {0.95, 0.90, 0.85, 0.80}, guards off; then the best
   candidate with `MOE_MAX_ENTROPY`/`MOE_MIN_GAP` engaged (read
   [qwen36b.c:566-606](../src/qwen36b.c#L566-L606) for exact semantics
   first). W-DECODE and W-SESSION, warm, ×3.
3. Record per cell: decode tok/s, `avg_k`, `saved=%`, effective-k
   histogram, `expert_hit`, `expert_disk`, J/token.
4. Quality: **class 3, the strongest gate in this card.** Full 12-prompt
   suite side-by-side vs baseline at every candidate setting; divergence
   stats; owner reviews the outputs. Fewer experts is the one idea here
   that changes what the model computes on every token.

**Acceptance:** a mass → {tok/s, avg_k, quality verdict} curve. A setting
may be *recommended* only with owner sign-off; the default does not change
in this program. Negative result (quality drops before meaningful speed
appears) closes the card and is worth publishing in the report — it
validates the trained top-k.

**Safety:** fewer experts = less bandwidth = cooler; expect J/token to
improve. Verify, don't assume.

---

## E-X10 — The Metal track (M0–M3): a native GPU arm for the C engine, running the custom q4 format in place

**Status: reopened by the owner, 2026-07-17.** This card previously
deferred Metal to the post-release track that
[TASKS_HARDWARE.md](TASKS_HARDWARE.md) recorded as a non-goal. The owner
explicitly reopened it ("can our C engine not have a parallel capability to
unlock Apple Metal and run our custom quantization?") — the answer is yes,
and this section is the design. The hardware card's non-goal is annotated
as superseded. House rules unchanged: staged, opt-in, measured before
claimed, and **M0 is the RUN-FIRST that can kill the whole track in one
day** — a good outcome if it does.

### Why this is native to Samosa, not a port (three unlocks)

1. **Unified memory: the GPU reads our bytes as they are.**
   `newBufferWithBytesNoCopy` wraps memory the engine already owns as a
   GPU-visible buffer — zero copy, no second model in RAM, no format
   conversion. A Metal shader reads whatever byte layout it is told to:
   teaching it `groupwise-symmetric-q4-v1` (16 packed nibbles + one f32
   scale per 32 weights) is ~150 lines of MSL mirroring
   [kernels.h:121-143](../src/kernels.h#L121-L143) line for line.
   "MLX cannot read our format" is true; "the GPU cannot" was never true.
2. **Zero build-system damage.** Metal shaders compile at runtime from a
   source string (`newLibraryWithSource`). The MSL lives as a C string in
   the binary: no `.metallib` artifact, no new build step, no third-party
   code — just `-framework Metal` on the Makefile's Darwin branch, an OS
   framework exactly like E-X7's Accelerate. The "no dependencies, no
   build system" identity survives literally.
3. **The expert cache anticipated this by name.**
   [expert_cache.h:8-13](../src/expert_cache.h#L8-L13) keeps payload
   allocation outside the module precisely to leave "mmap, malloc,
   **Metal**, and other storage choices" open, and `payload_alignment` is
   already a config field ([expert_cache.h:101-107](../src/expert_cache.h#L101-L107)).
   Set it to 16384, allocate the cache budget as **one page-aligned
   arena**, wrap the arena as a single MTLBuffer, and every slab the LRU
   manages is GPU-addressable by offset — LRU, pressure ladder, and stats
   all unchanged. The dispatcher already has a GPU seam to mirror
   ([kernels.h:347-357](../src/kernels.h#L347-L357), the `COLI_CUDA` hook,
   including its tested fall-back-to-CPU-on-failure pattern).

### Target architecture (what exists after M2)

**Split by tensor, not by op.** GPU owns the expert FFN — the
bandwidth/compute hog, computed in native q4. CPU keeps attention, norms,
router, KV, and sampler: serial, small, already fast with dotprod, and
AMX-augmentable (E-X7). The two run as a per-layer pipeline inside **one
command buffer per decode token**:

```
CPU: attn L → router L → write {expert offsets, k, activations} to shared args
     → signal cpu_ready[L] ──────────────┐
GPU:                     wait cpu_ready[L] → indirect-dispatch expert FFN(L)
     ┌──────────────────── signal gpu_done[L]
CPU: wait gpu_done[L] → attn L+1 …
```

- `MTLSharedEvent` wait/signal pairs are encoded per layer inside the one
  command buffer (`encodeWaitForEvent:`/`encodeSignalEvent:` between
  compute encoders), so dispatch overhead amortizes to **one commit per
  token** instead of ~48.
- The router writes expert *arena offsets* into an argument buffer and the
  FFN launches via `dispatchThreadgroupsWithIndirectBuffer:` — no CPU→GPU
  chatter beyond the event signal.
- The CPU's idle window while the GPU chews layer L is exactly where
  E-X4's prefetch reads and sampler prep belong — the tracks compound.
- Opt-in `SAMOSA_METAL=1`; startup logs a `[metal]` line (device, unified
  memory size, arena vs mmap mode); the CPU path remains the default and
  the runtime fallback, mirroring `cuda_failed`.

### The radical variant M0 must answer (page cache as the GPU's streaming layer)

Instead of the arena: mmap `experts.bin` read-only, wrap the whole mapping
as one no-copy MTLBuffer (21 GB of address space is nothing on arm64), and
let the shader index experts by **file offset** — which the manifest
already parses ([qwen36b.c:1180-1206](../src/qwen36b.c#L1180-L1206)). The
OS page cache then does streaming and eviction, the GPU-side twin of
E-X3's question. llama.cpp ships this exact pattern (mmap + no-copy Metal
buffers) on Apple Silicon — precedent, not proof. **The unknown that
decides it:** residency semantics. If Metal wires mapped pages at encode
time, whole-file wrapping is a footprint bomb and the arena wins; M0
measures the wired-memory delta directly. Two Apple-native garnishes
either way:

- `setPurgeableState(volatile)` on cold slabs lets the **OS** reclaim
  cache memory under pressure without asking us — a hardware-assisted
  version of the ecache pressure ladder
  ([expert_cache.h:91-99](../src/expert_cache.h#L91-L99)); a reclaimed
  slab re-enters through the existing miss path.
- `MTLIOCommandQueue` (macOS 13+) issues SSD→buffer loads off the CPU
  entirely — a candidate replacement for E-X4 Phase B's prefetch thread.

### The physics, honestly

- The GPU shares the same ~100 GB/s DRAM. It adds **zero bandwidth**; what
  it adds is more efficient compute and CPU/GPU concurrency. At ~9.5 tok/s
  we are far from the bandwidth ceiling, so there is real room — but no
  claim of "GPU = faster memory" may ever appear in a report.
- On a fanless chassis the honest metric is **J/token** (E-H1): GPU GEMM
  burns fewer joules per FLOP than NEON, and lower J/token is what lets a
  speed *sustain* instead of throttling away. Even tok/s parity at lower
  J/token is a win worth shipping.
- Decode stays sequential across layers. The only concurrency is the
  within-token CPU/GPU overlap above and the M3 speculation pipeline —
  never oversell it as parallel decoding.

### Known costs, each with a planned measurement

| Cost | Measured by | Mitigation if bad |
|---|---|---|
| Sync tax: ~48 event round-trips/token | M0.4 (µs per ping-pong × 48) | GPU takes consecutive layer *pairs*; or M1-only (prefill has no such tax) |
| Metal runtime + pipeline memory vs the 4.5 GB footprint ceiling | M0 footprint delta | arena mode; smaller pipeline set |
| GPU float reassociation ≠ CPU | class-2 quality protocol (same as E-X7) | report divergence stats; suite review |
| Identity: ObjC enters a pure-C codebase | — | one small fenced `.m` file (or C via `objc_msgSend`) + embedded MSL string; diff stays reviewable; owner accepted the direction 2026-07-17 |
| Driver/OS variance | log Metal feature set in `[metal]` line | claims scoped to the reference machine only, per standing rules |

### M0 — the spike  ~1 day  **RUN THIS FIRST — kills or funds the entire track**

Standalone `tools/metal_spike.m` (or `.c` + ObjC runtime), **zero engine
changes**; it includes [src/kernels.h](../src/kernels.h) directly so the
CPU reference (`matmul_i4_grouped`/`_idot`) is the exact production code.
Six numbered measurements, all in one report:

1. **Correctness.** Synthetic q4-group-32 tensors *and* one real expert
   slab read straight from `experts.bin` via manifest offsets. GPU
   dequant-dot vs CPU reference: report max abs/rel error. Expect f32
   reassociation noise (~1e-6 relative), not zero — say so.
2. **Throughput.** Expert-shaped matmuls at S ∈ {1, 8, 32, 128}: GFLOP/s
   GPU vs NEON 1-thread and 4-thread. The S=1 cell decides whether M2 is
   even plausible; the S≥32 cells decide M1.
3. **Energy.** `powermetrics` during sustained loops on both sides →
   J/GFLOP, CPU vs GPU. **Kill criterion: GPU J/GFLOP ≥ CPU ⇒ the track
   dies here** — on a fanless machine there is no reason left. Record and
   close.
4. **Sync latency.** 1,000 iterations of empty-dispatch
   `MTLSharedEvent` ping-pong → µs/round-trip → ×48 = predicted decode
   tax. >~200 µs/round-trip ⇒ M2 redesigns to layer pairs before it starts.
5. **No-copy arena.** `posix_memalign(16384, …)` arena +
   `newBufferWithBytesNoCopy` (StorageModeShared): creation succeeds, GPU
   reads the same bytes the CPU wrote.
6. **mmap leg.** Wrap a mapped region of the real `experts.bin`; run the
   kernel over a **cold** region; measure wired-memory and RSS delta
   (`vm_stat`, `footprint`) and first-touch latency. This single number
   decides arena vs whole-file for M1/M2.

**Acceptance:** all six numbers in
`docs/regressions/experiments/e-x10-m0-spike/report.md` with an explicit
go/no-go verdict per criterion. **Safety:** standard telemetry; a
sustained GPU loop is a new thermal profile for this chassis — watch
pressure continuously, bound loops to minutes, abort rules apply.

### M1 — GPU expert prefill  ~3–5 days  (gated on M0)

Prefill has no sync-tax problem (few, large dispatches) and lands on
time-to-first-token, which the felt-speed decision made first-class.

- Per layer, the CPU builds per-expert token lists (the MoE scatter), then
  one compute encoder per layer runs a 2D grid (expert × token-block) via
  indirect args. Activations f32 first; f16 staging as an optional second
  step (halves activation traffic; class-2 quality check).
- Integrates behind `SAMOSA_METAL=1` at the MoE call site with the CPU
  path intact as fallback.
- **Measure:** W-PREFILL at 2T/4T vs CPU baseline **and vs E-X7** — AMX
  and the GPU compete for prefill ownership. Run both; keep the winner;
  record the loser's numbers. A split verdict is legal and likely: AMX
  for resident dense + GPU for experts do not conflict.
- **Quality:** class 2. **Acceptance:** ≥25% end-to-end prefill
  improvement over the better CPU-only configuration, or close with data.

### M2 — the single-command-buffer decode pipeline  ~1–2 weeks  (gated on M0.2 S=1, M0.4, and M1 experience)

The target architecture above, built: arena (or mmap, per M0.6) expert
buffer, per-layer event ping-pong, indirect dispatch from the router,
double-buffered activation staging, KV and sampling untouched on the CPU.

- **Measure:** W-DECODE tok/s and J/token vs CPU baseline — and sweep CPU
  threads {1, 2, 4} with the GPU on. The most interesting cell is *fewer*
  CPU threads + GPU: if 2T+GPU beats 4T CPU-only at lower package power,
  that is the fanless-chassis jackpot (faster *and* cooler than today's
  `--fast`).
- **Quality:** class 2, full protocol.
- **Acceptance:** decode ≥ CPU baseline at equal-or-lower J/token with a
  clean quality run — parity-at-lower-J/token explicitly counts (it raises
  *sustained* speed). Otherwise close with the numbers; M1 can stand alone.

### M3 — CPU-drafts, GPU-verifies: the compound play  (gated on E-X8's α and M2)

The division of labor each processor is built for: the CPU runs the
serial, latency-sensitive `MOE_K=1` draft; the GPU verifies W tokens in
one batched pass — batching is precisely where GPUs stop being
latency-bound. The two pipeline: GPU verifies window N while the CPU
drafts window N+1.

- **The cost model shifts:** E-X8's speedup formula
  (`(E[accepted]+1) / (W·t_draft + t_verify(W))`) was gated at CPU verify
  cost. With `t_verify_gpu(W)` collapsed, a draft-agreement rate α that
  fails E-X8's CPU-only gate may still fund M3 — E-X8's report must
  therefore publish the *curve*, not just the verdict, so this cell can be
  recomputed without re-measuring.
- Greedy acceptance preserves token identity **when correctly
  implemented** — prove it with token-identity runs during bring-up, never
  assume it.
- This is the strongest single candidate for closing the last gap to 15:
  the multiplier (speculation) × the efficient verifier (Metal) × the
  cache pre-warm (draft touches the experts verify needs) stack on the
  same tokens.

---

## E-X11 — Yardstick: what do Metal-native engines achieve on this exact machine?  ~0.5–1 day, measurement-only

**A question, not a hypothesis.** The 12–15 tok/s target is currently
calibrated against nothing but our own CPU numbers. MLX (Apple's
Metal-native ML framework; `mlx-lm` runs quantized MoE LLMs on Apple
Silicon) and llama.cpp's Metal backend are the best available software for
this hardware. One afternoon of measurement answers: **what does the
platform actually deliver on this machine, and does a ~17 GB MoE even
survive 16 GB?** Both outcomes are valuable: a high number means the
platform ceiling is far away and E-X10 gains ambition; a swap-thrashing
failure **validates Samosa's streaming architecture with a number** instead
of an argument. Either way the target stops being calibrated in a vacuum.

**Explicit framing — a yardstick, not a migration proposal.** Adopting
either engine is a non-goal (below). Nothing measured here implies parity:
no public model matches our exact 35B-A3B custom quant, so the comparison
is directional and must be reported as such.

**Method:**
1. Sandboxed, outside the repo: `python3 -m venv ~/tmp-mlx &&
   ~/tmp-mlx/bin/pip install mlx-lm`. Nothing is added to Samosa, its
   build, or its dependencies. Delete the venv afterwards.
2. Closest public cousin — a 4-bit MLX community quant of **Qwen3-30B-A3B**
   (~17 GB, nearest MoE relative). Record the exact model repo and
   revision in the evidence. Note the download is ~17 GB; get owner OK on
   disk/bandwidth before pulling it.
3. One small fully-resident control (e.g. a ~4B dense 4-bit quant) for a
   clean platform-tok/s datapoint with zero memory pressure.
4. Workload mirrors W-DECODE: ~1k-token prompt, 256 new tokens, 3 runs;
   record `mlx_lm.generate`'s reported prompt/generation tok/s plus wall
   clock.
5. Optional leg: llama.cpp Metal with a GGUF of the same model — it mmaps
   weights and can run larger-than-RAM, making it the closest analogue to
   Samosa's own problem; its behaviour under that pressure is the most
   informative single number in the card.
6. **Safety telemetry is mandatory and stricter here** — this is the
   likeliest experiment in the program to swap: watch `sysctl vm.swapusage`
   continuously, **abort at +2 GB swap delta** (swap writes are the real
   wear vector — H1); powermetrics thermal/power throughout; the 30B model
   may fail to load or beachball the machine — run nothing else alongside,
   be ready to kill it, and record a failure verbatim as the result.

**Measure:** generation and prompt tok/s, peak memory, swap delta, thermal
pressure, J/token where obtainable — same table columns as everything else.

**Acceptance:** an engine × model → numbers table plus an interpretation
paragraph: where the platform ceiling appears to be, what that implies for
E-X10's priority, and a restatement that no migration is proposed. A
failed-to-run outcome is a complete, publishable result.

**Risks:** model mismatch invites false comparisons — never quote these
numbers next to Samosa's without the caveat in the same sentence; a
swapping run stresses the machine — the abort bound exists precisely so
curiosity doesn't cost SSD life.

---

## Non-goals (settled elsewhere or rejected here)

- **Growing the engine expert LRU.** Measured +41% slower — settled in
  [TASKS_HARDWARE.md](TASKS_HARDWARE.md). E-X3 may only shrink it.
- **Private AMX intrinsics.** Undocumented ISA, breaks on any silicon or OS
  revision, unsupportable. Accelerate or nothing.
- **ANE for the LLM.** Core ML's static-graph model is hostile to streamed
  q4 MoE decode. Genuinely promising for the vision tower
  ([TASKS_VISION.md](TASKS_VISION.md)) — that is a different program.
- **Shipping `-mcpu=native`/`-march=native`.** One binary serves many Macs;
  G10's exact trap. Runtime dispatch only (E-X6).
- **Changing default thread count or the owner's 2-thread comfort
  preference.** H3/H5 own thread policy; this program only feeds them
  measurements.
- **More threads as a "fix".** Bandwidth-bound decode on a fanless chassis:
  heat, not speed. E-X5 measures precisely once, then the question closes.
- **Publishing any new performance claim** before it is measured on the
  real model through the real interactive path, per the standing rules.
- **Adopting MLX or llama.cpp as the engine.** That is replacement, not
  optimization: a third-party framework against the product's
  no-dependency identity, no reader for the custom groupwise-q4 /
  row-quant formats, and no expert-aware disk streaming for the
  larger-than-RAM case Samosa is built around. E-X11 measures them as
  yardsticks only. Mining *ideas* is encouraged — MLX's open-source Metal
  kernels are the reference reading for E-X10 — the engines themselves
  stay out.

## Open questions

- What do `REF` and `teacher_corpus`/`teacher_output` actually do
  ([qwen36b.c:5228-5232](../src/qwen36b.c#L5228-L5232))? E-X8 step 0; may
  also give E-X2/E-X7 a ready-made scoring harness.
- Could the refinable **base planes** (`REFINE_*`,
  [qwen36b.c:5246-5255](../src/qwen36b.c#L5246-L5255)) serve as a cheaper
  draft than `MOE_K=1` — base-only experts instead of fewer experts? Worth
  one paragraph of analysis in E-X8's report.
- Is page-cache residency stable enough run-to-run on 16 GB for E-X3's
  differences to be trustworthy? The stability check is part of the method;
  if it fails, E-X3 downgrades to bounds, not point estimates.
- ~~Where does the 12–15 tok/s target bind?~~ **Answered by the owner,
  2026-07-17: generic felt speed** — an ordinary chat, no assumption of very
  long sessions. Reflected in W-DECODE's definition (~1k context, the gate),
  the run order (E-X8/E-X9 promoted, E-X2 demoted to footprint/safety), and
  the note that time-to-first-token keeps E-X6/E-X7 first-class.
