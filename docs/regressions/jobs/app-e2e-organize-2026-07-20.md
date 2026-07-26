# Jobs in the app — end-to-end organize/undo (2026-07-20)

Proof that a job runs **through the app path** (the Python gateway serving
`app.html` + the `/v1/jobs/*` SSE routes the Jobs tab calls), decoding a
plain-English goal into a deterministic plan, executing it with a visible
per-action stream, and undoing it.

## Setup

- Branch `issue-7-jobs` at the Phase-5 commit.
- Real files copied into a repo-root scratch folder `jobs-demo-scratch/inbox/`
  (deleted after the run): `Makefile`, `engine.c` (src/qwen36b.c), `jobs_fs.py`,
  `mascot.png` (assets/samosa-chat.png), `models.md`, `readme.md` (README.md),
  `notes.txt`, `tok.json` (tokenizer_qwen36.json) — 8 files, mixed types.
- Gateway started **with no model backend** (organize-by-type is deterministic
  and needs none), on 127.0.0.1:8799 to avoid colliding with a running Samosa.
  Command: `tools/samosa_gateway.py` loaded via importlib, `GatewayServer`
  served directly (skips `supervisor.start()`, which requires an installed
  backend).

## 1) The app is served with the Jobs tab

```
$ curl -fsS http://127.0.0.1:8799/ | grep -oE 'id="(tabJobs|jobsView|jobForm|jobActivity)"' | sort -u
id="jobActivity"
id="jobForm"
id="jobsView"
id="tabJobs"
```

## 2) Run (confirm mode) — the live event stream the Jobs tab renders

```
POST /v1/jobs/run  {"goal":"organize this folder by type","folder":".../inbox","mode":"confirm"}

  • decoding intent: 'organize this folder by type'
  • intent: organize — Sort the files into folders named for their type (PDF, JPG, …
  • counting files: 8 found {'text/plain': 7, 'image/png': 1}
  • plan: 8 moves, 0 skips
  • AWAIT APPLY — job_id= organize-this-folder-by-type-1784580709  moves= 8
  (files still in place after confirm)  ->  Makefile engine.c jobs_fs.py mascot.png models.md notes.txt readme.md tok.json
```

Confirm mode plans but **moves nothing** until Apply.

## 3) Apply — each action streamed

```
POST /v1/jobs/apply  {"job_id":"organize-this-folder-by-type-1784580709"}

    [1/8] move Makefile   -> Organized/TEXT/Makefile   ok
    [2/8] move engine.c   -> Organized/C/engine.c      ok
    [3/8] move jobs_fs.py -> Organized/PY/jobs_fs.py    ok
    [4/8] move mascot.png -> Organized/PNG/mascot.png   ok
    [5/8] move models.md  -> Organized/MD/models.md     ok
    [6/8] move notes.txt  -> Organized/TXT/notes.txt    ok
    [7/8] move readme.md  -> Organized/MD/readme.md     ok
    [8/8] move tok.json   -> Organized/JSON/tok.json    ok
  → applied 8 skipped 0
  ✓ Moved 8 files.

on disk:
  Organized/C/engine.c   Organized/JSON/tok.json   Organized/MD/{models,readme}.md
  Organized/PNG/mascot.png   Organized/PY/jobs_fs.py   Organized/TEXT/Makefile   Organized/TXT/notes.txt
  top-level inbox now: Organized
```

Extensionless `Makefile` is typed by content (UTF-8 text → TEXT); everything else
by extension (.c→C, .py→PY, .png→PNG, .md→MD, .txt→TXT, .json→JSON).

## 4) Undo — restore

```
POST /v1/jobs/undo  {"job_id":"organize-this-folder-by-type-1784580709"}
  → reverted 8 skipped 0
  ✓ Undo complete: 8 restored, 0 skipped.

inbox after undo: Makefile engine.c jobs_fs.py mascot.png models.md notes.txt readme.md tok.json  (+ empty Organized/)
```

All 8 files returned to the top level; the (now empty) `Organized/` folders are
left behind (undo restores files, not directory creation).

## Automated suite

```
$ make jobs-test
Ran 42 tests in 0.035s
OK (skipped=1)          # 1 skipped: Pillow not installed (image downscale)
test_gateway_jobs: OK   # gateway /v1/jobs/* run→apply→undo integration
```

## Scope / honesty

- **Run through the real gateway HTTP + SSE path** the browser Jobs tab uses; not
  yet clicked in an actual browser window (the HTML/JS is syntax-checked and the
  element IDs verified served). The organize path is deterministic and used **no
  model**; the model-driven intent-decode for open-ended goals still needs a
  backend loaded and has not been exercised on the real 24 GB model.
- Scratch folder `jobs-demo-scratch/` was deleted after the run.
