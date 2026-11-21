# samosa-agent 0.1.0

Native C11 headless conversations, evidence questions, finding and checked effects.
`make test`, `make sanitize`, `make release`. No Python. libc/POSIX and system
SQLite are runtime dependencies. Source bytes remain
owned by Sources; provider details remain owned by Inference. Agent builds alone.

Run with host-configured public argv JSON arrays:

```
build/samosa-agent --sources-command '["/installed/samosa-sources","--state","/private/source-state"]' --inference-command '["/installed/samosa-inference"]'
```

Pass one public question request on stdin; see contracts/question-v1.md.
Commands are passed directly to execvp. No request controls command/root/state.
`make test-interaction SOURCES_EXECUTABLE=/external/samosa-sources
INFERENCE_EXECUTABLE=/external/samosa-inference` qualifies separate executables.
No implicit sibling fallback. Implemented bounded subsets are described below;
remaining effects, fair admission, assembled quality and App gates remain open.
Existing production paths stay until
qualified replacements permit retirement.

S05 durable conversations: host adds --state /private/agent-state --owner alice.
Public create/get/submit/get_run/events requests retain messages, exact selected
scope and copied evidence. Follow-ups omit selectors and need no Sources command;
completed exact retries need no dependency command. Read
contracts/conversations-v1.md for revisions, runs, quotas and recovery. Native
separate-CLI workflow: `make test-conversation-native` with configured external
Sources/real Inference executables and LLAMA_ENDPOINT. Store tests include actual
reserved process SIGKILL, live peer refusal and successful admissions to quota.

S06b: successful turns persist public provider/context/session/revision refs.
Unknown/stale handles recreate the same provider once. Actual rendered input is
counted with at most10 passes and explicitly reported oldest-history omissions;
literal evidence is retained. `compact` takes conversation_id, expected_revision
and operation_id. It makes bounded marked excerpts of owned messages, retains
original messages and evidence, resets the session handle but keeps provider pin.
It uses neither Sources nor Inference and retries exactly. Subsequent replay
includes the derived summary once. This is an explicit lossy history operation.
`make test-session-native` requires the external dependencies listed in Makefile.
Hybrid engine hot reuse requires checkpoints covering the unchanged prefix;
qualification bounds4 checkpoints/min spacing64/ubatch64/cache256MiB/context4096.

S08a public helper calls self-exec a native guardian before spawning the configured
argv. A private lifeline closes when Agent exits; guardian stops its target group
and reaps its direct child. The same path handles timeout/oversized output without
shell interpolation. Executable users of the transport library must dispatch
command_guardian_dispatch at entry as main/tests do. No extra installed binary.
This owns per-call helpers; externally configured long-lived services stay separate.

S09c `find_files` takes job_id/query/max_inspect1..3. It searches Sources' public
index, reads exact pinned versions, verifies literal matches and returns qualified
selections with evidence and uninspected counts. No inference is required for
explicit literal matching; previews cannot prove matches and selection grants no
file action authorization. Document questions/finding share the evidence validator.
See contracts/finding-v1.md. `make test-find-interaction SOURCES_EXECUTABLE=/external/
samosa-sources` qualifies independently configured native public components.

S10a ordinary `chat` and durable `submit_chat` share the exact budget/provider/
session/run path with document questions. They require no Sources command and
return empty source citations/evidence. Persisted {kind:chat} scope permits
ordinary submit follow-up; explicit document selectors switch back. Failed switch
keeps last-good context. Real native session runner now also checks arithmetic4
and remembered7319 hot/cold/compact without Sources. See contracts/chat-v1.md.

S11a checked copy: host grants --action-root/--action-host-id. Create an immutable
plan from qualified version/mechanical identity, approve its exact revision1, then
execute. Eight files<=1MiB,32 plans/128 operations, atomic no-clobber publication,
per-file durable intent/receipts and actual SIGKILL reconciliation. No model is
required. Public get distinguishes committed historical receipts from uncertain
work. See contracts/actions-v1.md. `make test-actions-interaction
SOURCES_EXECUTABLE=/external/samosa-sources` tests public indexing/finding/approval/
copy/readback without opening another component's storage.

S11b1 checked copy undo creates a separate immutable inverse plan with its own
approval, then uses the same executor. Native no-replace quarantine and verified
inode/version checks preserve foreign replacements. Actual SIGKILL tests cover
capture and deletion before receipt; recovery preserves later files. Private
identity anchors prevent inode reuse. Action state and destinations must share a
filesystem; unsupported cross-device publication refuses before effects. Public
interaction also checks undo and retained Sources snapshots. Original receipts
retain history and expose reversed=true.

S11b2 checked move uses native no-replace quarantine/publication, retains the
original inode, binds both parent scopes and refuses cross-device capture. An
independent exact approval authorizes undo_move to restore an absent original
name. Capture/publication/inverse receipt crash fixtures preserve foreign later
names; public Sources interaction reads moved and restored content. Qualification
status and remaining assembled gates are recorded in the main verification file.
# Checked effects and Sources observations

Configured Sources receives automatic bounded receipt observations after checked
copy/move/inverse execution. Public execution results preserve physical history
and separately report Source reconciliation or its failure. Immutable plan-root,
host, owner, version and physical identities bind the association. Exact retries
return saved checkpoints offline; a new execution operation refreshes observations
without replaying completed effects. `make test-reconcile test-handoff` and the
external `test-actions-interaction` runner qualify the native handoff. Broader
organization, scheduling, App and retirement gates remain open.

S14a durable cancel_run resolves an owner-scoped operation_id or run_id, records a
cancel request while work remains live, and exposes cancel_requested with run
status. submit workers poll only their own control, reclaim their current owned
helper group and commit cancelled without replacing last-good conversation
context. Terminal success survives late cancellation; exact cancelled retry is
offline. `make test-run-cancel sanitize-run-cancel` proves peer/owner isolation,
process disappearance, no assistant commit, survivor and actual SIGKILL recovery.
See contracts/conversations-v1.md for remaining parent/provider/service gates.

S14b records bounded Inference children with durable parent references and uses
public operation cancellation/status before helper cleanup. Unconfirmed control
remains explicit, including parent death. `make test-child-operations
sanitize-child-operations` covers delayed registration, completion races, forged
parents and unrelated live work. Optional `tests/child_native.sh` runs an owned
managed actual-model provider with the separately built child-native driver.
The model is read in place; the runner creates a private owned engine copy.
See docs/S14b.md and the main checkpoint for qualification evidence.

Durable submit/submit_chat support a shared timeout_ms budget (1..60000), bound
into retry identity. Qualified Source reads and Inference count/generate receive
remaining time. Expiry records deadline_exceeded without changing last-good
assistant state or claiming user cancellation. `make test-budget sanitize-budget`
checks Source correlation, metadata removal and honest external-service outcomes;
`make test-run-cancel sanitize-run-cancel` also exercises durable deadline/retry.
