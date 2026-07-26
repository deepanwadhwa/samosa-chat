# Find ask_user Ornith checkpoint — 2026-07-21

Scope: T8 pause/resume checkpoint for Phase JF using Ornith 9B. No Qwen run was
started for this checkpoint.

Backend:

- `backend=ornith`
- Model: `/Users/deepanwadhwa/.samosa/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf`
- Server: `/Users/deepanwadhwa/.samosa/backends/prism-llama.cpp/build/bin/llama-server`

Gateway: `tools/samosa_gateway.py` on `127.0.0.1:8797`, driven through
`POST /v1/jobs/run` and `POST /v1/jobs/answer` SSE.

Fixture:

- Scratch folder:
  `/var/folders/d6/7z7gzbj16l59s8tn7h4mg8lm0000gn/T/samosa-ask-t8-xtx4tgwv/inbox`
- Files:
  - `titli_vaccination_2025.txt`
  - `momo_vaccination_2025.txt`
  - `grocery.txt`

Goal:

```text
find the vaccination record, but ask me which pet name to use before you read the records
```

## First live attempt

Ornith asked a clarifying question as ordinary final text instead of calling
`ask_user`, so the resume path did not trigger:

```text
seq4 tool_call fs_list path="." limit="20"
seq7 done "I see two vaccination files: ... Which pet name should I use?"
```

Fix added: if a find loop's final text ends in a question, the runner treats it
as `await_user`, persists `convo.json`, and resumes through `/v1/jobs/answer`.

## Passing live run

Run output:

```text
seq1 decode_intent job_id=find-the-vaccination-record-but-1784657418
seq2 intent kind=find
seq3 counting total=3 skipped=0 by_type={text/plain:3}
seq4 tool_call fs_survey recursive="."
seq5 survey total=3 skipped=0 by_type={text/plain:3}
seq6 tool_call fs_list path="." limit="20"
seq7 await_user "I see two vaccination files: `momo_vaccination_2025.txt` and `titli_vaccination_2025.txt`. Which pet name should I use?"
```

Answer command:

```sh
curl -N --max-time 180 -sS -X POST http://127.0.0.1:8797/v1/jobs/answer \
  -H 'Content-Type: application/json' \
  --data-binary '{"job_id":"find-the-vaccination-record-but-1784657418","answer":"Titli"}'
```

Resume output:

```text
seq8 tool_call fs_read_text path="titli_vaccination_2025.txt"
seq9 done "Found the vaccination record at `titli_vaccination_2025.txt`. It shows: **Titli vaccination record. Rabies booster May 2025.**"
```

Result: passed for Ornith. The job paused, accepted the user's answer, resumed,
read the correct file, and answered with the correct path. No mutating events
were emitted.
