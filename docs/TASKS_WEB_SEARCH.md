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

### D1 — No keyless default search provider — **REVERSED 2026-07-28, see Phase WK**

> **This decision no longer holds.** It was correct about every option that
> existed when it was written, and wrong by 2026-07-28: a keyless provider now
> exists that needs no scraping and no robots exemption. Phase WK (bottom of
> this card) supersedes it. The original reasoning is kept because the *rejected*
> options are still rejected for the reasons given.


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

> **The first of those two was cleared on 2026-07-28 by Phase WK**, which found
> a provider needing no key and therefore no owner credential. The second — a
> real model emitting a planner decision — is still not run.

---

# Phase WK — search that works without an API key

**Status: WK1–WK4 built and landed 2026-07-28 on `ui-chutni`.** Evidence:
[docs/regressions/web-search/keyless-2026-07-28/report.md](regressions/web-search/keyless-2026-07-28/report.md).

Phase W left search working only for someone willing to obtain an API key. The
owner's requirement is narrower and harder: **not AGPL, no extra dependency
install, and no key for everyday users.** WK meets it.

## Why D1 could be reversed

D1's conclusion — "no key-free search API is dependable enough to hardcode" —
rested on the only keyless options then available being *scrapes* of an HTML
endpoint whose own `robots.txt` forbids it. That is still true of DuckDuckGo.
What changed is that vendors began offering **anonymous access to a real search
API** as a way to get agents onto their platform.

The survey, with each rejection's cause, is in the evidence report. The short
version: Firecrawl's keyless tier **refused the reference Mac by IP**;
DuckDuckGo's Instant Answer API returns an empty document for ordinary queries;
Brave **deleted its free tier in February 2026**; SearXNG is AGPL and an
install. Parallel's Search MCP answered a real query from this machine with no
account, and is a hosted HTTPS endpoint — so nothing is vendored (no AGPL
exposure) and nothing is installed.

## WK1 — The keyless default provider

- A `parallel` preset, and `WEB_DEFAULT_PROVIDER` so it applies when
  `config.json` names no provider. **The four keyed presets stay.** A free tier
  can be withdrawn — Brave's was, five months ago — so the documented escape
  hatch to a provider the user controls is part of the design, not a leftover.
- No silent fallback chain between providers. Phase W's D1 deleted silent
  fallback for a reason that still holds: it makes an outage indistinguishable
  from a misconfiguration.
- Two executor gaps had to close, both driven by the real response shape:
  `result.structuredContent.results` (already reachable — `json_dotpath`
  handles nesting), and a **description field that is an array of passages**
  rather than a string, which the executor now joins.

## WK2 — Ask once, then remember

Search that needs no setup means the first web-touching turn would otherwise
send a user's words to a company they were never told about, from an app whose
entire pitch is that nothing leaves the machine. So consent is explicit,
asked once, and recorded in `search.consent`.

- `POST /v1/web/consent {granted: bool}` merges into `config.json` — every
  other key, including the user's API keys, is copied through verbatim, and the
  write is atomic and `0600`.
- **Unset ≠ denied.** Unset means ask; denied means never ask again. A
  two-state boolean could not express that, so it is a string.
- **With consent unanswered the tool loop does not run at all** — no planner
  call, no added latency, byte-identical to a pre-W install. This is W5's gate,
  re-asserted for the state every new install now starts in.
- `open_url` and the composer's Web page action are gated too: reading a page
  is an outbound request like any other.
- **Jobs' scheduled public inputs are deliberately *not* gated by this.** Those
  URLs are already an explicit per-job action the user typed, and retroactively
  disabling a working scheduled job on upgrade would be a regression wearing a
  privacy feature's clothes. `SAMOSA_OFFLINE` still covers everything.

## WK3 — `{session_id}`

A runtime placeholder beside `{query}`, generated per gateway process from
`/dev/urandom` and **never persisted**. The provider asks for a stable id per
session for rate-limiting; a value written to `config.json` would instead be a
permanent identifier linking every search the install ever makes.

## WK5 — Our own daily cap on the free tier

