# Find to act staged apply — 2026-07-21

Scope: find jobs may request a file move, but mutating tools stay behind the
existing Apply boundary in confirm mode.

Implementation shape:

```text
model -> fs_list / fs_read_* -> fs_move
confirm mode: fs_move becomes plan.jsonl + await_apply
/v1/jobs/apply: executes the staged move
/v1/jobs/undo: restores it
```

The normal preview boundary is unchanged for other tool loops; staging is
opt-in through the Jobs runner.

Verification:

```sh
python3 tests/test_gateway_jobs_find_move.py
```

```text
test_gateway_jobs_find_move: OK
```

```sh
make jobs-test
```

```text
Ran 69 tests in 0.282s
OK (skipped=1)
test_gateway_jobs: OK
test_gateway_jobs_answer: OK
test_gateway_jobs_find: OK
test_gateway_jobs_find_move: OK
test_gateway_jobs_model_call: OK
test_gateway_chat_tools: OK
```

```sh
make
```

```text
cc -O3 -Wno-unused-function -pthread src/qwen36b.c src/expert_cache.c src/vision.c -o qwen36b -lm
```

Result: passed. A scripted fake model can find a file, request `fs_move`, pause
at `await_apply`, then move and undo through the gateway routes.
