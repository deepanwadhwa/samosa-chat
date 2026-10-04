# Streamed folder turn: evidence encoding and summary recovery

The prior fix's direct budgeting test used complete UTF-8 strings. It did not
exercise the memory evidence builder's fixed-byte excerpts through a streamed,
conversation-bound request. The user then reported the generic
`folder_context_budget_failed` error for a basic folder overview.

An isolated gateway, its own application home, real Chutni indexing, a strict
JSON model stand-in, and 80 newly generated multilingual text files reproduced
the same failure. The browser-shaped request included a system message,
conversation/model identity, directory context, streaming, and an 8K response
preference. Before the fix, the request failed in 0.54 seconds, before any
evidence review or final synthesis. No user folder or memory was queried.

## Cause and change

The memory builder cut some previews/excerpts in the middle of a multibyte
character. JSON serialization correctly replaced the broken bytes, but the
budgeter compared its decoded prompt against the original raw evidence bytes.
That lookup failed, so it skipped summarization and returned the generic error.

All memory excerpt and identity-probe byte ceilings now retain complete UTF-8
characters. The budgeter also normalizes admitted evidence through the same
JSON encoder before locating it, covering legacy or otherwise malformed
reader text. Neither correction reads additional source data or changes scope.

Intermediate reviews now recover from a transient inference failure, empty
output, or an output-token truncation using at most four attempts per section.
On truncation, the output allowance grows within the active context budget and
the source section becomes smaller. The source offset advances only after a
complete review succeeds, so failed notes and unprocessed tails never become a
final answer. Complete leading thinking blocks are removed from intermediate
notes; unfinished blocks are retried. Persistent failures retain separate
codes for truncation, timeout, backend failure, and insufficient input space.
The blanket instruction to shorten a basic question has been removed.

## Checks

- The same generated 80-file streamed request now completes, with section
  reviews and a final synthesis call. All model requests contain valid JSON
  and stay inside the 8,192-token context.
- Unit checks exercise two-, three-, and four-byte character boundaries and
  the actual memory content-excerpt builder, including its truncation marker.
- Budget checks cover the serialization mismatch, successful recovery from
  one truncated/failed/empty response, reasoning removal, permanent failures,
  cancellation, all source facts including the tail, 4K context, missing
  tokenizer endpoints, and reduction. Every retry is checked for context fit.
- The streamed regression is part of `make test`, alongside the budgeting
  regression. Existing gateway, pause/cancel, and crash/restart tests pass.

The same full streamed request also completed against the resident Ornith
model in 333.38 seconds, with no error event. It identified the three generated
topics (orchard irrigation, coastal navigation, pottery). This live test covers
the complete folder-question path and successful condensation/synthesis; the
fixture's index-time summaries and decision plan use stand-ins to isolate
the changed path. The answer still misphrased the supplied subset as an indexed
count and speculated about metadata flags, so this is not an answer-quality
acceptance pass. Model latency and faithful synthesis remain separate concerns.

Local release `dev-2cba2a57a849` is installed and running. Health reports
Ornith ready; gateway and jobsd hashes match the checked build. The restart
deferred pending memory builds, and no user scope or conversation endpoint was
queried for verification. Live-model and installation evidence are retained
alongside this report.
Reproducing this error in generated data does not establish which private
source triggered the user's instance. Downloads, its memory, existing
conversations, and logs containing their content were not inspected.
The broader FW-6 answer-quality/acceptance gate remains open.
