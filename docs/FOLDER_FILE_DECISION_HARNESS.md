# Folder and file decision harness

The walkthrough exposed a missing control layer: lexical retrieval was being
used for folder overviews, membership counts and document properties. A short
search result was then presented as if it described the entire folder.

`tools/samosa_decision.py --mode memory` now extends the same OpenDecision
adapter already used by Jobs. It uses the pinned local DeBERTa checkpoint and
the existing `choice`, `noul` and `choice_fast` process. There are no branches
for the walkthrough questions, fixture people or reference codes.

## Decision hierarchy

1. Choose the request scope: the selected folder or particular files.
2. Independently select one or more read-only operations: overview, inventory,
   metadata and content. A compound question may require several operations.
3. Resolve qualified filenames against the authorised source inventory. An
   ambiguous basename may identify multiple files. Otherwise rank candidates
   with the decision model. Inventory operations inspect every supplied record.
4. For subject/group membership, the decision model distinguishes named
   identities from general topics. Subject selection is a separate local JSON judgement: it copies a
   complete name or topic from the question, excluding surrounding file-selection
   wording. The gateway requires a bounded span present in the current question;
   the subsequent membership judgement must keep that subject unchanged.
   For named membership, the current local chat backend checks every supplied
   inventory record and returns source IDs and literal supporting quotes.
   The gateway checks each quote against the actual
   relative filename or indexed preview. Named matches require the complete
   requested identity at word boundaries in the source, including beyond the
   quote's end; a cropped prefix cannot establish membership. If a model-selected
   named match has a deficient quote, its actual inventory filename can provide
   the full identity proof. Derived summaries and captions cannot serve as
   literal quote proof. For semantic membership, OpenDecision checks EVERY
   supplied source's actual filename and literal preview against a dynamic
   topic hypothesis. It does not depend on a generative shortlist, which
   previously omitted a synonym match. Probe inputs have at most 128 bytes of
   path and 1,000 bytes of literal preview; derived text is excluded. A negative
   decision over incomplete evidence remains uncertain. Invalid, unsupported and uncertain matches cannot increase
   the supported count. The gateway owns uniqueness and file-type arithmetic.
5. Collect current indexed evidence for the selected actions and generate the
   answer. Include all supplied filenames independently of content selection.
   PDF page totals come from persisted reader metadata, never the last hit or
   last extracted page. File formats and byte sizes come from source metadata.

`src/samosa_memory.h` owns execution and validation. Decision outputs cannot
introduce paths or executable operations. Folder data is marked untrusted.
Earlier assistant answers are excluded from evidence; the last earlier user
question may provide reference context for the current request.

## Coverage and failure behavior

The current batch contains at most 80 source records and 6,000 bytes of paths.
Previews share a 6,000-byte budget; selected content shares 14,000 bytes. An
overview uses at most 500 bytes per source. These are bounded indexed snapshots,
not unrestricted live file access or a claim that every document was read.
The evidence separately reports inventory, indexing and content coverage.
Incomplete inventories prohibit exhaustive absence and whole-folder membership
claims. Membership verification failure prohibits exact subject counts.
Unmatched files with missing or incomplete previews remain uncertain; the
gateway does not let a model turn an unread remainder into proof of absence.
When any association remains uncertain, answers must state a confirmed count
and an unverified remainder. Zero confirmed matches cannot establish that no
such file exists in the folder.
Literal source text takes precedence over stored model summaries, and derived
summaries or image captions are labelled when used as fallback evidence.
Duplicate artifacts with equal content and equal page selectors are supplied
once. Equal content on different numbered pages remains distinct. Previews
are not repeated when full selected content is already present. Once association
is verified, unrelated and uncertain previews are omitted from the answer; their inventory
paths and statuses remain supplied. Answers group similar files unless the
user requests a full listing.
Uncertain files are not content candidates for the final answer. Their unverified
excerpts previously prompted unsupported descriptions and absence claims even
after the gateway correctly marked the association uncertain.

An unavailable or invalid decision runtime supplies bounded inventory/previews
with an explicit limitation. Low-confidence questions with no selected action
fall back to bounded content inspection. Model routing is still fallible: the
current real-model evaluation includes a context-free meeting question whose
scope was misclassified, despite selecting the safe content fallback.

Memory decision subprocesses have a local 120-second budget and are owned by
the Stop control. The cancellation regression exercises the actual invocation,
child termination and removal of the temporary request. The quoted association
judgement has a separate 300-second budget: a real Qwen 20-file batch took
246.550 seconds and exceeded its earlier 180-second allowance. This does not
change the document review limits. Final generation uses
the existing document-turn timeout. Subject extraction has a separate 120-second
allowance and uses the same existing local JSON judgement path. Raw `<think>` blocks are filtered across
stream chunk boundaries before rendering or voice playback, including older
stored responses. Folder turns also request reasoning disabled from the backend.

## Runtime and Laya comparison

This reuses the already installed OpenDecision Python runtime. It adds no Laya
or second Python runtime to the application. It does **not** make the existing
decision runtime Python-free. A portable native replacement remains separate
work.

Laya 0.3.25 was installed with `--no-deps --target` into an isolated temporary
evaluation directory and tested with its cached typed-decision checkpoint.
The same 11-case production-vocabulary routing evaluation passed 10/11 with
OpenDecision and 5/11 with Laya. The separate Laya source probe also selected
unrelated Person Y and Xavier records. Laya has not replaced the application
decision model. These are local results, not general benchmark claims.
The routing gate measures scope and required operations, allowing additional
operations; it does not grade the newer named/topic branch. That branch also
has observed errors (a single-name Ada request was classified as a topic).
The literal quote check and subsequent evidence checks remain necessary.

## Reproduce

```sh
make test-memory-harness
python3 tests/test_samosa_decision.py
node tests/test_chutni_ui.mjs
make test

# Existing local decision environment; real checkpoint, not a stub:
/path/to/OpenDecision/.venv/bin/python tests/decision/evaluate_memory.py \
  --engine opendecision --output /tmp/memory-routing.json

# Isolated app, real backend, newly generated retained fixture:
SAMOSA_HOME=/path/to/isolated/home SAMOSA_TEST_URL=http://127.0.0.1:18642 \
  python3 tests/test_memory_harness_live.py --backend qwen \
  --fixture-root "$HOME/Documents/Samosa Walkthrough/New General Fixture" \
  --output /tmp/memory-live.json
```

The live suite uses changed names, duplicate basenames, content-only person
references, a seven-page PDF, a similarly named different person, an absent
person, a semantic topic and an empty folder. It exercises the public chat API rather than replaying expected model
answers. Real browser and model evidence, including failures, lives in
[`2026-10-03-decision-harness`](regressions/file-workflows/2026-10-03-decision-harness/).
These checks do not close the separate mixed-document review or scheduling gates.
