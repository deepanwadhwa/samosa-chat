# Samosa Chat — agent guide

Local browser + terminal chat app around Qwen3.6-35B-A3B. CPU-only C engine,
expert streaming from disk, no framework, no build system, no dependencies.

## Start here

**Working on a GitHub issue (#1–#5)? Read [docs/ISSUE_TASKS.md](docs/ISSUE_TASKS.md)
first — including its Working agreement — then your issue's spec.** The issues
themselves are one-line titles; the specs are where the work is defined.

| Issue | Spec | Branch |
|---|---|---|
| #1 Linux | [docs/TASKS_LINUX.md](docs/TASKS_LINUX.md) | `issue-1-linux` |
| #2 Windows (Docker) | [docs/TASKS_WINDOWS.md](docs/TASKS_WINDOWS.md) | `issue-2-windows-docker` |
| #3 Vision | [docs/TASKS_VISION.md](docs/TASKS_VISION.md) | `issue-3-vision` |
| #4 Internet | [docs/TASKS_INTERNET.md](docs/TASKS_INTERNET.md) | `issue-4-internet` |
| #5 Documents | [docs/TASKS_DOCUMENTS.md](docs/TASKS_DOCUMENTS.md) | `issue-5-documents` |
| — Hardware/perf | [docs/TASKS_HARDWARE.md](docs/TASKS_HARDWARE.md) | cross-cutting |

App-level plan: [docs/APP_TASKS.md](docs/APP_TASKS.md) (phases A2/A3 are
superseded in part — see ISSUE_TASKS.md). Serve API: [docs/SERVE_API.md](docs/SERVE_API.md).

## Open defects

**G9 (OPEN, #1)** — the cgroup pressure signal counts page cache and
over-triggers. `linux_memory_pressure_level()` uses `memory.current/memory.max`,
but cgroup v2's `memory.current` includes the page cache the engine fills by
streaming `experts.bin`. Measured on a **2-token** run: ratio 0.85 fired WARN
while real usage (`anon`) was 0.56 — the engine dumped 323 MB of its own expert
cache to relieve pressure that did not exist (2% hit rate, 1803 evictions). Lives
inside G2, the port's highest-risk change. Evidence:
[docs/regressions/linux/real-model-run.md](docs/regressions/linux/real-model-run.md);
spec: [docs/TASKS_LINUX.md](docs/TASKS_LINUX.md) **G9**.

**G10 (OPEN)** — the AVX2/AVX512 kernels are **dead code in every shipped
x86 build**. `install.sh` and the `Dockerfile` compile with `-O3` and no
`-march`, so `__AVX2__` is undefined and [kernels.h](src/kernels.h)'s scalar
remainder does 100% of the work. Measured **7.6× slower** (17.09 → 2.26 GFLOP/s).
`-march=native` is *not* the fix — one image serves many CPUs — so runtime
`cpuid` dispatch is required. **Cannot be validated on the reference Mac**: an
amd64 container there has no AVX2/AVX512/SSE4.2. Needs real x86 hardware.
Spec: [docs/TASKS_HARDWARE.md](docs/TASKS_HARDWARE.md) **H2**; evidence:
[docs/regressions/linux/x86-dispatch.md](docs/regressions/linux/x86-dispatch.md).

**Published-claim defect (OPEN)** — [README.md](README.md) and
[dist/MODEL_CARD.md](dist/MODEL_CARD.md) state that expert-streaming *reads* wear
the SSD. **They do not** — NAND endurance is consumed by writes (TBW/DWPD are
write ratings). The README also calls 9 GB of swap *writes* "tiny" beside 376 GB
of reads; those writes cause more wear than the reads do. It currently tells
users to avoid thinking mode to protect hardware, on a false premise.
Spec: [docs/TASKS_HARDWARE.md](docs/TASKS_HARDWARE.md) **H1** (owner decision —
touches a published README and model card).

**Resolved 2026-07-15:**

- **G8.1** (Fixed) — Linked `test_kv_cache` in the `Makefile` with `-lm` to avoid undefined references on Linux/glibc.
- **G8.2** (Fixed) — Removed the awk interval expression `/^[0-9a-f]{64}$/` from `dist/install.sh` for compatibility with older Debian bookworm mawk versions.
Full evidence: [docs/regressions/linux/report.md](docs/regressions/linux/report.md); spec: [docs/TASKS_LINUX.md](docs/TASKS_LINUX.md) **G8**.

## Non-negotiables

These come from the project owner and override convenience.

- **Never overstate platform support or performance.** Scope every claim to what
  was measured and say what was measured. "Runs on Linux" is not a sentence —
  name the distro, kernel, arch, libc, filesystem you actually ran on.
- **Builds ≠ tests pass ≠ works.** "Works" means the real 24 GB model produced
  correct tokens through the real interactive path. Unit tests passing is not
  working. This was violated on 2026-07-15 ("Samosa is now optimized for Linux"
  while `make test` did not build on Linux).
- **Evidence, not assertion.** If you did not run it, say "not run". Paste the
  command and its output. Commit logs under `docs/regressions/<slug>/`.
- **Credit the Qwen and colibrì teams at the TOP** of README/model card, never
  the bottom.
- **Outward publishing (Hugging Face, releases) waits for explicit
  confirmation.** So does destructive cleanup or migration.
- **Machine safety.** Don't run SSD-heavy packaging or uploads while the owner is
  chatting with the model. Watch memory pressure, swap delta, thermals.

## The model — two quantization schemes, get this right

"The int4 model" is wrong shorthand and loses what the release is named for.

- **Experts** (`experts.bin`, 20.9 GB): `groupwise-symmetric-q4-v1`,
  `group_size: 32` — gate, up, **and** down all q4 with **one scale per 32
  weights**. Parsed at `src/qwen36b.c:1180-1206`.
- **Resident** (`resident.safetensors`, 3.0 GB — attention, embeddings,
  `lm_head`, and the vision tower): a **different whole-row** scheme —
  `fmt = (nbytes == O*I) ? int8 : int4`, one F32 scale per output row
  (`src/qwen36b.c:1134-1152`).
- Norms, biases, `patch_embed.proj`: F32.

## Build, test, run

```sh
make            # portable build
make omp        # multithreaded (brew install libomp first, macOS)
make test       # self-contained: stubs engine + network, tiny fixtures, no 24 GB model
```

- Model (24 GB, hard-linked, never copy): `~/Documents/samosa-models/qwen36_group32_i8`
- Run: `~/.samosa/bin/samosa app` → `http://127.0.0.1:8642`
- Engine config is env-driven (`SNAP`, `TOKENIZER`, `SAMOSA_CHATS_DIR`,
  `OMP_NUM_THREADS`); serve flags are `--serve --port --tokenizer`.
- Reference machine: one 16 GB M3 MacBook Air. **The only machine Samosa has ever
  been measured on.** ~2.5 GiB footprint fresh, ~3.9–4.2 GiB warmed; ~5–7 tok/s
  decode; ~14 tok/s prefill (2T) / ~24 (4T); 24,576-token context cap.
- **Prefill is the binding constraint** on documents, vision, and web: a
  5,000-token document costs ~3.5–6 minutes to read *once*. Sessions amortize it
  to zero for every follow-up — that is the architecture's real advantage.

## Git

The owner pushes to GitHub manually; **agent shells have no push credentials**.
Commit to your issue branch and stop. Uncommitted work has never been through
CI — do not call it green.
