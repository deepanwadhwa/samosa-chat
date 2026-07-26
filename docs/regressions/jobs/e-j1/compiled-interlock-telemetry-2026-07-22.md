# E-J1 compiled interlock + active inference telemetry checkpoint (2026-07-22)

Scope: focused compiled-gateway regression for the two E-J1 evidence gaps that
do not require a vision backend: chat interlock observability and active
inference timing. This is an offline/fake-backend proof, not the full live
E-J1 acceptance rerun.

## What changed

- `/internal/v1/status` is now implemented by the compiled gateway and reports
  `inference_busy`, `interactive_active`, `last_interactive_ts`,
  `last_interactive_age_seconds`, and `interactive_cooldown_seconds`.
- Interactive `/v1/chat/completions` requests mark `interactive_active` while
  the proxy is in flight and record the last interactive completion time.
- `/v1/jobs/definition/run` honors `job.resources.pause_when_user_active:true`
  by pausing before the next unit while interactive chat is active or inside the
  cooldown window. It emits `job_paused reason:"interactive_chat"` and
  `job_resumed reason:"interactive_chat"` SSE events.
- Definition `item_complete` events now include `model_call_seconds` and running
  `active_inference_seconds`; the final `done` event includes total
  `active_inference_seconds`.
- `tools/run_e_j1.py` now summarizes active inference time from the streamed
  events and records whether an interactive interlock pause/resume was observed.

## Test shape

`tests/test_compiled_gateway.sh` still removes `python3` from `PATH`. The new
case starts a two-item definition run whose job has
`resources.pause_when_user_active:true`, then launches a slow interactive chat
while the first background model call is still active. The definition run
completes the first item, sees the active interactive chat before the second
item, emits `job_paused`, waits for the chat plus a short test cooldown, emits
`job_resumed`, then finishes the second item.

Assertions added:

- `/internal/v1/status` reports the configured cooldown and interactive state.
- Definition SSE contains `job_paused`, `job_resumed`, `model_call_seconds`,
  `active_inference_seconds`, and final `done`.
- The two-item definition output still writes two JSONL records.

## Verification

```text
$ make compiled-gateway-test
...
compiled gateway without python: PASS
```

Result: passed for the compiled/no-Python runtime path. Remaining E-J1
acceptance work is the live run shape from `docs/TASKS_JOBS.md`: a real
interactive chat during a real labeled batch, plus the image/multi-image
coverage that requires a vision-capable backend.
