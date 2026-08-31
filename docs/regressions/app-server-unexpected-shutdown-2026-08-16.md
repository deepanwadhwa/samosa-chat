# App server unexpectedly disappeared on refresh

## Root cause

Two shutdown paths made an ordinary browser action capable of taking down the
local Samosa gateway and, consequently, its selected model backend:

1. `assets/app.html` registered a `pagehide` handler that posted to
   `/v1/shutdown`. Browsers fire `pagehide` during an ordinary tab refresh, so
   refresh was interpreted as “quit the app.”
2. `/v1/shutdown` and `/v1/kill` were exceptions to the per-launch UI-token
   check. Any localhost page, including a stale Samosa tab from an older
   launch, could therefore stop a fresh gateway.

That sequence explains both symptoms seen together: the loaded Ornith backend
disappeared and Settings could no longer load the model list. The model itself
was not removed; its gateway/backend process had been stopped.

## Fix

- `samosa app` is browser-owned. A close event removes that tab from the active
  client set; closing the final tab stops the gateway and its complete model
  process group after a 2.5-second grace period. A refresh reconnects before
  that deadline and preserves the same process and selected model. Multiple
  tabs are safe because only the final close initiates shutdown.
- `samosa serve` remains explicitly persistent and ignores browser lifecycle
  closure, so headless and intentional background use still works.
- Both shutdown routes require the current random UI token.
- The explicit `samosa serve --stop` path reads that token and requests a
  clean shutdown before unregistering the launchd job; it uses a direct
  process fallback only if necessary.
- The Settings kill action uses the authenticated request helper.
- Lifecycle tests prove an unauthenticated shutdown receives HTTP 401 and the
  gateway remains healthy; authenticated shutdown and SIGTERM are recorded.

## Evidence for the next failure

The gateway now appends metadata-only JSONL events to:

```text
~/.samosa/logs/gateway-lifecycle.jsonl
```

Normal exits record `gateway_shutdown_requested`, `gateway_shutdown_observed`,
and `gateway_exiting`, including the exact mode (`api_shutdown`, `api_kill`,
`signal`, or `server_error`) and signal number where applicable. A per-port
active marker remains while the process is alive. If the process is killed in
a way that cannot run cleanup—SIGKILL, crash, power loss—the next successful
launch records `previous_exit_unrecorded` before replacing that marker.

Inspect the most recent lifecycle evidence with:

```sh
tail -n 50 ~/.samosa/logs/gateway-lifecycle.jsonl
```

This log contains lifecycle metadata only: no prompts, replies, transcripts,
or audio.
