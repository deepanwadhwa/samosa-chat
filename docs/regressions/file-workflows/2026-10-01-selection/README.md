# FW-1: durable selection increment

Date: 2026-10-01. Baseline commit: `2017598`; tested working tree is
uncommitted and includes pre-existing recovery changes. No CI claim.
Host: local macOS arm64 development machine. Tests use synthetic files and fake
model backends; this increment adds no real model inference.

## Implemented

- Selected paths persist per existing job in `selection.json`. Atomic writes
  reuse the gateway's existing writer; search evidence remains in `decision.json`.
  A separate small artifact prevents checkbox saves from overwriting search progress.
- The native selection endpoint validates membership in saved search results,
  duplicates, path size, and the 50-file bound. No classifier/Python runtime is
  needed to save or restore a selection.
- Reopening a search restores the original request, folder, and chosen subset.
  Follow-ups resolve the saved selection on the server; explicit paths remain
  supported for existing API clients.
- UI writes are serialized. Failed writes block follow-ups and preserve visible
  choices and the question for retry. Opening history is blocked during an action.
- New search evidence records exact modification times as strings. Subsequent
  preview reads validate inode/device, size, and recorded modification time and
  reject changes observed during extraction. Existing old records without a
  modification time retain compatibility but cannot detect every same-size edit.
- Preview/refinement controls describe their actual behavior. The misleading
  “Ask about file” button and “Check in detail” label were removed. Checkboxes
  now provide the single file-selection control, with its unused button CSS removed.
  No new screen,
  dependency, index, or conversation engine was introduced.

## Verification

| Command | Result |
|---|---|
| `make samosa-gateway` | Passed with compiler warnings treated as errors |
| `node tests/test_jobs_ui.mjs` | Passed: restore, job isolation, deselection, serialized edits, failed save and retry |
| `python3 tests/test_samosa_decision.py` | 8 cases: 5 passed, 3 optional real-model/extractor cases skipped |
| `make compiled-gateway-test` | Passed, including native selection scope checks, forced restart, server-resolved follow-up selection, and explicit-selection API compatibility |
| `make test` | Passed (exit 0), including the new source-change and selection regressions |
| `git diff --check` | Passed |

The initial sandboxed gateway test could not bind a local test port; rerunning
with approved local-port permissions passed. Gateway regression coverage also
includes attachments, document context, web search, and developer traces.

Source-change cases cover a same-inode edit of the same byte count, replacement,
rename/missing source, symlink substitution, and mutation during extraction.
Selection tests seed saved results to isolate persistence from classifier quality;
a tiny helper fixture verifies the gateway's follow-up scope, not answer accuracy.

Logs: [build](build.log), [focused cases](focused-tests.log),
[gateway regressions](compiled-gateway.log), [full suite](full-test.log).

## Outstanding acceptance

FW-1 remains open. Conversation association and common document/organize action
handoff are not implemented. Browser rendering/real-user walkthrough, real-model
document answers, model switching, and the full immigration fixture have not
been run. Changed-file status is established during follow-up preview reading,
not proactively refreshed on every result reopen. FW-2–7 remain open.
