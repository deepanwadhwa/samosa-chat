# Latest-chat reliability audit — 2026-08-11

Branch: `codex/latest-10-chat-analysis-20260811`

## Scope and evidence limits

Samosa keeps the message text in browser `localStorage` (`samosa-chat-v2`), not
under `~/.samosa/chats`. The local in-app browser bridge was unavailable during
the initial audit, and no exported Samosa backup was present. The user later
provided four labelled transcript excerpts for the cyclosporiasis failure;
the files labelled Chat B and Chat C are byte-identical (SHA-256
`fb619b5c6eb87377f5f7f8d057ee3412d12bd12d3727812d79c3759858260db6`).
Those excerpts support the web-research findings below, but cannot be reliably
mapped back to all ten server-side bindings.

The ten records below are the latest *created* server-side conversation
bindings, ordered by the `metadata.json` filesystem timestamp. They are not a
reliable latest-activity ordering: `updated_at` is currently written only when
the binding is created and none of the UUIDs are included in the runtime logs.
The findings correlate model bindings, backend timing/crash evidence, installed
binary layout, and shipped source. Per-conversation answer-quality review still
requires the browser state or an exported backup.

## Latest ten server-side bindings

| Created (America/New_York) | Conversation | Model |
|---|---|---|
| 2026-08-11 18:02:13 | `chat-550430d2-f3f4-4d87-9f4b-8e214461eadb` | Bonsai 27B Q1_0 |
| 2026-08-11 17:58:48 | `chat-1ce2f3ae-bca6-462a-8345-8e6c90a64e9d` | Ornith 9B Q4_K_M |
| 2026-08-11 17:54:30 | `chat-53308b42-600a-456c-a964-ccbe55570deb` | Maple |
| 2026-08-11 17:46:20 | `chat-5818b4a0-36b9-4f2f-bd66-709e3bee247a` | Maple |
| 2026-08-11 16:17:17 | `chat-cf465541-b6fc-4b99-8680-8279146012e9` | Maple |
| 2026-08-11 15:50:22 | `chat-4d776762-513d-4c04-af1f-892bacef36f4` | Maple |
| 2026-08-11 15:42:26 | `chat-e9685d78-f912-41db-918d-67fe69358dc1` | Maple |
| 2026-08-09 17:50:10 | `chat-3821918f-931b-4850-b467-72dbd14efe40` | Maple |
| 2026-08-09 17:48:52 | `chat-af3efa37-5da3-4b5d-8660-0fa87a4e7f03` | Maple |
| 2026-08-09 17:46:47 | `chat-812573bd-81a4-4d27-9aed-db13aa899550` | Maple |

Model distribution: Maple 8, Ornith 1, Bonsai 1.

## Transcript-derived web-research findings

### Page selection was hard-coded, not model-scored

The old gateway assigned scores in C based almost entirely on the hostname:
CDC/FDA/NIH/SEC/IRS received 120, another `.gov` received 100, and `.edu`
received 60. It did not score topical relevance. That is why a generic CDC
surveillance page beat a South Carolina page and articles whose titles already
contained a South Carolina count.

Fix in this branch: delete the domain score and ask the active local LLM to
rank the returned candidates against a self-contained version of the exact
question. The ranking contract explicitly includes subject, geography, time,
and requested number/detail, with primary-source preference used only after
direct relevance.

### Retrieval stopped because a page was readable, not because it answered

The old high-stakes retrieval loop set `authority_read = 1` after the first
successful HTML extraction. It never checked whether the fetched text supplied
the requested state-level fact. This exactly reproduces the transcript: the
CDC page was readable, contained national data, and retrieval stopped even
though it lacked the South Carolina answer.

Fix in this branch: after every fetch, a second bounded local-LLM judgement
checks whether the fetched title/text actually covers the requested subject and
scope. A merely readable or generally authoritative page is insufficient, so
the gateway proceeds to the next model-ranked candidate. Page reads remain
bounded to two and 6,000 characters each.

### Follow-up query planning could drop the subject

One follow-up was searched as generic words about “details,” “numbers,” and
“safety,” producing unrelated COVID and measles results. The planner already
received recent context, but its output contract did not require an explicit
resolved question or require every query to repeat the subject and geographic
scope.

Fix in this branch: the planner must return both `resolved_question` and
`queries`. The resolved question and every query must preserve the subject,
place, and time qualifiers from the conversation. The ranker and sufficiency
judge use that resolved question rather than the literal follow-up.

### What text is available for the links in “Sources in view”

The search provider returns a title, URL, and description/excerpt for each
result; Samosa bounds the description to 400 characters. Before this fix,
non-high-stakes answer synthesis received that metadata, while high-stakes
synthesis intentionally withheld it because a search excerpt is not verified
evidence. Only the single hostname-selected page was fetched for actual text.

After this fix, the ranker sees all bounded result metadata, and answer
synthesis still treats it only as discovery material. The gateway fetches the
model-ranked pages and supplies their bounded extracted text as evidence. This
uses the information the provider returned without converting an unverified
headline into a medical fact.

### Source presentation duplicated discovery and fetch states

