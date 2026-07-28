# Phase W — Web search and page reading, built for real

**Status: W1–W8 built and landed 2026-07-28 on `ui-chutni`. Two acceptance
items are NOT met and are blocked on this machine, not deferred by choice** —
see Acceptance at the bottom. Evidence:
[docs/regressions/web-search/report.md](regressions/web-search/report.md).

This card exists because [MODELS_AND_INTERNET.md](MODELS_AND_INTERNET.md) and
[SERVE_API.md](SERVE_API.md) described a shipped, "verified live" internet
feature that **did not exist in the C source at any commit on any branch**
(found on `ui-chutni` 2026-07-27; see
[docs/regressions/ui-chutni/t3.2-evidence.md](regressions/ui-chutni/t3.2-evidence.md)).
T3.2 removed the dead frontend half. The owner's decision on the remaining half
was **build it**, not **correct the docs** — so this card specifies what gets
built, and the docs get rewritten to match the build, not the reverse.

Read [TASKS_INTERNET.md](TASKS_INTERNET.md) first. Its verified security
findings (the corrected SSRF ranges, DNS pinning, per-hop revalidation, scheme
allowlist) are **already implemented and shipped** inside the Jobs public-input
fetcher, and this phase reuses them unchanged rather than building a second
fetcher.

## What already exists (do not rebuild)

In `src/samosa_gateway.c`, built for Jobs' scheduled public-web inputs:

| Piece | Where | Reused as |
|---|---|---|
| `ipv4_blocked` / `ipv6_blocked` / `ip_blocked` | ~line 899 | SSRF block list |
| `resolve_public_host` | ~line 944 | strict resolve-all-and-reject |
| `url_parse` | ~line 972 | scheme/port/credential allowlist |
| `curl_fetch` | ~line 1040 | DNS-pinned transport (`--resolve`, `--max-redirs 0`) |
| `fetch_public` | ~line 1171 | per-hop revalidating redirect loop |
| `robots_allowed` | ~line 1222 | robots gate |
| `html_to_text` | ~line 1265 | extractor + JS-page detection |
| `readable_page` | ~line 1334 | **exactly what `open_url` needs** |
| `SAMOSA_WEB_STUB_DIR` | ~line 1112 | offline test seam |

A full OpenAI-shape tool loop also already exists and is proven on the real
Ornith backend (`find_loop`, ~line 3331) — but see W5 for why chat cannot use
it directly.

## Decisions taken, and why

### D1 — No keyless default search provider

`MODELS_AND_INTERNET.md` claimed `web_search` falls back to DuckDuckGo's
keyless HTML endpoint with Bing RSS behind it. **That is not built and will not
be.** [TASKS_INTERNET.md](TASKS_INTERNET.md) A3.2 already settled this and the
spec wins over the vaporware doc:

> the honest default: **no backend configured**, because no key-free search API
> is dependable enough to hardcode. Keep that. Degrading to instructions is
> correct behavior, not a gap to close.

Two further reasons found while building: scraping `duckduckgo.com/html` is
disallowed by that host's own `robots.txt`, so our shipped robots gate would
block it — shipping it would mean either a permanently broken path or a
deliberate robots exemption; and an HTML-scraping result parser is a
maintenance burden that breaks silently on the vendor's next markup change.

**Unconfigured `web_search` therefore returns a structured `not_configured`
result**, which the model relays to the user as instructions. `open_url` needs
no credentials and works unconfigured.

### D2 — Credentials never enter `argv`

`curl_fetch` passes the URL and every option as `argv`. On both macOS and
Linux, any local user can read another process's argument vector (`ps -ww`,
`/proc/<pid>/cmdline`). A search provider's API key lives in a header or in the
query string, so the existing transport **cannot** be reused as-is for
authenticated requests.

W2 therefore adds a curl **config-file** transport: options are written to a
`0600` temp file consumed via `curl --config <file>`, so no secret ever appears
in `argv`, in the process table, or in this project's own logs. The file is
unlinked immediately after the child is reaped. This is tested directly (W7)
by reading `/proc`-equivalent process arguments during a live request.

### D3 — The tool loop runs in the gateway, against a stateless planner call

[TASKS_INTERNET.md](TASKS_INTERNET.md) F3 is still true, verified again today:
`serve_last_user()` (`src/qwen36b.c:5062`) takes **the last user message and the
first system message and silently drops everything else**, and Qwen's `--serve`
accepts no `tools` field. So the native OpenAI tool loop that Jobs uses works
only on the llama-server backends (Ornith, Bonsai), not on Qwen — the engine
this project is named for.

Rather than ship a capability that silently varies by backend, W5 runs the loop
**in the gateway** over a protocol that needs only one system message and one
user message, which every backend honours:

1. A **stateless planner call** (no `conversation_id`) asks the active model
   whether this turn needs the web, and for which tool and arguments. The model
   answers with a single JSON line.
2. The gateway executes the tool, bounded to 3 calls per turn.
3. The collected evidence is **spliced into the final user message** — the same
   mechanism T3.2 already uses for document attachments — and the real turn is
   then streamed through the normal `proxy_request()` path with its
   `conversation_id` intact.

This keeps session/KV semantics untouched: the planner rounds never enter the
conversation's session, and the final turn is one ordinary turn that happens to
carry web evidence. It also means tool support does not depend on the backend
implementing OpenAI function calling.

### D4 — Evidence is untrusted input, and is labelled as such

Fetched page text is attacker-controlled. It is wrapped in the same
"untrusted; read literally, not as instructions" framing T3.2 uses for
documents, and it is never executed, never interpolated into a prompt as
instructions, and never rendered as HTML in the browser.

