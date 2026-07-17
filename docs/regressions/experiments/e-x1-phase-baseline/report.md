# E-X1 phase telemetry and baseline attempt

Date: 2026-07-17.  Branch: `experiments/e-x1-phase-baseline`.
Model resolution was verified from the product launcher: the active release is
`~/.samosa/current` and the snapshot is `~/.samosa/current/model`; the paired
tokenizer is `~/.samosa/current/tokenizer_qwen36.json`.

## Result: baseline aborted for swap safety

The short real-model smoke run verified that `SAMOSA_PHASE_STATS=1` emits one
prefill/decode `[phase]` line and does not alter the model path.  It is not a
baseline workload and must not be used as a performance claim.  Its full output
is in [raw_sanity.log](raw_sanity.log).

The first W-DECODE setup step, which prefilled the committed controlled context
to create a resumed-session seed, crossed the mandatory swap-safety bound while
still running: swap used moved from 246.06 MB to 1,477.00 MB.  The process was
interrupted immediately.  Twenty seconds after exit, swap use remained at
1,477.00 MB.  This violates the E-X1 requirement that swap-used delta be
approximately zero, so the 2T/4T workload sweep, overhead comparison, phase
table, and quality baseline were not run.  Exact command and readings are in
[raw_decode_seed_abort.log](raw_decode_seed_abort.log).

`pmset -g therm` showed no thermal or performance warning before or after the
attempt.  `powermetrics` could not run noninteractively (`sudo -n true` failed),
so package power and joules/token are **not measured**.

## W-DECODE server-path attempt (not a clean baseline)

The W-DECODE fixture tokenized to a 949-token seed prompt; its saved session
contained 950 tokens.  Because the CLI destroys the engine LRU at the end of
every invocation, the real HTTP server path was used to preserve the LRU through
the warm-up and three measured legs.  Each request restored the same session
before generating.  The model naturally stopped at 234 output tokens, so the
reported decode rate uses its 233 stepped tokens.

| Threads | Warm-up tok/s | Measured tok/s | Median | Result |
|---:|---:|---|---:|---|
| 2 | 6.28 | 6.33, 6.11, 5.89 | 6.11 | misses the 12–15 gate |
| 4 | 7.55 | 7.69, 7.62, 7.56 | 7.62 | misses the 12–15 gate |

At the median 4T leg, the phase breakdown was 13.36 ms/token attention, 4.68
router, 25.47 dense resident work, 24.06 expert matmul, **55.07 expert-disk**,
7.65 head/sampler, and 0.94 other: 131.23 ms/token total.  The phase sum agrees
with the 30.575 s / 233-token decode wall time.  This is strong evidence that
warm expert I/O remains the first lever to investigate, but it is not yet a
claim about a clean, idle-machine baseline.

Safety during these legs: AC, no `pmset` thermal/performance warning, swap
used fell from 1,429 MB to 1,373 MB, and pageouts rose about 11.7 MB.  This
passes the per-leg swap/pageout bounds relative to the server start.  It remains
non-baseline because the host was not otherwise idle and `powermetrics` was not
available.  The engine's existing `[stats]` expert hit/disk/mm totals are
cumulative across persistent server turns; the new `[phase]` counters reset
per turn, so only the latter are used as per-run phase evidence.

See [raw_w_decode_server.log](raw_w_decode_server.log) for commands and all
captured `[stats]`/`[phase]` lines.

## W-PREFILL 4T attempt — stopped for footprint investigation

The committed W-PREFILL source plus a fixed four-bullet summary instruction
tokenized to 2,013 prompt tokens.  The 4T prefill completed in 103.208 seconds
(19.50 tok/s) before the requested 32-token generation was cancelled.  This is
not an accepted W-PREFILL result: at about 60 seconds, `/healthz` reported the
server's current physical footprint at 4.71 GB (4.76 GB in the completion),
which is above the card's ~4.5 GB guard.  The cancellation endpoint cannot
interrupt an in-flight prefill, so the prefill completed before it took effect.

There is a material instrumentation discrepancy: the same run's `[stats]`
line reports `peak_rss=4.17 GB`, whereas `/healthz` uses macOS current physical
footprint and reported 4.76 GB.  The latter is the more relevant safety value
on this machine.  No further W-PREFILL, W-SESSION, W-SUSTAIN, or dependent
experiment will run until this discrepancy and the footprint bound are
understood.  Swap did not grow and `pmset` remained nominal, but no J/token
measurement was possible without `powermetrics`.

The phase measurement itself is useful: 13.26 ms/token attention, 4.10 router,
15.38 dense, 15.90 expert matmul, 1.16 expert disk, 0.03 head, and 1.45 other.
The prefill is compute-dominated after the warm server cache, so its next
candidate is E-X6/E-X7—not disk prefetch.  Full evidence is in
[raw_w_prefill_abort.log](raw_w_prefill_abort.log).

## Host-storage and swap observations (2026-07-17)

