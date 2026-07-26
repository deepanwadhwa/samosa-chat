# launchd real load + fire — 2026-07-22

Closes the stated gap in
[`native-scheduler-public-url-handoff-2026-07-21.md`](native-scheduler-public-url-handoff-2026-07-21.md):
the launchd lifecycle had only ever been exercised in **dry-run**
(`SAMOSA_LAUNCHD_DRYRUN=1`, temp `LaunchAgents` dir) so the suite never touched a
real launchd domain. This records the first **real** `launchctl load` of the
shipped plist on the reference machine, and confirms the loaded agent actually
fires the compiled daemon and does real work.

## Environment

- macOS arm64, Darwin 25.5.0, 16 GiB M3 Air (the reference machine).
- Installed release under test: `~/.samosa/current -> releases/dev-a24a14f99624`,
  program `~/.samosa/current/bin/samosa-jobsd` (the binary launchd invokes).
- No `com.samosa.jobsd` agent was installed beforehand; no `schedule.json`
  existed anywhere under `~/.samosa/jobs`, so a real load could only fire the one
  test schedule (checked before loading).

## Method

1. **Armed faithfully.** A throwaway gateway (fake OpenAI backend, isolated
   `SAMOSA_HOME`) armed an always-eligible **report** job via the real
   `POST /v1/jobs/schedule/arm` route: `job_id=launchd-firetest`,
   `window 00:00–00:00` (24 h), `run_on_battery:true` (time/power independent),
   folder = a two-file scratch dir. Report jobs run `samosa-fs survey` only — **no
   model**, SSD-light, safe to fire.
2. Relocated the resulting `schedule.json` + `job.json` into
   `~/.samosa/jobs/launchd-firetest/` (the daemon loads `job.json` via the
   schedule's `job_path`; it does not re-check `job_sha256` at run time —
   `run_scheduled_job_native`, `src/samosa_gateway.c`).
3. Wrote the **exact** shipped plist (byte-for-byte from `launchd_plist_build`,
   `home=~/.samosa`) to `~/Library/LaunchAgents/com.samosa.jobsd.plist` and ran
   the same verbs the gateway's non-dry install path runs:
   `launchctl unload` (clear) → `launchctl load -w` → `launchctl list`.

The `jobs_launchd_install` C wrapper itself is dry-run-tested; the OS-level piece
it defers to — `/bin/launchctl load -w <plist>` accepting the plist and launchd
spawning the program — is what this run exercises for the first time.

## Result — PASS

```
--- launchctl load -w ---            load exit: 0
--- launchctl list com.samosa.jobsd ---
{
    "LimitLoadToSessionType" = "Aqua";
    "Label" = "com.samosa.jobsd";
    "OnDemand" = true;
    "LastExitStatus" = 0;
    "PID" = 11869;
    "Program" = "/Users/deepanwadhwa/.samosa/current/bin/samosa-jobsd";
    "ProgramArguments" = ( ".../samosa-jobsd"; "jobsd-once"; );
    "StandardOutPath" = "/Users/deepanwadhwa/.samosa/logs/jobsd.out.log";
    "StandardErrorPath" = "/Users/deepanwadhwa/.samosa/logs/jobsd.err.log";
};
list exit: 0
```

RunAtLoad fired the daemon within ~1 s. `~/.samosa/jobs/launchd-firetest/events.jsonl`:

```
{"seq":1,"ts":"2026-07-22T14:38:05Z","type":"scheduled_job_start","job_id":"launchd-firetest","job_path":".../launchd-firetest/job.json"}
{"seq":2,"ts":"2026-07-22T14:38:05Z","type":"scheduled_job_complete","kind":"report"}
```

`schedule.json` after the run: `"enabled":false, "last_status":"complete",
"last_reason":"complete"` — the one-shot retired itself, so subsequent
StartInterval fires would defer (idempotent). No stray `caffeinate -s` remained.

Cleanup: `launchctl unload -w` (exit 0), plist removed, `launchctl list
com.samosa.jobsd` then reports not loaded, test job dir removed. Machine left
clean.

## Honest scope — what this does and does not prove

- **Proven:** real `launchctl load -w` of the shipped plist succeeds in the user
  Aqua GUI domain on Darwin 25.5; launchd registers the agent and RunAtLoad
  immediately spawns the installed `samosa-jobsd jobsd-once`; the daemon polls
  `~/.samosa/jobs`, runs an eligible report schedule to completion, marks it
  complete, exits 0; unload/remove is clean.
- **Not proven here:** the literal *overnight* sleep/wake cycle — that macOS,
  after the lid is closed for hours, wakes or catches up to fire a `StartInterval`
  tick or a missed window. launchd coalesces timer fires across sleep and runs
  them on wake; that real sleep/wake + missed-window behavior is still the
  outstanding "End-to-end hardening" check and is **not** claimed from this run.
- The daemon's decisions JSON (its stdout echo) was empty in
  `jobsd.out.log` when read immediately after the job's terminal event — most
  likely the process had written `events.jsonl` but not yet flushed stdio / exited
  when the log was read (the compiled suite already captures that stdout when the
  binary is run directly). `events.jsonl` is the authoritative record and is
  correct.
