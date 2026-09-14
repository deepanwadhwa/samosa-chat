# Document planner timeout regression

Verified locally on 2026-09-12 with Qwen3.6 35B A3B and the 506-page
`Indica--A-Deep-Natural-History-of-the-Indian-Subcontinent--Pranay-Lal.pdf`.

The internal planner had a 60-second receive timeout but needed 69–77 seconds
to process its 562-token initial prompt. Its discarded response triggered a
two-page fallback, followed by another planner timeout. The original live
test accepted a truncated answer after 237.23 seconds because it checked only
for the author's name.

The repair shortens the planner instruction and removes redundant attachment
identifiers from its evidence input. A failed planner gets one bounded opening
read without another planning attempt. The fallback reads one PDF page. Answer
instructions retain source attribution and coverage rules with less overhead.
Skipping reading also requires the planner to supply a nonempty supporting
quote that actually occurs in the filename (or mark the source irrelevant).
A bare finish or invented quote triggers the bounded opening read. This fixes
a live failure where the model stopped at `book.pdf` despite lacking an author.
Document planning also has its own 180-second receive deadline: a subsequent
page-evidence call was measured at 85 seconds, so shortening the initial prompt
alone did not fix the timeout. Other internal web calls retain 60 seconds.

The final installed build repeated the repaired lookup in 69.78 seconds
(first content at 67.03 seconds), with no page reads or OCR:
“Based on the filename, the author is Pranay Lal.” Planning completed at 34.46
seconds. This remains slow: measured native model
prefill was about 8.5 tokens/second. This change does not accelerate the model
engine or claim to make full-book OCR fast.

The neutral-filename test (`book.pdf`) also passed: one native text page, no
OCR, and a complete answer naming Jane Doe and citing page 1. The final run took
111.17 seconds using cached native page text; the earlier uncached native read
took about 0.3 seconds. The model first selected
six pages unnecessarily; the instruction now requires a one-page opening read
for author lookups, and the live test enforces that bound.

`tests/test_document_decisions_live.py` now requires a normal terminal event
and supports `--metadata-only` and `--max-seconds`. The deterministic harness
also verifies that a failed planner is not called again after fallback reading.
