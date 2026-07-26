# samosa-fs containment check — 2026-07-21

Scope: T3 containment demo for the read-only `samosa-fs` metadata sidecar.
This is not a real-model Jobs claim.

Host: macOS Darwin arm64, local development checkout on `issue-7-jobs`.

## Baseline

Command:

```sh
make jobs-test
```

Result: passed.

Relevant output:

```text
Ran 60 tests in 0.481s

OK (skipped=1)
test_gateway_jobs: OK
test_gateway_chat_tools: OK (tool call dispatched through samosa_tools.execute_tool, round-trip completed)
```

## Huge-file metadata scan

Command:

```sh
tmp=$(mktemp -d)
truncate -s 1073741824 "$tmp/huge.bin"
printf 'demo_dir=%s\n' "$tmp"
/usr/bin/time -l ./samosa-fs list --max-file-bytes 1048576 "$tmp"
```

Output:

```text
demo_dir=/var/folders/d6/7z7gzbj16l59s8tn7h4mg8lm0000gn/T/tmp.HRxGDSWW4l
{"ok":true,"items":[{"path":"/var/folders/d6/7z7gzbj16l59s8tn7h4mg8lm0000gn/T/tmp.HRxGDSWW4l/huge.bin","name":"huge.bin","media_type":"application/octet-stream","input_sha256":"0e8e2a515c340018bdfcef69774c0e4dd5015021f2ee852c65256bd7a9f834c7","size":1073741824,"mtime":1784655175.000000000}],"skipped":[]}
        0.00 real         0.00 user         0.00 sys
             2424832  maximum resident set size
                   0  average shared memory size
                   0  average unshared data size
                   0  average unshared stack size
                 300  page reclaims
                   1  page faults
                   0  swaps
                   0  block input operations
                   0  block output operations
                   0  messages sent
                   0  messages received
                   0  signals received
                   0  voluntary context switches
                   2  involuntary context switches
            62968051  instructions retired
            15867401  cycles elapsed
             1999160  peak memory footprint
```

Result: the sidecar reported the real 1 GiB file size while staying bounded by
the 1 MiB scan cap. macOS `/usr/bin/time -l` reported 2,424,832 bytes maximum
resident set size and 1,999,160 bytes peak memory footprint.
