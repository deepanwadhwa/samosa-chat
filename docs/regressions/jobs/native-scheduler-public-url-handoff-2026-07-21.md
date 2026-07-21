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
