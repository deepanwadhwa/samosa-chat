# E-X3 — page-cache residency preparation (2026-07-17)

## Status

Preparation is complete; the E-X3 budget sweep is **not run**. This work did
not start qwen36b, load model weights, or perform inference. Runtime
measurements remain parked behind the thermal-retry decision recorded in the
E-X1 report.

## New probe

tools/pagecache_residency.c maps a supplied regular file read-only and calls
mincore(2) over that mapping. It never dereferences the mapping, so the probe
queries page-cache residency without itself warming experts.bin. It reports
the system page size, page count, resident page count, resident bytes, and
percentage; the --json mode is intended for evidence capture.

The focused build/test passed:

    make pagecache-residency-test

The test exercises the JSON schema against a 32 KiB temporary fixture and
checks that the residency counters are self-consistent. It does not make a
claim about the fixture's current cache state.

## Idle snapshot (not a baseline)

The safe, read-only command below produced
[raw_pagecache_idle.json](raw_pagecache_idle.json):

    ./pagecache-residency --json ~/.samosa/current/model/experts.bin

At that instant, 282,908 of 1,278,208 16-KiB pages were resident:
4,635,164,672 bytes (4.64 GB decimal), or 22.133174% of the 20,942,159,872-byte
expert file. This is a volatile idle snapshot after earlier experiment
activity, not a before/after comparison and not evidence for a cache-budget
recommendation.

## Recorded default and next runtime work

The current engine default is the byte budget equivalent to 16 expert slots
per layer (default-16-slot); the code clamps it to total expert bytes and
prints the resolved size in the runtime [ecache] line. When the thermal gate is
reopened, E-X3 still needs the prescribed warm W-DECODE triplicates for the
default, −25%, −50%, and minimum viable EBUDGET_GB budgets, plus one DIRECT=1
control. Each leg must pair this probe with the normal physical-footprint,
VM-delta, and live powermetrics checks.
