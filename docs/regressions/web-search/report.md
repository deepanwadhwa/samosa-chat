# Phase W — web search and page reading: what was built and what was verified

Date: 2026-07-28 · Branch: `ui-chutni` · Spec: [docs/TASKS_WEB_SEARCH.md](../../TASKS_WEB_SEARCH.md)

Machine: the reference 16 GB M3 MacBook Air, macOS (Darwin 25.5.0), arm64,
APFS. **The only machine any of this has run on.**

## Why this phase exists

[MODELS_AND_INTERNET.md](../../MODELS_AND_INTERNET.md) and
[SERVE_API.md](../../SERVE_API.md) described a shipped internet feature —
model-invoked `web_search`/`open_url`, three `/v1/web/*` routes, a declarative
multi-provider search executor — and claimed it was "verified live against a
config-defined provider" and "exercised by `make test`". None of it existed in
the C source at any commit on any branch (found on `ui-chutni`, 2026-07-27; see
[t3.2-evidence.md](../ui-chutni/t3.2-evidence.md)). T3.2 removed the dead
frontend half. The owner's decision on the rest was **build it**.

## Verified on this machine

### Live, against the real public internet

Transcript: [live-fetch-2026-07-28.txt](live-fetch-2026-07-28.txt) — a real
compiled gateway, no stub seam, real DNS, real TLS.

| Case | Result |
|---|---|
| `GET /v1/web/config`, nothing configured | `fetch_available:true`, `search_configured:false`, with the reason |
| `POST /v1/web/fetch` → `https://example.com/` (text/html) | title and body text extracted |
| `POST /v1/web/fetch` → `rfc-editor.org/rfc/rfc9110.txt` (text/plain, 120 KB+) | read, capped at `MAX_PUBLIC_TEXT_BYTES`, `truncated:true` |
| `POST /v1/web/search`, nothing configured | `409 search_not_configured` naming the missing config |
| Chat turn with `web_urls` | page read, two SSE activity events, evidence reached the turn |

The gateway log after the run contains one line (its own startup banner) — the
check that led to silencing curl's stderr, which had been putting fetch
diagnostics there and would put a key-bearing URL there for the query-string
presets.

### Offline, in `make compiled-gateway-test`

[tests/test_web_search.sh](../../../tests/test_web_search.sh), also run under
`-fsanitize=address,undefined` (clean, exit 0):

- **SSRF, 14 cases, all blocked** — loopback, `169.254.169.254`, RFC1918,
  `192.168/16`, CGNAT, `[::1]`, `::ffff:127.0.0.1`, `fd00::/8`, `fe80::/10`,
  decimal-encoded `2130706433`, non-standard port, `file://`, `gopher://`,
  URL credentials. Run against a gateway with the stub transport **off**,
  because that seam short-circuits ahead of the resolver and would otherwise
  make every one of these pass for the wrong reason.
- **Route auth** — all three `/v1/web/*` routes 401 without the UI token.
- **Executor** — dot-path result extraction (`web.results`), field mapping
  (`content`→`description`), `{query}` URL-encoding, `{api_key}` resolution
  from provider config, POST-body providers, JSON-escaping of a quoted query
  into a POST body, `javascript:` result URLs dropped.
- **Unresolved placeholder** — reported as not-configured, names the missing
  key, and **no request is sent**.
- **Provider URL is SSRF-checked** — a `searxng` `base_url` of `127.0.0.1` or
  `169.254.169.254` is blocked; a non-standard port is refused.
- **Credential handling** — with a configured key, the key does not appear in
  curl's argv (the test records the child's full argument vector, which is what
  a local `ps` reader sees), does not appear in the gateway's stdout/stderr
  logs, does not appear in `/v1/web/config`, and does not appear in the chat
  stream. The config file carrying it is `0600`.
- **Tool loop** — `web: true` drives a planner round, the chosen tool runs, and
  the evidence reaches the answering turn; a decoy URL inside a `<think>` span
  is not acted on; a second planner round sees the accumulated findings.
- **The gate** — a turn with neither `web` nor `web_urls` is a byte-for-byte
  passthrough, identical to pre-Phase-W, with no planner call and no SSE
  preamble. Asserted by exact string equality on the response.
