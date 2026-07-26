# E-J1 JSS PDF live interlock rerun

This is the authoritative live interlock rerun for the 2026-07-22 PDF slice.

- Backend: compiled `samosa-gateway` + Ornith 9B (`supports_images:false`)
- Corpus: four labeled JSS PDFs copied from `/Users/deepanwadhwa/Downloads`
- Harness: `tools/run_e_j1.py` against `/v1/jobs/definition/preview` and
  `/v1/jobs/definition/run`
- Interlock probe: `/v1/chat/completions` opened 5 seconds after run start with
  `max_tokens:16`
- Result: 4/4 records `passed`, 44/48 fields correct, 0 `review_required`,
  0 failed
- Timing/interlock: `active_inference_seconds=124.93`, 1 `job_paused`, 1
  `job_resumed`
- Machine safety: `Pages throttled=0`, `Swapins=0`, `Swapouts=0` before/after

The earlier sibling directory
`jss-pdf-ornith-interlock-2026-07-22/` is marked `ABORTED.md` and is debug-only:
the chat probe was allowed to reason too long and the gateway was cancelled
manually. This directory is the clean evidence to cite.
