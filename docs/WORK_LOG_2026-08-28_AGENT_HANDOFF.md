# Agent handoff: regression fixes and CI verification (2026-08-28)

This file records the exact state left by the previous agent. It is intentionally
written as a continuation note: do not assume the branch is clean or that the
Docker CI is green.

## User-requested scope

The work addressed the reported regressions in GCC/Linux builds, voice UI
fixtures, VisionPsy routing, document orchestration tests, PDF paging, and the
Maple memory audit. It also carried forward the requested removal of the legacy
Chutni implementation and the orphaned Gigatoken prototype.

No commit or push was performed.

## Changes already present

### Legacy code removal

- Deleted src/samosa_chutni_db.c.
- Deleted the duplicated private SQLite tree under src/sqlite/.
- Deleted the four legacy tests/test_chutni_db*.sh scripts.
- Deleted src/samosa_gigatoken.h, Gigatoken tests, the token oracle, the Rust
  adapter, and its build helper.
- Removed their Makefile targets/default branches.
- The maintained vendor/chutni service remains; this deletion did not remove
  the current Chutni MCP/gateway service.

### Gateway and routing fixes

- Fixed the chained one-line if statements that GCC diagnosed as
  -Werror=misleading-indentation at
  src/samosa_gateway.c:7828.
- Added lettering and line break to the exact-text/OCR route in
  vision_route_fallback() so prompts such as “copy the lettering exactly and
  preserve line breaks” do not start VisionPsy unnecessarily.
- A failed VisionPsy inspection now releases the turn-wide specialist lease and
  helper session before the one bounded lower-resolution retry.
- VisionPsy E2E readiness checks now require health JSON containing
  "ready":true, rather than treating any HTTP 200 as readiness.

### Test and fixture fixes

- Updated tests/test_voice_ui.mjs to locate the current
  async function sendPrompt(...) declaration rather than the removed exact
  sendPrompt(text) spelling.
- Removed manual fake-backend startup/PID ownership from document, motto, tier2,
  R7/R6, and PDF paging tests. The gateway now owns the backend through
  SAMOSA_BONSAI_SERVER and SAMOSA_BACKEND_PORT.
- Added deterministic temporary fake OCR sidecars to the document, motto, and
  tier2 orchestration tests so those tests do not require the optional OCR model
  pack.
- R7/R6 now explicitly skips with build guidance when det.bin, rec.bin, or
  charset.txt is absent.
- PDF paging waits for "ready":true and exercises the real seven-page fixture.
- Maple fixture generation no longer constructs a duplicate full reference
  model; tools/test_maple_memory_safety.sh now invokes the AST-based
  tools/audit_maple_fixture_models.py bounded-dimension audit.

### Linux linker portability

The first real Debian GCC run exposed an additional issue masked on macOS:
floor() was unresolved because Linux requires an explicit math library.

The following rules now use -ldl -lm on non-Darwin systems:

- Makefile:25-33 defines DL_LDFLAGS.
- Makefile:217-220 applies it to samosa-gateway.
- Makefile:262-265 applies it to samosa-jobsd.
- dist/install.sh:317-320 applies the same flags to both staged release
  binaries.

## Validation completed

These completed successfully before the Docker work was stopped:

- macOS make test (full local suite): PASS.
- make test-visionpsy: PASS.
- node tests/test_voice_ui.mjs: PASS.
- make doc-read-test: PASS.
- make motto-test: PASS.
- make tier2-test: PASS.
- make doc-read-pdf-paging-test: PASS.
- make test-maple-memory: RESULT: SAFE.
- Host Linux/Clang fallback portability and compiled-gateway checks: PASS.
- git diff --check: PASS.
- Shell syntax and relevant Python compilation checks: PASS.

Explicit, expected skips:

- make r7-r6-test: skipped because the OCR pack is not installed.
- make ocr-test: skipped for the same missing OCR-pack artifact.
- Real-model gates were not runnable because Maple, VisionPsy, Molmo2,
  summarizer, OCR, and PDFium artifacts are not present in this workspace.

## Docker CI chronology and exact remaining uncertainty

Docker Desktop was initially unreachable. It was started with docker desktop
start; afterward docker info reported Docker Server 29.0.1.

### Debian

1. make ci-debian run #1 reached GCC linking and failed at samosa-jobsd:

       /usr/bin/ld: ... undefined reference to floor
       make: *** [Makefile:264: samosa-jobsd] Error 1
       make: *** [ci-debian] Error 2

   This was the reason the samosa-jobsd -ldl -lm rule was added.

2. make ci-debian run #2 started after that patch, using
   debian:bookworm-slim, GCC 12, and the x86_64 Docker platform. It was still
   compiling when the user asked to end the work. The container was stopped;
   Docker reported exit 137 from the forced stop. Therefore Debian CI is
   not certified PASS yet.

### Ubuntu

make ci-ubuntu-full was not run after Docker became available. Earlier
daemon-unavailable attempts are obsolete and must not be treated as test
results.

## Workspace state

- No commit, branch operation, or push was performed.
- git diff --check is clean.
- The worktree is intentionally very dirty and includes many unrelated existing
  modifications and untracked files; preserve them.
- An untracked generated build-linux-ubuntu/ directory is present. Do not
  delete it without confirming ownership/scope.
- No Docker CI container remains running.

## Recommended next-agent checklist

1. Inspect git status --short and preserve unrelated user changes.
2. Re-run make ci-debian and capture the complete exit status. Confirm both
   samosa-gateway and samosa-jobsd link with -ldl -lm.
3. If Debian passes, run make ci-ubuntu-full and capture its complete result.
4. Re-run git diff --check.
5. Review the focused diff before any staging:

       git diff -- Makefile dist/install.sh src/samosa_gateway.c \
         tests/test_voice_ui.mjs tests/test_doc_read.sh \
         tests/test_doc_read_pdf_paging.sh tests/test_motto_scenario.sh \
         tests/test_tier2_escalation.sh tests/test_r7_r6_handwriting.sh \
         tests/test_visionpsy_e2e.sh \
         tests/fixtures/maple/generate_maple_model_fixtures.py \
         tools/test_maple_memory_safety.sh

6. Only after both Docker targets are genuinely green should a user explicitly
   authorize staging, commit, and push.
