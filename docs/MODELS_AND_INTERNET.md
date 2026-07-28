# Models and Internet sources

The Samosa app can run any of these local backends:

- **Bonsai 27B 1-bit** through Prism's Metal-enabled `llama-server`
- **Ornith 9B** (DeepReinforce Ornith-1.0-9B, Q4_K_M GGUF) through the same `llama-server`
- **Qwen3.6 35B A3B** through Samosa's streaming expert engine

Choose the model under **Settings → Model**. Samosa unloads the old backend
before starting the new one, so both models are never resident at once. A switch
starts a new conversation because the models use different tokenizers and
conversation-state formats. Bonsai is text-only unless a compatible vision
projector is installed; Qwen retains the app's existing image support.

The gateway discovers Bonsai at:

```text
~/.samosa/backends/prism-llama.cpp/build/bin/llama-server
~/.samosa/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf
```

These can be overridden with `SAMOSA_BONSAI_SERVER` and
`SAMOSA_BONSAI_MODEL`.

Ornith is discovered at `~/.samosa/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf`
(override with `SAMOSA_ORNITH_MODEL`); it runs through the same Prism
`llama-server` binary as Bonsai. Ornith is a reasoning model — with thinking
on, it can spend most of a small token budget inside its reasoning trace, so
raise the max-tokens setting or switch thinking off for short answers. It is
text-only as installed.

## Internet sources

> **Status.** Built and landed 2026-07-28 (Phase W, then Phase WK). Read
> [regressions/web-search/report.md](regressions/web-search/report.md) and
> [regressions/web-search/keyless-2026-07-28/report.md](regressions/web-search/keyless-2026-07-28/report.md)
> before relying on any claim here: together they list exactly what was run on
> the reference Mac and what was not.
>
> **Verified live on the reference Mac:** page reading, and search through the
> default keyless provider (real network, no stub, 8 results).
> **Not verified:** the four *keyed* presets — nobody here holds credentials
> for those, so they remain transcribed from each vendor's published API. And
> **no real model has ever emitted a planner decision**; every tool-loop test
> to date had the decision supplied by a fake backend.
>
> An earlier version of this section described all of this as already shipped
> and "verified live against a config-defined provider". That was false: none
> of it existed in the C source at any commit. See the T3.2 evidence doc.

Samosa asks once, on first use, whether it may reach the internet at all.
Until you answer, nothing goes out and web turns cost nothing — no extra model
call, no added latency. You can change the answer any time in Settings, and
`SAMOSA_OFFLINE=1` overrides it outright.

With access on, the composer's **+** menu carries two web actions:

- **Web page** — paste one public `http(s)` URL to read for this message.
- **Web search** — let the model decide whether to search, and for what.

A turn that uses neither is byte-for-byte identical to a turn from before this
feature existed: no extra model call, no added prompt text, no latency.

Once a turn opts in, the **model decides** what to do with the access. The
gateway asks it, in a separate stateless call, whether the turn needs
`web_search` (search the public web) or `open_url` (read one public page); the
model answers with a single JSON line. The gateway runs the tool, feeds the
findings back, and asks again — up to 3 tool calls per turn. Tool activity
appears in the thinking area as it happens. There is no keyword trigger: within
an opted-in turn, "find an IMAX theater near Clemson" works the same as
"search for …". The planner prompt carries the host's current local date.

Fetched page text is treated as untrusted input, labelled as such to the model,
and never rendered as HTML.

Samosa blocks private, loopback, link-local, transition, multicast, and
reserved addresses; resolves each host itself and pins curl to the validated
IP; disables curl's own redirect following and revalidates every hop; rejects
credentials in URLs and non-standard ports; allows only `http`/`https`; limits
responses to 5 MB and page text to 120 KB; and times out after 20 seconds. It
honours `robots.txt` for page reads. A page that needs JavaScript is reported
as unreadable rather than passed on as navigation chrome.

**Be aware of what is not tested:** DNS rebinding against a hostname whose
second resolution is private is defended against by IP pinning but has no test,
because that needs a controlled resolver this repo does not have.

## Connecting a search service

