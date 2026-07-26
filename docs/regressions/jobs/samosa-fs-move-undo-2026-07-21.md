# samosa-fs move/undo sidecar — 2026-07-21

Scope: deferred wave 2 starter slice, porting the filesystem mutation primitive
from in-process Python to the constrained `samosa-fs` sidecar. The Python tool
registry still owns preview/execute gating and calls the sidecar as a shim.

Implemented:

```text
samosa-fs move --root ROOT [--size N] [--mtime T] [--sha256 H] SRC DST
samosa-fs undo --root ROOT SRC DST
```

The sidecar revalidates regular-file source identity, rejects symlink sources,
checks size/mtime/hash when provided, creates destination parents inside the
jail, then uses a no-clobber hard-link + inode assertion + unlink move. Undo is
the same constrained operation in reverse.

Verification:

```sh
make samosa-fs
```

```text
cc -O2 -Wall -Wextra -Werror -std=c11 src/samosa_fs.c -o samosa-fs
```

```sh
python3 -m unittest tests.jobs.test_jobs_fs.TestMoveEngine tests.jobs.test_tools.TestFsTools -v
```

```text
Ran 14 tests in 0.035s
OK
```

```sh
make jobs-test
```

```text
Ran 67 tests in 0.291s
OK (skipped=1)
test_gateway_jobs: OK
test_gateway_jobs_answer: OK
test_gateway_jobs_find: OK
test_gateway_jobs_model_call: OK
test_gateway_chat_tools: OK
```

```sh
make
```

```text
cc -O3 -Wno-unused-function -pthread src/qwen36b.c src/expert_cache.c src/vision.c -o qwen36b -lm
```

```sh
sh tests/test_gateway_installer.sh
```

```text
gateway installer: PASS
```

Result: passed. Jobs apply/undo now exercise `samosa-fs` when the sidecar is
available, while retaining the old Python implementation as a compatibility
fallback.
