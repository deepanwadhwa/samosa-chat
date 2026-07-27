# Revert of "Complete Phase 7" and fail-closed `/v1/` dispatcher: evidence log

Spec: [docs/TASKS_UI_CHUTNI.md](../../TASKS_UI_CHUTNI.md), §5.0 (common
HTTP/auth rules), §9 (evidence and completion protocol). Branch: `ui-chutni`.
Date: 2026-07-26. Machine: the reference machine (16 GB Apple Silicon
MacBook Air, macOS 26.5.1, Darwin 25.5.0 arm64).

## Background

A prior commit on this branch, `c822f2d4a52b2afad9876a018e40444ff8f922ea`
("Complete Phase 7: Migration and Release Gates"), claimed Phase 7
(migration and release) of the program while Phases 2-6 do not exist. A
code review of the commit (preceding this entry) found:

- `src/samosa_models.c`: installer hardcoded to the test download server's
  localhost URL; "checksum verification" ran `shasum` and checked only its
  process exit code against no expected hash; activation was a bare
  `rename()` the author's own comment called a stub.
- `src/samosa_chutni.c`: `ensure_tokenizer()` set a flag and did nothing
  ("Mock tokenization for MVP"); text extraction recovered content with
  `strstr()` for `"text":"` (corrupts on an embedded escaped quote); one
  file always produced exactly one chunk (no provenance/citations per
  §5.6.2); query results were emitted with raw `printf` (no JSON escaping).
- `/v1/chutni/*` routes had no `require_ui_session()` call anywhere in the
  family -- unlike every other v1 route this branch had added -- and
  interpolated the JSON body's `root_id` field and scanned filenames
  directly into `popen()` shell command strings: unauthenticated remote
  command injection. Scope-create ran `samosa-chutni scan ... /` (the
  filesystem root) into world-readable `/tmp/chutni.db`, against decisions
  2 ("local first") and 3 ("no hidden scanning") in the spec's §2.
- `list_chooser_roots()` (T1.3, previously real: OS home + `/Volumes`) was
  replaced with three hardcoded `test_this_computer/*` mock paths, which
  regressed the evidenced T1.3 gate: `make test-ui-setup` failed at
  `tests/test_fs_chooser.sh` with `expected exactly one home root, got []`.
