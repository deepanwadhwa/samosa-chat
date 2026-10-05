# Manual workflow UI cleanup, 2026-10-02

Continuation on base commit `20175987e142c7363dbebbc87488e03b5740ae03`
with the existing uncommitted connected-workflow and automatic-memory work.
Host: local macOS arm64. This increment changes the UI and its existing DOM
fixture; retrieval, model execution, and filesystem operations are unchanged.

## Changes

- Group Ask, Copy/Move, and Refine controls by purpose. Keep the existing
  handlers and reviewed plan approval.
- Reduce sorting to Best matches first and Name: A to Z. Remove model names
  and probabilities from action and file-match progress. Per-file match details
  still expose diagnostic scores and prompts.
- Remove the stale dynamic source-details renderer and its callers. It claimed
  Jobs never used folder memory, contradicting the implemented cache reuse.
  A short static explanation describes current-file checks and eligible
  saved-text reuse. Saved-task memory coverage/status remains unchanged.
- Honor Show all even when a saved selection exists. Unselected candidates
  become visible without changing the selection.
- Preserve keyboard focus after deselection removes a row: move to the next
  checkbox (or the previous one at the end), then the filter when none remain.

## Verification

`node tests/test_jobs_ui.mjs`, `node tests/test_chutni_ui.mjs`, and
`node tests/test_sidebar_ui.mjs` pass. The Jobs fixture exercises Show all with
a nonempty selection, keyboard deselection through the last row, and the
existing selection-save failure/retry and conversation handoff cases. Whole
app script syntax and `git diff --check` pass.

The full `make test` retry passed (exit 0), including compiled gateway,
folder-memory recovery, detached service, application lifecycle, and LAN
checks. `ui-cleanup-full-suite-passed.log` records the run. The first attempt
stopped because the sandbox prevented `test_samosa_serve` from binding its
local HTTP server; `ui-cleanup-sandbox-failure.log` records that failure.

The cleanup is included in the subsequent minimal visual update; installation
and visual verification are recorded with that update.

## Acceptance limits

The Browser runtime initialized, but selection returned “No browser is
available” and discovery returned `[]`. No browser was controllable through
the supported connection. No rendered screenshots, real keyboard events,
390px/1440px layout, 200% zoom, contrast, or unfamiliar-user walkthrough is
claimed. DOM focus assertions establish handler behavior, not browser
accessibility acceptance.

Real-model runs were not repeated for this UI-only increment. Existing real
model evidence and its retrieval/latency limitations still apply. FW-6 stays
open; FW-7 stays gated. No new dependency or alternate workflow was added.
