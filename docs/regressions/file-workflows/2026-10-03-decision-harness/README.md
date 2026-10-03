# Folder/file decision harness — 2026-10-03

Host and installed binary hashes are in `run.json`. This is a local macOS arm64
acceptance record; CI and other platforms were not run. Documents are generated
fixtures retained under `~/Documents/Samosa Walkthrough`. Chats and scopes use
`/private/tmp/fw6-walkthrough-home`, API port 18642 and backend port 18643.
The user's PDF and existing conversations were not opened.

## What changed

The existing Jobs OpenDecision adapter chooses folder/file scope and independent
read-only overview, inventory, metadata and content operations. Subject membership
checks every supplied inventory record, requires a literal quoted proof, validates
full named identities or independently checks semantic-topic quotes, and computes
counts in C. Qualified paths resolve duplicate basenames. Persisted reader metadata
supplies PDF page totals. Incomplete previews remain uncertain. Current literal
content takes precedence over summaries; duplicate artifacts are removed from the
answer context. Prior assistant answers are never evidence.

The composer has no blue inner focus box; keyboard focus remains visible on its
outer container. Thinking is suppressed in model requests and filtered across
stream boundaries before rendering or speech. Folder cards have one Ask action,
accurate partial/error wording and visible normal Documents fixture paths.

## Negative results preserved

- `ornith-browser-earlier-failures.json`: failures during earlier qualification.
- `ornith-browser-answers.json`: seven earlier automatic checks passed, but their
  multipart gate was too weak: manual review found an unsupported unique-PDF
  assumption. The later browser test requires ambiguity to be explained.
- `qwen-overview-overstrict-test.json`: an assertion incorrectly required an
  arbitrary project name despite a grounded category summary. The corrected
  overview test checks actual subjects and categories.
- `ornith-missing-subject-failure.json`: before grounded identity proofs, an
  absent person was incorrectly associated with differently named people.
- `ornith-absence-coverage-failure.json`: the new matcher correctly returned
  zero confirmed matches and one uncertain PDF, but the final answer still
  overstated absence. The coverage instruction and acceptance gate were tightened.
- `ornith-filename-interpretation-failure.json`: automatic checks passed, but
  manual review found the model calling two known basename matches unconfirmed.
  Exact inventory path resolution now bypasses association judgement.
- `ornith-overview-overstrict-test.json`: a grounded summary omitted a person's
  name. The revised overview gate requires project, shopping and gardening
  categories, rather than prescribing which names a short summary must repeat.
  `ornith-general-answers.json` retains a later missing-file assertion failure:
  the answer correctly said "no confirmed match", which the original regex
  failed to recognize. Only that language gate was corrected.
- `absolute-proof-score-failure.log`: a generic absolute relevance predicate
  rejected known positive topic evidence. `topic-proof-probe.log` records the
  successful natural topic hypothesis and negative controls, rather than hiding
  the failed predicate.
- `qwen-browser-timeout-failure/` and its trace/runtime logs: the first original
  multipart browser run failed. Its 180-second association deadline expired,
  although the native Qwen backend finished that check in 246.550 seconds.
  The browser's 600-second overall deadline also expired before the answer
  finished. The new association allowance is 300 seconds; the browser test
  allows 900 seconds for routing, verification and final generation together.
  Document review still uses the retained 150-second call and six-minute limits.

## Decision-model comparison

`opendecision-routing.json` and `laya-routing.json` use the same 11 questions,
production vocabulary and 0.5 action gate: OpenDecision 10/11, Laya 5/11. The
metric is expected scope plus all required actions; additional actions are allowed.
It does not grade the named/topic branch, which also has observed classification
errors. OpenDecision misclassifies the scope of a context-free meeting question;
bounded content fallback still provides evidence. Laya's separate source probe
selected unrelated people. These results do not justify replacing OpenDecision.
The existing OpenDecision Python environment is reused; Laya is isolated under
`/private/tmp/samosa-laya-eval`. No Laya dependency is shipped. The existing local
decision runtime is still Python, and a portable/native replacement is separate work.

## Reproduction

```sh
make test-memory-harness
python3 tests/test_samosa_decision.py
node tests/test_chutni_ui.mjs
make test

/Users/deepanwadhwa/Documents/projects/OpenDecision/.venv/bin/python \
  tests/decision/evaluate_memory.py --engine opendecision --output /tmp/routing.json
/Users/deepanwadhwa/Documents/projects/OpenDecision/.venv/bin/python \
  tests/decision/evaluate_memory.py --engine laya \
  --laya-path /private/tmp/samosa-laya-eval --output /tmp/laya.json

SAMOSA_HOME=/private/tmp/fw6-walkthrough-home SAMOSA_TEST_URL=http://127.0.0.1:18642 \
  python3 tests/test_memory_harness_live.py --backend ornith \
  --fixture-root "$HOME/Documents/Samosa Walkthrough/General Harness Qwen" \
  --reuse-scope 556a286809767ef7c35d43f641aff87f \
  --empty-scope 4a937476a37450b042743cd8d8c11529 \
  --case membership --case ambiguous_basename --case missing_subject \
  --case semantic_topic --output /tmp/ornith.json

SAMOSA_PLAYWRIGHT_ROOT=/private/tmp/fw6-browser \
  SAMOSA_TEST_URL=http://127.0.0.1:18642 SAMOSA_TEST_EVIDENCE=/tmp/browser \
  node tests/test_memory_harness_browser.mjs
```

Final installed qualification is in progress. Prior Qwen general and installed
answers are retained separately, rather than presented as evidence for changes
made after those runs. Timings include overlapping local regression load and are
not controlled benchmarks. The mixed-document review and broader FW-6 gates
remain open; scheduling stays gated.

The final local `make test` exited 0, including the clean runtime-only installer,
inventory parity, compiled gateway, decision-contract and UI checks. No CI run
is claimed. `qwen-identity-final.json` records the final installed Qwen membership
and missing-subject answers: 3 confirmed matches and 0 confirmed matches,
respectively, with uncertainty disclosed in the negative answer. Their observed
times were 280.872 and 235.050 seconds. `tests/assert_memory_trace.py` independently
checks executed actions and the gateway's validated unique counts, preventing a
plausible final answer from hiding a missing verification step.
