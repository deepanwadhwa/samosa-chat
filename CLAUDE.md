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
| — Apple Silicon experiments | [docs/TASKS_EXPERIMENTS.md](docs/TASKS_EXPERIMENTS.md) | macOS-only |

User-facing docs: [INSTALL](docs/INSTALL.md) · [USAGE](docs/USAGE.md) · [PERFORMANCE](docs/PERFORMANCE.md) · [DESIGN](docs/DESIGN.md) · [ROADMAP](docs/ROADMAP.md)

App-level plan: [docs/APP_TASKS.md](docs/APP_TASKS.md) (phases A2/A3 are
superseded in part — see ISSUE_TASKS.md). Serve API: [docs/SERVE_API.md](docs/SERVE_API.md).

## Open defects

**J11 (FIX LANDED on `issue-7-jobs`, real-Downloads re-dogfood pending, #7)**
— the shipped find-job path (`jobs_find`, `src/samosa_gateway.c`) failed the
owner's Titli dogfood a second time (2026-07-23) from five compounding defects
rooted in one inversion — C hardcoded the intelligence the JF spec assigned to
the model: a tokenizer that treated the **letter "t" as a delimiter** (so
"Titli"/"cat" could never become search terms; a wallpaper zip outscored a
file named after the cat), a hardcoded "What is your pet's name?" fallback,
answer-restarts that discarded the whole run (violating JF.3), state that
persisted only goal+folder, and PDFs read text-layer-only while the prompt
steered away from `doc.read`'s OCR cascade. Zero find-path test coverage let
all five ship. **Phase JI rebuilt the find path** (model triages → skim index
→ classify → verify → structured `finish()`); the condemned code
(`candidate_score`, `build_candidates`, the canned question, the goal-restart
path) is demolished — verified absent. JI.0–JI.8 pass offline
(`make compiled-gateway-test` + `make jobs-test`, both `-Werror`), and
E-JI1/E-JI2 ran on real Ornith against a **50-file synthetic fixture** with
committed SSE evidence. **Remaining to fully close:** the real-Downloads
re-dogfood that originally exposed J11 — the true "works" gate per the
non-negotiables (synthetic fixtures are not the owner's real folder). Evidence:
[docs/regressions/jobs/titli-find-2026-07-23.md](docs/regressions/jobs/titli-find-2026-07-23.md)
(root cause) and
[docs/regressions/jobs/e-ji1-2026-07-23/report.md](docs/regressions/jobs/e-ji1-2026-07-23/report.md)
(real-model gate);
rebuild spec: [docs/TASKS_JOBS_INTELLIGENCE.md](docs/TASKS_JOBS_INTELLIGENCE.md)
**Phase JI** (supersedes the shipped implementation, enforces the JF spec's
model-decides design).

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

**Published-claim defect (H1) — FIXED in `c829f20`; this entry was stale and
was re-reported as open on 2026-07-28. Verify the files before reopening it.**
[dist/MODEL_CARD.md](dist/MODEL_CARD.md) now states the correct physics — "SSD
reads do **not** consume drive endurance. Flash endurance is rated in TBW
(Terabytes *Written*, JEDEC JESD218) and DWPD (Drive *Writes* Per Day);
program/erase cycles wear NAND, reads do not" — and [README.md](README.md)
carries no wear claim at all, only that Qwen performance depends on SSD
throughput. Nothing tells users to avoid thinking mode to protect hardware.

Left here as a marker rather than deleted: this entry outlived its defect and
cost the owner time when an agent relayed it without opening the files. **A
defect record is not evidence.** Check the source before repeating anything in
this section.

**Internet-search feature did not exist (BUILT 2026-07-28 on `ui-chutni`;
real-provider verification still pending)** — the owner's decision was build it,
not correct the docs. Phase W ([docs/TASKS_WEB_SEARCH.md](docs/TASKS_WEB_SEARCH.md))
shipped the three `/v1/web/*` routes, the declarative multi-provider search
executor, the model-decided `web_search`/`open_url` loop, the `SAMOSA_OFFLINE`
kill switch, and the composer's Web page / Web search actions; both docs were
rewritten to describe what is actually built. **Page reading is verified live
against real public pages on the reference Mac. Search is not: no credentials
for any provider exist on this machine, so the five presets' request/response
shapes are transcribed from vendor documentation and have never been observed
working — a wrong field name in a preset would not have been caught.** The tool
loop has only ever been driven by the fake backend; no real model has emitted a
planner decision. Evidence, including four defects found while building:
[docs/regressions/web-search/report.md](docs/regressions/web-search/report.md).
The original defect record follows.

[docs/MODELS_AND_INTERNET.md](docs/MODELS_AND_INTERNET.md)
and [docs/SERVE_API.md](docs/SERVE_API.md) describe a fully built feature (model-
invoked `web_search`/`open_url` tools, `GET /v1/web/config`, `POST /v1/web/fetch`,
`POST /v1/web/search`, a declarative multi-provider search-config executor with
DNS pinning and SSRF protections) and claim it was "verified live against a
config-defined provider" and "exercised by `make test`". **None of it exists in
the C source, on any branch, at any commit** — grepped `web_search`/`open_url`/
`v1/web/` across `src/` and full git history; nothing. `assets/app.html` calls
these endpoints from its Settings "Internet source" card; verified live that the
compiled gateway 404s all three. Issue #4's own spec
([docs/TASKS_INTERNET.md](docs/TASKS_INTERNET.md)) already documents "Refolded
into Samosa Jobs — reused as scheduled public-web input" as the *replacement*
design, so these two docs describe an earlier, abandoned design that was never
removed or corrected. **The dead frontend half is now gone** — T3.2 removed the
Settings "Internet source" card, its CSS, and its three `/v1/web/*` call sites,
and the composer's `+` menu shows Web page as a visibly disabled item stating
the real reason. **What remained open was the owner decision on the docs:** build
the standalone chat-composer web-search feature for real (a security-sensitive
undertaking — SSRF/DNS-pinning, a provider-config executor, a model tool-calling
loop), or correct `MODELS_AND_INTERNET.md`/`SERVE_API.md`, which still described
it as built and verified. **Decided 2026-07-28: build it.** Evidence:
[docs/regressions/ui-chutni/t3.2-evidence.md](docs/regressions/ui-chutni/t3.2-evidence.md).
This entry itself was added on `ui-chutni`, not `main`, since
that is the only branch this agent session can commit to — reconcile onto
`main` per the working agreement in docs/ISSUE_TASKS.md when convenient. The
same applies to Phase W's own doc rewrites, which edit `main`-resident files
from this branch.

**Phase W + WK (#4) — both original gates now CLEARED; Qwen and a full E-I4
remain.**
Phase WK (2026-07-28, `ui-chutni`) made search work with **no API key, no
account, and no install**, per the owner's requirement: the default provider is
Parallel's Search MCP, which answers anonymous requests over ordinary HTTPS
(nothing vendored, so no AGPL exposure). Because it needs no credential, the
reference Mac could finally run the real-provider gate Phase W never could —
**a live keyless search through the compiled gateway returned 8 results**, so
that preset's shapes are observed rather than transcribed. The four keyed
presets are kept as the escape hatch (Brave deleted its free tier in Feb 2026 —
a free tier disappearing is a demonstrated risk, not a theoretical one) and
remain **unverified**. Search text now leaves the machine, so WK added
**ask-once consent** (`POST /v1/web/consent`, stored in `config.json`); until
it is answered the tool loop does not run at all and a `web:true` turn is
byte-identical to a pre-W turn. WK5 adds Samosa's **own 100/day cap** on the
free tier (the provider publishes none), never applied to a user's own key.
**Gate (b) is also cleared** — the tool loop ran on the real **Ornith 9B**.
*It never needed the 24 GB Qwen*: the planner is one stateless call with one
system and one user message, which every backend runs. 6 web turns, **0
malformed planner replies**, searched for time-sensitive questions and
correctly declined for static ones — and it **found a real defect**: the model
re-requested a URL that had just 403'd, burning its last tool call. Now refused
in code (WK6) with a regression test built from what the model actually did.
Still open: the loop on **Qwen** specifically, and a proper E-I4 (only
wall-clock so far — 86 s / 122 s per web turn, 42 s for a declined one).
Also unverifiable here: whether keyless access works from **other** IPs
(Firecrawl's keyless tier refused this Mac by IP). Evidence:
[real-model-planner.md](docs/regressions/web-search/keyless-2026-07-28/real-model-planner.md)
and
[docs/regressions/web-search/keyless-2026-07-28/report.md](docs/regressions/web-search/keyless-2026-07-28/report.md);
spec: [docs/TASKS_WEB_SEARCH.md](docs/TASKS_WEB_SEARCH.md) **Phase WK**.

**Resolved 2026-07-27 (on `ui-chutni`, T3.2):**

- **`doc.read` PDF crash** (Fixed) — the OCR-failure path in
  `doc_read_handler()` (`src/samosa_gateway.c`) freed `ext_json`/`arena_ext`/
  `ext_raw` and then `break`-ed out of the *inner page* loop, so the
  unconditional cleanup after that loop freed all three a second time. Any PDF
  page that escalated to OCR and failed aborted the **whole gateway process** —
  Chat, Jobs, and backend supervision with it. Pre-existing and reachable
  before T3.2; nothing had exercised the path. Caught by ASan via the new
  `tests/test_attachments.sh` document case.
- **`doc.read` discarded readable PDFs when OCR was unavailable** (Fixed) —
  `needs_image` is a sparseness heuristic (`toks < 20`), so a short but
  perfectly readable page escalated to OCR; with no OCR pack installed (the
  reference Mac has none) the entire document read failed, throwing away
  text-layer content already in hand. Now falls back to that text, labeled
  `source: "text_layer_ocr_unavailable"`. A page with no text layer at all
  still fails honestly.

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
                # covers the compiled gateway, Chutni, and the Kimi preflight
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
