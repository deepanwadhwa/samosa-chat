# Native Scheduler / Public URL Handoff — 2026-07-21

This is a checkpoint for continuing the native Jobs work after machine
shutdown. It records what was read, what was changed, and what remains.

## Starting Point

- Branch: `issue-7-jobs`
- Reference commit mentioned by the owner: `17494a4`
- Task doc: `docs/TASKS_JOBS.md`
- Relevant section: "Native Jobs completion estimate (2026-07-21)"

The task is not to build Jobs from scratch. Python prototypes already exist in
`tools/samosa_jobs.py` and `tools/samosa_gateway.py`. The documented gap is
native compiled parity:

- native background scheduler;
- native public-URL input pipeline;
- tests proving this works with `python3` unavailable;
- only after parity, removal/reduction of obsolete Python runtime orchestration.

## Files Read

- `docs/TASKS_JOBS.md`
- `Makefile`
- `tools/samosa_jobs.py`
- `tools/samosa_gateway.py`
- `src/samosa_gateway.c`
- `src/json.h`
- `tests/jobs/test_run_job.py`
- `tests/test_gateway_web.py`
- `tests/test_gateway_jobs.py`
- `tests/test_compiled_gateway.sh`

## Existing Prototype Behavior To Port

Python scheduler behavior in `tools/samosa_jobs.py`:

- `arm_scheduled_job()`
- `scheduler_decision()`
- `record_missed_window()`
- `host_power_status()`
- `launchd_plist()`
- `install_launchd_plist()`
- `arm_overnight_job()`
- `list_armed_schedules()`
- `jobsd_once()`
- `run_scheduled_job()`

Python public input behavior in `tools/samosa_gateway.py`:

- public HTTP(S)-only fetch boundary;
- DNS/redirect SSRF checks;
- robots.txt checks;
- byte/text limits;
- per-host rate limiting;
- `readable_page()`;
- `update_job_public_inputs(job_id, urls)`;
- persisted state under `<jobs_root>/<job_id>/public/`;
- unchanged pages produce no changed item;
- changed pages produce exactly one new text unit.

## Code Changed So Far

Only `src/samosa_gateway.c` has been modified so far. The work is partial and
uncommitted.

Added includes:

- `netdb.h`
- `arpa/inet.h`
- `netinet/in.h`

Added constants:

- `MAX_PUBLIC_JOB_URLS`
- `MAX_PUBLIC_FETCH_BYTES`
- `MAX_PUBLIC_TEXT_BYTES`

Added helper functions:

- `stable_hash_bytes()`
- `text_hash_hex()`
- `path_basename_const()`
- `valid_job_id()`
- `slugify_to()`
- `rfc3339_now_to()`
- `parse_hhmm()`
- `minutes_in_window()`
- `current_minutes_local()`
- `host_on_battery()`
- `schedule_decision()`
- `write_schedule_with_status()`

Added scheduler functions:

- `jobs_schedule_arm()`
- `append_job_event_file()`
- `type_folder_for()`
- `run_scheduled_job_native()`
- `jobsd_once_native()`
- `jobs_launchd_plist()`

## Current Limitations Of The Partial C Work

The new functions are not wired into `gateway_handler()` yet.

No public URL native C implementation has been added yet.

No Makefile target has been added for `samosa-jobsd`.

No compiled tests have been updated yet.

Nothing has been built or run after the edits.

The current `run_scheduled_job_native()` is deliberately small:

- supports report jobs by running `samosa-fs survey`;
- supports extension/type organize jobs by running `samosa-fs list` and
  `samosa-fs move`;
- writes `events.jsonl`;
- updates `schedule.json` to complete/failed;
- does not yet run model-backed extraction definitions;
- does not yet queue model-required records beyond the basic scheduler event
  shape.

## Immediate Next Steps

1. Compile immediately to catch C issues introduced by the partial edit:

   ```sh
   make samosa-gateway
   ```

2. Wire native scheduler routes in `gateway_handler()`:

   - `POST /v1/jobs/schedule/arm` -> `jobs_schedule_arm()`
   - `POST /v1/jobsd/once` -> `jobsd_once_native()`
   - `GET /v1/jobs/launchd-plist` -> `jobs_launchd_plist()`

