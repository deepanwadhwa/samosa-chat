# Native question contract version 1

One request <=16 KiB, one JSON response. Each has contract_version=1. Describe
reports component/version/operations. Question accepts exactly: op=question,
question (nonempty <=2048 UTF-8 bytes), source_id, version (64 lowercase SHA-256
hex), start_byte, end_byte (exclusive, selected length <=65536).

Agent validates the Sources result matches selected source/version/range and is
literal qualified UTF-8 evidence. It describes Inference, pins the returned
arbitrary provider ID, counts the actual assembled prompt, reserves 256 output
tokens, then sends exactly the counted prompt for generation. Sources is called
once; invalid evidence stops before Inference. All child bodies <=1 MiB; deadlines
10 seconds each. Direct argv subprocesses, no shell interpolation of user data.

Result: answer, citations (evidence_id/source_id/version/range), admitted_evidence,
model_request (provider/prompt/input_tokens/reserved_output_tokens), usage.
Model request is an explicit headless acceptance diagnostic for this subset.
No claimed grounding quality for fake output. No implicit whole-file fallback.
No filesystem effects; document instructions have no action authorization.

Failure: {contract_version:1,error:{code}}, nonzero exit status. Errors include
invalid_request/unknown_field/invalid_selector/dependency_unavailable/source_failed/
invalid_evidence/provider_unavailable/unsupported_capability/token_count_failed/
provider_mismatch/context_limit/generation_failed; transport errors child_io,
child_unavailable, child_failed, response_too_large, deadline_exceeded,
invalid_configuration and resource_limit. No request can override host commands.

S04 extends exactly one selector: byte fields above OR start_page/end_page
(inclusive1-based<=4), never mixed. PDF literal evidence must contain every selected
absolute page with explicit text status; partial text_layer_only and actual total
preserved. Citations use the same selected page range. Native provider details stay
inside Inference; Agent's count/budget/prompt/commit path is shared with fake/text.
