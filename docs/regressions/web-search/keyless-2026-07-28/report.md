# Phase WK — keyless search, and the two Phase W gates it clears

**Date:** 2026-07-28 · **Branch:** `ui-chutni` · **Machine:** the reference
16 GB M3 MacBook Air (macOS 15.5, Darwin 25.5.0, arm64)

Phase W shipped a working search executor that nobody could use without first
obtaining an API key. This phase makes search work on a fresh install with no
signup, and — because the provider that makes that possible is reachable from
this machine — it also clears the first of Phase W's two failed acceptance
items, which had never been run at all.

## What was asked for

> not AGPL · no extra dependency install · no API key for everyday users

## What was surveyed, and what each was rejected for

Every claim below was checked against the live endpoint or the vendor's own
page on 2026-07-28; none is taken from memory.

| Candidate | Verdict |
|---|---|
| **Parallel Search MCP** | **Chosen.** Keyless, answered a real query from this Mac. Hosted HTTPS, so no vendored code (no AGPL) and no install. |
| Firecrawl keyless | **Refused this machine**: `HTTP 403 — "your IP address looks suspicious, so Firecrawl can't be used without an API key from here"`. Keyless-by-IP reputation silently denies users behind CGNAT, VPNs, and shared addresses — the everyday users this phase is for. |
| DuckDuckGo Instant Answer | Keyless and official, but **returned an entirely empty document** for both queries tried (`Abstract:""`, `RelatedTopics:[]`, `Results:[]`). It is an instant-answer endpoint, not a web index. |
| DuckDuckGo `html`/`lite` scraping | Already rejected by Phase W D1 (robots-disallowed, silent markup breakage); independently confirmed to be rate-limited with 202/captcha responses under automated use. |
| Brave Search API | **Its free tier was removed in February 2026** — replaced by $5 metered credits with a card on file and overage billing. The shipped `brave` preset no longer has a free path. |
| SearXNG (self-hosted) | AGPL-3.0 *and* a Docker/Python install: fails both stated constraints. (Calling a third party's instance is not a licence event, but public instances commonly disable `format=json`.) |
| Mojeek / Marginalia / OpenWebSearch.eu | Paid; shared public key with rate limits and no commercial terms; and no public search API launched yet, respectively. |

Brave is the reason this phase does **not** delete the keyed presets: a free
tier can be withdrawn with no notice, and that happened to one of ours five
months ago.

## The live probe that decided it

```console
$ curl -sS -X POST https://search.parallel.ai/mcp \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json, text/event-stream' \
    -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
HTTP/2 200
content-type: application/json
mcp-session-id: 01191fa9-0f25-4777-958c-22263cbf8bf9
```

Three properties made this implementable without a new transport, and all three
were observed rather than assumed:

1. **No `initialize` handshake and no session id are required** — a bare
   `tools/call` succeeds on a cold connection.
2. **The reply is one `application/json` body, not SSE frames**, despite the
   transport being called "streamable HTTP".
3. **`result.structuredContent` carries the parsed object** alongside the text
   rendering, so no second parse of an embedded JSON string is needed.

A real `web_search` call returned HTTP 200 and 60,205 bytes: 10 ranked results,
each with `url`, `title`, `publish_date`, and an `excerpts` array.

## What was built

- **`parallel` preset**, the first that needs no credential, and the default
  when `config.json` names no provider (`WEB_DEFAULT_PROVIDER`).
- **Array-valued description mapping** — `excerpts` is a list of passages, not
  one snippet. Keeping only the first would discard most of what makes these
  results answerable without a follow-up fetch, so the executor joins them.
- **`{session_id}`**, a runtime placeholder alongside `{query}`. Generated once
  per gateway process from `/dev/urandom` and **never written to disk**: the
  provider asks for a stable id per session, and persisting one would create a
  permanent identifier linking every search the install ever makes.
- **Ask-once consent** (`POST /v1/web/consent`, stored as `search.consent`).
  Until it is answered, the tool loop does not run at all — no planner call, no
  latency, byte-identical to a pre-W install.

## Gate results

| | Item | Result |
|---|---|---|
| ✅ | `web_search` verified against a **real configured provider** | **Now met.** Phase W could not run this at all. See below — a live search through the compiled gateway, keyless. |
| ❌ | The tool loop verified against a **real model** | **STILL NOT RUN.** The loop was driven end to end against the *live* provider today, but the planner rounds were answered by the fake backend. No real model has yet emitted a `web_search` decision. This remains `TASKS_INTERNET.md` E-I1's local stage. |
| ❌ | `TASKS_INTERNET.md` E-I4 (end-to-end cost of a web turn) | **STILL NOT MEASURED.** |

### The live search, through the compiled gateway

`live-session.txt` / `live-search.json`. Real `curl`, real network, no stub, no
credential anywhere on this machine:

```
### 1. Fresh install, before consent
{"offline":false,"fetch_available":false,"search_configured":true,"consent":"unset",
 "keyless":true,"web_available":false,"provider":"parallel","reason":""}

### 2. Search refused before consent
{"error":{"message":"Samosa has not been allowed to reach the internet yet.",
          "type":"invalid_request_error","code":"consent_required"}}

### 5. LIVE search against real search.parallel.ai, keyless
ok: True  provider: parallel  n: 8
 - Who is and has been Secretary-General of the United Nations?
   https://ask.un.org/faq/14625
 - Secretary-General | United Nations
   https://www.un.org/sg/en
```

`config.json` after consent, mode `-rw-------`:

```json
{"search":{"consent":"granted"}}
```

### The tool loop, against the live provider

`live-chat-turn.txt` — planner → live search → evidence spliced into the
answering turn:

```
data: {"choices":[{"index":0,"delta":{"reasoning":"Searching the web for \"careers page\"…\n"}}]}
data: {"choices":[{"index":0,"delta":{"reasoning":"Found 8 results.\n"}}]}
{"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"saw web evidence"}}]}
```

The search was real; **the planner decision was not** — the fake backend chose
`web_search` and supplied the query. That is exactly the gap the second gate
above still names.

## Known quality limitation, stated rather than smoothed over

`excerpts` are passages lifted from the page, and the first one is often
navigation chrome rather than substance. From the live run:

```
desc: Toggle navigation [Welcome to the United Nations](https://www.un.org/)  + 中文 ...
```

Titles and URLs are clean, and the model can still pick a page to open, but
these are not editorially-written snippets and should not be described as such.
No filtering was added: guessing at which passage is boilerplate is the kind of
heuristic that fails silently on the next site.

## Offline reproduction

`make compiled-gateway-test` — `tests/test_web_search.sh` section 3b pins the
preset against the response shape recorded here (dot-path, excerpts join,
JSON-RPC envelope, `{session_id}` resolution and stability, no invented
`Authorization` header), and sections 1/1b/4e cover the consent states. Both
suites build with `-Werror`; `make test` passes in full.

## What can still break without anything here catching it

- **Parallel changing its response shape or withdrawing keyless access.** The
  fixture in 3b pins what we built against; it cannot detect that the live
  service has moved. Only re-running the live check does.
- **Whether keyless access works from other people's IP addresses.** It worked
  from this one. Firecrawl refused this same machine, which is direct evidence
  that keyless access is granted per-IP and can be denied — so this is a real
  risk, not a theoretical one, and it is unverifiable from a single machine.
- **Rate limits.** Parallel publishes no numbers for the free tier ("light
  use"). Handfuls of queries were sent today without a 429; sustained use is
  untested.