3. Add a `samosa-jobsd` compiled target.

   The cleanest first pass is probably to build the same source with a macro
   that skips backend startup and runs `jobsd_once_native()` from `main()` when
   invoked as `jobsd-once`.

4. Add native public URL input functions to `src/samosa_gateway.c`.

   Port the Python behavior conservatively. Do not use `curl` blindly; preserve
   DNS and redirect checks. The native version needs at least:

   - parse and validate HTTP(S) URLs;
   - reject non-standard ports;
   - resolve host and block private/local/special networks;
   - enforce redirect checks on every hop;
   - robots.txt gate;
   - byte/time limits;
   - HTML-to-readable-text extraction good enough for tests;
   - `<jobs_root>/<job_id>/public/state.json`;
   - item text files under `<jobs_root>/<job_id>/public/items/`;
   - changed/unchanged/new status response.

5. Wire native public input route:

   - `POST /v1/jobs/public-inputs/update`

6. Extend `tests/test_compiled_gateway.sh` while `PATH` excludes `python3`.

   Add checks for:

   - schedule arm succeeds;
   - same definition arm is idempotent;
   - changed same `job_id` is rejected;
   - one-shot scheduler runs inside a cross-midnight window;
   - battery policy defers when `on_battery=true`;
   - public input first fetch returns `changed:1`;
   - repeated unchanged fetch returns `changed:0`.

7. Run focused tests:

   ```sh
   make compiled-gateway-test
   make jobs-test
   ```

## Important C Notes

- The repo uses `src/json.h`, a small permissive parser. It keeps strings in
  allocations owned by the parsed tree, so do not keep `jval->str` pointers
  after `json_free()`.
- `write_small_file()` is the existing atomic write helper; use it for state.
- `job_state_path()` validates job IDs and roots all job state under
  `g->jobs_root`.
- The compiled test intentionally removes `python3` from `PATH`; new acceptance
  paths must not call Python.
- `samosa-fs` has no plan command. For native scheduled organize, either keep
  the current C planning loop or add a sidecar plan command intentionally.

## Current Git State At Handoff

At the time this file was written, `git status --short` showed:

```text
 M src/samosa_gateway.c
```

This handoff file itself will add:

```text
?? docs/regressions/jobs/native-scheduler-public-url-handoff-2026-07-21.md
```

## Continuation — 2026-07-21 (native scheduler wired + tested)

This picks up directly from the checkpoint above and completes handoff steps
1–3, 6, and 7 (the scheduler half). The public-URL fetch pipeline (steps 4–5)
is deliberately **not** started here; it is the next milestone.

### What landed

- **Fixed the build.** The checkpointed helpers did not compile — `jobsd_once_
  native()` uses `opendir`/`readdir`/`struct dirent`/`closedir` but `dirent.h`
  was not included. Added `#include <dirent.h>`. `make samosa-gateway` was
  verified to fail before the include and pass after.
- **Wired three native routes** into `gateway_handler()`:
  - `POST /v1/jobs/schedule/arm` → `jobs_schedule_arm()`
  - `POST /v1/jobsd/once` → `jobsd_once_native()`
  - `GET  /v1/jobs/launchd-plist` → `jobs_launchd_plist()`
- **Added the `samosa-jobsd` one-shot.** `main()` now takes `argc/argv`; invoked
  as `samosa-jobsd jobsd-once` (or `samosa-gateway jobsd-once`) it loads config,
  runs `jobsd_once_native(g, -1, NULL)` once, prints the decisions JSON, and
  exits — **no backend start, no listener bind**. New `samosa-jobsd` Makefile
  target builds the same source under the launchd-friendly name.
- **Extended `tests/test_compiled_gateway.sh`** (runs with `python3` removed from
  `PATH`). New assertions: arm succeeds; identical re-arm is idempotent; a
  changed definition under the same `job_id` is rejected with
  `code:"schedule_definition_changed"`; a cross-midnight window (22:00–06:00)
  defers outside the window, defers inside-but-on-battery, runs inside-on-AC to
  `scheduled_job_complete`, and does **not** re-run once finished; the launchd
  plist references `samosa-jobsd` + `jobsd-once`; and the **standalone compiled
  daemon** runs an armed 24h/`run_on_battery` job with `python3` unavailable,
  writing `events.jsonl`. `compiled-gateway-test` now also builds+passes
  `samosa-jobsd`.

