#!/bin/sh
set -eu
# T3.3: the Chutni scope-view logic, checked against the acceptance rules.
#
# The rendering itself cannot be seen from here, so what is testable is the
# part that decides *what* gets rendered: which state maps to which label,
# which states offer a safe next action, and when a progress row is allowed to
# claim a number. Those are pure functions in assets/app.html and are pulled
# out of the shipped file -- not copied into the test -- so this cannot drift
# from what the browser actually runs.

APP=${SAMOSA_APP_HTML:-assets/app.html}
command -v node >/dev/null 2>&1 || {
  echo "test_chutni_views.sh: SKIP (node is not installed; view logic unverified)" >&2
  exit 0
}
[ -f "$APP" ] || { echo "test_chutni_views.sh: $APP not found" >&2; exit 1; }

node - "$APP" <<'JS'
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");

// Pull the named functions out of the shipped page by brace matching, so the
// test exercises the real source rather than a transcription of it.
function extract(name) {
  const start = html.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`missing function ${name}`);
  let i = html.indexOf("{", start), depth = 0;
  for (let j = i; j < html.length; j++) {
    if (html[j] === "{") depth++;
    else if (html[j] === "}" && --depth === 0) return html.slice(start, j + 1);
  }
  throw new Error(`unbalanced ${name}`);
}
const busy = html.match(/const CHUTNI_BUSY = new Set\(\[[^\]]*\]\);/);
if (!busy) throw new Error("missing CHUTNI_BUSY");

const kb = html.match(/const KB = \d+;/);
if (!kb) throw new Error("missing KB");
const src = [kb[0], busy[0], extract("chutniPresentation"), extract("chutniPhaseRows"), extract("bytesLabel")].join("\n");
const scope = {};
new Function("exports", src + "\nexports.chutniPresentation=chutniPresentation;exports.chutniPhaseRows=chutniPhaseRows;exports.bytesLabel=bytesLabel;exports.CHUTNI_BUSY=CHUTNI_BUSY;")(scope);
const { chutniPresentation, chutniPhaseRows, bytesLabel, CHUTNI_BUSY } = scope;

let failures = 0;
const check = (ok, msg) => { if (!ok) { console.error("  FAIL:", msg); failures++; } };

// Every state the gateway and sidecar can produce must render deliberately.
const STATES = ["unbuilt", "queued", "building", "canceling", "ready", "paused_user",
                "paused_chat", "permission", "disconnected", "failed", "failed_initial",
                "canceled_initial"];
for (const state of STATES) {
  const p = chutniPresentation({ state, evidence_generation: 1, warnings: [] });
  check(!!p.chip, `${state}: no label`);
  // "building" and "queued" are already the words a person would use; what
  // must never leak is an internal identifier like paused_user or failed_initial.
  check(!/_/.test(p.chip), `${state}: raw internal state name leaked into the label "${p.chip}"`);
}

// An unknown future state must still be inert-safe, not blank.
check(!!chutniPresentation({ state: "something_new", warnings: [] }).chip, "unknown state has no label");

// T3.3: every error state has a safe next action.
for (const state of ["permission", "disconnected", "failed", "failed_initial", "canceled_initial"]) {
  const p = chutniPresentation({ state, evidence_generation: 1, warnings: [] });
  check(p.actions.length > 0, `${state}: an error state with no next action`);
  check(!!p.why, `${state}: an error state that does not say what happened`);
}
// An unreadable scope document is an error state too.
check(chutniPresentation({ state: "ready", unreadable: true, warnings: [] }).actions.length > 0,
      "unreadable scope offers no next action");

// A build in flight must always be stoppable.
for (const state of ["building", "queued", "paused_user", "paused_chat"]) {
  const p = chutniPresentation({ state, warnings: [] });
  check(p.actions.includes("cancel"), `${state}: running work cannot be canceled`);
}
// Paused-by-user and paused-for-chat must be distinguishable to the reader.
const pu = chutniPresentation({ state: "paused_user", warnings: [] });
const pc = chutniPresentation({ state: "paused_chat", warnings: [] });
check(pu.chip !== pc.chip || pu.why !== pc.why, "user pause and chat pause read identically");
check(pu.actions.includes("resume") && pc.actions.includes("resume"), "a paused build cannot be resumed");

// T3.3: never call tokenized-but-unpublished evidence Ready.
for (const state of ["building", "queued", "failed_initial", "canceled_initial", "unbuilt"]) {
  const p = chutniPresentation({ state, chunks_indexed: 999, warnings: [] });
  check(!/^ready/.test(p.chip), `${state}: unpublished evidence labelled "${p.chip}"`);
}

// Ready vs Ready-improving are separate, and improving still reads as ready.
const ready = chutniPresentation({ state: "ready", warnings: [] });
const improving = chutniPresentation({ state: "ready", enhancement_state: "improving", warnings: [] });
check(ready.chip === "ready", `plain ready reads as "${ready.chip}"`);
check(/^ready/.test(improving.chip) && improving.chip !== "ready",
      `improving should extend ready, got "${improving.chip}"`);

// T3.3: unknown totals render indeterminate; known totals render the pair.
const rows = chutniPhaseRows({ state: "building", phase: "tokenize", chunks_indexed: 5, chunks_total: 50 });
const determinate = rows.filter(r => r.determinate);
check(determinate.length > 0, "a known completed/total produced no determinate row");
check(determinate.every(r => r.total > 0), "a determinate row with a zero total");

const unknown = chutniPhaseRows({ state: "building", phase: "tokenize", chunks_indexed: 5 });
check(unknown.length > 0, "a running phase produced no row at all");
check(unknown.some(r => !r.determinate), "an unknown total did not render indeterminate");

// Division by zero must not masquerade as a real proportion.
const zero = chutniPhaseRows({ state: "building", phase: "extract", files_indexed: 0, regular_files_seen: 0 });
check(zero.every(r => !r.determinate), "a zero total was treated as a known proportion");

// Summaries are their own row and never merged into evidence progress.
const both = chutniPhaseRows({ state: "building", phase: "tokenize", chunks_indexed: 5, chunks_total: 50,
                               enhancement_state: "improving", chunks_summarized: 1 });
const labels = both.map(r => r.label);
check(new Set(labels).size === labels.length, "two phases share one label");
check(labels.some(l => /summar/i.test(l)), "summary progress is not shown separately");

// A settled scope shows no progress rows at all.
check(chutniPhaseRows({ state: "ready", chunks_indexed: 10, chunks_total: 10 }).length === 0,
      "a ready scope still renders progress");

// No row may carry an ETA or a blended percentage field.
for (const r of both) check(!("eta" in r) && !("percent" in r), "a phase row carries a speculative ETA/percent");

// Byte labels: absent is absent, never "0 B".
check(bytesLabel(undefined) === null && bytesLabel(null) === null, "a missing size rendered as a number");
check(bytesLabel(0) === "0 B", "zero bytes should still render as zero");
check(bytesLabel(1536) === "1.5 KB", `1536 rendered as ${bytesLabel(1536)}`);

if (failures) { console.error(`test_chutni_views.sh: ${failures} failure(s)`); process.exit(1); }
console.log("test_chutni_views.sh: PASS");
JS
