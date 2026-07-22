# E-Gate10 — real public-page change check (2026-07-22)

Gate 10 of the native Jobs acceptance list: *"…followed by one real public-page
change check."* This records the **real network** run — no stub, no mock. It was
run **with the owner's explicit sign-off** to make outbound requests.

## What binary ran (honest scope)

- **`build/samosa-gateway`** (the freshly built binary with the public-URL
  pipeline, 156,880 bytes), booted with a fixture Ornith backend and an isolated
  temp `SAMOSA_HOME` so real `~/.samosa/jobs` was untouched.
- **NOT** the installed release: `~/.samosa/current/bin/samosa-gateway` is still
  the pre-pipeline binary (88,592 bytes, dated before this work) and does not have
  `/v1/jobs/public-inputs/update`. Re-installing/packaging the new binary and
  re-running from `~/.samosa` is the remaining half of Gate 10 and is **not done**
  — it overwrites the installed release and waits for owner confirmation.

## Targets (owner asked me to pick safe URLs)

- `https://example.com/` — IANA-reserved documentation domain; static → exercises
  `new` then `unchanged`.
- `https://www.cloudflare.com/cdn-cgi/trace` — returns a `ts=` timestamp that
  changes each call → exercises `changed`.
- `https://httpbin.org/uuid` — attempted for `changed`; was returning `503` /
  timing out during the run (external flakiness), which instead validated graceful
  error handling.

## Results

### new → unchanged, real static page

```
POST /v1/jobs/public-inputs/update  {"job_id":"real-example","urls":["https://example.com/"]}
#1: {"checked":1,"changed":1, records:[{"status":"new","title":"Example Domain","hash":"86ad14bc89ac0c90","text_chars":127}]}
#2: {"checked":1,"changed":0, records:[{"status":"unchanged","hash":"86ad14bc89ac0c90"}]}

extracted item text (real HTML -> text):
  Example Domain
  This domain is for use in documentation examples without needing permission. Avoid use in operations.
  Learn more

state.json:
  {"pages":{"https://example.com/":{"hash":"86ad14bc89ac0c90","title":"Example Domain","last_seen_at":"2026-07-22T13:40:52Z"}}}
```

This confirms, on real data: DNS resolution of `example.com` → a real global
(Cloudflare) address that **passed** the SSRF blocklist (the previously-untested
allow path), a real HTTPS fetch via `curl`, real `robots.txt` handling (a 404
robots → allowed), HTML→text extraction, and `new`→`unchanged` change detection.

### changed, real dynamic page

```
POST /v1/jobs/public-inputs/update  {"job_id":"real-cf","urls":["https://www.cloudflare.com/cdn-cgi/trace"]}
#1: {"changed":1, records:[{"status":"new","hash":"753934cadee985be"}]}      # ts=1784727845.000
#2: {"changed":1, records:[{"status":"changed","hash":"4a9ba8201e36d995"}]}  # ts=1784727848.000

items/ held two distinct versions:
  trace-753934cadee9.txt  -> ts=1784727845.000
  trace-4a9ba8201e36.txt  -> ts=1784727848.000
```

The same URL returning different bytes 3 s apart was correctly detected as
`changed`, writing exactly one new item.

### graceful failure on a real error

`https://httpbin.org/uuid` returned `503` and, on an earlier attempt, timed out
(`curl (28)` after 20 s). Each was recorded as a per-URL
`{"status":"error","error":"fetch failed with HTTP 503"}` (or `HTTP 0` for the
timeout); the batch still returned `200`, no item was written, and `state.json`
was left intact. No crash, no partial state.

## Verdict

Gate 10's real-fetch check **passes on the built binary**: all three change
transitions (`new` / `unchanged` / `changed`) verified against live sites, SSRF
allow+block both exercised on real DNS, robots honored, real failures handled
without corrupting state. **Open:** re-package + install the new binary and
re-run from the `~/.samosa` release (owner-gated, overwrites the installed
release). Until then, do not describe the *installed release* as covering this
feature.
