# Issue #5 — Document intelligence

**Read [APP_TASKS.md](APP_TASKS.md) Phase A2 first.** A plan already exists
(A2.1 extraction, A2.2 full-document ingestion, A2.3 retrieval, A2.4
limitation surfacing). This document does not replace it. It resolves a
conflict between that plan and issues #1/#2, proposes a portable redesign, and
adds the experiments.

Also read [ISSUE_TASKS.md](ISSUE_TASKS.md) for shared ground truth.

A2's design position is the strongest idea in the app program and must not be
relitigated: **sessions are the document feature.** Reading a document once into
a conversation snapshot amortizes the prefill cost forever. Retrieval exists
only for documents that exceed the context budget. Everything below serves that.

## The conflict: A2.1 makes documents macOS-only

[APP_TASKS.md:231-237](APP_TASKS.md) specifies extraction via:

- `/usr/bin/textutil` for `.docx/.rtf/.html` — **macOS-only**
- a PDFKit helper compiled by the installer with the CLT toolchain —
  **macOS-only**

Both were reasonable when macOS was the only target. **Issues #1 and #2 make
them a trap:** ship them and document intelligence silently becomes a macOS-only
feature of a cross-platform product — discovered by a Linux user, at runtime,
after they attach a file.

This is the same shape as the ImageIO trap flagged in
[TASKS_VISION.md](TASKS_VISION.md). Decide it once, here, before A2.1 is
written. Rewriting extraction later costs more than designing it portable now.

### Decision (2026-07-15, project owner) — SETTLED

**Extract document text with `libpdfium` (BSD-3), linked directly via its C API
from a sandboxed sidecar binary, and hand the model plain text.** The model does
the rest — which is exactly A2.2's design.

- **Library: pdfium.** Not PyMuPDF (AGPL-3.0 — incompatible; see below). Not
  poppler (GPL-2.0).
- **No Python.** pypdfium2 is a ctypes wrapper over the same C library; the C
  API is linked directly. See "Python is not required" below.
- **Not the hand-written C extractor.** Considered and rejected; recorded below
  so it is not relitigated.
- **`libpdfium` becomes a SHA-256-pinned, per-platform release artifact.** The
  trust trade was made knowingly — see "The honest cost" below.

The rest of this document assumes this decision. **Do not reopen it; implement
it.**

### PyMuPDF is AGPL-3.0 — verified, and it conflicts with this project

Read directly from the published wheel (`pymupdf-1.28.0`, 2026-07-15):

```
License: Dual Licensed - GNU AFFERO GPL 3.0 or Artifex Commercial License
```

Samosa is **Apache-2.0** ([LICENSE](../LICENSE)). AGPL-3.0 is copyleft and
incompatible: shipping PyMuPDF inside the Docker image, or in an installer-created
venv, distributes AGPL code as part of the product — which would require
relicensing Samosa to AGPL, and would extend the AGPL's network clause to anyone
running it as a service. "Let the user pip-install it themselves" is a grey area,
not a fix, and a bad foundation for a published product.

The requirement was "PyMuPDF **or something similar** that's open source and
available on every platform". A permissive library satisfies every word of that:

