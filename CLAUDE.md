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

App-level plan: [docs/APP_TASKS.md](docs/APP_TASKS.md) (phases A2/A3 are
superseded in part — see ISSUE_TASKS.md). Serve API: [docs/SERVE_API.md](docs/SERVE_API.md).

## Open defects — resolved 2026-07-15

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
