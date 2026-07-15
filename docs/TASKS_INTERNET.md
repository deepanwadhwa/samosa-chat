# Issue #4 — Internet search

**Read [APP_TASKS.md](APP_TASKS.md) Phase A3 first.** A plan already exists
(A3.1 URL ingestion, A3.2 web search, A3.3 model-initiated tool calls). This
document does not replace it. It adds what was verified on 2026-07-15, corrects
two things in it that are wrong, and specifies the experiment that should run
before any of it is built.

Also read [ISSUE_TASKS.md](ISSUE_TASKS.md) for shared ground truth.

A3's design position is right and should not be relitigated: **the app reaches
the internet; the model consumes what the app fetched.** v1 user-initiated, v2
model-initiated behind a gate.

## Verified findings

### F1 — The tool-call tokens exist  (good news for A3.3)

The shipped tokenizer has all four as atomic added-tokens:

```
248058 '<tool_call>'      248059 '</tool_call>'
248066 '<tool_response>'  248067 '</tool_response>'
```

A3.3's premise — that the model was trained for tool calling — is supported at
the tokenization level. The parser can rely on single-token span boundaries
rather than string matching.

### F2 — There is no chat template to extend  (cost A3.3 knew nothing about)

`chat_template` is **not present** in the shipped `tokenizer_qwen36.json`
(verified: the file has `version`, `truncation`, `padding`, `added_tokens`,
`normalizer`, `pre_tokenizer`, `post_processor`, `decoder`, `model` — no
template). `tokenizer_config.json` is not shipped at all.

