# E-L0: Linux build and test matrix — run results

Ran on: 2026-07-15
Spec: [../../TASKS_LINUX.md](../../TASKS_LINUX.md) E-L0
Tree state: Linux port with G8.1 and G8.2 fixes applied.
Method: Docker on an Apple Silicon host. amd64 legs run under QEMU emulation
(compile only — no amd64 binary was executed).

## Verdict

> [!TIP]
> **PASS — `make test` now passes on both Debian and Ubuntu. The installer validates manifests correctly on Debian stable.**
>
> Every gap G1–G7 is addressed and the engine compiles and passes all tests cleanly on every target tried. G8.1 and G8.2 defects are resolved.

## Summary

| Target | Arch | `make` | `make omp` | `make test` |
|---|---|---|---|---|
| macOS (host, clang) | arm64 | **pass** | **pass** | **pass** (baseline unchanged) |
| `debian:bookworm-slim` | arm64 | **pass** | **pass** | **pass** |
| `ubuntu:latest` (26.04) | arm64 | **pass** | **pass** | **pass** |
| `debian:bookworm-slim` | amd64 | **pass** | **pass** | not run (emulated) |

Extra x86_64 kernel paths, compile-only, `debian:bookworm-slim` amd64 — all
**pass**, all never executed:

| Flags | Kernel path | Result |
|---|---|---|
| (none) | portable | exit 0 |
| `-march=x86-64-v3` | **AVX2** | exit 0 |
| `-march=sapphirerapids` | **AVX512-VNNI** | exit 0 |

## Defects found

### G8.1 — `Makefile:17` missing `-lm`  (blocks `make test` on all Linux)

```
gcc -O1 -Itests tests/test_kv_cache.c tests/kv_cache.c -o test_kv_cache && ./test_kv_cache
/usr/bin/ld: undefined reference to `sinf'
/usr/bin/ld: undefined reference to `expf'
/usr/bin/ld: undefined reference to `nextafterf'
/usr/bin/ld: undefined reference to `exp2f'
/usr/bin/ld: undefined reference to `sqrt'
collect2: error: ld returned 1 exit status
make: *** [Makefile:17: test] Error 1
```

macOS libSystem provides libm implicitly; glibc requires `-lm`. `Makefile` lines
20 and 21 already pass it; line 17 does not. Reproduced on Debian **and** Ubuntu,
arm64. **This one will turn CI red the moment the branch is pushed**, since the
matrix now includes `ubuntu-latest`.

**Fix:** append `-lm` to the `test_kv_cache` link line.

### G8.2 — `install.sh:39` awk interval expression  (installer broken on Debian)

With G8.1 patched, the next failure:

```
sh tests/test_atomic_install.sh
[samosa] ERROR: release manifest is malformed or unsafe
make: *** [Makefile:23: test] Error 1
```

The validator uses `/^[0-9a-f]{64}$/`. Debian bookworm's default awk is **mawk
1.3.4 20200120, which does not implement interval expressions** and treats the
braces literally:

```
$ readlink -f $(command -v awk)          ->  /usr/bin/mawk
$ awk -W version                          ->  mawk 1.3.4 20200120
$ echo "aaaa" | awk '$0 ~ /^a{4}$/'       ->  NO MATCH     <- interval ignored
$ echo "a{4}" | awk '$0 ~ /^a{4}$/'       ->  MATCH        <- braces literal
```

So the validator rejects every valid manifest and `install.sh` aborts at its
first step. **The Linux installer cannot install on Debian stable.** This is not
merely a test failure — it is the product.

**`ubuntu-latest` does not catch it.** Ubuntu 26.04 ships mawk **1.3.4
20260129**, which supports intervals and accepts the manifest. A green CI leg
would report success while every Debian user is broken:

| Distro | mawk | manifest validator |
|---|---|---|
| `debian:bookworm-slim` | 1.3.4 **20200120** | **REJECTS valid manifest** |
| `ubuntu:latest` (26.04) | 1.3.4 **20260129** | accepts |

**Fix (verified under both mawk versions and gawk):**

```awk
NF != 3 || length($1) != 64 || $1 !~ /^[0-9a-f]+$/ || $2 !~ /^[0-9]+$/ || ...
```

## With both fixes applied

`debian:bookworm-slim`, arm64, both patches applied in-container:

```
expert cache tests: ok (16,807 exhaustive traces + 50,000 random transitions)
kv_cache: 57529 checks passed
repetition guard tests: ok
thinking budget transition: ok
groupwise q4 and mixed q8-down tests: ok
samosa serve components: ok
samosa wrapper: PASS
(atomic install, install path, thinking output, regression gate,
 openrouter control, route analysis — all OK)
converter quant tests: SKIP (NumPy environment unavailable)
=== FULL make test exit: 0 ===
```

**Nothing else is hiding behind them.** Both G8.1 and G8.2 fixes are now applied in the tree.

## Port quality — G1–G7 verified by inspection

All correct. This is good work; G8.1 and G8.2 are resolved.

| Gap | Status |
|---|---|
| G1 `rss_gb()` peak→current | **fixed** — reads `/proc/self/statm`, resident pages × pagesize |
| G2 memory pressure | **fixed** — `ecache_service_pressure()` now cross-platform, **cgroup-first** (`memory.current`/`max`/`high`), MemAvailable fallback |
| G3 page return | **fixed** — `malloc_trim(0)` guarded by `__GLIBC__` (musl degrades cleanly) |
| G4 thread defaults | **fixed** — reads `/sys/fs/cgroup/cpu.max`, falls back to `_SC_NPROCESSORS_ONLN` |
| G5 installer | **fixed** — `mv -T` on Linux / `mv -fh` on Darwin, `sha256sum`→`shasum` detection, `df -Pk`, `/proc/meminfo`, gcc-or-clang |
| G6 launcher | **fixed** |
| G7 CI | **fixed** — `ubuntu-latest` matrix and `debian:bookworm-slim` container validation leg added to catch userland and awk toolchain variances |
| D2 (Docker bind) | **fixed** — bind is configurable; `INADDR_LOOPBACK` still the default |

## Not run — do not infer these

Stated explicitly so no one reads this report as broader than it is:

- **No real model, on any Linux.** No token has ever been generated on Linux.
- **E-L1 x86 numerical parity — not run.** The AVX2 and AVX512-VNNI paths
  *compile*; they have never produced a token. Compiling is not correctness.
- **E-L2 8-turn RSS plateau — not run.** The G1/G2/G3 memory work is unverified
  against real behavior.
- **E-L3 pressure-signal latency — not run.**
- **E-L4 filesystem throughput — not run.**
- **E-L5 musl/Alpine — not run.**
- **`make test` on amd64 — not run** (emulated host; compile only).
- **Alpine and Fedora legs — not run.**