**Search works out of the box, with no account and no API key.** The default
provider is [Parallel](https://parallel.ai)'s Search MCP, which answers
anonymous requests. Nothing is installed for this and no code is vendored: it
is an ordinary HTTPS call over the same pinned transport everything else uses.

What that costs you, stated plainly: **your search text is sent to Parallel.**
Not your chats, not your files, not the model — only the words being searched
for, and only after you have said yes. Pages found in the results are then
fetched by your own Mac directly. If you would rather no third party see search
text at all, decline the prompt or set `SAMOSA_OFFLINE=1`; page reading and
everything else still work.

Two honest caveats about a free tier:

- **Parallel publishes no rate limits** for keyless use ("light use"). Heavy
  use may start failing, and Samosa will say so rather than degrade silently.
- **A free tier can be withdrawn.** Brave's free search tier was deleted in
  February 2026. That is exactly why the keyed presets below are kept.

### Using your own provider instead

Name one in `~/.samosa/config.json` and it replaces the default. Credentials
never leave your machine except to the service you configured. The key is
passed to `curl` through a `0600` config file, never on a command line, so it
is not visible to other processes on your machine; it is also kept out of logs
and out of any error the model or the browser sees. Samosa never ships a shared
API key.

Presets exist for `brave`, `tavily`, `serpapi`, `google` (Programmable
Search), and `searxng` — name one and supply only its credentials. (Note that
`brave`'s free tier ended in February 2026; it now requires a card on file.)

```json
{
  "search": {
    "provider": "brave",
    "providers": {
      "brave":   { "api_key": "YOUR_BRAVE_KEY" },
      "tavily":  { "api_key": "YOUR_TAVILY_KEY" },
      "serpapi": { "api_key": "YOUR_SERPAPI_KEY" },
      "google":  { "api_key": "YOUR_GOOGLE_KEY", "cx": "YOUR_ENGINE_ID" },
      "searxng": { "base_url": "https://your-searxng.example" }
    }
  }
}
```

**Any other HTTP JSON search API** can be described declaratively — no code
changes. `{query}` is the URL-encoded search text; every other `{name}`
placeholder resolves from the provider's own config values; `results` is a
dot-path to the result array in the response; `fields` maps title/url/
description within one result. A `body` object makes the request a POST. A
`description` field may point at an **array of strings** as well as a string;
an array is joined, which is how the default provider's multi-passage excerpts
are handled.

```json
{
  "search": {
    "provider": "my-service",
    "providers": {
      "my-service": {
        "url": "https://api.example.com/v2/search?q={query}",
        "headers": { "Authorization": "Bearer {api_key}" },
        "api_key": "YOUR_KEY",
        "results": "data.hits",
        "fields": { "title": "name", "url": "link", "description": "summary" }
      }
    }
  }
}
```

Every other `{name}` placeholder must resolve to a value in that provider's own
config. An unresolved one is an error, not an empty string — a blank
`Authorization` header would send a credential-less request to a third party
and come back looking exactly like an outage. A provider that is present but
incomplete reports as *not configured*, and says which value is missing.

Redirects are not followed on a search request: resending your key to whatever
host a `3xx` names is the classic way to leak it. A failing provider is
reported, never silently swapped for another — there is no fallback path to
swap to.

The config file is re-read per request, so adding a key or flipping `offline`
takes effect without restarting the app.

**Verification status of the presets:** the generic executor is exercised
offline by `make compiled-gateway-test` (URL/body/header construction,
placeholder resolution, dot-path result extraction, field mapping, and the
credential-handling rules above).

- The **default `parallel` preset was verified live** on the reference Mac on
  2026-07-28 — real network, real `curl`, no stub, 8 results with titles, URLs,
  and joined excerpts. Its request and response shapes are therefore observed,
  not transcribed.
- The **four keyed presets** still follow each vendor's published API and
  **have not been observed working**: no credentials for any of them exist on
  the machine this was built on. A wrong field name in one of those would not
  have been caught.

Keyless access was verified from **one** IP address. Providers grant anonymous
access per-IP and can refuse it — Firecrawl's keyless tier refused this same
machine outright — so it is not guaranteed to work from every network.

Set `SAMOSA_OFFLINE=1` before starting the app, or set `"offline": true` in
the config file, to disable every outbound path — page reads, search, and the
scheduled public-web inputs Jobs uses. The environment variable wins over the
file. `GET /v1/web/config` reports which mode is active, and the composer
disables both web actions and says why.

## Context and model switching

Only one backend runs at once. Model switching stops the old process first.
Each GGUF conversation ledger is keyed by both chat ID and backend, preventing
Bonsai and Ornith from silently sharing incompatible model-facing state.

Auto context is backend-aware:

- Qwen uses Samosa's memory/KV calculation.
- Prism fits Bonsai and Ornith separately because model weights and K/V costs
  differ. The resulting capacities therefore need not match.

## Compaction privacy

The active local model creates continuation memory. No cloud summarizer is
used. Samosa atomically stores the smaller replacement under
`~/.samosa/chats`; the browser transcript remains visible and local.
