# Find real-model checkpoint — 2026-07-21

Scope: T7 real-model checkpoint for Phase JF. This is a negative result:
the offline find plumbing works, but the real 24 GB model did not complete the
planted-file task within the run window.

Host: macOS Darwin arm64, local development checkout on `issue-7-jobs`.

Backend: real `qwen36b` serving Qwen3.6 35B A3B from
`~/Documents/samosa-models/qwen36_group32_i8`.

Gateway: `tools/samosa_gateway.py` on `127.0.0.1:8797`, driven through
`POST /v1/jobs/run` SSE, the same route used by the Jobs tab.

Fixture:

- Scratch folder:
  `/var/folders/d6/7z7gzbj16l59s8tn7h4mg8lm0000gn/T/samosa-find-t7-uio5_4c6/inbox`
- Planted file: `titli_vaccination_2025.pdf`
- Clutter: 30 text notes plus 10 placeholder PDF-looking files.

PDF extraction preflight:

```sh
DYLD_LIBRARY_PATH=dist ./samosa-extract --json \
  /var/folders/d6/7z7gzbj16l59s8tn7h4mg8lm0000gn/T/samosa-find-t7-uio5_4c6/inbox/titli_vaccination_2025.pdf
```

Relevant parsed output:

```text
True Titli vaccination record. Patient: Titli. Rabies booster given May 2025. Veterinarian: Samosa Animal Clinic.
```

## Attempt 1

Command:

```sh
curl -N --max-time 900 -sS -X POST http://127.0.0.1:8797/v1/jobs/run \
  -H 'Content-Type: application/json' \
  --data-binary '{"goal":"find Titli'\''s vaccination medical record and tell me the file path","folder":".../inbox","mode":"confirm"}'
```

Result: failed before the tool loop. The intent decoder returned
`kind:"organize"` because `_ORGANIZE_RE` matched the word `file` in
`file path`. The job stopped at `await_apply`; no files were moved because this
was confirm mode.

Fix added immediately after the run: remove bare `file` from `_ORGANIZE_RE` and
add `test_decode_find_file_path_is_not_organize`.

## Attempt 2

Same command after restarting the gateway with the patched decoder.

SSE events received before timeout:

```text
seq1 decode_intent goal="find Titli's vaccination medical record and tell me the file path"
seq2 intent kind=find
seq3 counting total=32 skipped=9 by_type={application/pdf:2,text/plain:30}
seq4 tool_call fs_list path=".../inbox" limit="100"
seq5 tool_call fs_list path="/" limit="50"
seq6 tool_call fs_list path="." limit="20"
seq7 tool_call fs_read_text path="note_00.txt"
curl: (28) Operation timed out after 900006 milliseconds with 1135 bytes received
```

Observed behavior:

- The model did enter the read-only find loop.
- The model started with metadata (`fs_list`), which is the intended strategy.
- The jail refused the attempted absolute-root listing; the safety boundary held.
- The model recovered to a relative listing.
- The model then read the wrong text-file candidate and did not return the
  planted PDF path before the 900s client timeout.

Backend stats from `/tmp/samosa-t7-home/backend.log`:

```text
[stats] prompt=480 generated=70 stop=model prefill=109.649s decode=18.978s total=128.626s peak_rss=3.36 GB
[stats] prompt=1499 generated=20 stop=model prefill=367.916s decode=5.484s total=373.400s peak_rss=3.36 GB
[stats] prompt=442 generated=21 stop=model prefill=103.917s decode=5.678s total=109.595s peak_rss=3.36 GB
[stats] prompt=1100 generated=21 stop=model prefill=274.763s decode=6.062s total=280.825s peak_rss=3.36 GB
```

Conclusion: T7 is not passed. The implementation has the read-only loop and
jail behavior, but the real-model prompt/tool ergonomics are not good enough to
claim find jobs work. Next fixes should make the find prompt more explicit
about relative paths, sufficient `fs_list` limits, and filename-first candidate
selection before reading arbitrary text files.
