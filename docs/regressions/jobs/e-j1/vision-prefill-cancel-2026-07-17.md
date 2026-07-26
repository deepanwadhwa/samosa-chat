# E-J1 rendered-image prefill cancellation — 2026-07-17

This is a machine-safety experiment, not a completed E-J1 acceptance run.
It exercises the first image-bearing page of the supplied JSS corpus:
`v109i02.pdf`, page 1 (443 extracted text tokens; `has_raster_figure: true`).

## Environment

- Reference host: 16 GiB M3 MacBook Air, macOS arm64, AC power at 100%.
- Model: local groupwise-q4 Qwen3.6-35B-A3B snapshot.
- Server: `make omp`, `OMP_NUM_THREADS=2`; `/internal/v1/status` reported
  `threads: 2`.
- Jobs request: the JSS metadata job, `thinking:"off"`, `max_tokens:512`, one
  rendered PDF page.

## Initial result: unsafe cancellation gap

The generic E-J1 harness began its one-page preview against the live server.
`POST /v1/cancel` returned `{"cancelled":true}` at `13:09:05Z`, but
`/internal/v1/status` still reported `inference_busy:true` at each five-second
poll through `13:10:13Z` (at least 68 seconds). A foreground chat request was
also left queued behind the request. The server was hard-stopped to retain a
bounded experiment.

No swapouts or page throttling occurred during that interval: `Swapouts` stayed
at `188362`, `Pages throttled` stayed at `0`; after stopping the server, memory
free recovered to 73%.

Cause: the server checked its cooperative cancellation flag in text prefill,
but the synchronous vision tower did not observe it.

## Fix and repeat

`vision_forward` now accepts the optional server cancellation flag and checks
it within the expensive projection and attention loops. Both initial and
continued generation paths treat an aborted vision pass as a cancelled turn.

The same preview was repeated with cancellation issued four seconds after the
request started:

```sh
SAMOSA_JOBS_DIR=/tmp/samosa-e-j1-vision-cancel \
SAMOSA_SERVE_URL=http://127.0.0.1:8642 \
SAMOSA_EXTRACTOR="$PWD/samosa-extract" \
SAMOSA_EXTRACT_TOKENIZER="$PWD/tokenizer_qwen36.json" \
SAMOSA_ENGINE="$PWD/qwen36b" TOKENIZER="$PWD/tokenizer_qwen36.json" \
python3 dist/samosa_jobs.py preview jobs/jss-article-metadata.json \
  --file "$PWD/v109i02.pdf"
```

`POST /v1/cancel` returned `{"cancelled":true}` at `13:15:30Z`; the first
five-second poll at `13:15:35Z` reported `inference_busy:false`, and all three
subsequent polls remained clear. Server stats reported `prompt=1253`,
`generated=0`, `stop=cancelled`, `prefill=11.354s`, and `peak_rss=3.02 GB`.
`Swapouts` again stayed at `188362`; `Pages throttled` stayed at `0`.

`make test` and `make jobs-test` passed after the change.

## Status

The rendered-image interruption gate now passes for this one bounded page.
An uncancelled run of the same page subsequently stayed inside the safety
envelope (no swapout, throttling, or thermal warning) but reached the preview
client's 300-second timeout without a model response. Server statistics after
the cancellation show why: the 1,253-token prompt had generated **zero** tokens
after 312.495 seconds of prefill (4.01 tok/s), with 75.50 GB read from the
expert shards, a 1.1% expert-cache hit rate, and 3.87 GB peak RSS. It never
entered decode, so lowering its 512-token output cap would not solve this
specific result. This is a measured cost/latency limit, not an accuracy result.

The timeout exposed a runner defect: `preview` returned without cancelling the
still-active inference. That path now uses the same cancel-and-wait cleanup as
`run`; the live timeout was cancelled and the slot cleared on the first
five-second poll, with `Swapouts` still `188362`.

E-J1 acceptance remains open: it still needs the full labeled representative
batch, field-accuracy/malformed-rate results, and a recorded live chat
interlock pause/resume. The current JSS page shape must also be resized or
otherwise measured to a completed response before it is used for an unattended
117-page batch.
