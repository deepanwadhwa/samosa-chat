# File workflow implementation inventory

Reviewed 2026-10-01 against the current working tree. Acceptance and pending
work are tracked in [the workflow tickets](TASKS_FILE_WORKFLOWS.md); executable
evidence is in [the connected-workflow record](regressions/file-workflows/2026-10-01-connected/README.md).
Automatic memory continuation reviewed 2026-10-02.

| Path or control | Disposition and reason | Removal condition |
| --- | --- | --- |
| Per-file question button and deep-reading labels in the shortlist | Removed; the saved selected set has one Ask action and explicit reader coverage. | Removed already. |
| Dynamic Jobs source-memory disclaimer and score-based sort variants | Removed in the 2026-10-02 UI cleanup. Static source details describe eligible saved-text reuse; ordinary progress uses plain labels. Existing per-file diagnostics remain available. | Removed already; rendered UI acceptance remains pending. |
| Semantic folder-name pruning before inventory | Removed; generic directory names hid relevant files. Policy bounds and exclusions remain. | Removed already. |
| Serif display styling and separate color overrides | Replaced in existing CSS with system typography and neutral colors. | Browser layout and keyboard qualification remains pending. |
| Selected-file organization preview | One `/v1/jobs/selection/plan` path builds an immutable reviewed copy/move plan. | Retained as the selected-workflow entry point. |
| `/v1/jobs/apply` and `/v1/jobs/undo` | Shared execution handlers serve selected plans and older job plans; the current UI calls both. Durable journals support recovery. | Retain while saved jobs or current recipes use them; require explicit compatibility migration before deletion. |
| Older sort/classify/report and folder-watch recipes | Retained; the current Run job UI still dispatches these through `/v1/jobs/run` and scheduling APIs. They are live callers, not safe dead-code deletions. | Review their product scope and saved-job migration separately before retiring. |
| Attachment reader and selected-file reader | Reuse the existing extraction/cache machinery. Selected references add identity validation and per-call labels; uploads remain an independent supported input. | Retained because both inputs are live. |
| Chutni enrichment and selected evidence | Exact full-document extraction cache can satisfy bounded selected reads. Arbitrary portable summaries are not promoted to evidence. | Broader passage reuse requires source identity and reader-contract qualification. |
| Folder parsing to memory | Existing folder recipes hand off to the single durable Chutni worker. Waiting requests live in existing job state, and registered exclusions are retained. Automatic builds skip optional model summaries/captions. | Retained; unregistered portable stores still require registration. |
| Later Jobs PDF previews | Reuse complete matching host-reader entries after checking current bytes and reader fingerprint; uncertain/incomplete entries fall back to extraction. | Retained; no second cache or index service was added. |
| Folder-memory conversation context | Retained for folder questions; switching to it starts a separate conversation. Selected-task chat rejects mixed scope. | Retained while folder questions remain supported. |
| Legacy identity-incomplete selections | Refused for evidence rather than silently reset or treated as current files. Existing saved records remain readable. | Explicit user reselection establishes current identities. |
| Private staging, receipt and operation-lock files | Retained for recovery and ownership checks; unknown staging is not deleted automatically. | Cleanup needs a proven ownership/liveness contract; lock identity must remain stable across processes. |

Static caller review supports these dispositions. It does not qualify browser
interaction, saved-work migration for every historical format, or complete
retrieval accuracy. FW-6 remains open until those acceptance gates pass.
