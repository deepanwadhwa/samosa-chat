# FW-6 continuation, 2026-10-03

This is the earlier failed experiment record. Later installer, inventory-parity,
full-suite and folder-chat corrections are recorded in the
[decision harness evidence](../2026-10-03-decision-harness/README.md).
The mixed-document review and broader FW-6 acceptance remain open.

Base commit: `20175987e142c7363dbebbc87488e03b5740ae03`, with the existing
uncommitted file-workflow work preserved. Host: macOS arm64. All document
acceptance uses independent synthetic fixtures and newly created conversations.
The user's PDF and existing conversations were not opened.

## Changes

- An 18,000-byte section experiment reduced review calls from three to two,
  but two later live runs failed. It was reverted: retained limits are
  14,000 bytes, 150 seconds per call and six minutes total. No reliable
  response-time improvement is shipped by this continuation.
- Answer instructions exclude internal review commentary. Source summaries
  retain page references and the warning against treating summaries as exact
  quotations; byte offsets and internal section numbers no longer enter final
  synthesis evidence. These final wording changes still require live answer
  qualification at the retained review limits.
- Model labels, runtime status, OCR cards and visual-reading cards use shared
  light/dark colors. OCR settings describe reading scanned text rather than
  obsolete detector/recognizer implementation details. Existing controls and
  the single system typeface are retained; no dependency was introduced.
- The HTTP app test and installer UI smoke check use the current document title,
  instead of the former app name and removed marketing slogan. Runtime-only
  installer failure diagnostics now show the end of the log, where errors occur.

## Measured performance

The first updated Ornith run (development release `dev-6b22efb30844`,
`Ornith-1.0-9B-Q4_K_M.gguf`) finished in 315.44 seconds versus the earlier
397.21-second cached-extraction baseline: 20.6% faster. Two reviews took
241.52 seconds; synthesis took about 74 seconds. Counts 7101, 7120, 7137,
7140 were correct and cited pages 1, 20, 37, 40. All 40 pages were recorded.
Extraction was cached, so this is not a cold OCR timing. The runtime snapshot
showed about 6.1 GB model RSS and zero swap use. Regression checks overlapped
the model run; these are observed local timings, not controlled benchmarks.

That experimental answer still reported section boundaries as reading gaps. It is a failed
commentary-cleanup acceptance result, despite correct cited facts. The final
source-label changes were made after this measurement.

The next live run failed in the second review with `document_review_incomplete`
under concurrent regression load. No final answer was emitted. This is retained
in [failed live run](ornith-review-failure.log). A further run with a three-minute
per-call allowance also failed; see [retry](ornith-final-retry-failure.log).
The larger-section and longer-call changes were reverted after these failures.

Final reader contract, document harness, normal/small-context 40-page extraction,
review-failure, and Jobs/Chutni/sidebar/detail DOM checks pass. The compiled
gateway check failed report/inventory parity: 16 versus 18 files in sequential
snapshots. [Focused log](focused-regressions.log) preserves the failure.

## Acceptance limits

Browser setup failed before selection with `Importing module "node:process"
is not allowed in node_repl`. No tab or existing conversation was opened.
There are no rendered screenshots or verified 390px/1440px layouts, 200% zoom,
real keyboard interactions, browser reopening, or unfamiliar-user walkthrough.
DOM tests establish handler behavior only.

The first full-suite run stopped on obsolete app-name/slogan assertions; those
are fixed. The following run passed those checks but failed the clean
runtime-only installer. Its truncated diagnostics hid the cause; the final
rerun exposed a stale `Your model.` slogan check in `dist/install.sh`.
It now checks `<title>Samosa</title>`. Shell syntax passes; the runtime-only
installation gate has not yet been rerun with that fix. See
[full-suite failure](full-suite-failure.log). No full-suite pass is claimed.

FW-6 remains open. FW-7 and scheduling remain gated. No broad retrieval or
arbitrary-document accuracy claim follows from these fixtures.
