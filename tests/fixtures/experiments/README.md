# E-X1 deterministic workloads and quality suite

These files are deliberately plain UTF-8 text.  They are versioned inputs, not
claimed token counts: the reference tokenizer is the authority.  Before a
baseline run, record `wc -w` and the encoded-token count in the evidence
report.  If a tokenizer/model update moves a workload materially away from its
target shape, adjust the fixture in a reviewable commit rather than silently
changing the command.

All experiment invocations use `--greedy --no-thinking --seed 1729` and a
freshly built `qwen36b`.  The runner must archive the command plus every
`[stats]`, `[phase]`, `[ecache]`, and `[seqio]` line.

## Workloads

- `workloads/w_decode_context.txt` seeds a saved session.  Resume that session
  and generate 256 tokens for W-DECODE; use the tokenizer count to tune the
  seed turn to approximately 1,000 saved-context tokens.
- `workloads/w_prefill_document.txt` is the W-PREFILL source document.  Ask
  for a concise summary and generate 32 tokens.
- W-SESSION is the same saved-session procedure after extending the context to
  at least 4,096 tokens, then generating 128 tokens.
- W-SUSTAIN repeats the W-DECODE command for ten minutes under the thermal
  protocol in `docs/TASKS_EXPERIMENTS.md`; it is never an unattended loop.

## Quality suite

Run every file in `prompts/` in lexical order.  `quality_source.md` is the
committed source for the two summary prompts.  The long-document QA prompt
uses the repository's Jobs corpus fixture when that fixture is present; until
then it is explicitly not run rather than substituted with an untracked file.

For reproducible baseline assembly, prompts 01, 02, 03, 05, and 08 through 11
are continuations of one saved session seeded with the complete
`workloads/w_prefill_document.txt` source.  Restore the same source-only
session before each prompt; do not let one prompt's answer become another
prompt's context.  Prompts 04, 06, and 07 are standalone.  Prompt 12 is
recorded as `not run` until the committed Jobs corpus fixture exists.  Use the
same `--greedy --no-thinking --seed 1729` / API-equivalent controls and archive
the response for every executed prompt.

The suite is a compatibility baseline: archive exact outputs before comparing
any numerics- or policy-changing experiment.

## Safety capture

For every real-model run, keep the privileged `powermetrics` thermal trace
live-readable and treat it as the authoritative thermal gate.  `pmset` may be
recorded as supplementary host state but does not replace the trace.  Stop the
model and record an abort on the first sustained non-Nominal pressure for a
workload that requires Nominal thermal state; do not start another workload
until the owner has selected a cooling/retry strategy.