The provider publishes **no** rate limit for anonymous use — "light use" is the
whole specification. Shipping an app with no bound of its own onto a free tier
is how the free tier stops being free, and how one runaway loop gets an IP
refused for everyone behind it.

- **100 keyless searches per local calendar day**, counted in
  `<home>/web-usage.json`, configurable as `search.daily_limit` (`0` = no cap).
- **Only the keyless default is counted.** A user who supplied their own key
  has their own quota and their own bill; rationing that would be us limiting
  something we do not pay for.
- The budget is taken **before** the request, not after a success, so a
  provider failing slowly cannot be retried without bound. A failed call still
  costs a unit — the point is that what we send a free service is bounded no
  matter what happens.
- At the cap: `429 search_daily_limit` and a message saying it resets tomorrow
  or that a key removes it. **This is deliberately not a 409** — nothing is
  misconfigured and there is nothing for the user to fix today.
- A missing, corrupt, or stale-dated counter reads as zero. A broken counter
  file must never lock someone out of a working feature.
- `GET /v1/web/config` reports `searches_today` and `daily_limit`; the UI
  mentions it only inside the last quarter of the budget, because "3 of 100
  used" would make a cap nobody reaches look like a limitation.

## WK6 — No verbatim repeat of a tool call within a turn

Found by the first real-model run, not by review. Ornith 9B, asked the price of
a Raspberry Pi 5, searched, opened `raspberrypi.com/products/raspberry-pi-5`,
got `HTTP 403` — and then asked for **the same URL again**, spending the turn's
last of three tool calls on a page that had just failed.

The findings block already said it had failed. The model repeated it regardless,
so a firmer prompt is not the fix. `web_already_tried()` refuses a verbatim
repeat of any `open_url` URL or `web_search` query within a turn and tells the
planner to choose differently or stop. Exact string comparison, not normalised:
stopping a literal repeat is the goal, and deciding that two different-looking
URLs are "the same" is a judgement this has no business making.

Regression test: `tests/test_web_search.sh` 4f, driven by a fake planner that
asks for the identical URL every round — reproducing what the real model did.

## WK4 — Frontend

A one-time card above the composer — not a launch-time modal, which is the kind
of consent people click through unread. Either button is an answer; there is no
dismiss, because a card that can be ignored gets ignored and the feature then
silently never works. Settings gains a plain-language statement of what is and
is not sent, plus a toggle that reverses the choice.

## Acceptance

| | Item | Result |
|---|---|---|
| ✅ | Search works on a fresh install with no key, no account, no install | live through the compiled gateway; 8 results |
| ✅ | Not AGPL, no new dependency | hosted HTTPS over the existing pinned curl transport; nothing vendored |
| ✅ | `web_search` verified against a real provider (**Phase W's first failure**) | **cleared** — real network, real curl, no stub |
| ✅ | Nothing outbound before consent is granted | asserted on the transport's own argv: no call made |
| ✅ | Consent write preserves an existing API key and unrelated keys | asserted against a populated `config.json` |
| ✅ | With consent unset, `web:true` is byte-identical to a plain turn | asserted by exact string equality |
| ✅ | The tool loop verified against a **real model** | **Met on Ornith 9B**, the backend installed on this machine. 6 web turns, **0 malformed planner replies**, searched for time-sensitive questions and correctly declined for static ones. Found one real defect → WK6. [real-model-planner.md](regressions/web-search/keyless-2026-07-28/real-model-planner.md) |
| ⚠️ | `TASKS_INTERNET.md` E-I4: what a web turn costs end to end | **Partially measured**: 86 s and 122 s for two web turns, 42 s for a declined one, on Ornith 9B. Not a controlled with/without comparison, and not on Qwen |
| ⚠️ | The loop on **Qwen** specifically | Not run. W5's protocol is backend-independent by construction, but that is an argument, not a measurement |
| ⚠️ | Keyless access works from **other** IP addresses | **Unverifiable from one machine.** It worked from this one; Firecrawl's keyless tier refused this same machine, so per-IP denial is a demonstrated failure mode, not a hypothetical |
| ⚠️ | Free-tier rate limits | Parallel publishes no numbers ("light use"). Handfuls of queries today drew no 429; sustained use is untested |
