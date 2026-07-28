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

> **Status.** Built and landed 2026-07-28 (Phase W). Read
> [regressions/web-search/report.md](regressions/web-search/report.md) before
> relying on any claim here: it lists exactly what was run on the reference
> Mac and what was not. In particular, **no search provider has ever been
> contacted from this machine** — nobody here holds credentials for one — so
> the five presets below are transcribed from each vendor's published API and
> are unverified. Page reading was verified live.
>
> An earlier version of this section described all of this as already shipped
> and "verified live against a config-defined provider". That was false: none
> of it existed in the C source at any commit. See the T3.2 evidence doc.

Nothing reaches the network unless you ask for it on that turn. The composer's
**+** menu carries two web actions:

- **Web page** — paste one public `http(s)` URL to read for this message.
- **Web search** — let the model decide whether to search, and for what.
  Only offered when a search provider is configured.

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

**There is no default search provider, on purpose.** No key-free search API is
dependable enough to hardcode, and scraping one would be disallowed by that
host's own `robots.txt` — which Samosa honours. With nothing configured,
`web_search` reports that it is unconfigured and the model tells you how to fix
it; `open_url` needs no credentials and works either way.

To give your local models search, connect a service you have credentials for in
`~/.samosa/config.json`. Samosa never ships a shared API key, and credentials
never leave your machine except to the service you configured. The key is
passed to `curl` through a `0600` config file, never on a command line, so it
is not visible to other processes on your machine; it is also kept out of logs
and out of any error the model or the browser sees.

Presets exist for `brave`, `tavily`, `serpapi`, `google` (Programmable
Search), and `searxng` — name one and supply only its credentials:

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
description within one result. A `body` object makes the request a POST.

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
credential-handling rules above). The preset request/response *shapes* follow
each service's published API and **have not been observed working** — no
credentials for any of them exist on the machine this was built on. A wrong
field name in a preset would not have been caught.

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
