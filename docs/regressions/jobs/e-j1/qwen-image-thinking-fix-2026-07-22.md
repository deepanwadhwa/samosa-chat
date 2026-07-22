# E-J1 Qwen image — thinking fix + honest image-path status (2026-07-22)

Continues the other agent's Qwen image smoke
([qwen-image-smoke-2026-07-22/report.md](qwen-image-smoke-2026-07-22/report.md)),
which found the image job returned a JSON scalar (`extracted:0`,
`invalid_model_output`) instead of a schema object.

## Root cause found + fixed (real bug)

`model_extract` (`src/samosa_gateway.c`) disabled reasoning **only** the
llama.cpp way — `chat_template_kwargs:{enable_thinking:false}`. The **Qwen C
engine ignores that field**; it only honors a **top-level `"thinking"`** field
(`qwen36b.c`: `json_get(root,"thinking")` → `"off"` sets `no_thinking`). So every
Qwen job ran with **reasoning on**, burned its token budget thinking, and emitted
no JSON. (The Ornith/PDF path passed because llama-server *does* honor
`chat_template_kwargs`.)

**Fix:** `model_extract` now also sends top-level `"thinking":"off"`. Qwen honors
it; llama-server ignores the unknown field and still uses `chat_template_kwargs`,
so the passing Ornith path is unchanged.

**Confirmed on Qwen text:** with `thinking:"off"`, Qwen returns clean
`{"ok": true}` — `finish_reason:stop`, 6 completion tokens, ~8 s. Before the fix
the same engine spent the whole budget reasoning. `make jobs-test` and `make
test` stay green.

## Image path — still open, and honest about why

Even with the fix, the **image** definition job still returns
`review_required / extracted:0` on a real rendered JSS page (543×768 PNG,
`v109i03` page 1). This is **not** a plumbing bug — verified by code read:
`definition_image_data_uri` builds a valid `data:image/png;base64,…` URI and the
content array matches the serve contract (`{"type":"text"...},{"type":"image_url",
"image_url":{"url":…}}`). The scalar is Qwen's own vision→JSON output.

**Could not iterate the diagnosis on this hardware.** A single *cold* Qwen vision
inference on the 16 GB M3 ran **8+ minutes** (one direct "describe the page"
call timed out at 520 s); every gateway restart drops the expert page cache, so
each attempt re-streams experts cold. This is the documented "prefill is the
binding constraint," amplified by 576 image tokens on the 24 GB streaming model.
Capturing the raw vision output (to see whether Qwen emits prose, an error, or
garbage) needs a single completing inference with output logging — best done on
warmer cache / faster hardware, not by repeated cold restarts here.

## Status

- **Kept:** the `thinking:"off"` fix (correct, necessary, proven on text; Ornith
  path unaffected).
- **Still open (unchanged from the other agent's honest note):** image /
  multi-image acceptance. `docs/TASKS_JOBS.md` already reflects this.
- **Next:** capture one raw Qwen image response with logging to classify the
  scalar-`0` output; then the real labeled image corpus + multi-image/page
  reduction.
