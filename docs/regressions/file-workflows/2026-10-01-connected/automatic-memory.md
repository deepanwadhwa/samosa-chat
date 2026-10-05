# Automatic folder-memory handoff

Dates: 2026-10-01–02. Baseline: `2017598`, existing uncommitted connected-workflow
implementation. Host: macOS arm64. No ticket closure or browser acceptance claim.

Jobs discovery, Folder report, duplicate finding, type sorting and inbox
classification now request the existing durable Chutni build after a successful
inventory. The handoff is saved in each job's `memory.json` and event history.
Saved discovery results expose the current scope/build state on reopen.
Selected-file questions retain their selected scope; this does not automatically
attach the whole folder to their conversation.

New folders receive a registered portable store. Registered folders refresh
incrementally without resetting artifacts or replacing their exclusions. Root
identity and portable exclusion compatibility are checked. Requests made while
another build is active remain in durable job state and are drained by the same
single worker, including after restart. Interrupted active builds retain the
existing paused/resume behavior.

Automatic builds populate reusable document-reader extraction/OCR evidence and
portable text artifacts. They skip optional model summaries/captions. Explicit
Chutni builds retain those features. The selected document reader continues to
validate source identity/hash and reader compatibility before reusing its cache.
Later Jobs PDF previews also reuse complete matching host-reader entries. The
gateway passes the actual reader fingerprint and existing cache directory to
the helper; the helper hashes the current anchored source bytes and validates
full page count/order, successful coverage and line text before taking a leading
page preview. Changed bytes, uncertain/partial entries and older fingerprints
fall back to extraction. This change does not add a second cache or index
service. Other preview formats retain their existing reads.

Scan errors and `complete_for_policy: false` now produce `ready_partial` and
`completed_partial`, even when no safety budget set the older `partial` flag.
The result note also discloses metadata-only files and enrichment failures.
The readable-file count is not an exhaustive retrieval guarantee.

An adjacent portable store without Samosa registration is preserved and
reported as `registration_required`; automatic refresh does not overwrite an
unknown policy. Changed/unavailable portable exclusions report `policy_changed`.
These cases still require the existing memory registration/preview flow.

Verification uses `tests/assert_folder_memory.py` inside the compiled gateway
gate, with the real scanner and Chutni service and a fake model backend. It
checks automatic creation, indexed evidence, changed-source withholding,
incremental refresh retaining unchanged sources, two outstanding folder
handoffs, preservation after forgetting registration, and retrieval/event
history after SIGKILL/restart. This is native API evidence, not a real-model or
browser walkthrough.
An unreadable source also verifies `ready_partial`, `complete_for_policy: false`
and a positive scan-error count.

`tests/test_samosa_decision.py` checks that a complete matching PDF entry avoids
extraction and does not leak the second page into a first-page preview. Different
reader fingerprints, incomplete page arrays, uncertain reads and changed bytes
require fresh extraction. Three pre-existing optional runtime tests skip when
their model/extractor dependencies are absent.

Browser skill setup succeeded, but selection reported “No browser is available”;
the documented discovery check returned `[]`. Layout, keyboard, zoom and the
complete browser walkthrough remain unverified. FW-6 stays open and FW-7 stays
gated. This increment does not qualify exhaustive retrieval or responsiveness.

The first sandboxed gateway gate could not bind its local port. It was rerun
with local-port permission and passed creation/refresh/restart checks. The final
suite results and logs are recorded in the companion connected README.

## Final verification and real local app

The final `make test` passed (exit 0), including automatic memory and Chutni
crash/resume tests. Jobs/sidebar DOM fixtures and `git diff --check` passed.
The first full run stopped in the gateway shell gate after the mixed-folder
checks; it did not log the failing assertion. A traced native rerun passed,
followed by the final clean full run. The initial failure is retained rather
than assigned an unproven cause.

Installed release: `dev-b90bd22349fd`. The first real probe encountered connection
refusal after an app-owned launch without an available browser. Starting the
gateway with `serve --foreground` made the local test setup reachable.

The real v1 three-file fixture created ready memory automatically: three readable
files, all 40 PDF pages read and cached, and zero optional summaries. A later
Jobs discovery returned `shared_pdf_cache`, reused the same memory scope, kept
its own selection empty, and preserved the original two-file selection. The
first reuse probe had an incorrect fixture-string assertion; its corrected
page-one/no-page-37-leak assertion passed. The low relevance score for the PDF
remains visible; cache reuse does not qualify classifier ranking.

Ornith (`Ornith-1.0-9B-Q4_K_M.gguf`) passed both cited questions: page 37 in
12.28 seconds and the appointment follow-up in 32.55 seconds. Qwen (`qwen`,
existing expert-streamed installation) passed the same two questions in an
independent fork in 54.60 and 103.61 seconds. This is the small v1 fixture,
not a repeat of the complete larger organize/browser acceptance or an isolated
latency benchmark. The original selection remained intact.

Point observations: Qwen RSS 3,441,296 KiB and gateway 10,112 KiB during its
run. Restored Ornith RSS 5,869,328 KiB and gateway 4,624 KiB. Swap used 0 MiB
at both observations. These are not peak measurements. Ornith was restored;
`/healthz` confirmed `backend: ornith`, `ready: true`, and `app_owned: false`.

Commands:

```sh
make test
node tests/test_jobs_ui.mjs
node tests/test_sidebar_ui.mjs
python3 tests/test_samosa_decision.py
tools/install_local_dev.sh
python3 tests/test_file_workflow_live.py --backend ornith
python3 tests/assert_folder_memory_live.py job-1790956731-27459-441641930
python3 tests/test_file_workflow_live.py --backend qwen --clone-from job-1790956731-27459-441641930
git diff --check
```

Logs beside this document: `automatic-memory-full-suite-passed.log`,
`automatic-memory-initial-full-failure.log`, `automatic-memory-native-passed.log`,
`automatic-memory-install.log`, `automatic-memory-ornith-passed.log`,
`automatic-memory-qwen-passed.log`, `automatic-memory-later-jobs-passed.log`,
`automatic-memory-later-jobs-fixture-failure.log`,
`automatic-memory-live-connection-failure.log`, and `automatic-memory-restored.json`.
