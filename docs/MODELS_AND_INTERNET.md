# Models and Internet sources

The Samosa app can run any of these local backends:

- **Bonsai 27B 1-bit** through Prism's Metal-enabled `llama-server`
- **Ornith 9B** (DeepReinforce Ornith-1.0-9B, Q4_K_M GGUF) through the same `llama-server`
- **Maple** (2-bit MoE) through Samosa's native Metal engine
- **Qwen3.6 35B A3B** through Samosa's streaming expert engine
- **VisionPsy-Nano 460M** (auxiliary vision model, macOS arm64) through Samosa's native MLX C++ helper (`samosa-visionpsy`)
- **Molmo2 4B Native Q4** (optional auxiliary image/video model, macOS arm64)
  through the on-demand MLX/AVFoundation helper (`samosa-molmo2`)

Choose the model under **Settings → Models**. Samosa unloads the old chat backend
before starting the new one, so two chat models are never resident at once.
VisionPsy-Nano 460M operates as the compatibility/fallback image specialist when
Molmo2 is not installed, loaded on demand for visual turns and evicted immediately after synthesis. It is
pinned to commit `ca7a1bad19a08fe8e474bee24275a2e377bbabba` from
[`KaedeTai/VisionPsy-Nano-460M-MLX`](https://huggingface.co/KaedeTai/VisionPsy-Nano-460M-MLX)
and runs in the bundled `samosa-visionpsy` MLX C++ helper. The application does
not install or invoke Python, `mlx-vlm`, Transformers, Pillow, or PyTorch.

### VisionPsy artifact identity

The first release uses the standard model, not VisionPsy Flash, and the BF16
precision actually published in that conversion. The verified download is
1,018,322,795 bytes across these files:

| File | SHA-256 |
|---|---|
| `model.safetensors` | `0cc93c1027a58aec26c4350115b5eca02a836aba06277fe700de854ff078572f` |
| `config.json` | `f25b85a4aef0df1867e0221310b86d87058de9ea6fadc50e993a2de5b08851a8` |
| `preprocessor_config.json` | `7d039134cbf96a4daa956e1aa256b604975cc6184bfdce9911105fddabbb39da` |
| `processor_config.json` | `a09a8dd0ad8e9ef92b5e2f2a1f78df1aee12f40ff87c3e0f7d3c7877a26e718d` |
| `tokenizer.json` | `9da3f64c3f7b940520c48fc81dc203149080934d4c7f52201883d24d0cd390a8` |
| `tokenizer_config.json` | `926bab5b0063ec24731caa635cc60df8d76ad3efa1b92dbc4a82912aca96a63d` |
| `chat_template.jinja` | `153280e3ff55d19da1398bdb3914ee2a51b80429bfaedde11d7d216c39db80f3` |

The upstream model is [`qvac/VisionPsy-Nano-460M`](https://huggingface.co/qvac/VisionPsy-Nano-460M)
and its catalogue licence is Apache-2.0. Samosa records both the upstream model
and KaedeTai conversion identities; a mutable `main` URL is never used for an
install.

### VisionPsy network and repair behavior

Samosa contacts Hugging Face only after the user chooses **Download** or
**Download and continue**. The model manager supports resumable `.partial`
files, verifies every size and hash, and atomically promotes only the complete
set. After installation, visual inference is fully local and works with model
downloads/network access disabled. Ordinary text and OCR-only turns neither
download nor start VisionPsy.

If verification or installation fails, the pending chat turn and attachments
remain available. Use **Retry** in the inline card or the Vision model card in
**Settings → Models**; Samosa reuses verified files and repairs only missing or
invalid artifacts. A corrupt staged file is never reported as installed and
never replaces an already verified model.

### Molmo2 native package identity

Molmo2 is pinned to `allenai/Molmo2-4B` commit
`042abfa7a38879a376cec03d949eff0aefaa0600` under Apache-2.0. Samosa does not
install the upstream 19,403,574,432-byte FP32 checkpoint. The native
`molmo2-pack` tool accepts only these source shards:

| Source shard | Bytes | SHA-256 |
|---|---:|---|
| `model-00001-of-00004.safetensors` | 4,891,799,000 | `065ed844edb74cc3f3992415dc412cfae4c2f6e324de76dc84fffe3a652f6fd3` |
| `model-00002-of-00004.safetensors` | 4,844,690,992 | `ac1506897468e69d5af2f46127d30274eecb71c60745e7d27ea0319a27addbb4` |
| `model-00003-of-00004.safetensors` | 4,844,691,024 | `da71a15c961f92361b64c3b45f5465d370c9992f032f0f4adf544d0b208ed573` |
| `model-00004-of-00004.safetensors` | 4,822,393,416 | `519e694862a7d6d82e539424733f6f55fd8118c3c37e328c2dd942a6d4a16120` |

The pinned source `config.json` SHA-256 is
`17e072e3c3b29d9be7a348c74b88658e67ccce094c31b21f87646f6cecd2a76f`;
the pinned `tokenizer.json` SHA-256 is
`95e80901c901584f416b8fd4349fd60022774b89ba4377626511f0562cc599f7`.
The package contract validates all 706 tensor names, FP32 source dtypes, and
exact shapes before conversion. Packages use stable Samosa tensor names, Q4
group-64 for large language matrices, BF16 for the vision tower/connector, a
4 GiB total ceiling, per-file hashes, and processor fingerprint
`808de9add76144a557348c5f5180a8408b12ca83592c7a6a257ae69c968e51df`.
The locally qualified package is 3,372,298,903 bytes, has manifest SHA-256
`a07230dd952e97969921223328e7812020bb58711728450cdfb2205e3a15b770`,
and passed real image, video, and Ornith handoff gates with zero swap growth on
the 16 GiB reference Mac. See
[`regressions/molmo2-4b-q4-2026-08-26.md`](regressions/molmo2-4b-q4-2026-08-26.md).

### Molmo2 network and lifecycle behavior

The catalog intentionally has no Molmo2 artifact URL because the qualified
3.37 GB package has not been uploaded to an authorized public release location.
Clicking install returns `native_pack_required`; it does not begin a hidden FP32
download. A locally installed verified package appears as Ready. Follow the
reviewed local conversion procedure in [INSTALL.md](INSTALL.md).

After a valid package is installed, inference is offline. Text-only work never
starts Molmo2. Every admitted visual image task, plus video, multi-image
comparison, temporal tracking, and localization, prefers Molmo2 and acquires
the global specialist lease. VisionPsy remains the standard-image fallback on
installations without Molmo2. A two-image request is sent as one
labelled native Molmo2 multi-image prompt; it is not reduced to separate image
captions. On constrained Macs the primary backend is stopped first.
The gateway decodes bounded timestamp windows sequentially, terminates Molmo2,
then restarts the primary model for a greedy, non-thinking grounded evidence synthesis.
That synthesis is explicitly told that the local specialist inspected the
attachment bytes, so it must answer from the observation rather than claim it
cannot see the image or infer from its filename.

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

For an explicit **Web search**, the planner first resolves follow-up wording
into a self-contained question, so the subject, place, and time window survive
into every query. The active local model then ranks the search provider's
titles, URLs, and excerpts for direct relevance to that question. Samosa fetches
at most two ranked pages and asks the model whether each page contains enough
evidence to answer before stopping. Source domains are not assigned fixed
authority scores, and merely fetching a readable generic page does not end the
search. Provider order is used only as a bounded fallback if the ranking call
does not return valid JSON.

Fetched page text is treated as untrusted input, labelled as such to the model,
and never rendered as HTML. The result list includes provider-supplied title,
URL, description, and excerpt text; full article text is available only after a
page is fetched. In high-stakes turns, provider snippets are withheld from the
answering prompt, while the ranker may still use them to select pages. The
source list in the UI is URL-deduplicated and collapses when the answer finishes.

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

- **Parallel publishes no rate limits** for keyless use ("light use"), so
  Samosa imposes its own: **100 searches per day**. At the cap it says so and
  suggests adding a key; it resets the next day. Change it with
  `"daily_limit"` under `"search"` (`0` removes the cap), and note that
  searches through *your own* provider are never counted or capped.
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
