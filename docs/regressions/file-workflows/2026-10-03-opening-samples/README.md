# Bounded folder-memory samples — 2026-10-03

All document inputs were generated independently. Downloads and its files were
not opened for inspection or testing. Deployment used only runtime health and
the build's control state to pause it and preserve that pause across restart.
Earlier automatic memory handoffs remain deferred. Scheduling remains gated.

## Reading behavior

Memory builds now catalog metadata and file identities, then extract opening
samples targeting 3,000 Unicode characters per supported file. Text reads stop
at a complete UTF-8 character. PDF extraction requests one page at a time and
stops after the complete page that reaches the target. OCR uses the same budget.
An oversized first page or image is kept as one complete unit. Sparse PDFs
stop after 12 pages; the existing document timeout is retained. These guards
are recorded separately from reaching the character target or end of file.

The entire collected sample goes to the summarizer, including page overshoot;
the old token-budget field is retained for API compatibility and no longer
clips manual-build samples. The native summarizer maps the sample in bounded
chunks rather than silently stopping after ten chunks. Failed summaries are
reported separately. OCR-readable images do not require an extra model caption.

This bounds content extraction and OCR, not source-identity hashing: catalog
and artifact integrity checks can still read the file's bytes for hashes.
Previously stored artifacts are preserved unless explicitly rebuilt. Samples
use a separate cache contract and cannot satisfy a full-document cache lookup.
Source metadata retains total page count, sampled pages, actual character
count, stopping reason and OCR review flags. The answering harness distinguishes
inventory completeness from content completeness and cannot infer absence from
unread content.

## Visibility

The card shows files finished, the actual active file before processing, current
page, collected characters, extraction/OCR/saving/summary activity, sampled versus
fully read files and failed readings/summaries. Page activity updates during a
file. Throughput uses the current pass's elapsed time; small rates are displayed
with enough precision instead of rounding to `0.0`. Old builds are clearly
identified and their old file label is shown as the last completed file. The
obsolete summary-budget UI was replaced with the reading target and coverage
explanation. Pause wording describes catalog checking and cache reuse accurately.

## Verification

- `make test-chutni-sampling`: real reader control flow with deterministic
  sidecars. Covers accumulated short pages, oversized first page, Unicode
  budgets, OCR stopping, blank-page guard, warm sample caches, no complete-cache
  pollution, ASCII/accented/emoji text boundaries and malformed-byte bounds.
- Document-reader cancellation/cache contracts and summarizer supervisor pass.
- Chutni gateway, controls, crash/resume and inventory parity tests pass.
  The integration test proves text tails are not indexed past 3,000 characters.
- UI card rendering and behavior tests pass, including active-file labels,
  page/character activity, accurate small rates, legacy cards and pause controls.
  JS syntax and 20 frozen API contract tests pass. Browser visual inspection
  was unavailable because the browser runtime import was rejected for
  `node:process`; DOM checks do not claim browser layout/paint verification.
- Real PDFium/Tesseract samples: native 20-page PDF stopped after 3 pages and
  3,213 characters; mixed PDF stopped after 3 pages and 3,097 characters, with
  two OCR pages; oversized PDF stopped after 1 page and 5,024 characters; blank
  PDF stopped after 12 pages. See the reader summaries alongside this file.
- Real portable enrichment with the installed native T5 runtime processed the
  six generated files, stored five summaries, reported zero failures, and
  excluded unread-tail sentinels. The blank PDF correctly had no summary.
  `real-enrichment-summary.json` records the final run with warm reader caches;
  its timing is not a promise of sub-second cold OCR or summarization.

The fixture is `~/Documents/Samosa Walkthrough/Opening Samples`. Reproduce it
in a fresh directory with `tools/gen_chutni_sampling_fixture.py` using the
existing test-only ReportLab/Pillow dependencies. No new Python dependency was
added to the application.

The first `make test` run failed at a crash test that selected the newest Jobs
directory by mtime while automatic memory work was running. The test now takes
the job ID from its own SSE stream. The compiled gateway gate subsequently
passed, as did all remaining top-level test stages. Logs preserve the initial
failure and the successful subsequent gates; the first run is not described
as a successful monolithic `make test`.

Release `dev-6909b5cecf67` is installed and running on port 8642 with Ornith
ready. Installed gateway, service and UI hashes match the tested source.
The user's earlier build remains paused; Resume uses the new sampling policy.
