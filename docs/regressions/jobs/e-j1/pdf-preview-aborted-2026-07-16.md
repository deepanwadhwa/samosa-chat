# E-J1 PDF preview — aborted safety result (2026-07-16)

This is an incomplete E-J1 run and a negative result, not an acceptance claim.
It records why the JSS article-metadata job was changed to page units before a
full model batch is attempted.

## Environment and command

- macOS arm64, 16 GiB RAM.
- Current local `qwen36b` build, groupwise-q4 model snapshot at
  `/Users/deepanwadhwa/samosa_release_upload`.
- `samosa-extract` and the local verified `tokenizer_qwen36.json` were used.

The server started successfully with a 2.93 GiB RSS and reported eight threads:

```json
{"status":"ok","model":"qwen3.6-35b-a3b","rss_gb":2.93,"context_limit_tokens":24576}
{"interactive_active":false,"queue_depth":0,"inference_busy":false,"threads":8}
```

The attempted preview was deliberately bounded to the only JSS PDF that the
auto planner initially considered a whole-file unit:

```sh
SAMOSA_JOBS_DIR=/tmp/samosa-e-j1-jobs \
SAMOSA_SERVE_URL=http://127.0.0.1:8642 \
SAMOSA_EXTRACTOR="$PWD/samosa-extract" \
SAMOSA_EXTRACT_TOKENIZER="$PWD/tokenizer_qwen36.json" \
TOKENIZER="$PWD/tokenizer_qwen36.json" \
python3 dist/samosa_jobs.py preview jobs/jss-article-metadata.json \
  --file "$PWD/v109i03.pdf"
```

The adapter measured 20,817 exact document tokens (20,788 when page counts are
summed; tokenization across page boundaries is non-additive). The request was
still active after about 2.5 minutes and produced no model output.

## Safety observation

Before the request, `vm_stat` included 202,284 free pages and 227,443 pages in
the compressor. During the request it reached 4,597 free pages and 654,035
compressed pages; the later snapshot reported 97,846 swap-ins and 188,362
swap-outs. The initial command did not capture the baseline swap counters, so a
swap delta cannot honestly be calculated from this run.

Terminating the preview client did **not** clear `inference_busy`; it remained
true through six five-second polls. The server launched for this test was then
hard-stopped to return the laptop to a safe state. Afterward, the server was no
longer listening and free pages recovered to 422,668.

The retry was repeated with `OMP_NUM_THREADS=2`, a forced page unit, and an
explicit `POST /v1/cancel` after eight seconds. The endpoint returned
`{"cancelled":true}`, but `inference_busy` remained true for five subsequent
five-second polls. At that point the engine checked its cancellation flag only
after the current monolithic prefill returned.

## Follow-up: bounded text-prefill cancellation

The server prefill path was changed to checkpoint its cancel flag every 32 text
tokens, while the CLI retains its established single-prefill path. The offline
suite (`make test` and `make jobs-test`) passed after the change. Three real
two-thread checks then passed:

| Request | Result |
| --- | --- |
| 1,516-token plain-text prompt, cancel after 3 s | slot cleared within the next 5-second poll; response had `finish_reason:"cancelled"`, 0 completion tokens, 4.34 GiB RSS |
| JSS `v109i03.pdf` page 2, extracted text only (1,341 model prompt tokens), cancel after 3 s | slot cleared within the next 5-second poll; response had `finish_reason:"cancelled"`, 0 completion tokens, 4.34 GiB RSS |
| 17-token prompt, no cancel | completed normally in 2.9 s with content `OK` and `finish_reason:"stop"` |

The rendered-image/vision prefill portion has not yet had an equivalent
interruption measurement. It is therefore not claimed as verified here.

## Follow-up: one-page JSS extraction smoke

With the same two-thread cap, extracted text from `v109i03.pdf` page 2 was sent
through the actual Jobs request shape with `max_tokens: 128` and background
priority. It completed with 1,341 prompt tokens and 100 completion tokens at
5.78 decode tokens/s (4.39 GiB RSS). The response passed the job's output-schema
validator with no errors or warnings. It correctly returned:

```json
{
  "title": "openTSNE: t-SNE Dimensionality Reduction and Embedding in Python",
  "journal": "Journal of Statistical Software"
}
```

The remaining fields were `null` on that individual page. This is expected for
a page-level unit and is why the real job's final document record is produced by
the deterministic reducer. It is a smoke result only: it is not a four-document
field-accuracy result, and it does not cover image-bearing pages.

## Consequences

1. This run has no correctness, malformed-rate, throughput, or field-accuracy
   result. E-J1 acceptance is still open.
2. The runner calls the explicit cancel endpoint on a timeout, and bounded
   **text** prefill cancellation is now measured. A rendered-image prefill
   interruption check and the full E-J1 safety/accuracy batch remain required
   before describing unattended PDF Jobs as verified.
3. `jobs/jss-article-metadata.json` now forces page units. That honors the
   single-image limit and avoids submitting a 20k-token article in one request.
4. A resumed E-J1 run must start with `vm_stat` captured both before and after,
   use the measured host-safe thread setting, exercise a single page first, and
   only then expand to the full labeled batch.
