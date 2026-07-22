# Aborted/tainted interlock attempt

This directory is not the final E-J1 interlock evidence.

The run opened a real `/v1/chat/completions` request mid-batch and the compiled
definition route did emit `job_paused` / `job_resumed`, but the harness chat
probe did not cap `max_tokens`. Ornith generated a long reasoning response, so
the probe was cancelled through `/v1/cancel` and the gateway was stopped with
`/v1/kill`. That cancellation tainted the extraction accuracy: 3/4 records fell
to `review_required`.

Keep this only as a debugging record. The clean rerun must use the bounded chat
probe in `tools/run_e_j1.py`.