The gateway emitted one source event for the search result and another for the
page fetch. The browser merged only when both URL *and kind* matched, so the
same URL appeared separately as “Found” and “Read,” and the nine-row list stayed
open after the answer.

Fix in this branch: normalize and merge sources by URL across result/page kinds,
retain the strongest state (`Read` is not downgraded by a later `Found` event),
and render the source list as a native collapsible `details` element. It remains
expanded during research, collapses once when the answer completes, and then
respects the user's manual choice.

### Transcript-derived regression

The compiled-gateway fixture now reproduces the salient failure: a generic CDC
page is result 1, a directly relevant South Carolina page with the answer in
its title/text is result 2, and the current message is a referential follow-up.
The test requires the provider query to retain cyclosporiasis, South Carolina,
2026, and safety scope; requires the model ranker to fetch result 2; and requires
the sufficiency judgement to stop there without fetching the generic CDC page.
A companion fixture deliberately ranks a readable generic page first and
requires a false sufficiency judgement to advance to the state page.

The proposed Falconsai T5 summarizer was also evaluated. The existing Prism
runtime can execute T5 without Python, but the available third-party Q4_K_M
artifact produced corrupted output in a real native run and is not wired into
this branch. The verified packaging path and evidence-preservation gates are in
[`NATIVE_SUMMARIZER_PLAN.md`](../NATIVE_SUMMARIZER_PLAN.md).

## Confirmed findings

### 1. Maple requests could crash the entire backend

Severity: high.

`backend.log` contains four uncaught Maple failures at its rotating-cache
boundary. The most specific diagnostic is:

```text
Maple prefill chunk [512,576) cache_offset=576: Maple layer=0
query_len=64 cache_offset=576 mask=64x512: [broadcast_shapes]
Shapes (64,512) and (1,16,64,575) cannot be broadcast.
```

The attention-mask width defect is already corrected in ancestor commit
`4937479` and covered by the native 512+64 cache-boundary regression. However,
the HTTP worker still allowed any generation exception to escape the thread,
which invokes `std::terminate` and takes down Maple for every chat.

Fix in this branch: contain generation exceptions, log the private diagnostic,
return a safe structured stream/API error, and emit an OpenAI-compatible
terminal `finish_reason` (`stop` or `length`) on successful responses.

### 2. A crashed/truncated stream looked like a successful answer

Severity: high.

The browser finalized the assistant message whenever the response body reached
EOF. It did not require either a `[DONE]` sentinel or a non-null
`finish_reason`, and it ignored structured SSE errors. Because Maple sent HTTP
200 streaming headers before inference, a backend crash produced a normal EOF
from the browser's perspective. Blank or partial answers could therefore be
saved without an error.

Fix in this branch: track protocol completion, render structured backend
errors, and turn premature EOF into a visible retryable error while preserving
any partial text.

### 3. Installed PDF reading was broken by a non-relocatable rpath

Severity: high for document chats.

`server.log` contains 268 `Library not loaded: @rpath/libpdfium.dylib`
occurrences. The installed extractor and `libpdfium.dylib` were both in
`release/bin`, but the extractor's sole `LC_RPATH` pointed to the developer SDK
directory, which the runtime sandbox could not read.

Fix in this branch: build the extractor with a loader-relative rpath
(`@loader_path` on macOS, `$ORIGIN` on Linux), repair older locally built
extractors while staging them, and include the optional extractor and library
in the local release hash so a rebuilt sidecar cannot silently reuse a stale
release.

### 4. The latest Bonsai run consumed the full output cap

Severity: UX/performance observation; needs transcript review.

The latest Bonsai backend task processed a 1,777-token prompt and generated
exactly 2,048 tokens in 410.327 seconds. Generation throughput declined from
about 11.8 to 5.48 tokens/second. Hitting exactly the configured cap strongly
suggests a length-limited answer, but without the saved message text it is not
possible to distinguish a genuinely long requested answer from repetition,
rambling, or a missing stop condition. No behavioral change is made from this
evidence alone.

### 5. Chat diagnostics are not currently auditable end to end

Severity: medium, developer experience.

Server metadata has model identity but no messages or outcome, `updated_at`
does not track subsequent turns, and backend logs omit `conversation_id`.
Consequently, logs can establish aggregate failures and timing but cannot
attribute them to one of the eight Maple conversations. A privacy-conscious
follow-up should add explicit user-controlled transcript export and structured
per-turn diagnostics (conversation ID, model, terminal reason, timing, and
error class; never prompt text by default).

## Verification completed

- `node tests/test_web_activity_ui.mjs`
- `tests/test_web_search.sh` against the compiled gateway and fake local model
- `make test-ui-setup` (outside the restricted runner so its localhost fixtures could bind)
- `make samosa-maple`
- real two-token Maple HTTP stream emitted `finish_reason: length` followed by `[DONE]`
- `make test-maple-cache-boundary` (outside the restricted runner so MLX could use Metal)
- `make extract-test PDFIUM_DIR=...` (outside the restricted runner so its nested sandbox test could execute)
- clean-room local development install with no staged models; the installed
  extractor resolved `@rpath/libpdfium.dylib` via `@loader_path` and extracted
  the PDF fixture successfully
- `git diff --check`
