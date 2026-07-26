# E-JO1 Field Organize Checkpoint — 2026-07-21

Scope: field-based organize using the live Ornith extraction records from
`docs/regressions/jobs/e-j1/ornith-text-2026-07-21/output.jsonl`.

This tests the deterministic JO field-rule half after an Ornith live-model
extraction gate. It does not use a second model call during organization.

## Scratch Folder

```text
/tmp/samosa-ejo1-field-20260721145922/inbox
```

The folder contains scratch copies of the 10 labeled text receipts. The live
Ornith records were remapped to these scratch paths and annotated with
`status:"passed"` plus each scratch file's SHA-256 so the organizer could treat
them as validated extraction results.

## Rule

```json
{"by":"field","field":"date"}
```

Each receipt should move into a folder named for its extracted date.

## Results

```text
Plan:  10 moves, 0 skips, 0.001 s
Apply: 10 applied, 0 skipped, 0.023 s
Undo:  10 reverted, 0 skipped, 0.020 s
```

Move quality against hand labels:

```text
Precision: 10 / 10 (100.0%)
Recall:    10 / 10 (100.0%)
```

Hash/path safety:

```text
after plan:  hash multiset identical
after apply: hash multiset identical
after undo:  hash multiset identical
before paths == after undo paths: true
```

Evidence files in this directory:

```text
records.json
01_plan.json
02_apply.json
03_undo.json
00_before.json
01_after_plan.json
02_after_apply.json
03_after_undo.json
summary.json
```

Result: passed for date-field organization over the Ornith text extraction
checkpoint.