| Library | License | Verdict |
|---|---|---|
| **pdfium** (Google) | **BSD-3** | **Recommended** — see below. Stable **C API**, rasterizes pages (the seam #3 needs for scanned PDFs), prebuilt for every platform × arch. |
| pypdfium2 | Apache-2.0 / BSD-3 | A **ctypes wrapper over pdfium**. The library is pdfium; the Python is 0.15 MB of bindings. |
| pdfminer.six | MIT | Pure Python. Slower, text-only, no rendering. |
| pypdf | BSD-3 | Pure Python, weaker extraction. |
| PyMuPDF / MuPDF | **AGPL-3.0** | Incompatible with Apache-2.0. |
| poppler / `pdftotext` | **GPL-2.0** | Incompatible to distribute. |

### Python is not required — and Docker is not what would force it

Measured from the published wheel, 2026-07-15:

```
pypdfium2-5.11.0-py3-none-macosx_12_0_arm64.whl        3.6 MB
  pypdfium2_raw/libpdfium.dylib                        7.73 MB   <- the library
  pypdfium2_raw/bindings.py                            0.15 MB   <- ctypes wrapper
License: BSD-3-Clause, Apache-2.0
```

`nm` on that dylib exports `_FPDF_InitLibrary`, `_FPDF_LoadDocument`,
`_FPDFText_CountChars`, `_FPDFText_GetText`, `_FPDF_RenderPageBitmap` as plain C
symbols, and `bindings.py` calls them **through ctypes**. There is no Python in
the PDF parsing — pypdfium2 is a thin wrapper over a 7.4 MB C library with a
stable C ABI.

**So the extraction sidecar should be a small C binary linking `libpdfium`
directly. No Python, no pip, no venv — identical on macOS, Linux, and in the
container.**

Two points worth being explicit about, because the intuition runs the other way:

- **Docker does not force Python. It is where Python would be *cheapest*** (one
  image layer you control). The Python cost lands hardest on the **native**
  installs — venv, pip, network at install time, and `python3-venv` being a
  separate package on Debian/Ubuntu. That is precisely where lightweight matters.
- **The Python path ships the same prebuilt binary anyway.** `libpdfium.dylib`
  *is* the wheel. So Python does not avoid the binary-blob question; it stacks a
  runtime on top of it. Here it is strictly additive weight for no benefit.

### The honest cost of going pdfium-direct

Not free, and the trade should be made knowingly:

- **A prebuilt `libpdfium` becomes a release artifact.** Building pdfium from
  source needs Chromium's `gn`/`ninja` toolchain — hours and a huge checkout,
  not viable inside a `curl | sh` installer. So we fetch a prebuilt (the
  `pdfium-binaries` project, permissive — the same builds pypdfium2 vendors).
  **This shifts the trust model:** today the installer compiles all C from source
  the user can read. Mitigation: pin it by SHA-256 in the release manifest —
  [install.sh](../dist/install.sh) already verifies every file that way, so the
  machinery exists. The Docker image ships binaries regardless.
- **Per-platform artifacts.** macOS arm64/x64, Linux x64/arm64, plus musl for
  Alpine. `INSTALL_FILES` ([install.sh:64](../dist/install.sh#L64)) is a flat
  list today and would need platform-conditional entries. Small but real.
- **~300–500 lines of C** instead of ~100 lines of Python: page iteration,
  `FPDFText_GetText` returns UTF-16LE and needs UTF-8 conversion, error
  handling. Offset against **not** building venv provisioning across
  macOS/Debian/Docker — roughly a wash, possibly cheaper.
- pdfium is C++ internally, so the sidecar links a C++ runtime (system libc++ on
  macOS, libstdc++ on Linux, `libstdc++6` in the image). Minor.

**This is the chosen design.** Same library as pypdfium2, same BSD-3 license,
`FPDF_RenderPageBitmap` for #3, ~7.4 MB, zero runtime dependencies, identical on
every platform.

### Rejected alternative: a hand-written zero-dependency C extractor

Recorded so it is not relitigated. A minimal C extractor (~800–1500 lines:
FlateDecode via miniz, `BT`/`ET`/`Tj`/`TJ` operators, `ToUnicode` CMaps) was
considered and **rejected**. It was viable — it would have handled text PDFs and,
via `DCTDecode` → `stb_image`, ordinary JPEG scans for the #3 seam.

Why pdfium won:

- **It swapped a supply-chain risk for a memory-safety risk we own and
  maintain.** pdfium is fuzzed continuously by Google and sandboxed in Chrome.
  ~1000 lines of our own parsing against a hostile format is more likely to carry
  an exploitable overflow than pdfium is.
- **Coverage.** It would fail on `JBIG2` and `JPEG2000` scans (which Acrobat's
  "optimize scanned PDF" emits), encrypted PDFs, and exotic CID fonts.
- **It cannot rasterize**, only extract embedded images. Fine for scans, useless
  for a vector chart. pdfium's `FPDF_RenderPageBitmap` covers both.

For the record, since the question was asked directly: **a general
zero-dependency PDF rasterizer is not writable.** pdfium's own bundled-license
manifest, read from the wheel, is the shopping list — `freetype, libjpeg_turbo,
libopenjpeg, libpng, libtiff, lcms, agg23, icu, zlib, abseil, simdutf`. Font
rasterization, path filling with anti-aliasing, color management, four image
codecs. Realistically 50k–200k lines. It *is* pdfium.

### The rest of the formats get easier, not harder

| Type | A2.1 says | Use instead | Weight |
|---|---|---|---|
| `.txt`, `.md`, source | native | native — already portable | 0 |
| `.pdf` | PDFKit helper (macOS-only) | **`libpdfium`** via its C API | 7.4 MB |
| `.docx` | `textutil` (macOS-only) | **miniz (MIT, single file) + XML text strip** — a `.docx` is just a ZIP holding `word/document.xml`; ~200 lines | ~0 |
| `.html` | `textutil` (macOS-only) | **reuse A3.1's extractor** — [TASKS_INTERNET.md](TASKS_INTERNET.md) already needs one. Build once, use twice. | 0 |
| `.rtf` | `textutil` | Drop with a clear message. Rare. | 0 |

Total added weight: **one 7.4 MB C library and one vendored MIT source file.**
Vendoring miniz is consistent with existing practice — [NOTICE](../NOTICE)
records that `json.h`, `tok.h`, `st.h`, `compat.h`, and `kernels.h` already
originate from colibrì.

### Architecture: a separate, sandboxed extractor process

The shipped release is **dependency-free today** — `INSTALL_FILES` in
[install.sh:64](../dist/install.sh#L64) is C sources, the wrapper, `app.html`,
and the tokenizer. The product principle is "dependency-free C server". The
extractor must not change that.

`POST /v1/documents` spawns a **short-lived `samosa-extract` binary** as a
subprocess, which prints `{text, pages, tokens}` as JSON on stdout and exits.
The model server links nothing new and stays exactly as it is; only the sidecar
links `libpdfium`.

**Why isolate it** (challenged and re-argued with the project owner,
2026-07-15 — "what attack surface? everything is running locally"). Three
reasons, strongest first:

1. **Robustness, which needs no attacker at all.** PDF parsers hang and crash on
   *malformed* files routinely — cheap scanners emit broken PDFs. In-process, one
   bad file takes down the resident server and the user loses a warm 2.5 GiB
   model and their session over a document that was merely corrupt. Out of
   process, the parent kills the child and returns a clear error. **This argument
   holds even if the threat model is empty.**
2. **"Local" describes delivery, not trust.** The untrusted input is the
   *document*, not the network. Users open PDFs they did not write — emailed
   invoices, downloaded papers, a vendor's spec. Adobe Reader was a local app
   with no server, and that is exactly where PDF exploits landed for fifteen
   years.
3. **#4 makes it genuinely remote.** A3.1 fetches URLs and ingests
   `application/pdf` through the *same* path ("ingests exactly like a
   document"). Once [TASKS_INTERNET.md](TASKS_INTERNET.md) ships, a remote party
   chooses the bytes that reach the parser, reachable from a chat message.

An earlier draft of this document called PDF "one of the most attacked surfaces
in computing" — that line is about Chrome's renderer and was overstated here.
The reasons above are the real ones. Out of process, the extractor gets:

- `RLIMIT_AS` / `RLIMIT_CPU` and a wall-clock timeout the parent enforces
- no network (the sidecar never opens a socket — assert it in the test)
- a hard kill that cannot corrupt the resident model or the expert cache
- a clean, specific error to the user instead of a dead server

**Do this even though it costs a process spawn per document.** Against a
multi-minute prefill, a few milliseconds of `fork`/`exec` is free.

Install story — and note this is now *uniform*, which is the win:

- **Docker** ([TASKS_WINDOWS.md](TASKS_WINDOWS.md)): the sidecar and `libpdfium`
  are in the image.
- **macOS / Linux native:** `libpdfium` is one more manifest-verified file
  fetched by the installer; the sidecar compiles alongside the engine with the
  toolchain [install.sh:120](../dist/install.sh#L120) already requires.
- **No venv, no pip, no network beyond the existing download, no per-distro
  package hunting.**

Document support must still degrade cleanly if `libpdfium` is missing for a
platform: a clear "PDF support unavailable on this build", never a crash and
never silent garbage.

### #3 finishes this feature

[APP_TASKS.md:243-246](APP_TASKS.md) requires scanned PDFs to "fail loudly not
silently". Correct for now — **but do not build that failure as permanent.**
Qwen3.6's vision tower ships already (see [TASKS_VISION.md](TASKS_VISION.md))
and is strong at OCR. When #3 lands, a scanned page becomes an image and the
model reads it.

Design the scanned-PDF path as a clean "not yet — this looks like a scanned
document, which needs vision support (issue #3)", with the seam left where #3
can plug in. **#3 is not a competing priority for #5; it is the second half of
it.**

pdfium supplies the seam directly: `FPDF_RenderPageBitmap` renders any page to a
bitmap, and embedded image XObjects are reachable too. When #3 lands, a scanned
page becomes an image the vision tower reads. No extra library is needed.

## Verified ground truth

**Prefill is the entire cost model.** ~14 tok/s (2 threads) / ~24 tok/s (fast).
A 5,000-token document costs **~3.5–6 minutes to read once**. A2's design
answer — sessions make that pay-once-per-document — is exactly right, and it is
this architecture's genuine advantage over re-prompting.

**Context: 24,576 tokens total, enforced** ([qwen36b.c:3092](../src/qwen36b.c#L3092)),
~40 KiB/token KV ≈ **960 MiB** at the cap. A2.2's "24K tokens ≈ 1 GB KV" is
consistent with the measured figure.

**Decode is brutally expensive in expert reads.** [APP_TASKS.md:149](APP_TASKS.md)
records a measured **933-token control that requested 376.77 GB of expert
reads** (~404 MB per generated token). Prefill is different: the sequential path
at [qwen36b.c:2620-2632](../src/qwen36b.c#L2620-L2632) reads a whole layer's
expert region in one 16 KB-aligned read (`seq_buf`), so a batch shares expert
loads instead of paying per token — which is why prefill is 2–4× faster per
token than decode. **Nobody has measured the expert-read volume of a large
document prefill.** E-D2 does.

**Long-context generation is untested, and documents are exactly the untested
regime.** A0.4 is open: the safety cap is implemented, but the long-context
regression is not done, and [APP_TASKS.md:144](APP_TASKS.md) notes "the
stack-overflow class was invisible to every existing test". **A2.2 depends on
A0.4.** A 20K-token document turn is the first thing that will find whatever
A0.4 was meant to find. Do not ship A2.2 with A0.4 open.

## Experiments

### E-D1 — Portable extraction matrix  ~1–2 days  **Gates A2.1**

A2.1's 12-file corpus (2 each: txt, md, text PDF, scanned PDF, docx, html),
extended:

- Run on **macOS, Linux, and inside the container** and diff the output. Same
  file → same text. This is the check that makes the portability decision real
  rather than aspirational. Pin the library versions: a pypdfium2 bump that
  changes extraction output across platforms is a regression users will see.
- Add adversarial cases, since this feature feeds untrusted files to a PDF
  parser: a `.docx` that is not a valid ZIP; a zip bomb as `.docx`
  (**decompression-ratio cap mandatory**); a PDF with no `ToUnicode` map; a PDF
  with a 50 MB embedded image; an encrypted PDF; a malformed PDF that hangs the
  parser (**the timeout must fire**); a UTF-16 `.txt`; a file whose extension
  lies about its type (sniff content, don't trust the name).

**Acceptance:** as A2.1 states — no temp files left behind, 100 MB rejected
clearly (20 MB default cap) — **plus** identical extraction across all three
platforms, and every adversarial case failing safely with a specific message
rather than a hang, a crash, or garbage.

### E-D2 — Ingestion cost and ETA accuracy  ~1 day  **Gates A2.2's UX**

A2.2 promises an ETA within ±20% "with measured, not optimistic, N". Measure it:
documents of 1K, 5K, 12K, and 20K tokens, at 2 and 4 threads. Record prefill
tok/s, wall clock, peak RSS, **and expert-read volume** (the engine already
reports it — the 376.77 GB figure came from somewhere).

Two things this must answer:

- Does prefill tok/s hold at 20K tokens, or degrade as KV grows? The ETA formula
  depends on the answer, and a linear estimate will be wrong if it degrades.
- What is the expert-read volume of a 20K-token prefill? This is the machine-
  safety number: if a single document ingestion pulls hundreds of GB through the
  SSD, that is a thermal and time reality the user must be told about before they
  commit — consistent with the standing guardrail about heavy work while the
  user is actively chatting.

**Acceptance:** a measured token-count → seconds → RSS → expert-GB table. ETA
within ±20% across all four sizes at both thread settings.

### E-D3 — Long-context document turn  ~1 day  **BLOCKED on A0.4**

The first real 20K-token document turn is the first real long-context test this
engine has had. Run A0.4's bounded design first, then this.

**Acceptance:** a 20K-token document ingests, answers, saves a session, and
resumes after restart — under the standing machine guard, with zero new swap.
If A0.4's stack-overflow class resurfaces here, that is a finding, and it is
better found by this test than by a user.

### E-D4 — Retrieval grounding  ~1–2 days  **Gates A2.3**

A2.3's BM25-in-C is the right call, and its stated reason is the right reason:
no embedding model exists locally, "do not pretend otherwise". Do not be tempted
by mean-pooled `embed_tokens` vectors — token embeddings are not sentence
embeddings, and a weak retriever that looks principled is worse than a strong
lexical one that is honest.

Test as A2.3 specifies (100-page PDF, index < 30 s, ≥7/10 grounded with correct
citations, absent answers refused explicitly). Add: a query whose terms never
appear literally but whose concept is present — **BM25's known weakness**. Report
the failure rate rather than tuning it away. It belongs in A2.4's limitation copy.

### E-D5 — Two documents in one conversation  ~0.5 day

A2.2's acceptance says "two documents in one conversation compose". Worth its own
attention: 2 × 8K documents = 16K tokens plus the question, against a 24,576 cap.
Test the boundary — 3 documents must fail cleanly at the cap
([:4220](../src/qwen36b.c#L4220) returns `400 context_limit`), not truncate
silently. Verify the user is told *which* document did not fit.

## Tasks

A2.1–A2.4 stand as written, with these amendments:

### A2.1+ — `samosa-extract`: a sandboxed C sidecar  ~3–4 days

A small C binary linking `libpdfium` (PDF) plus vendored miniz + an XML strip
(`.docx`) and the shared A3.1 extractor (HTML). Rlimits, no network, parent-
enforced timeout. `libpdfium` becomes a SHA-256-pinned, per-platform manifest
entry; the sidecar compiles alongside the engine.

Survives #1/#2 and needs no runtime — whereas the PDFKit version is thrown away
the moment Linux ships, and the Python version would add a venv to every native
install.

**Acceptance:** E-D1 passes on macOS, Linux, and in the container, with
identical output. The sidecar cannot outlive its timeout, exceed its memory
rlimit, or open a socket — **test each with a crafted file, not by inspection**.
The model server links nothing new: verify with `otool -L` / `ldd` that
`qwen36b` has no new dependencies.

### A2.2 — Full-document ingestion  ~2 days  **BLOCKED on A0.4**

As specified. Ingest at the fast thread setting by default (bounded work, worth
the heat, user-overridable) with a progress bar fed by real prefill telemetry.

**Acceptance:** as written, plus E-D2's ETA accuracy and E-D3's long-context arm.

### A2.3 — Retrieval  ~2–3 days  (unchanged)

As specified. Show used chunks as citations. Surface the honest per-question cost
(~5K tokens ≈ 3.5–6 min at 2T) **before** the user commits.

### A2.4+ — Limitation surfacing  ~0.5 day

As specified, **but first resolve the "int4 doubling artifact"**.
[APP_TASKS.md:280](APP_TASKS.md) says document answers "inherit the int4
doubling artifact". That phrase appears **exactly once in the entire repo and is
defined nowhere** — not in [REGRESSION_LEDGER.md](REGRESSION_LEDGER.md), not in
the README, nowhere. There is a [repetition_guard.h](../src/repetition_guard.h)
that may or may not be related.

It cannot ship in user-facing copy while undefined. Either reproduce it, name it,
and document it in the regression ledger — or delete the claim. **An undefined
known-limitation is exactly the kind of stale claim this project's accuracy bar
exists to catch.**

Add E-D4's BM25 concept-query failure rate to the limitation copy, measured.

## Non-goals

- OCR of scanned documents in v1 — **deferred to #3, not rejected.** Leave the seam.
- An embedding model. Nothing local; BM25 is the honest answer.
- Document editing or generation. Read-only.
- Cloud/Drive/Dropbox sources. Local files and, via A3.1, URLs.
- Indexing a whole folder. One document at a time; the session is the unit.

## Open questions

- **Does the 20 MB cap match the 24K token cap?** A 20 MB text file is roughly 5M
  tokens — 200× over. The size cap and the token cap protect different things, and
  the token count is the one users hit. A2.1 already token-counts before ingestion
  and reports it, which is right; make sure the *message* leads with tokens, not
  megabytes.
- **Where does the retrieval index live?** A2.3 does not say. `~/.samosa/chats/<id>/`
  next to the session snapshot is the natural home — it shares the artifact's
  lifetime and gets deleted with the conversation. Decide before A2.3.
- **Is `conversation_id` the right handle for a document session?** Today it is
  `[A-Za-z0-9_-]{1,64}` ([SERVE_API.md:34](SERVE_API.md)) with one sealed
  `session.qws` per id. A document session is exactly that plus provenance. If
  documents need metadata (source path, hash, ingest time), decide where it lives
  before A2.2 seals the format — [APP_TASKS.md:350](APP_TASKS.md) already flags
  format versioning as a task.
