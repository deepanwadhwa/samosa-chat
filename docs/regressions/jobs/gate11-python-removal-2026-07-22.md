# Gate 11 — Python jobs modules removed, tests consolidated (2026-07-22)

Gate 11 of the "Native Jobs completion estimate" (docs/TASKS_JOBS.md). Gates
1–10 had passed (Gate 10 closed on the installed release, commits `a24a14f`,
`ec5f3e3`), so the superseded Python orchestration was removed. Owner chose the
**"retire duplicates, note gaps"** path.

## Preconditions verified on this checkout

- `make jobs-test` exit 0 (Python path, before removal).
- `make compiled-gateway-test` exit 0 (compiled path, `python3` removed from PATH).
- The release ships the **C** gateway: `dist/install.sh` compiles
  `engine/samosa_gateway.c` → `bin/samosa-gateway`; `tools/package_hf.py` bundles
  `samosa_gateway.c`. No `tools/*.py` is in the shipped runtime
  (`tests/test_gateway_installer.sh` already asserts `samosa_jobs.py` absent from
  `current/bin`). **Deleting the Python modules therefore changes only the test
  tree, not the product.**

## Removed

Superseded Python runtime (not shipped; native C parity proven):

- `tools/samosa_jobs.py` — Python J1 runner
- `tools/samosa_gateway.py` — Python gateway (imported the other three)
- `tools/samosa_tools.py` — Python tool/permission layer
- `tools/jobs_fs.py` — Python filesystem core

Duplicate Python tests (their subjects are gone and/or the behavior is covered
against the compiled binaries):

| Retired test | Was testing | Now covered by |
|---|---|---|
| `tests/test_gateway_jobs.py` | Py gateway run/report/find | `test_compiled_gateway.sh` (`/v1/jobs/run` report + find) |
| `tests/test_gateway_jobs_answer.py` | Py await_user/answer | `test_compiled_gateway.sh` (await_user → `/v1/jobs/answer`) |
| `tests/test_gateway_jobs_find.py` | Py find loop | `test_compiled_gateway.sh` (find → `fs_read_text`) |
| `tests/test_gateway_jobs_find_move.py` | Py find→move/apply/undo | `test_compiled_gateway.sh` (await_apply → apply → undo) |
| `tests/test_gateway_jobs_definition.py` | Py definition preview/run | `test_compiled_gateway.sh` (definition preview/preview3/run) |
| `tests/test_gateway_jobs_model_call.py` | Py bounded model call | `test_compiled_gateway.sh` (definition run path) |
| `tests/test_gateway_chat_tools.py` | Py tool dispatch | `test_compiled_gateway.sh` (find loop tool calls) |
| `tests/test_gateway_web.py` | Py static app + Brave-search URL | new app/logo asserts in `test_compiled_gateway.sh`; Brave search is a non-goal (HR: no search engines) and was Python-only |
| `tests/jobs/test_run_job.py` | Py J1 runner internals | see **Gaps** below |
| `tests/jobs/test_tools.py` | Py tool layer | superseded by native tools (find loop in compiled test) |
| `tests/jobs/test_jobs_fs.py` | Py `jobs_fs` **and** the `samosa-fs` binary | replaced by `tests/jobs/test_samosa_fs.py` |

## Added / changed

- **`tests/jobs/test_samosa_fs.py`** (new) — direct CLI coverage of the shipped
  `samosa-fs` sidecar (magic-byte typing, UTF-8 fallback, content dedup,
  O_NOFOLLOW symlink rejection, metadata-only oversized-read cap, per-file
  metadata), asserting the binary's own observed output. It honors `$SAMOSA_FS`
  (default `build/samosa-fs`). Note: the old `test_jobs_fs.py::TestSamosaFsSidecar`
  hard-coded a `<repo>/samosa-fs` path that does not exist and so **silently
  skipped** under `make jobs-test`; this replacement actually runs.
- **`tests/test_compiled_gateway.sh`** — added a static app-page (`GET /` →
  "Compiled Samosa") and logo (`/assets/samosa-chat.png` → 200) assertion, moving
  the only unique coverage from the retired `test_gateway_web.py` onto the
  shipped gateway.
- **`Makefile`** — `jobs-test` no longer depends on the Python modules; it runs
  the `samosa-fs` CLI test and `compiled-gateway-test`. `make test` drops
  `test_gateway_web.py`.
- Docs: `docs/TASKS_JOBS.md` (Gate 11 marked done; actor-split runner now the
  compiled gateway), `docs/SIDECAR_CONTRACT.md` (watchdog pattern now cites the C
  caller), `tests/jobs/__init__.py`.

## Verification

```
$ make jobs-test    # samosa-fs CLI (4 tests OK) + compiled gateway PASS
jobs-test exit: 0
$ make test         # full suite, test_gateway_web.py removed
make test exit: 0
```

## Gaps recorded honestly (the "note gaps" half of the decision)

The **fine-grained J1 unit contract** that `tests/jobs/test_run_job.py` encoded
against the Python runner is now covered only at **integration level** by
`test_compiled_gateway.sh`, not as isolated unit assertions. Specifically, these
J1.x edge cases no longer have a dedicated test:

- **J1.2 planner granularity** — the exact `auto` decisions (single_image;
  fits_budget vs over_context; `image_pages ≥ 2` → per-page; forced-file
  multi-image warning; line-boundary chunking with overlap). The compiled path
  bounds reads to 1–5 pages structurally but does not re-assert every branch.
- **J1.5 output-validator edge cases** — `bool`-is-not-`int`, JSON-typed enum
  (`True != 1`), `trailing_prose` warning vs `unparseable` error, the
  string-aware brace scanner.
- **J1.7 recovery/replay** — torn-line tolerance, orphaned-artifact recovery
  (rename-succeeded/event-missing), processed-set idempotency.
- **J1.9 deterministic reducer** — scalar merge, `reduce_conflict`, missing-page
  review semantics.

If the C reimplementation of any of these is to be guarded at unit granularity,
that is a **follow-up** (new C-level tests), deliberately deferred here per the
owner's "retire duplicates, note gaps" choice. The behaviors themselves ship in
`src/samosa_gateway.c` and are exercised end-to-end by the compiled suite.

**Follow-up applied after Gate 11:** `tools/run_e_j1.py` now drives the compiled
gateway directly (`/v1/jobs/definition/preview` and
`/v1/jobs/definition/run`) and saves the streamed SSE events as run evidence.
It no longer shells out to the deleted `dist/samosa_jobs.py` path.
