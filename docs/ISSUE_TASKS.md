# Issue task program — index

Task specifications for GitHub issues #1–#5, written to be executed by an
agent with no prior context on this repo. Each issue has its own document;
this one holds what they share: verified ground truth, the conflicts between
issues, and the order to do them in.

| Issue | Title | Spec | Shape of the work |
|---|---|---|---|
| [#1](https://github.com/deepanwadhwa/samosa-chat/issues/1) | Optimize samosa for Linux | [TASKS_LINUX.md](TASKS_LINUX.md) | Restoration, not a port — see below |
| [#2](https://github.com/deepanwadhwa/samosa-chat/issues/2) | Optimize Samosa for Windows | [TASKS_WINDOWS.md](TASKS_WINDOWS.md) | **Docker.** Decided — no native port. #1 is a prerequisite |
| [#3](https://github.com/deepanwadhwa/samosa-chat/issues/3) | Add vision capabilities | [TASKS_VISION.md](TASKS_VISION.md) | Forward pass only — weights already ship |
| [#4](https://github.com/deepanwadhwa/samosa-chat/issues/4) | Add internet search | [TASKS_INTERNET.md](TASKS_INTERNET.md) | Extends [APP_TASKS.md](APP_TASKS.md) Phase A3 |
| [#5](https://github.com/deepanwadhwa/samosa-chat/issues/5) | Add document intelligence | [TASKS_DOCUMENTS.md](TASKS_DOCUMENTS.md) | Extends [APP_TASKS.md](APP_TASKS.md) Phase A2 |
| — | **Hardware: best from the user's machine, honestly** | [TASKS_HARDWARE.md](TASKS_HARDWARE.md) | Cross-cutting (#1 + #2 + macOS). Not a GitHub issue |

Issues #4 and #5 already have plans in [APP_TASKS.md](APP_TASKS.md) (Phases A3
and A2). Those specs **extend and correct** that plan rather than replace it;
read the phase in `APP_TASKS.md` first, then the spec.

[TASKS_HARDWARE.md](TASKS_HARDWARE.md) answers "how does a user get the best out
of their hardware without killing it" (asked 2026-07-15). It holds **H2 — runtime
SIMD dispatch, the single biggest performance win in the program (7.6× measured,
and it makes the machine cooler, not hotter)** — plus the correction of a
published claim: **reads do not wear SSDs; writes do.**

## How to read these specs

Every claim below was verified on 2026-07-15 by the method stated next to it.
Claims that were **not** verified are marked *unverified* or live under "Open
questions". The house rule from this repo's task program applies: acceptance
criteria are measured, not assumed, and **a negative result is a result** — an
experiment that kills a task has done its job. Do not soften a measurement to
make a task survive.

Two standing rules from the project owner:

- **Never overstate platform support or performance.** Scope every claim to
  what was measured, and say what was measured. "Runs on Linux" means a named
  distro, kernel, arch, and filesystem you actually ran on.
- **Verify against the real model end-to-end before calling anything done.**
  Unit tests passing is not "working". The bar is the interactive local app
  against the real 24 GB model.

---

## Working agreement — read before writing any code

This section is mandatory and applies to every issue. It exists because it was
already violated once: on 2026-07-15 an agent reported "Samosa is now optimized
for Linux" when `make test` did not build on Linux and the installer could not
install on Debian. See [TASKS_LINUX.md](TASKS_LINUX.md) G8 for both defects.

### 1. One branch per issue — but the plan lives on `main`

**The task program is shared documentation and belongs on `main`:** `CLAUDE.md`,
`docs/ISSUE_TASKS.md`, and **all five `docs/TASKS_*.md` cards**. Every branch
references them and they cross-link to each other, so scattering them across
branches breaks the program.

This was got wrong once, on 2026-07-15. The cards were split onto issue branches,
which left `main` with **no task program at all**, `CLAUDE.md` on only
`issue-1-linux` (so agents on any other branch got no auto-loaded guide, no index
and no working agreement), and **every cross-link broken on every branch** —
`ISSUE_TASKS.md` pointing at `TASKS_VISION.md` that was not on its branch, and
vice versa.

| Goes on `main` | Goes on the issue branch |
|---|---|
| `CLAUDE.md` | implementation (`src/`, `dist/`, `Makefile`, `ci.yml`) |
| `docs/ISSUE_TASKS.md` | issue-specific tooling (`tools/run_e_v1.py`) |
| `docs/TASKS_*.md` (all five) | that issue's evidence (`docs/regressions/<slug>/`) |

Update a card as you learn — but land card changes on `main`, not buried in a
feature branch where the other four issues cannot see them.

**Implementation** for an issue lives on its own branch, cut from `main`:

| Issue | Branch |
|---|---|
| #1 | `issue-1-linux` |
| #2 | `issue-2-windows-docker` |
| #3 | `issue-3-vision` |
| #4 | `issue-4-internet` |
| #5 | `issue-5-documents` |

Do not mix issues in one working tree. As of 2026-07-15 the tree contained
uncommitted work for #1 (engine, installer, CI), #3 (`tools/run_e_v1.py`), and
#4 (`tools/run_e_i1.py`, `tests/tool_call_cases.json`) simultaneously, on
`main`, with nothing committed — so none of it could be reviewed, tested, or
reverted independently. That is what this rule prevents.

Commit as you go. **The user pushes to GitHub manually; agent shells have no
push credentials.** Uncommitted work has never been through CI — do not describe
it as passing CI.

### 2. Definition of done — evidence, not assertion

A task is done when you can **show** it, not when you believe it. For every
acceptance criterion, produce:

- **The command you ran, and its output.** Paste it. "make test clean on Ubuntu"
  is not evidence; the terminal output is.
- **Where it ran.** Exact distro/version, kernel, arch, libc, filesystem,
  container or bare metal. `uname -a` and `/etc/os-release`.
- **A committed log** under `docs/regressions/<issue-slug>/` for anything a
  reviewer cannot re-run cheaply, following the existing convention in
  `docs/regressions/`.

If you did not run it, say "not run". **"Should work" and "is now supported" are
not statuses.** A task with unrun experiments is in progress, however much code
was written.

### 3. Compiling is not running; running is not working

Three distinct claims, three distinct bars — never promote one to another:

1. **Builds** — the compiler exited 0. Says nothing about correctness.
2. **Tests pass** — `make test` exits 0 *on the target platform*.
3. **Works** — the real 24 GB model produced correct tokens through the real
   interactive path, on that platform.

The 2026-07-15 Linux report conflated (1) with (3). The code compiled cleanly on
Linux aarch64 and x86_64 — and `make test` still failed, and no model had ever
run.

### 4. Green CI does not mean covered

CI is one configuration. It is not the support claim. Verified example: the
manifest validator in `install.sh` fails on Debian bookworm's mawk but passes on
Ubuntu 26.04's newer mawk — so a green `ubuntu-latest` leg would have reported
success while every Debian user was broken. **When you add a platform to the
support claim, name the configurations you actually tested, and say which ones
CI covers and which it does not.**

### 5. Run the RUN-FIRST experiments first

Each spec marks its cheapest, most decisive experiment **RUN THIS FIRST**. They
are ordered that way because they can kill or resize the issue before code is
written. Do not skip one because the implementation looks obvious — E-L0 (~0.5
day) would have caught both G8 defects before a line of the port was written.

## Shared verified ground truth

**Reference machine.** One 16 GB M3 MacBook Air, macOS, Apple Silicon. This is
the *only* machine Samosa has ever been measured on. Every number below comes
from it.

**Model.** Qwen3.6-35B-A3B, published at
`deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32`. 24 GB across `experts.bin`
(20.9 GB) and `resident.safetensors` (3.0 GB).

**Quantization — get this right, it is two different schemes and "the int4
model" is wrong.** Verified from the shipped `manifest.json` and
[qwen36b.c:1180-1206](../src/qwen36b.c#L1180-L1206):

- **Experts (`experts.bin`, 20.9 GB — the bulk of the model):**
  `{"format": "groupwise-symmetric-q4-v1", "group_size": 32}`. All three expert
  projections (gate, up, **and** down) are **q4 with group-32 symmetric scales**
  — one scale per 32 weights along the reduction axis. The engine parses this
  into `expert_group_size = 32`, `expert_down_bits = 4`
  ([:1191](../src/qwen36b.c#L1191), [:1204-1205](../src/qwen36b.c#L1204-L1205)).
  A *mixed* variant exists in the code (`groupwise-q4-gate-up-row-q8-down-v1`,
  q4 gate/up + row-q8 down) but **is not what shipped**. One detail:
  [:1775](../src/qwen36b.c#L1775) forces `down_bits = 8` for the MTP layer only.
- **Resident tensors (`resident.safetensors`, 3.0 GB — attention, embeddings,
  `lm_head`, and the vision tower):** a **different, whole-row** scheme.
  [:1134-1152](../src/qwen36b.c#L1134-L1152) sets `t.fmt = (nbytes == O*I) ? 1
  (int8) : 2 (int4)` with `t.s = falloc(O)` — **one F32 scale per output row**,
  `qgroup = 0`. Not group-32.
- **Norms, biases, and `patch_embed.proj`** stay F32.

So "group-32" names the **expert** scheme, which is what distinguishes this
release from the superseded whole-row `...-int4` repo — group-wise scales track
local dynamic range far better than one scale per row. Calling the whole thing
"the int4 model" loses exactly the property the release is named for. The
comment at [qwen36b.c:248-250](../src/qwen36b.c#L248-L250) states the
distinction: "0 means the legacy one-scale-per-row q4 container. A positive
value selects grouped-q4 gate/up."

**Runtime shape** (from [SERVE_API.md](SERVE_API.md), verified 2026-07-14):
~2.5 GiB resident footprint fresh, plateauing at 3.9–4.2 GiB warmed; decode
~5–7 tok/s on the 2-thread cool default; prefill ~14 tok/s (2T) / ~24 tok/s
(4T); enforced 24,576-token total context cap; ~40 KiB/token KV.

**Prefill is the binding constraint on every feature in #3, #4, and #5.** A
5,000-token document costs ~3.5–6 minutes to read *once*. The session snapshot
architecture is what makes that survivable: reading is pay-once-per-artifact,
and every follow-up turn is free. When you design a feature here, the question
is not "how fast is this" but "does this land in a session snapshot".

**Platform-specific code inventory** (verified by grep, 2026-07-15). This is
the shared surface for #1 and #2:

| Site | What it is | Non-Apple status |
|---|---|---|
| [qwen36b.c:62-69](../src/qwen36b.c#L62-L69) `rss_gb()` | `TASK_VM_INFO` phys_footprint | Falls back to **peak** RSS — wrong metric |
| [qwen36b.c:1868](../src/qwen36b.c#L1868) `mem_available_gb()` | mach `host_statistics64` | **Has a `/proc/meminfo` branch already** |
| [qwen36b.c:1927](../src/qwen36b.c#L1927) `ecache_service_pressure()` | `kern.memorystatus_vm_pressure_level` | **Entire body is `#ifdef __APPLE__` — no-op** |
| [qwen36b.c:3900](../src/qwen36b.c#L3900) | `malloc_zone_pressure_relief` | `#ifdef __APPLE__` — no page return |
| [qwen36b.c:4343](../src/qwen36b.c#L4343) | `hw.perflevel0.physicalcpu` cool default | `#ifdef __APPLE__` — OpenMP grabs all cores |
| [compat.h](../src/compat.h) | `posix_fadvise` + `O_DIRECT` shims | Self-described **no-op on Linux** |
| [st.h:82-88](../src/st.h#L82-L88) | O_DIRECT twin fd | Prefers real `O_DIRECT` when defined |
| [kernels.h](../src/kernels.h) | Hot matmul/dot kernels | **AVX2 + AVX512-VNNI paths already exist** |
| [samosa_http.h:12-16](../src/samosa_http.h#L12-L16) | POSIX sockets | Fine on Linux; **absent on MSVC** |

**The engine has Linux/x86 ancestry.** [compat.h:1-4](../src/compat.h#L1-L4)
describes itself as a shim "per piattaforme non-Linux (oggi: macOS)" and states
"Su Linux questo header e' un NO-OP totale". [st.h:94](../src/st.h#L94) cites a
measured O_DIRECT benchmark on **ext4-in-VHDX** (0.8 → 2.3+ GB/s) — that is a
WSL2 disk. macOS was the port; Linux was the origin. Issue #1 is therefore
substantially a **restoration and verification** job, not a greenfield port,
and issue #2 has a documented precedent of running under WSL2.

## Cross-issue conflicts — resolve these before building

**1. `APP_TASKS.md` A2.1 mandates macOS-only tools — SETTLED 2026-07-15.**
The plan specifies `/usr/bin/textutil` and a PDFKit helper; both are macOS-only
and would silently make documents a macOS-only feature.

**Decided: `libpdfium` (BSD-3), linked directly via its C API from a sandboxed
sidecar binary.** Not PyMuPDF — it is AGPL-3.0 (verified from the published
wheel), incompatible with this project's Apache-2.0 license. Not Python —
measured, pypdfium2 is a 0.15 MB ctypes wrapper over the same 7.4 MB C library,
so Python is additive weight for no benefit. Not a hand-written C extractor —
considered and rejected. `.docx` is vendored miniz + an XML strip; HTML reuses
#4's extractor. Full rationale and the rejected alternatives are recorded in
[TASKS_DOCUMENTS.md](TASKS_DOCUMENTS.md). **Implement it; do not reopen it.**

**2. #3 changes what #5 can do.** `APP_TASKS.md` A2.1 says scanned PDFs "must
fail loudly not silently". If the vision tower lands, scanned pages become
images and Qwen3.6's OCR handles them. Do not design the scanned-PDF failure
path as permanent; make it a clean "not yet" that #3 can fill in. **pdfium
rasterizes pages** (`FPDF_RenderPageBitmap`), so choosing it puts that seam in
place for free.

**3. #3's image decoder must not be `ImageIO`.** The obvious macOS answer
(ImageIO/CoreGraphics) would make vision macOS-only and collide with #1/#2 the
same way `textutil` does. Vendor a portable single-header decoder instead. This
matches how the repo already vendors `json.h`, `tok.h`, `st.h`.

**4. #4's tool-calling (A3.3) needs engine work nobody has scoped.** The serve
API reads **only the last user message and the first system message**
([qwen36b.c:4073-4081](../src/qwen36b.c#L4073-L4081)); every other message in
the array is discarded. Tool results cannot be injected as messages today.
A3.3 assumes they can. See [TASKS_INTERNET.md](TASKS_INTERNET.md).

**5. #2's Docker decision promotes two #1 gaps to blockers.** A container gets
a cgroup memory limit, but the expert cache sizes itself from `/proc/meminfo`,
**which reports the host's RAM inside a container** — so it over-budgets and
gets OOM-killed, with no reclaim path (`ecache_service_pressure()` is a no-op
off Apple). And the listener is hardcoded to `INADDR_LOOPBACK`
([samosa_http.h:211](../src/samosa_http.h#L211)), so a published port reaches
nothing. Gaps G2/G4 in [TASKS_LINUX.md](TASKS_LINUX.md) are therefore Docker
**prerequisites**, not nice-to-haves. Do them cgroup-first.

## Suggested order

The dependency that matters most: **#1 unblocks #2**, and **the two cheapest
experiments in the program can kill or resize the two largest issues**. Run
them first, before anyone writes C.

1. **E-V1** ([TASKS_VISION.md](TASKS_VISION.md)) — validate the shipped vision
   weights numerically. Pure Python, no C, ~1 day. Decides whether #3 is a
   ~2-week forward-port or a re-quantization project.
2. **E-I1** ([TASKS_INTERNET.md](TASKS_INTERNET.md)) — measure tool-call JSON
   reliability against the OpenRouter FP8 control that already exists in
   `tools/run_openrouter_control.py`. ~0.5 day. A3.3 already declares >20%
   malformed a no-go; find out now rather than after 3–4 days of C.
3. **#1 Linux** — L0 (does it compile?) is a few hours and scopes the rest. Do
   G2/G4 **cgroup-first**, since #2 depends on them (conflict 5).
4. **#2 Windows/Docker** — mostly falls out of #1 once G2/G4 and the
   configurable bind are done. Now ~1 week, not ~3.
5. **#5 Documents** then **#4 Internet** — both extend existing plans and both
   are gated on the long-context regression debt (A0.4).
6. **#3 Vision** — after E-V1 returns.

## Standing open questions

- **The "int4 doubling artifact"** is referenced exactly once, at
  [APP_TASKS.md:280](APP_TASKS.md), and defined nowhere in the repo. Either it
  is real and needs documenting, or it is stale and needs deleting. It is cited
  as a known limitation users will be shown — it cannot stay undefined. Resolve
  it before writing any user-facing copy that inherits it.
- **`tokenizer_config.json` is not shipped**, and the chat template is
  hardcoded as C string concatenation at
  [qwen36b.c:3440-3452](../src/qwen36b.c#L3440-L3452). Any feature needing a
  different template (tools, images) must hand-port it from upstream and match
  it exactly. There is no template engine to extend.
