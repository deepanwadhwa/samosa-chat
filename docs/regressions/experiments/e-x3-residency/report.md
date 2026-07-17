# E-X3 — page-cache residency and expert-cache budget (2026-07-17)

## Status

**Partial result; default retained.** The first material reduction in the
expert-cache budget was a performance negative under the recorded safety
protocol. The full E-X3 sweep is not complete: the -50%, minimum-viable, and
`DIRECT=1` legs remain deferred. No engine default changes are recommended.

The live `powermetrics` collector reported `Nominal` thermal pressure for the
entire 20-minute captured interval (12:25:00–12:45:59 EDT), including every
model request. The physical footprint gate was below the owner-authorized
strict 5 GB ceiling throughout. The raw collector capture is
[raw_e_x3_2t_powermetrics.log](raw_e_x3_2t_powermetrics.log).

## Probe added before runtime work

[tools/pagecache_residency.c](../../../../tools/pagecache_residency.c) maps a
supplied regular file read-only and calls `mincore(2)` over that mapping. It
never dereferences the mapping, so it queries page-cache residency without
warming `experts.bin`. It reports the system page size, page count, resident
page count, resident bytes, and percentage; `--json` is intended for evidence
capture.

The focused build/test passed:

    make pagecache-residency-test

The test exercises the JSON schema against a 32 KiB temporary fixture and
checks that the residency counters are self-consistent. It does not make a
claim about the fixture's current cache state.

## Method

Both legs used two engine threads, phase statistics, the same 951-token
W-DECODE session, `thinking: off`, temperature 0, seed 1729, and a 256-token
continuation. The session was restored from the same saved source before each
request, so the three measured continuations within a leg began from the same
context. One warm request preceded each triplicate and is reported only as a
warm-up.

The control was the normal default-16-slot budget (2.07 GB cache budget; 1.29
GB cached payload and 656 entries observed). The comparison set
`EBUDGET_GB=1.55`, approximately 25% below the nominal budget (1.55 GB cached
payload and 786 entries observed). Each request was monitored continuously:
the request would have been stopped if live thermal pressure left `Nominal` or
the physical footprint reached 5 GB.

Raw server, client, and response evidence is retained as
`raw_e_x3_default_2t_*` and `raw_e_x3_budget_1_55_2t_*`. All six measured
responses have the same content SHA-256:

    5b7237368368054bc8776cf861068f359d9936f2ab321cef5871d5cf4a1a56d1

## Throughput and footprint

| Leg | Warm tok/s | Measured tok/s | Median tok/s | Physical footprint, GB |
| --- | ---: | --- | ---: | --- |
| Default | 6.14 | 6.18, 6.17, 6.11 | 6.17 | 4.56, 4.56, 4.56 |
| `EBUDGET_GB=1.55` | 5.83 | 5.99, 5.94, 5.98 | 5.98 | 4.62, 4.62, 4.62 |

The smaller cache was 3.1% slower by median token throughput and used 60 MB
more physical footprint. It therefore fails the intended throughput and
footprint direction, despite staying within the hard cap.

## Phase accounting

Median milliseconds per generated token from the three measured requests:

| Leg | Attention | Router | Dense | Expert MM | Expert disk | Head | Other | Phase sum |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Default | 23.39 | 7.96 | 28.49 | 30.76 | 61.74 | 8.85 | 0.86 | 162.05 |
| `EBUDGET_GB=1.55` | 23.33 | 7.95 | 28.77 | 30.90 | 60.80 | 8.90 | 6.73 | 167.38 |

The reduced-budget leg showed a modest lower expert-disk phase, but its
`other` phase was 5.87 ms/token higher. The observed phase sum agrees with
the reciprocal end-to-end median rate within 1%; this report does not assign a
causal explanation beyond the measured attribution.

## I/O, residency, and energy

| Measure | Default | `EBUDGET_GB=1.55` |
| --- | ---: | ---: |
| Expert-cache hit rate | 42.1% | 48.2% |
| Approx. bytes read per measured request | 93.85 GB | 85.44 GB |
| Median estimated CPU energy per decode token | 1.186 J | 1.187 J |
| Page-cache residency after session seed | 4.87 GB (23.237454%) | — |
| Page-cache residency after triplicate | 4.65 GB (22.198891%) | 4.60 GB (21.980225%) |

The pre-run idle snapshot was 4.64 GB (22.133174%) and is retained in
[raw_pagecache_idle.json](raw_pagecache_idle.json). The two later snapshots
are [default after seed](raw_e_x3_default_2t_after_seed_pagecache.json),
[default after triplicate](raw_e_x3_default_2t_after_runs_pagecache.json), and
[reduced-budget after triplicate](raw_e_x3_budget_1_55_2t_after_runs_pagecache.json).
These are volatile observations, not a controlled proof that a particular
budget caused their absolute residency levels.

## Safety observations and decision

Global swap usage remained 1,268.94 MB across both legs; that is the machine's
pre-existing baseline, not process-private swap. Per-request pageout deltas
were small: 3.6/4.1/4.1 MB for the default triplicate and 4.0/3.2/3.8 MB for
the reduced-budget triplicate (warm-ups: 3.0 and 12.2 MB respectively). No
thermal sample left `Nominal`, and all observed physical footprints were
below 5 GB.

Keep the default expert-cache policy. Do not run the harsher budgets or the
`DIRECT=1` control in the current session: the first reduction already has a
measurable 3.1% throughput regression and a higher footprint, with no energy
benefit. The remaining E-X3 legs are explicitly deferred rather than silently
treated as passed.
