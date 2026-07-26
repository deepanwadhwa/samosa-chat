# Enter/Send Approval Boundary

Status: spec, 2026-07-21.

This freezes the approval boundary for future outward-action tools: email,
forms, uploads, posting, calendar invites, messages, or any action that sends
data to another person, service, website, or account. It extends the Jobs
Apply boundary to network/account actions without adding any connector yet.

## Principle

The model may retrieve, compare, classify, transform, draft, and ask the user.
It may not send, submit, post, upload, invite, buy, delete remote data, or
change account state without an explicit user approval step immediately before
the action.

Approval is not a prompt instruction. It is a runtime state transition:

```text
model proposes outward action -> app records a draft -> await_send
user reviews exact target + payload -> /send executes once
```

No model turn may execute the final send directly in confirm mode.

## Scope

Outward actions include:

- Sending email, chat, SMS, or comments.
- Submitting forms.
- Uploading or attaching files.
- Creating or editing remote records.
- Scheduling or modifying calendar events.
- Publishing posts or repository changes.
- Any authenticated request with user-account side effects.

Read-only web tools such as `web_search` and `open_url` are not outward
actions, but they must stay SSRF- and public-web constrained as they are today.

## Event Contract

Outward-action jobs use the existing event stream shape with a new pause event:

```json
{"type":"draft_send","channel":"email","target":"person@example.com","subject":"...","body":"..."}
{"type":"await_send","job_id":"...","draft_id":"...","channel":"email"}
{"type":"sent","draft_id":"...","ok":true}
```

`await_send` is the only event that can enable a UI send button. It must include
a stable `draft_id`; the actual draft payload is read from job state so the UI
and endpoint approve the same bytes.

## Draft State

Drafts live under the job directory:

```text
<job_dir>/drafts/<draft_id>.json
```

Each draft contains:

```json
{
  "draft_id": "...",
  "created_at": "2026-07-21T00:00:00Z",
  "channel": "email",
  "tool": "email_send",
  "target": "person@example.com",
  "payload": {"subject": "...", "body": "..."},
  "attachments": [],
  "evidence": [{"path": "receipt.pdf", "reason": "source of invoice number"}]
}
```

The payload must be exact. The user approves the actual target, body, fields,
attachments, and account identity, not a summary.

## Tool Modes

Every outward tool has two conceptual operations:

```text
email_draft / form_draft / calendar_draft     non-mutating; writes draft state
email_send  / form_submit / calendar_create   outward; execute only after approval
```

In confirm mode, a model call to an outward execution tool is staged as a draft
and returns `await_send`. In execute mode, outward tools still require approval
unless a future owner-approved policy explicitly permits automatic sends for a
specific low-risk channel. File `execute` is not network/account `execute`.

## User Review Requirements

The UI must show, before Send:

- Account or browser profile that will act.
- Destination or form origin.
- Exact payload.
- Attachments and their source paths.
- Any hidden fields or inferred values if available.
- A clear destructive/irreversible warning when applicable.

The send button label must name the action, such as `Send email`, `Submit form`,
or `Create event`. Generic `Continue` is not enough for outward actions.

## Endpoint Shape

Future endpoints should mirror Jobs apply:

```text
POST /v1/jobs/send {"job_id":"...","draft_id":"..."}
```

The endpoint reloads the draft from disk, validates that it is still pending,
executes the connector/sidecar once, appends `sent`, and marks the draft
consumed. Replaying the same `draft_id` must be idempotent: return the recorded
result, not send twice.

## Connectors and Sidecars

The connector-specific implementation is deliberately separate from this spec.
Allowed implementation shapes:

- Browser automation against a visible user-controlled browser session.
- Account connectors with explicit OAuth/session ownership.
- A compiled sidecar for constrained local protocol work.

Disallowed:

- Model-generated shell, Python, JavaScript snippets, or arbitrary HTTP.
- Hidden sends from read-only tools.
- Storing credentials in job state.
- Sending with a guessed account identity.

## Tests Required Before Implementation

Minimum fake-connector tests:

- Draft-only path emits `draft_send` and `await_send`; no connector call occurs.
- `/v1/jobs/send` executes exactly once and records `sent`.
- Duplicate `/send` for the same `draft_id` does not send twice.
- Missing/changed draft fails closed.
- Preview/chat contexts refuse outward tools.
- Ask-user resume can produce a draft, but still pauses at `await_send`.

Live tests require an owner-approved sandbox account or local test page. No
real recipient, production form, or external post should be used as the first
live checkpoint.

## Non-Goals

- No email, form, or calendar connector is added by this spec.
- No automatic outward action policy is approved here.
- No model-generated programs or arbitrary HTTP requests.
- No release/HF publishing behavior.
