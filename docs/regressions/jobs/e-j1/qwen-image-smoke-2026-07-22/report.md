# E-J1 Qwen image smoke

This run was a narrow live smoke of the compiled definition route's image path,
not a broad E-J1 image acceptance run.

- Backend: compiled `samosa-gateway` + Qwen3.6 35B A3B
- Health before run: `supports_images:true`, `ready:true`
- Input: one 1x1 PNG fixture generated in the run temp directory and copied here
  as `tiny.png`
- Request path: `/v1/jobs/definition/run`
- Result: terminal SSE completed in `model_call_seconds=83.750`
- Output: `review_required`, `reasons:["invalid_model_output"]`,
  `extracted:0`
- Safety during/after run: `Pages throttled=0`, `Swapins=0`, `Swapouts=0`

Interpretation: the compiled route now reaches a vision-capable backend with an
image job and returns timing telemetry, but this does **not** close image
acceptance. The model returned a JSON scalar instead of the requested schema
object. Multi-image/page reduction was not exercised; the current compiled
definition route still processes listed files independently and does not reduce
multi-page image records.