- **Offline kill switch** — via `config.json` and via `SAMOSA_OFFLINE=1`
  (env wins over file); blocks all three routes **and** the Jobs public-input
  fetcher, and the transport is never invoked.

### Regression suites re-run, all exit 0

`make test`, `make compiled-gateway-test`, `make test-ui-setup`,
`make jobs-test`, `make doc-read-pdf-paging-test`.
`test_compiled_gateway.sh` and `test_attachments.sh` were additionally re-run
under ASan/UBSan — they cover the pre-existing Jobs public-input fetcher, which
now runs through the rewritten transport.

## NOT verified — stated, not glossed

- **No search provider was ever contacted.** No credentials for Brave, Tavily,
  SerpAPI, Google Programmable Search, or a searxng instance exist on this
  machine. The executor was exercised end-to-end against a fake `curl` that
  runs the real request-building code, so **the five presets' request and
  response shapes are transcribed from each vendor's published API and have
  not been observed working.** A wrong field name in a preset would look
  exactly like this evidence looks.
- **No real model ran a tool loop.** Every planner round in these tests was
  answered by `tests/fake_openai_backend.c`. What is proven is that the gateway
  builds the planner prompt, parses a JSON decision out of a reply wrapped in
  `<think>` and a code fence, runs the tool, feeds findings back, and splices
  evidence. What is **not** proven is that Qwen3.6, Ornith, or Bonsai actually
  emits a usable decision. `E-I1`'s upstream stage passed at 0% malformed on
  FP8 ([tool-call-validation](../tool-call-validation/report.md)); its local
  stage was never run, and this loop uses a JSON-line protocol rather than
  native tool calls, so that result does not transfer directly.
- **DNS rebinding is not tested.** The pinning that defeats it is implemented
  (`--resolve` to a pre-validated IP, `--max-redirs 0`, per-hop revalidation),
  but proving it needs a controlled resolver, which this repo does not have.
  `TASKS_INTERNET.md` E-I2 asked for one; it was never built.
- **No browser.** All frontend coverage is Node DOM fixtures
  (`tests/test_composer_ui.mjs`). No click-through in Safari, Chrome, or
  Firefox; no screenshots — `screencapture` remains non-functional in this
  environment.
- **No cost measurement.** `TASKS_INTERNET.md` E-I4 asked what a web turn
  actually costs end to end (fetch + extract + prefill). Not run. The estimate
  in the docs is still A3's arithmetic from page size and measured prefill
  rate, not a measurement of this code.

## Defects found and fixed while building

1. **`samosa_http_json_error()` did not escape its message.** A comment
   asserted every caller passes a fixed string with no JSON metacharacters.
   Phase W's errors quote configuration keys, so an unescaped `"` produced
   malformed JSON and gave config- and server-controlled text a route into the
   error body. Fixed in `src/samosa_http.h` for every caller rather than by
   avoiding quotes in the new messages.
2. **`{base_url}` was percent-encoded.** The `searxng` preset is
   `{base_url}/search?q={query}`; encoding the prefix turned `https://host`
   into `https%3A%2F%2Fhost` and the URL never parsed, so that preset could
   never have worked. URL-prefix placeholders are now inserted raw after being
   validated by the same parser the assembled URL goes through.
3. **The composer's Directory item lost its disabled state.** Dropped while
   editing `applyCapabilities()`; it would have rendered enabled with no click
   handler. Caught by `tests/test_composer_ui.mjs`.
4. **curl's stderr reached the gateway log.** Some curl diagnostics quote the
   request URL, which for the serpapi and google presets carries the API key in
   its query string. `show-error` removed; nothing consumed it.

## Design decisions worth re-reading before extending

Recorded in full in [TASKS_WEB_SEARCH.md](../../TASKS_WEB_SEARCH.md) — D1 (no
keyless default provider), D2 (credentials never in `argv`), D3 (the tool loop
runs in the gateway, not over the backend's `tools` API, because Qwen's
`--serve` drops message history), D4 (fetched text is untrusted input).