The template is hardcoded C string concatenation at
[qwen36b.c:3440-3452](../src/qwen36b.c#L3440-L3452):

```c
static char *qwen_chat_prompt(const char *user, const char *system, int no_thinking) {
    const char *prefix = "<|im_start|>user\n";
    const char *middle_think = "<|im_end|>\n<|im_start|>assistant\n<think>\n";
    ...
```

So tool-schema injection means hand-porting upstream's tool template into
`snprintf` calls and matching it **exactly** — any drift degrades tool behavior
in ways that look like model weakness rather than a template bug. Recover the
canonical template from upstream `tokenizer_config.json` and pin it in a test.

### F3 — The serve API discards message history  **(blocks A3.3)**

[qwen36b.c:4073-4081](../src/qwen36b.c#L4073-L4081), `serve_last_user()`, takes
**the last user message and the first system message. Every other message in the
array is dropped**, silently. [SERVE_API.md:29](SERVE_API.md) states it plainly:
"The chat body accepts one or more text messages and uses the last user
message." Multi-turn context comes from `conversation_id` sessions, not from the
messages array.

A3.3 specifies "result injection as tool messages". **There is nowhere to inject
them.** A `{"role": "tool", ...}` message would be accepted by the API and
silently ignored — the worst failure mode available, because it looks like it
works.

This is unscoped work in A3.3's ~3–4 day estimate: either extend the API to
consume a real message array (which touches session/KV semantics — turns are
prefilled from snapshots, not from history) or thread tool results through the
session machinery instead. **Re-cost A3.3 once a direction is chosen.**

### F4 — Nothing tool-shaped exists in the codebase

Confirmed by grep across `src/`, `dist/`, and `docs/SERVE_API.md`: zero matches
for `tool_call`, `function_call`, `"tools"`, or `tool_choice`. A3.3 is entirely
greenfield. F1 means the model probably knows how; F2/F3 mean the engine does not.

## Corrections to A3.1's security requirements

A3.1's SSRF list is a good start and is **incomplete in ways that matter**. It
calls the requirements non-negotiable, so they should be right.

**Bug: `fd00::/8` should be `fc00::/7`.** IPv6 unique local addresses are
`fc00::/7` — that is `fc00::` through `fdff::`. `fd00::/8` covers only the upper
half and lets every `fc00::/8` address through.

**Missing IPv4 ranges:** `0.0.0.0/8` (this host), `100.64.0.0/10` (CGNAT —
reachable on many home ISPs), `192.0.0.0/24`, `198.18.0.0/15` (benchmark),
`224.0.0.0/4` (multicast), `240.0.0.0/4` (reserved), `255.255.255.255/32`.

**Missing IPv6 cases:** `::/128`; `fe80::/10` (link-local); **`::ffff:0:0/96`
IPv4-mapped — `::ffff:127.0.0.1` defeats a naive IPv4-only check**; `64:ff9b::/96`
(NAT64, which can reach IPv4 private space); `2002::/16` (6to4).

`169.254.169.254` — cloud metadata — is already covered by A3.1's `169.254/16`.

**Missing: DNS rebinding.** "Resolve-then-connect" is the right instinct but is
a TOCTOU race as written. The resolved IP must be **pinned and connected to
directly**, with the `Host` header carrying the original name. If the code
resolves, validates, then hands the *hostname* to the HTTP client, the client
re-resolves and an attacker returns `127.0.0.1` on the second lookup.

**Missing: per-hop redirect revalidation.** A3.1 caps redirects at 5 but does not
say each hop's resolved IP is revalidated. It must be — a public URL redirecting
to `http://169.254.169.254/` is the canonical SSRF.

**Missing: scheme allowlist.** Reject everything but `http`/`https`. `file://`,
`gopher://`, `dict://`, `ftp://` are all live SSRF vectors when a URL is passed
to a general-purpose fetcher.

## Experiments

### E-I1 — Tool-call reliability  ~0.5 day  **RUN FIRST — may delete A3.3**

A3.3 already sets its own no-go: ">20% malformed = no-go, ship A3.1/A3.2 only
and say so in the README". **That gate should be evaluated before the 3–4 days of
C, not after**, and the harness to do it already exists.

[tools/run_openrouter_control.py](../tools/run_openrouter_control.py) runs a
bounded upstream Qwen control through OpenRouter against
`qwen/qwen3.6-35b-a3b`, FP8 via AkashML, driven by
[tests/openrouter_control_cases.json](../tests/openrouter_control_cases.json)
with `require_regex` scoring and a results directory. It already reads the API
key safely (stdin, never in argv or results).

**Method, in two stages:**

1. **Upstream ceiling.** Add a `tool_call_cases.json` in the existing schema: 20
   prompts that should trigger a tool call, with 3 tool schemas
   (`web_search`, `fetch_url`, `read_document`), 3 seeds. Score **malformed-JSON
   rate** inside `<tool_call>` spans, plus wrong-tool and hallucinated-argument
   rates. This is FP8 upstream — **the best case Samosa could ever reach.**
2. **Local floor.** Only if stage 1 passes: the same suite against the local
   model (group-32 q4 experts + row-wise int8/int4 resident weights — see
   [ISSUE_TASKS.md](ISSUE_TASKS.md) for why "int4 model" is the wrong shorthand).

**Why this ordering matters:** if upstream FP8 already exceeds 20% malformed,
the local quantized model is very unlikely to beat it, and A3.3 is dead for
~0.5 day of API spend instead of ~4 days of engine work plus the F3 API
redesign. If upstream is clean and local is not, that is a *quantization*
finding worth publishing on its own — it belongs in the regression ledger either
way.

**Acceptance:** a malformed-rate table (upstream FP8 vs local group-32) with a clear
verdict against A3.3's own 20% gate. **A no-go verdict is a successful
experiment.** Write it up and move the effort to A3.1/A3.2, which need no tool
calling at all.

### E-I2 — SSRF suite  ~1 day  **Gates A3.1**

Build the suite from A3.1's list **plus every correction above**. At minimum 30
cases: each blocked range direct; redirect chains into each; DNS rebinding (a
hostname whose second resolution is `127.0.0.1` — a real test needs a controlled
resolver, so build one); `::ffff:127.0.0.1`; decimal/octal/hex IP encodings
(`http://2130706433/`); `http://[::1]/`; non-HTTP schemes; a 5 MB+ body against
the size cap; a slowloris-style trickle against the 20 s timeout.

**Acceptance:** 100% blocked. This is the one place in the program where a
partial pass is a fail — it runs on the user's machine, on their network, behind
their firewall.

### E-I3 — HTML extraction quality  ~1 day

A3.1 estimates "a ~200-line heuristic extractor is enough". Test it: 20 real
article URLs across news, docs, blogs, and a JS-heavy SPA. Compare extracted
text against a readability reference.

**The SPA case is the honest one to look at.** A dependency-free C extractor
cannot execute JavaScript, so JS-rendered pages will yield navigation chrome or
nothing. That is fine — but it must be *detected* and reported ("this page
requires JavaScript; I couldn't read it"), never passed to the model as if it
were the article. Silent garbage is the failure mode to design against.

**Acceptance:** ≥ 15/20 extract cleanly; the SPA failures are detected and
reported, not silently degraded.

### E-I4 — What does a web turn actually cost?  ~0.5 day  **Shapes the UX**

Measure end-to-end: fetch (1–3 s) + extract (<1 s) + **prefill**. A typical
article is ~3–5K tokens, and at the 2-thread default (~14 tok/s prefill) that is
**~4–6 minutes**. At the fast setting (~24 tok/s), ~2–3.5 minutes.

**The honest conclusion, stated up front:** on this machine, a web search is a
multi-minute operation. That is not a bug to optimize away; it is the cost of
running a 35B model locally on a fanless laptop. The UX must say so before the
user commits, exactly as A2.2 does for documents. A3.2's "user picks from top 5"
flow is well-suited to this — it puts the expensive step behind a deliberate
choice.

Consider defaulting web ingestion to the fast thread setting, as A2.2 already
does for documents: it is bounded work and worth the heat.

**Acceptance:** a measured cost table; UI estimates within ±20%.

## Tasks

A3.1 and A3.2 stand as written in [APP_TASKS.md](APP_TASKS.md), with these
amendments:

### A3.1+ — URL ingestion  ~2 days (was ~1–2)

As specified, plus the corrected SSRF requirements above, the scheme allowlist,
IP pinning, per-hop revalidation, and E-I3's JS-page detection.

Add an explicit **offline kill switch** (`SAMOSA_OFFLINE=1` and a UI toggle)
that hard-disables every outbound path. The product principle is "local-only by
default; the internet features reach out, nothing reaches in" — a user who wants
the original guarantee back must be able to have it, verifiably, and the UI must
show which mode is active.

### A3.2 — Web search  ~1–2 days  (unchanged)

The design is right, including the honest default: **no backend configured**,
because no key-free search API is dependable enough to hardcode. Keep that.
Degrading to instructions is correct behavior, not a gap to close.

### A3.3 — Model-initiated tool calls  **BLOCKED on E-I1; re-cost after F3**

Do not start until E-I1 returns. If it passes, re-estimate: the published ~3–4
days does not include the F3 message-history work, which is a real API/session
change.

A3.3's own gate — "with tools disabled, output must remain byte-identical to
pre-A3.3 behavior" — is exactly right and matches the gate V3 sets in
[TASKS_VISION.md](TASKS_VISION.md). Keep it. Test it with a fixed seed on the
real model, not on a stub.

## Non-goals

- A search backend of our own, or a bundled API key.
- Authenticated fetching, cookies, logins. A3.1 already forbids it; it stays
  forbidden.
- Background/scheduled fetching. Every network action is user-visible and
  user-initiated, and is logged in the transcript.

## Open questions

- **Does thinking mode interact with tool calls?** The engine has a thinking
  budget and closure machinery ([thinking_budget.h](../src/thinking_budget.h)),
  and `<think>` / `<tool_call>` spans could interleave. E-I1's local stage should
  test with thinking on *and* off — a tool call emitted inside a `<think>` block
  that gets closed by the budget transition is a plausible and nasty failure.
- **Where does F3 land?** Extending the messages array is the general fix and the
  more invasive one; threading tool results through the session machinery fits
  the existing architecture better. Decide before A3.3 starts.
