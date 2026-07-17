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

Do not rerun E-X1 until the owner confirms the machine is otherwise idle and
swap has returned to the pre-run level.  Start a privileged `powermetrics
--samplers cpu_power,thermal -i 1000` capture in another terminal before the
next attempt.  Only then run the one warm-up plus three measured runs for each
2T/4T workload leg.
