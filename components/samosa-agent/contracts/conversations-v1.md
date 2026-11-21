# Native conversation contract version 1

Host config --state (owner-only private Agent directory) and --owner (trusted host
identity) never come from request JSON. Every request op/contract_version=1,
unknown fields refuse. CLI owner configuration is not a substitute for later
public service authentication; that gate remains open.

- create: returns random conversation_id/revision0.
- get: conversation_id; consistent snapshot returns revision, selected_scope,
  admitted_evidence and ordered messages. Reads cannot interrupt a live worker.
- submit: conversation_id,expected_revision,operation_id,question; optional
  source_id/version and exactly one start/end byte or page range. Initial scope
  required. Omit selector to reuse last-good scope/evidence without Sources.
- get_run: exactly one run_id or operation_id; owner-scoped durable result.
- cancel_run: exactly one run_id or operation_id; durable owner-scoped request.
- events: conversation_id, optional after (global event sequence), up to100
  ordered committed events. Each status/revision agrees with its transaction.

Accepted submit/get_run response: contract_version,run_id,operation_id,
conversation_id,status,cancel_requested,input(normalized request),result. Status
running/completed/failed/interrupted/cancelled. Completed result has revision/answer/citations/copied evidence/
usage. Failure/interrupted result has typed error/revision when known. Run input
retains selection intent even if it failed; last-good conversation scope/evidence
update together only on successful assistant commit. No duplicate persisted
model_request trace. A completed exact retry returns the identical original
result before dependencies/current revision checks. A lost process's exact retry
recovers interrupted (or cancelled after a durable cancel request) under exclusive
work lock, never replays it.

Limits: one active/zero queued host work,64 conversations,256 runs,512 messages per
conversation,512 events, question2KiB, result1MiB,8MiB total semantic bytes,
16MiB database, history assembly64KiB. Reserve3MiB semantic headroom before
accepting work; quota failure does not append a user/run/event. No eviction of
pinned versions/receipts. Streaming/paged large history/service admission and full
native-session lifecycle are later gates, not claims of this bounded CLI subset.

Errors: unknown_conversation/unknown_run,stale_revision,idempotency_conflict,
selection_required,invalid_selector,busy,storage_limit,state_unavailable,
store_busy/store_failed/resource_limit/context_history_limit and policy errors
in terminal run results. A failed read/answer still preserves the durable user
message and intent; it does not publish an assistant answer or new admitted scope.

S06b adds `continuation` (public provider/context/session/revision reference) and
`context_policy` (history/kept/omitted counts and session_replayed) to successful
results. `get` exposes continuation and clearly marked derived_summary alongside
all original messages. Only successful generation replaces last-good continuation.
`compact` accepts contract_version/op/conversation_id/expected_revision/operation_id.
It is idempotent, increments revision once, emits a completed run/event, retains
messages/evidence, stores bounded derived message excerpts and resets only the
opaque handle. Provider/context stay pinned. No dependency calls occur. Excerpts
are lossy, explicitly marked and refer to owned message sequence; they are never
source evidence. Subsequent typed history starts with that summary once plus new
messages. This prerelease native schema adds owner-local context fields; legacy
chat databases are not accepted or migrated by this interface.

Durable cancellation: cancel_run accepts contract_version=1, op=cancel_run and
exactly one operation_id or run_id, resolved only within the host-configured
owner. Unknown/foreign runs refuse. A running run gets a durable cancel_requested
flag without acquiring the live worker's work lease. get_run and cancellation
responses expose this flag alongside authoritative status. Terminal runs are not
changed by later requests; cancellation is a request, not a completion claim.

The submit/submit_chat worker polls its own control record through per-dispatch
configuration. On cancellation it closes only the current owned helper lifeline,
waits for that guardian's group cleanup, and commits status=cancelled with a typed
result error. It does not append an assistant message or replace last-good scope,
evidence or continuation. The admitted user message/revision remains historical.
The last cancel observation and terminal publication share one SQLite write
transaction, so a committed success cannot be retroactively cancelled. Exact
terminal retries return the saved run result offline. Exclusive work-lease proof
recovers a dead running worker as cancelled when cancellation was already durable;
otherwise it recovers as interrupted. No PID is used as lease authority.

When Inference advertises operations, cancellation and operation_parents, Agent
records each count/generate child before calling it and binds its parent to the
durable run_id. Run responses expose up to16 children with operation_id, kind,
status and cancellation. Exact child cancellation uses public operation_cancel
and operation_get while the original helper remains alive. Only matching child
and parent references qualify an observation. Cancellation reports
observed_cancelled/completed/failed/interrupted when terminal status is observed;
unavailable, forged or unfinished control remains unconfirmed. Parent death
recovery also preserves unfinished children as unconfirmed. Helper cleanup alone
does not prove long-lived provider cancellation. Completion races preserve the
observed child outcome, independently of the parent's final semantic status.

Source parent/deadline envelopes, concurrent workers and authenticated service
admission remain required gates. Host callbacks are scoped configuration, not
public callable function pointers or a shared global cancellation target.

S14c2: submit/submit_chat optionally accept timeout_ms as an integer1..60000.
Receipt-time monotonic budget covers admission and all dependency calls/retries;
an earlier host deadline wins. Budget is part of operation retry identity.
Finished exact retries remain offline even with an expired host deadline.
Expiry before admission rolls back; expiry after admission records a failed run
with deadline_exceeded, preserves the admitted user/revision and last-good
assistant/evidence/continuation, and does not set cancel_requested.
Timed dependency work requires request_budgets capability. Inference count and
generate receive remaining relative time; qualified Source reads also receive
exact parent/child references and durable child observations. Source references
are removed before evidence admission. Helper teardown does not prove an
external Source service stopped: absent a valid response, outcome is unconfirmed.
Public Inference cancellation/status cleanup retains its separate bounded window.
Cooperative native/syscall checks do not promise absolute kernel/fsync deadlines.
