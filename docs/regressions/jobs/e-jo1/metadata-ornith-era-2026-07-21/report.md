# E-JO1 Metadata Organize Checkpoint — 2026-07-21

Scope: model-free metadata organize checkpoint after the Ornith-first decision.
No model call is involved in this half; it exercises the deterministic
plan/apply/undo/reapply path through the current Jobs layer and `samosa-fs`
move/undo sidecar.

## Scratch Folder

```text
/tmp/samosa-ejo1-meta-20260721145708/inbox
```

Constructed from a scratch copy of repo files:

```text
117 top-level entries
1 nested regular file
1 symlink
1 deliberately wrong-extension duplicate: wrong_extension.jpg
```

Current gateway Jobs organize runs non-recursive. The nested regular file was
retained and included in hash/path inventories, but not planned for movement.
The wrong-extension duplicate was skipped by content-dedup discovery.

## Run

Sequence:

```text
run_job("organize this folder by type", mode="confirm")
apply_job(job_id)
undo_job(job_id)
run_job("organize this folder by type", mode="confirm")
apply_job(job_id_reapply)
```

Results:

```text
Plan:    115 moves, 0 plan skips, 0.049 s
Apply:   115 applied, 0 skipped, 0.307 s
Undo:    115 reverted, 0 skipped, 0.275 s
Re-plan: 115 moves, 0 plan skips, 0.047 s
Reapply: 115 applied, 0 skipped, 0.266 s
```

## Hash Inventory Law

Regular-file content hash multiset:

```text
before:        117 files
after plan:    117 files, hash multiset identical
after apply:   117 files, hash multiset identical
after undo:    117 files, hash multiset identical
after reapply: 117 files, hash multiset identical
```

Undo path restoration:

```text
before paths == after undo paths: true
```

Evidence files in this directory:

```text
00_before.json
01_after_plan.json
02_after_apply.json
03_after_undo.json
04_after_reapply.json
01_plan.events.jsonl
02_apply.events.jsonl
03_undo.events.jsonl
04_replan.events.jsonl
05_reapply.events.jsonl
summary.json
```

Result: passed for the current model-free metadata organize surface. Remaining
E-JO1 coverage: true recursive/nested organize behavior if that is promoted to
the product surface, plus the field-based organize half after the live Ornith
extraction gate is expanded beyond text.