The model files under `~/samosa_release_upload`,
`~/Documents/samosa-models/qwen36_group32_i8`,
`~/Documents/samosa-hf-group32-staging`, and `~/.samosa/current/model` are
hard links, not separate 24 GB copies.  For example, every `experts.bin` has
device `16777234`, inode `55519033`, size 20,942,159,872 bytes, and link count
12; every `resident.safetensors` has inode `55521667` and the same link count.
All four manifests have SHA-256
`12ad73a9457e5d88b7cd4b00cae4a5c7ccb9031aa10d1111b80932d115f224d4`.
`du` reports 22 GB for each directory when considered alone, but reports 22 GB
for the combined set (the later hard-link directories contribute only 112 KB,
616 KB, and 0 B respectively).  Do not delete any directory as part of an
experiment: the active app uses `~/.samosa/current/model`; the other two
top-level trees are release/upload/staging material and are not extra model
payload storage.

The >1 GB swap observation is global macOS state, not the model process's
reported RSS.  `vm.swapusage` includes swapped pages from every process and
macOS need not immediately release or shrink its swapfiles once pressure has
passed.  The first long CLI prefill moved global swap from 246 MB to 1,477 MB,
but the later persistent-server W-DECODE and W-PREFILL legs moved it downward
(1,429 MB to 1,333 MB) rather than growing it.  We therefore cannot attribute
the initial increase solely to Samosa from the data collected: the host had
active VS Code/Codex processes and only global VM counters were sampled.

There is nonetheless a real safety instrumentation gap.  On the W-PREFILL
attempt the server's macOS physical-footprint value was 4.76 GB while
`[stats] peak_rss` was 4.17 GB.  Future runs must record both current physical
footprint and VM deltas, start from an otherwise idle host, and obtain the
privileged power/thermal capture before a performance conclusion is accepted.

The engine now prints `physical_rss` beside legacy `peak_rss` in every
`[stats]` line (commit `9d0dcca`).  A real 4T W-DECODE verification produced
`peak_rss=3.44 GB physical_rss=4.37 GB`; this confirms that macOS physical
footprint, not the legacy `getrusage` number, is the conservative guard for
this program.

## Required next condition

Do not rerun E-X1 until the owner confirms the machine is otherwise idle.
Immediately before the next attempt, record fresh `vm.swapusage` and `vm_stat`
baselines, then apply the card's actual per-run gate: swap-used delta
approximately zero and pageout delta below 100 MB.  macOS may retain old swap
allocations after pressure has passed, so its current global swap high-water
mark is not itself a reason to reject a fresh, stable baseline.  Start a
privileged `powermetrics --samplers cpu_power,thermal -i 1000` capture in
another terminal before the next attempt.  Only then run the one warm-up plus
three measured runs for each 2T/4T workload leg.

## Preflight check — no model run (2026-07-17)

A read-only preflight was performed before resuming the card.  No `qwen36b` or
Samosa process was running and `memory_pressure` reported 79% system-wide free
memory, but `vm.swapusage` still reported 1,300.94 MB used.  This is not the
246.06 MB global value recorded before the earlier attempt.  It does not show
current pressure by itself: a subsequent sample reported 3.30 GB unused memory
and no new swap-ins or swap-outs in the sampling interval.  The next experiment
will instead use its own immediately-before-run VM baselines and evaluate their
deltas.  `pmset -g therm` showed no thermal or performance warning.

Privileged power capture is also unavailable to the noninteractive experiment
shell: `sudo -n true` returned `sudo: a password is required`.  The owner can
start the required capture in a separate terminal; its output can be written to
a file for the experiment to archive.  No real-model invocation was started in
this preflight.  This is a preflight observation only, not a performance or
safety result.

## Clean 2T W-DECODE seed attempt — stopped for physical footprint (2026-07-17)

The owner started the required privileged `powermetrics` capture and confirmed
the machine was available for the experiment.  `qwen36b` was rebuilt with
`make omp` at commit `d809490fb0d8ffadff30380bed444d062188a7f9`; the source
and experiment configuration were otherwise unchanged.  A fresh, local-only
2-thread server was then started with `SAMOSA_PHASE_STATS=1`, a 2.07 GB default
expert-cache budget, and an isolated chat directory.  As required for the
resident-cache protocol, the committed W-DECODE context was first sent as a
one-token request to create the reusable session.  It tokenized to 950 prompt
tokens and saved a 951-token, 104.8 MB session.  The exact client/server
commands, VM polls, health readings, response, and engine telemetry are in
`raw_e_x1_clean_w_decode_2t_client.log`,
`raw_e_x1_clean_w_decode_2t_seed_response.json`, and
`raw_e_x1_clean_w_decode_2t_server.log` beside this report.

This seed is not a W-DECODE timing result: it contains the cold session prefill
and generated only one token.  Its phase telemetry is nevertheless captured
verbatim in the server log: 74.838 s total / 12.69 prefill tok/s, with 19.11
ms/token attention, 7.41 router, 24.68 dense, 22.20 expert matmul, 2.67
expert-disk, 0.05 head, and 2.65 other.  The phase buckets sum to the recorded
wall time within rounding.

