# E-I1 local stage — the planner, driven by a real model

**Date:** 2026-07-28 · **Branch:** `ui-chutni` · **Machine:** the reference
16 GB M3 MacBook Air (macOS 15.5, Darwin 25.5.0, arm64)
**Backend:** **Ornith 9B** (`ornith-1.0-9b-Q4_K_M.gguf`, 5.6 GB) via
`prism-llama.cpp`'s `llama-server` — the backend already installed and selected
on this machine.
**Search:** the live keyless provider, real network, consent granted.

This clears Phase W's second failed acceptance item. It had been recorded as
"not run" because every planner decision to date came from the test's fake
backend.

**Correction to an earlier claim in this work:** this gate was described as
needing the 24 GB Qwen model. That was wrong. The planner is a single stateless
call carrying one system message and one user message — the protocol W5 chose
precisely so it would work on *every* backend. Ornith 9B runs it, and did.

## Method

Six questions through `POST /v1/chat/completions` with `"web": true`, on the
compiled gateway with the real Ornith backend and the real search provider.
Three questions have answers that change over time; three do not. Nothing about
the question text tells the planner which is which — that judgement is the
thing under test.

## Results

| # | Question | Model's decision | Correct? |
|---|---|---|---|
| 1 | Latest stable SQLite version and release date | `web_search "latest stable version of SQLite release date 2026"` → `open_url https://sqlite.org/releaselog/current.html` | ✅ searched, and picked the authoritative page |
| 2 | Current prime minister of Japan | `web_search "current prime minister of Japan 2026"` | ✅ searched |
| 3 | What SSRF stands for | **declined — no tool call** | ✅ correctly did not search |
| 4 | Current price of a Raspberry Pi 5 8GB | `web_search "Raspberry Pi 5 8GB RAM current price 2026"` → `open_url` ×2 (same URL) | ⚠️ searched correctly; **repeated a failed URL** — see defect below |
| 5 | Explain a Bloom filter | **declined — no tool call** | ✅ correctly did not search |
| 6 | Sunset time in Reykjavik today | `web_search "sunset time Reykjavik July 28 2026"` → `open_url timeanddate.com/...` (403) → `open_url sunrise-sunset.org/...` (JS-only) | ✅ searched, **chose a different page after a failure** (the behaviour Q4 lacked), and answered correctly from excerpts alone: sunset **22:48** |

**Malformed planner replies: 0.** Every reply parsed into a usable decision.
Q1's reply arrived wrapped in a reasoning span, which the existing extractor
unwrapped as designed.

The model appended the current year to three of four search queries unprompted,
and Q6 went further and used the full local date ("July 28 2026") — the planner
prompt carries the host's date, and the model used it.

**The repeat in Q4 is not universal, which is what makes it worth fixing in
code.** Q6 hit the same situation — an `open_url` that failed — and correctly
moved to a *different* host. Same model, same prompt, same turn structure, two
different behaviours. A defect that appears only sometimes is exactly the kind
that survives prompt tuning and reappears in front of a user.

Q6 also shows the real ceiling here, and it is not the planner: two of the
three pages it chose were unreadable (one `403`, one JavaScript-only). The
planner's judgement was sound each time; the open web simply refuses a
command-line fetcher fairly often.

**And Q6 answered correctly anyway** — "the sun sets in Reykjavik today at
**22:48**", with sunrise, day length, and last light — entirely from the search
excerpts, because every page read failed. That is the clearest evidence
available that the excerpts carry real information despite often *starting*
with navigation chrome, and the reason no boilerplate-stripping heuristic was
added: the passage that looked like junk was sitting next to the answer.

### The answers the tool loop produced

- **Q1:** "the latest stable version of SQLite is **3.53.4**, released on **July
  24, 2026**" — matching the page it chose to open (`SQLite Release 3.53.4 On
  2026-07-24`).
- **Q2:** named the sitting prime minister, citing the Prime Minister's Office
  site from the results.
- **Q4:** "$135", with the 2026 DRAM-driven price increases tabulated.
- **Q3:** answered from the model's own knowledge, no web access, correct.

## Defect found and fixed — WK6

**Q4 asked for the identical URL twice.** It searched, chose
`raspberrypi.com/products/raspberry-pi-5`, got `HTTP 403`, and then asked for
**the same URL again** — spending the turn's last of three tool calls on a page
that had just failed.

The findings block already told it the page had failed. It repeated anyway, so
"say it more firmly in the prompt" is not a fix. The loop now refuses a
verbatim repeat of any tool argument within a turn (`web_already_tried`) and
tells the planner to choose differently or stop. Comparison is exact, not
normalised: the aim is to stop a literal repeat, not to guess when two
different URLs are "the same".

Regression test: `tests/test_web_search.sh` section 4f, with a fake planner
that asks for the same URL every round — reproducing what Ornith actually did.
The page must be fetched exactly once.

## Cost (partial answer to E-I4)

Wall-clock per full web turn, measured:

| Question | Elapsed |
|---|---|
| Q1 (search + page read + answer) | **86 s** |
| Q2 (search + answer) | **122 s** |
| Q3 (declined, no web) | **42 s** |
| Q4 (search + 2 page attempts) | **171 s** |
| Q5 (declined, no web) | **359 s** |
| Q6 (search + 2 page attempts + answer) | **302 s** |

Q1's timing detail from the backend: `prompt_n 2683`, `prompt_ms 16755`
(≈160 tok/s prefill), `predicted_n 620`, `predicted_ms 43620` (≈14 tok/s
decode) — on the final answering turn alone.

This is **not** the full E-I4 measurement: it is Ornith 9B, not Qwen, and it is
wall-clock for a handful of questions rather than a controlled comparison
against the same turns without web.

What it does establish: a web turn here runs **86 s to 359 s**, and the spread
is driven by how much the model *writes*, not by the web. Q5 took 359 s while
touching the network **zero** times — a long unaided explanation of Bloom
filters — which is longer than any of the web turns. Decode at ~14 tok/s
dominates; the planner rounds are small beside it.

## What this still does not cover

- **Qwen.** The gate is met on Ornith; the 24 GB Qwen has not been run through
  the web loop. The protocol is backend-independent by construction, but that
  is an argument, not a measurement.
- **A larger sample.** Six questions is enough to show the decisions are usable
  and to find one real defect. It is not a rate.