### Evidence

```text
$ make samosa-gateway            # before dirent.h: 13 errors (incomplete struct dirent, closedir)
$ make samosa-gateway samosa-jobsd   # after: exit 0
$ make compiled-gateway-test     # exit 0 → "compiled gateway without python: PASS"
```

Manual end-to-end of the standalone binary (report job and organize/move job)
confirmed `scheduled_job_start`/`scheduled_job_complete` events, `schedule.json`
marked `complete`+`enabled:false`, `applied.jsonl` recording moves, and a second
poll deferring (idempotent one-shot).

### Acceptance gates (from TASKS_JOBS "Native Jobs completion estimate")

- **Gate 1** (compiled `samosa-jobsd`, runs with python unavailable) — **met and
  tested.**
- **Gate 2** (idempotent arm; changed definition rejected) — **met and tested.**
- **Gate 3** (cross-midnight windows) — window logic **met and tested**; the
  **missed-window `run_next_start` policy is NOT wired** (see below).
- **Gate 4** (battery/AC policy before work starts) — **met and tested** via the
  `on_battery` override; `caffeinate`/keep-awake for a run's lifetime is **not**
  implemented.
- **Gate 5** (review-required queued, daemon never blocks on a question) — the
  scheduled runner is non-interactive by construction; no review-queue path is
  exercised yet.
- Gates 6–11 (Kill of a daemon run, public URLs/SSRF/robots, changed-page units,
  installed-release integration, Python removal) — **not started.**

### Honest remaining work (do not overstate)

1. **Missed-window policy is dead code.** `schedule_decision()` will honor
   `run_next_start` only if a `missed` **boolean** is true, but nothing ever sets
   it — `jobsd_once_native()` writes `last_status:"missed"` instead. Wire these
   together (or drive the decision off `last_status`) before claiming Gate 3's
   missed-window half. Not done here to avoid shipping half-correct policy.
2. **launchd lifecycle** — only the plist *generator* exists. Install/uninstall/
   status, log paths, and `caffeinate` keep-awake are unimplemented.
3. **Kill for a scheduled run** — `/v1/kill` stops interactive sidecars; it does
   not yet own a running daemon job + its sidecars + keep-awake (Gate 6).
4. **Native public-URL pipeline (handoff steps 4–5)** — untouched. The
   `MAX_PUBLIC_*` constants and `netdb.h`/`arpa/inet.h` includes are staged but
   no fetch/SSRF/robots/change-state code exists. This is the largest remaining
   chunk and the one with real security surface.
5. **Real acceptance** — no sleep/wake missed-window test, no real public-page
   change check, no installed-release run. Offline tests passing is not "works".

## Continuation — 2026-07-21 (scheduler policy + native public-URL pipeline)

This session closed the remaining native gaps except the two that require the
real world (live-network acceptance) or the owner (deleting the Python modules).
All of it builds `-Werror` clean and is exercised by `make compiled-gateway-test`
(exit 0, with `python3` removed from `PATH`); the whole surface was also run
under ASan/UBSan with **zero** errors.

### What landed

- **Missed-window policy is now real (was dead code).** `arm` records a
  `deadline_epoch` (first wall-clock instant at/after arm whose local time equals
  the window end, DST-correct via `mktime`). `schedule_decision()` takes a
  `window_expired` signal: inside the window → run; expired + `run_next_start` →
  `missed_window` run (catch-up after the laptop slept through the window);
  expired + `skip` → `window_expired` and the schedule is retired
  (`enabled:false`). Deterministically tested with a `now_epoch` override.
- **Keep-awake.** A scheduled run spawns `/usr/bin/caffeinate -s` for its
  lifetime (macOS; no-op elsewhere), tracked so a Kill releases it. Off by
  `keep_awake:false`.
- **Kill covers scheduled runs.** All scheduled children — sidecars, curl,
  caffeinate — go through `run_capture`/`spawn_tracked`, so `/v1/kill`'s existing
  `jobs_stop()` tears them down. The standalone `samosa-jobsd` installs a
  SIGINT/SIGTERM handler that SIGKILLs tracked children and exits.