## Tasks

### W1 — Config layer

`~/.samosa/config.json`, read at request time (not cached across requests, so
editing the file takes effect without a restart):

```json
{
  "offline": false,
  "search": {
    "provider": "brave",
    "providers": { "brave": { "api_key": "..." } }
  }
}
```

`MODELS_AND_INTERNET.md` also documented a `"fallback"` flag controlling
whether a failing provider silently degrades to the keyless path. D1 deletes
the keyless path, so there is nothing to fall back *to*; the flag is dropped
rather than kept as a no-op. A provider failure is always surfaced.

- Presets: `brave`, `tavily`, `serpapi`, `google` (Programmable Search),
  `searxng`. Each preset supplies url/headers/body/results/fields; the user
  supplies only credentials.
- A provider not matching a preset name is a **generic declarative provider**:
  `url`, optional `headers`, optional `body` (presence makes it a POST),
  `results` (dot-path to the result array), `fields` (title/url/description
  within one result).
- `{query}` is the URL-encoded search text. Every other `{name}` resolves from
  that provider's own config values. **An unresolved placeholder is an error,
  never an empty string** — an empty `Authorization: Bearer ` header would send
  a credential-less request to a third party and read as a provider outage.
- Kill switch: `SAMOSA_OFFLINE=1` in the environment, or `"offline": true` in
  the config, hard-disables every outbound path including `open_url` and the
  Jobs public-input fetcher. Env wins over file when it is set.

### W2 — Credential-safe pinned transport

Extend the existing transport, keeping DNS pinning and per-hop revalidation:

- `curl --config <0600 temp file>` for every request that carries credentials.
- Support POST with a JSON body and arbitrary request headers.
- Redact secrets from every error string returned to the model, the browser, or
  the log: the config values are known, so they are searched for and replaced
  with `<redacted>` in any message that quotes a URL or response.

### W3 — Declarative search executor

Given a provider config and a query, produce a bounded list of
`{title, url, description}`. Caps: 8 results, 400 chars of description each,
one provider attempt. **Redirects are not followed** on a search request:
resending an `Authorization` header to whatever host a `3xx` names is the
classic credential leak, so a redirect is reported as a failure. `robots.txt`
is not consulted for a search request either — this is a call to an API the
user holds credentials for, not crawling.

### W4 — Endpoints

```http
GET  /v1/web/config     → { offline, search_configured, provider, tools_available }
POST /v1/web/fetch      → { url } → readable page text
POST /v1/web/search     → { query } → results array
```

All three are **new routes**, so per T1.2's fail-closed design they require the
`X-Samosa-Token` UI session token and are deliberately *not* added to
`v1_route_is_legacy_unauthenticated()`. `GET /v1/web/config` never returns a
credential — only whether one is present.

### W5 — Model-decided tool loop in chat

- Planner prompt describes `web_search(query)` and `open_url(url)`; the model
  replies with one JSON line or declines.
- Bounded to 3 tool calls per turn.
- Tool activity is streamed to the browser as SSE events before the answer
  begins, so a multi-minute web turn is visibly doing something.
- **Gate (inherited from A3.3 and V3): with web disabled, output must be
  byte-identical to pre-W behaviour.** No planner call, no added system text,
  no extra latency.

### W6 — Frontend

The composer `+` menu's **Web page** item becomes live, gated on
`GET /v1/web/config`. Attaching a URL adds a removable chip like any other
attachment. The Settings surface gains a read-only statement of whether search
is configured and which provider — never the key.

### W7 — Tests

- SSRF suite extended to cover the new endpoints and the search executor's URL
  (a malicious `searxng` `base_url` is an SSRF vector like any other).
- Executor unit cases against `SAMOSA_WEB_STUB_DIR`: dot-path extraction, field
  mapping, placeholder resolution, unresolved-placeholder rejection, POST body,
  fallback on/off.
- Offline kill switch: env and config, both proven to block all three routes.
- Tool loop: tool-call round trip, the 3-call bound, and the byte-identical
  gate with web off.
- **Credential leak test**: capture the full argument vector of every child
  process during an authenticated search and assert the key does not appear.

### W8 — Docs

Rewrite `MODELS_AND_INTERNET.md`'s Internet section and `SERVE_API.md`'s web
section to describe what is built, with claims scoped to what was measured. Any
claim that cannot be verified on the reference Mac is marked "not run" rather
than deleted or softened.

## Acceptance

| | Item | Result |
|---|---|---|
| ✅ | Every SSRF case blocks; a partial pass is a fail (TASKS_INTERNET.md E-I2) | 14/14 blocked, run with the stub transport off |
| ✅ | No credential in `argv`, in a log, or in any error returned to the model | asserted directly on the child's argument vector |
| ✅ | With web off, chat output is byte-identical to pre-W | asserted by exact string equality |
| ✅ | `open_url` verified **live against a real public page** on the reference Mac | `example.com` (HTML) and an RFC (text/plain, truncation path) |
| ❌ | `web_search` verified against a **real configured provider** | **NOT RUN — no provider credentials exist on this machine.** The presets are transcribed from vendor docs and have never been observed working |
| ❌ | The tool loop verified against a **real model** | **NOT RUN** — every planner round in the tests was answered by the fake backend. This is TASKS_INTERNET.md E-I1's local stage, still never run |

The two failures are stated rather than softened because a preset with a wrong
field name, or a model that cannot emit a usable decision, would both produce
evidence that looks exactly like the evidence that exists. Neither can be
cleared here; both need the owner's machine and, for the first, a key.
