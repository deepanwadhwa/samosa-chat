# Folder prompt budgeting

The reported folder overview sent 16,185 prompt tokens to an 8,192-token
Ornith model. The evidence builder bounded characters but never checked the
assembled chat template, instructions, question, and evidence against the
active model context.

The gateway now checks the assembled folder turn before forwarding it. It
reads the running engine's context from `/props`, applies its chat template,
and uses its tokenizer. If those tokenizer endpoints are unavailable, it
uses a conservative UTF-8 byte bound with template overhead.

The final answer reserves one quarter of the context, up to 2,048 tokens,
respecting a smaller requested response length. A further 256 tokens are
reserved as margin. Evidence that fits goes directly to the answer model.
Oversized evidence is reviewed in question-focused sections, each with its
own measured prompt and 384-token response budget. Intermediate prompts are
capped at 4,096 tokens when the tokenizer is available. Whole UTF-8 characters
and, where possible, complete lines are retained across section boundaries;
the preceding file label accompanies a continuation.

Every admitted evidence byte is processed. Notes are reduced again if the
combined final prompt still does not fit. The inventory/coverage preamble,
computed counts, question, and system instructions stay outside model-generated
notes. Derived notes are explicitly identified as paraphrases. This does not
expand the underlying inventory or read previously unindexed content.

Canceled, failed, empty, truncated, or non-shrinking reviews stop the turn
without forwarding an incomplete synthesis. Oversized questions/instructions
that cannot fit even without file records are rejected before inference.
The UI reports section review and preparation of the answer. The application
implementation is C and uses the resident answer backend.

## Verification

Only generated fixtures and repository test fixtures were used. Downloads,
its Chutni memory, and existing conversations were not opened or queried.

- `make test-prompt-budget`: direct small turn, large evidence, multiple
  reduction rounds, missing tokenizer endpoints, active 4K context despite
  larger saved settings, inference failure, truncated output, cancellation
  before and during inference, and an oversized question. Every model call
  is checked against the fake engine's context; every original fact reaches
  a review, including the final file. The suite is part of `make test`.
- `make test-memory-harness chutni-gateway-test`: action contracts, memory
  gateway, pause/cancel, and scanner crash/restart tests passed.
- `make compiled-gateway-test`: compiled gateway, settings, attachment,
  document prefix, web search, and developer trace regressions passed.

The live Ornith check used 80 generated UTF-8 records and an initial prompt of
16,044 tokens. Five review requests used 4,082 / 4,081 / 4,082 / 4,081 / 607
prompt tokens, each returning with `finish_reason=stop`. The condensed final
prompt was 793 tokens. A final synthesis with the additional instruction that
uninspected sampled-out content is not a reading failure used 818 prompt tokens
and returned a 155-token answer without thinking tags or a context error.
The answer identified the generated irrigation/rainfall/water-use theme.
The live test establishes context fit and completion, not perfect model
adherence to every coverage qualifier; the final model phrased the sampling
caveat around a subset of the records despite the preserved global caveat.

Local release `dev-035c4f81d7b1` contains the fix. Restart uses
`SAMOSA_DEFER_PENDING_MEMORY=1` so pending folder builds are not resumed.
Only health and installed binary identity are checked after restart; the
user's folder scope and memory are not queried.

These tests do not close the broader FW-6 acceptance/scheduling gate.