- **launchd lifecycle.** `POST /v1/jobs/launchd/install` writes the plist to
  `~/Library/LaunchAgents` (overridable) + creates the log dir, then
  `launchctl load`; `.../uninstall` unloads + removes; `GET .../status` reports
  installed/loaded. A dry-run mode (`SAMOSA_LAUNCHD_DRYRUN`, and always off
  macOS) manages the plist file but never touches a real launchd domain — the
  suite uses it so it never installs an agent on the dev machine.
- **Native public-URL fetch pipeline (the big one).** `/v1/jobs/public-inputs/
  update`. Per hop: parse+validate (http/https only, standard ports only, no
  credentials), robots gate, per-host rate limit, resolve the host and reject if
  **any** resolved address is private/loopback/link-local/CGNAT/transition
  (IPv4+IPv6, strict against rebinding), then `curl --resolve host:port:ip
  --max-redirs 0` pinned to that validated IP, with redirects followed manually
  and re-validated. HTML→text drops script/style/svg/noscript/template, decodes
  common entities, extracts `<title>`. Verified offline that
  `127.0.0.1`, `169.254.169.254`, `10.0.0.5`, and `[::1]` are all blocked at the
  resolver, and that bad scheme/port/credentials are rejected at parse.
- **Change-state.** `<job>/public/state.json` + `items/`. New/changed page → one
  item text+meta file and a `changed` record; unchanged → nothing. State is
  written atomically and preserves pages from earlier runs. (Change digest is
  FNV-1a, not SHA-256 — no crypto dependency; it is a change detector, not a
  security primitive.)
- **Comparison workflow.** A scheduled job with a `public_inputs` array runs the
  fetch + change-state deterministically and emits a `scheduled_job_complete`
  event carrying `checked`/`changed`; the local folder and changed items are left
  on disk for the model comparison step (which reuses the existing definition
  model path — not re-implemented here).

### Test seam (honest note)

`SAMOSA_WEB_STUB_DIR` replaces the network transport with local files keyed by a
slug of the URL, and lets robots.txt be served locally. It exists so the change-
state / robots / HTML-extraction contract is testable offline. URL parse
validation still runs in stub mode; only the resolver+curl are replaced, and the
seam is inert unless the env var is set. The SSRF resolver/blocklist is therefore
tested on the **no-stub** gateway (literal blocked IPs resolve without network).

### Acceptance gates — status now

Gates 1–4, 6–9 are **met and tested offline**. Gate 5 (review-required queued):
the scheduled runner is non-interactive by construction and never blocks on a
question; a dedicated review-queue for model-backed scheduled extraction is not
yet exercised. **Gate 10's real-fetch check PASSED on the built binary
(2026-07-22, with owner sign-off)** — `new`/`unchanged`/`changed` all verified
against live sites (`example.com`, `cloudflare.com/cdn-cgi/trace`), SSRF
allow+block both exercised on real DNS, robots honored, real failures
(`httpbin` 503/timeout) handled without corrupting state; evidence in
[`e-gate10-real-fetch-2026-07-22.md`](e-gate10-real-fetch-2026-07-22.md). **Still
open in Gate 10:** it ran against `build/samosa-gateway`, not a re-installed
`~/.samosa` release (the installed binary predates the pipeline); re-packaging and
installing the new binary overwrites the installed release and waits for owner
confirmation. **Gate 11 (removing `tools/samosa_gateway.py`
/ `tools/samosa_jobs.py`) is deliberately NOT done**: it is owner-gated on gate
10, and those modules still back the green `make jobs-test` / `make test` Python
suites. Do not delete them until gate 10 passes and the Python tests are migrated
or retired with the owner.

### Known limitations left in code (not defects, but scoped)

- `url_join` handles absolute and root-relative redirects fully and other
  relative forms approximately (origin + base-dir + loc); most real redirects are
  absolute.
- The robots parser is a conservative subset (agent-token or `*` group,
  longest-match Allow beats Disallow); errors/non-text robots → allowed, matching
  the reference.
- IPv6 blocklist covers the dangerous ranges explicitly rather than replicating
  Python's full `is_global`; normal global unicast is allowed, internal/transition
  ranges are blocked.