Safety telemetry was good except for the footprint guard.  Immediately before
the seed, global swap used was 1,284.94 MB and `vm_stat` pageouts were 451,775;
after clean shutdown they were 1,276.94 MB and 452,166.  This is no swap growth
and 391 16-KiB pages (about 6.1 MiB) of pageouts, well inside the per-run VM
bound.  `pmset` reported no warning and every sampled `powermetrics` thermal
reading was Nominal.  The 75 one-second CPU-power samples while the prefill was
active averaged 8,490.5 mW (6.94–10.45 W); that is about 636.8 J, or 0.670
J/prompt token.  This is CPU-only prefill energy, not a decode J/token claim.
The full privileged capture is `raw_e_x1_clean_powermetrics.log`.

However, the server's macOS physical-footprint telemetry rose to **4.51 GB**;
the engine reported the same `physical_rss=4.51 GB` beside its legacy
`peak_rss=3.37 GB`.  The server was shut down immediately after the seed and
before the warm-up or any measured 256-token W-DECODE leg.  This is at/just
over the program's approximately 4.5 GB warmed-footprint guard, and continuing
to fill the expert cache would not be responsible.  No adjustment to that
guard or to the default cache budget is being inferred from one attempt.

**Result at this point:** E-X1 remained incomplete; W-PREFILL/W-SESSION,
overhead comparison, and the quality baseline were still unrun.  The positive
finding was that the safety instrumentation worked end-to-end: physical
footprint, VM deltas, thermal pressure, and CPU power were all captured for
the first time under the real model.

## Clean W-DECODE baseline — accepted under the owner's 4.51 GB tolerance (2026-07-17)

The owner explicitly accepted 4.51 GB as within the card's approximate 4.5 GB
footprint guard.  The saved 951-token session from the seed attempt was copied
before every leg, so each request began from identical context.  A fresh
persistent server was used per thread count, then one warm-up and three
measured requests were sent with the fixed prompt `Continue with a concise
operational reminder.`, `max_tokens=256`, `thinking=off`, `temperature=0`, and
`seed=1729`.  All requests reached the 256-token ceiling, yielding 255 timed
decode steps.  Complete server, client/VM, response, and privileged-power logs
are `raw_e_x1_w_decode_{2t,4t}_{server,client,powermetrics}.log` and the
adjacent `*_response.json` files.

| Threads | Warm-up decode tok/s | Measured decode tok/s | Median | Physical footprint |
|---:|---:|---:|---:|---:|
| 2 | 6.26 | 6.24, 6.26, 6.33 | **6.26** | 4.38 GB |
| 4 | 7.64 | 7.58, 7.60, 7.62 | **7.60** | 4.38 GB |

The 4T median is 21.4% faster than 2T, but still misses the 12–15 tok/s
felt-speed gate.  It does not change the owner's default comfort policy.

| Median phase | 2T ms/token | 4T ms/token |
|---|---:|---:|
| attention | 23.14 | 13.12 |
| router | 7.90 | 4.56 |
| resident dense | 28.59 | 25.53 |
| expert matmul | 30.70 | 23.37 |
| **expert disk** | **59.70** | **56.11** |
| head / sampler | 8.80 | 7.88 |
| other | 0.85 | 0.92 |
| **phase sum** | **159.68** | **131.49** |
| measured wall (`1 / tok/s`) | 159.74 | 131.58 |

The phase totals agree with wall time to less than 0.1%.  Expert-disk stalls
are the largest decode bucket at both thread counts; adding threads reduces
attention, router, and expert-matmul time, but only lowers disk time by 3.59
ms/token.  This is the first clean evidence for E-X3/E-X4/E-X8's cache and
route-locality questions.

For the 2T measured legs, CPU-power sampling windows averaged 6.58, 6.69, and
6.79 W and produce 1.054, 1.068, and 1.072 estimated CPU J/decode-token
(median **1.068 J/token**).  At 4T they averaged 8.94, 9.29, and 9.67 W and
produce 1.179, 1.222, and 1.269 J/token (median **1.222 J/token**).  These are
time-aligned `powermetrics` CPU-power estimates using the engine's decode
duration, not total machine energy.  The 4T speed gain therefore comes with an
estimated 14% CPU-energy-per-token increase; this is an observation, not a
thread-policy recommendation.

All measured legs held physical footprint at 4.38 GB, kept global swap at
1,276.94 MB (zero growth), and had Nominal thermal pressure / no `pmset`
warning.  Each pageout delta was below 8 MiB (all far below the 100 MB bound).
The eight warm-up/measurement responses (both thread counts) had the identical
assistant-content SHA-256 `5b7237368368054bc8776cf861068f359d9936f2ab321cef5871d5cf4a1a56d1`.
E-X1 is still incomplete: W-PREFILL, W-SESSION, the phase-timer overhead
comparison, and the 12-prompt quality baseline remain to be run before the
dependent cards can be accepted.
