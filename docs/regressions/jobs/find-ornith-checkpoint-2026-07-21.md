# Find Ornith checkpoint — 2026-07-21

Scope: fast loop-debugging checkpoint for Phase JF using Ornith 9B instead of
the 24 GB Qwen model. This proves the Jobs find loop and prompt/tool ergonomics
against a real local model through the app route. It is not the formal T7 Qwen
acceptance gate unless that gate is changed.

Host: macOS Darwin arm64, local development checkout on `issue-7-jobs`.

Backend:

- `backend=ornith`
- Model: `/Users/deepanwadhwa/.samosa/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf`
- Server: `/Users/deepanwadhwa/.samosa/backends/prism-llama.cpp/build/bin/llama-server`

Gateway: `tools/samosa_gateway.py` on `127.0.0.1:8797`, driven through
`POST /v1/jobs/run` SSE, the same route used by the Jobs tab.

Fixture:

- Scratch folder:
  `/var/folders/d6/7z7gzbj16l59s8tn7h4mg8lm0000gn/T/samosa-find-t7-uio5_4c6/inbox`
- Planted file: `titli_vaccination_2025.pdf`
- Clutter: 30 text notes plus 10 placeholder PDF-looking files.

Command:

```sh
curl -N --max-time 300 -sS -X POST http://127.0.0.1:8797/v1/jobs/run \
  -H 'Content-Type: application/json' \
  --data-binary '{"goal":"find Titli'\''s vaccination medical record and tell me the file path","folder":".../inbox","mode":"confirm"}'
```

SSE output:

```text
seq1 decode_intent job_id=find-titli-s-vaccination-medical-1784656762
seq2 intent kind=find
seq3 counting total=32 skipped=9 by_type={application/pdf:2,text/plain:30}
seq4 tool_call fs_list path="." limit="50"
seq5 tool_call fs_read_document path="titli_vaccination_2025.pdf"
seq6 done summary="The file is **`titli_vaccination_2025.pdf`** — it contains Titli's vaccination record (Rabies booster given May 2025 at Samosa Animal Clinic)."
```

Result: passed for Ornith. The model used the intended metadata-first strategy,
kept paths relative, read the planted PDF through the document sidecar, and
answered with the correct file path. No mutating events were emitted.

Backend timing from `/tmp/samosa-ornith-t7-home/backend.log`:

```text
turn 1: prompt eval 488 tokens in 3262.97 ms; eval 54 tokens in 3607.15 ms; total 6870.13 ms
turn 2: prompt eval 1152 tokens in 6985.13 ms; eval 68 tokens in 4569.66 ms; total 11554.80 ms
turn 3: prompt eval 86 tokens in 760.40 ms; eval 105 tokens in 6965.46 ms; total 7725.86 ms
```

Follow-up: continue using Ornith for find-loop iteration. Keep the Qwen
negative checkpoint separate unless the owner changes the formal T7 gate.
