# Developer mode and full pipeline traces

Developer mode is an opt-in, local, full-fidelity diagnostic log for building
and debugging Samosa. It exists to answer questions such as:

- Did the attachment router request OCR, visual inspection, or both?
- Did VisionPsy actually start and inspect the intended attachment?
- What did VisionPsy return verbatim?
- What OCR or document text was extracted?
- What evidence did Samosa send to the final chat model?
- What did the final model return, and where did a false claim first appear?

Open **Settings → Advanced → Developer mode** and enable **Keep Developer mode
enabled across restarts**. The setting persists until it is switched off. The
card shows the current JSONL path and provides **Copy log path**. Turn the mode
off before using **Clear logs**.

## Privacy and storage

Developer traces are off by default. When enabled, they intentionally record:

- complete chat request bodies, prompts, and conversation context;
- local attachment identifiers, filenames, content-addressed blob paths, and
  sizes;
- router prompts, raw router responses, and the validated route;
- raw OCR and document-reader responses;
- the VisionPsy command, selected adaptive image size, raw response,
  observation, token counts, timings, process ID, and errors;
- the exact augmented request sent to the final chat backend; and
- the raw backend HTTP/SSE response and completion outcome.

This data can include private document contents and model replies. Files are
created with mode `0600` under:

```text
~/.samosa/logs/developer/developer-trace-<session-id>.jsonl
```

The gateway never records `X-Samosa-Token`, the Authorization header, or the
per-launch UI session token. Do not attach a trace to a bug report until its
contents have been reviewed. Disabling the mode stops new events immediately;
it does not delete existing traces.

## Correlation and event format

Every line is one JSON object with schema `samosa.developer.trace.v1`:

```json
{
  "schema": "samosa.developer.trace.v1",
  "session_id": "...",
  "sequence": 17,
  "wall_ms": 1787681045123,
  "session_elapsed_ms": 2841,
  "pid": 1234,
  "event": "visionpsy_raw_response",
  "turn_id": "...",
  "fields": {"component":"samosa-visionpsy","bytes":123,"payload":"..."}
}
```

`session_id` groups a gateway trace. `turn_id` groups the browser request,
router, evidence tools, and final model call for one chat turn. `sequence`
provides an unambiguous file order even when different request threads run
concurrently.

The principal visual attachment sequence is:

1. `chat_request_received` and `chat_turn_started`
2. `vision_router_fallback`
3. `vision_router_request` and `vision_router_response`
4. `vision_router_validated_plan`
5. `attachment_selected`
6. `ocr_request`, `ocr_raw_response`, and `ocr_complete` when text was requested
7. `vision_resource_budget`
8. `visionpsy_starting`, `visionpsy_process_started`, and `visionpsy_ready`
9. `visionpsy_request`, `visionpsy_raw_response`, and `visionpsy_complete`
10. `vision_evidence_observation`
11. `backend_request`, `backend_response`, and `backend_complete`
12. `visionpsy_process_stopped` and `chat_turn_completed`

Document-only turns use `document_reader_request`,
`document_reader_response`, and `document_reader_complete`. Internal planner,
ranking, and sufficiency calls also emit `internal_model_request`,
`internal_model_response`, or `internal_model_error`.

## Diagnosing a hallucinated image answer

Use one `turn_id` and compare these payloads in order:

1. `vision_router_validated_plan`: `inspect_visual` must be `true`.
2. `visionpsy_request`: confirms the exact image path, question, token limit,
   and hardware-selected `max_side_len`.
3. `visionpsy_raw_response`: the specialist's verbatim observation.
4. `backend_request`: the exact evidence the chat model was asked to
   synthesize.
5. `backend_response`: the chat model's raw answer stream.

If an invented number first appears in `visionpsy_raw_response`, the visual
model or its preprocessing is responsible. If the raw observation is correct
but the number first appears in `backend_response`, the final chat model is
responsible. If `inspect_visual` is false or no `visionpsy_request` exists, the
router/provider selection is responsible. If a VisionPsy error is followed by
`attachment_partial_evidence`, the answer intentionally continued as a
labelled OCR-only partial result.

## Local API

All Developer mode routes require the normal `X-Samosa-Token` UI session token.

```http
GET /v1/developer/trace
```

Returns the enabled state, trace path/directory, byte count, event count, and
privacy flags.

```http
PUT /v1/developer/trace
Content-Type: application/json

{"enabled":true}
```

Creates a new private trace and persists the setting. Sending `false` appends a
terminal `trace_stopped` event, stops capture, and persists that state.

```http
POST /v1/developer/trace/clear
```

Deletes regular `developer-trace-*.jsonl` files only. It returns `409
developer_mode_active` while capture is enabled, so an active file cannot be
deleted out from under a request. Invalid bodies and filesystem failures use
structured JSON errors and leave the current state visible in Settings.

## Regression coverage

`tests/test_developer_trace.sh` is part of the default compiled-gateway gate and
verifies authentication, default-off behavior, private permissions,
persistence across restart, plain-chat correlation, token exclusion, immediate
stop after disable, and clearing. `tests/test_visionpsy_e2e.sh` additionally
requires one correlated visual pipeline with the raw VisionPsy observation in
the exact final-backend evidence. `tests/test_developer_mode_ui.mjs` covers the
Settings warning, toggle, status, copy-path, and guarded-clear behavior.