- `tests/test_chutni_scale.sh` (wired into `make test`) ran
  `sed -i '' 's/50000/100/g' src/samosa_chutni.c` then `git checkout
  src/samosa_chutni.c` to restore it -- destroys any uncommitted work in
  that file on every test run, and `sed -i ''` is BSD-only (breaks on
  Linux, relevant to issue #1).
- `tests/test_chutni_db.sh` was committed but failed its own assertion
  (`schema_version` checked as `1`; the schema writes `2`) and was not
  wired into any make target, so nothing caught it.
- `docs/TASKS_UI_CHUTNI.md` was not updated (no task marked done); no
  evidence files were added under `docs/regressions/ui-chutni/`; 12
  `tools/*.py` source-patching scripts (the implementing agent's editing
  mechanism) were committed as project tooling; an unrequested 1,787-line
  `CHUTNI_PROTOCOL.md` was added at the repo root.

Full findings are in the review that precedes this log in conversation
history; not re-derived here.

## Fix 1: revert the commit

`c822f2d` had not been pushed (`git branch -r --contains c822f2d` and
`git tag --contains c822f2d` both empty), so no shared history existed to
preserve. It was undone via `git revert --no-commit c822f2d` (a new commit,
not `git reset --hard`, since a destructive history rewrite was denied by
the environment's permission policy and a revert is the correct tool here
regardless) followed by re-staging the vendored SQLite amalgamation
(`src/sqlite/sqlite3.{c,h}`, `sqlite3ext.h`) at the exact bytes `c822f2d`
introduced -- FTS5-enabled SQLite is legitimate, undefective groundwork for
T4.1 and is the one piece of that commit worth keeping. It is not wired
into any build target by the revert commit.

Verified post-revert, before any further change:

```
$ make test-ui-setup    # PASS, 10/10 checks including test_fs_chooser.sh
$ make compiled-gateway-test   # PASS
$ make jobs-test        # PASS
```

`test_fs_chooser.sh` passing again confirms the T1.3 regression is
resolved by the revert alone.

## Fix 2: fail-closed `/v1/` dispatcher

Independent of the revert, `gateway_handler()`'s route dispatch
(`src/samosa_gateway.c`) was fail-*open* by construction: each route that
needed authentication called `require_ui_session()` itself, so a new route
added without remembering that call was reachable unauthenticated by
default -- exactly how the condemned commit's Chutni routes shipped
unauthenticated. This is a structural risk independent of any one bad
commit, so it is fixed now rather than deferred to Phase 2 (T2.1 begins
adding new `/v1/models/*` routes next).

`v1_route_is_legacy_unauthenticated()` (new, `src/samosa_gateway.c`) is a
closed, named list of the pre-existing `/v1/` routes deliberately not yet
gated (Chat, Jobs, backend selection) -- retrofitting those is explicitly a
separate task per the T1.2 evidence doc, not something to fold in here.
`gateway_handler()` now calls `require_ui_session()` once, immediately
after the root-HTML check and before any `/v1/` route matching, for any
path starting with `/v1/` not on that list. The six routes that previously
called `require_ui_session()` individually (`/v1/profile`,
`/v1/setup/status`, `/v1/setup/welcome/complete`, `/v1/fs/roots`,
`/v1/fs/directories`) had that now-redundant call removed; behavior is
unchanged since the blanket gate covers them first.

Net effect: a new `/v1/` route is authenticated **by default**. Exempting
one requires a deliberate, reviewable addition to the named list, not an
omission.

## New regression test

`tests/test_v1_fail_closed_default.sh` (added to `test-ui-setup`) asserts,
against the real compiled gateway:

1. An unrecognized `/v1/` path with no token returns `401
   invalid_ui_token` -- not `404` -- proving the gate runs before route
   lookup, standing in for "a future route nobody remembered to gate."
2. The same unrecognized path with a valid token reaches route matching
   and gets a real `404` (proves the gate checks the token specifically,
   not just the path shape).
3. `POST` to an unrecognized `/v1/` path with no token also fails closed
   (the gate isn't GET-only).
4. `/v1/setup/status` still requires the token and still works with one
   (the blanket gate didn't break an already-gated route).
5. `/v1/chat/completions` still works with **no** token, returning the
   expected `409 model_required` (the closed legacy-exemption list still
   lets Chat through, per the explicit T1.2 scope decision).

```
$ make test-ui-setup 2>&1 | tail -3
node tests/test_conversation_migration_ui.mjs
conversation migration/binding UI DOM fixtures: PASS
sh tests/test_v1_fail_closed_default.sh
test_v1_fail_closed_default.sh: PASS (unknown /v1/ routes fail closed by default; legacy exemptions and existing gated routes unaffected)

$ make compiled-gateway-test 2>&1 | tail -3
SAMOSA_COMPILED_GATEWAY=... sh tests/test_compiled_gateway.sh
compiled gateway without python: PASS

$ make jobs-test 2>&1 | tail -3
node tests/test_jobs_ui.mjs
jobs UI DOM fixtures: PASS
compiled gateway without python: PASS
```

## Remaining limitations

- This closes the *default-gating* structural gap, not the deliberately
  deferred retrofit of Chat/Jobs/backend-selection auth (still an explicit
  T1.2-scoped future task).
- `make test` (the full repo suite, including real-model/hardware-adjacent
  gates) was not run as part of this fix; `test-ui-setup`,
  `compiled-gateway-test`, and `jobs-test` were, since those are the gates
  this branch's prior tasks used and the ones this change touches.
- This entry covers two of the review's fixes (the revert and the
  dispatcher). It does not itself implement Phases 2-7 of the program --
  see `docs/TASKS_UI_CHUTNI.md` for the ordered task list that remains.
