import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
function extractFunction(name) {
  const start = app.indexOf(`      function ${name}(`);
  const end = app.indexOf("\n      }\n", start);
  assert.ok(start >= 0 && end > start, `${name} must remain extractable`);
  return app.slice(start, end + "\n      }\n".length);
}
const coverageSource = extractFunction("chutniCoverageFacts");
const chutniPresentation = eval(`(${extractFunction("chutniPresentation")})`);
const unreadablePresentation = chutniPresentation({ state: "ready_partial", scan_errors: 1 });
assert.match(unreadablePresentation.why, /1 file could not be read completely/);
assert.doesNotMatch(unreadablePresentation.why, /safety limit|unspecified/);
assert.match(chutniPresentation({ state: "ready_partial", limiting_reason: "maximum_files" }).why,
  /safety limit.*maximum_files/);
assert.doesNotMatch(chutniPresentation({ state: "ready_partial", limiting_reason: "none" }).why,
  /safety limit|unspecified/);
const monitorSource = extractFunction("chutniMonitorFacts");
const payloadSource = extractFunction("chutniActionPayload");
const { chutniCoverageFacts, chutniMonitorFacts, chutniActionPayload } = eval(
  `(() => { const CHUTNI_BUSY = new Set(["building","queued","canceling","improving"]);` +
  `${coverageSource}${monitorSource}${payloadSource};` +
  `return {chutniCoverageFacts,chutniMonitorFacts,chutniActionPayload};})()`
);

assert.deepEqual(
  chutniCoverageFacts({
    files_indexed: 4,
    regular_files_seen: 4,
    content_readable_files: 3,
    metadata_only_files: 1,
    content_artifacts: 9,
  }),
  [
    ["Files found", "4 of 4"],
    ["Content-readable", "3"],
    ["Metadata only", "1"],
    ["Searchable content artifacts", "9"],
  ],
);
assert.doesNotMatch(app, /Files remembered|facts\.push\(\["Passages"/);
assert.deepEqual(
  chutniCoverageFacts({
    state: "building",
    scan_files_seen: 37,
    scan_sources_indexed: 35,
    enrichment_files_done: 12,
    enrichment_files_total: 35,
  }),
  [
    ["Files scanned", "37"],
    ["Files cataloged", "35"],
    ["Files finished", "12 of 35"],
  ],
);
assert.deepEqual(
  chutniMonitorFacts({
    progress_started_ms: 1,
    scan_text_artifacts: 8,
    scan_metadata_artifacts: 4,
    pdf_pages_read: 19,
    ocr_outputs: 3,
    image_captions: 2,
    summaries_created: 10,
    scan_errors: 1,
    enrichment_failures: 2,
  }),
  [
    ["Plain-text artifacts stored", "8"],
    ["File records written", "4"],
    ["Native PDF pages stored", "19"],
    ["OCR artifacts stored", "3"],
    ["Image captions", "2"],
    ["Summaries", "10"],
    ["Errors", "3"],
  ],
);
assert.equal(chutniActionPayload({ active_job_id: "job-123" }), '{"job_id":"job-123"}');
assert.equal(chutniActionPayload({}), '{"job_id":null}');
assert.match(app, /Live activity/);
assert.doesNotMatch(app, /chutniSummaryBudget|Save budget|Full searchable extraction is separate/);
assert.match(app, /Reading target:/);
assert.match(app, /Content beyond these samples is not verified/);
const rateLabel = eval(`(${extractFunction("chutniRateLabel")})`);
assert.equal(rateLabel(13 / 294), "0.04 files/s");
assert.equal(rateLabel(0.001), "<0.01 files/s");
assert.equal(rateLabel(1.234), "1.2 files/s");
assert.deepEqual(chutniMonitorFacts({
  progress_started_ms: 1, content_reading_policy: "opening_sample_v1",
  text_files_sampled: 2, pdf_pages_processed: 4, ocr_units_completed: 3,
  characters_read: 7000, files_fully_read: 1, files_sampled: 2, files_read_failed: 0,
}), [["Text samples read", "2"], ["PDF pages processed", "4"], ["OCR units completed", "3"],
  ["Characters collected", "7000"], ["Files read in full", "1"], ["Files sampled", "2"],
  ["Files with reading failures", "0"]]);

// Render the shipped card against a minimal DOM. This verifies visible text
// and controls; it does not claim browser layout or paint coverage.
class CardElement {
  constructor(tag) { this.tag = tag; this.children = []; this.textContent = ""; this.className = ""; this.style = {}; }
  appendChild(child) { this.children.push(child); return child; }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  querySelector(selector) {
    for (const child of this.children) {
      if (child.className.split(" ").includes(selector.slice(1))) return child;
      const nested = child.querySelector(selector); if (nested) return nested;
    }
    return null;
  }
  text() { return [this.textContent, ...this.children.map(child => child.text())].join(" "); }
}
const cardFixture = eval(`(() => {
  const document = { createElement: tag => new CardElement(tag), createTextNode: text => { const node = new CardElement("text"); node.textContent = text; return node; } };
  const CHUTNI_BUSY = new Set(["building", "queued", "canceling", "improving"]);
  const KB = 1024; const activeChat = () => null; const chutniAction = () => {};
  ${["bytesLabel", "whenLabel", "durationLabel", "chutniRateLabel", "chutniCoverageFacts", "chutniMonitorFacts", "chutniPresentation", "chutniPhaseRows", "chutniCard"].map(extractFunction).join("\n")}
  return chutniCard;
})()`);
const liveCard = cardFixture({ id: "generated", display_name: "Generated samples", state: "building", phase: "extract",
  content_reading_policy: "opening_sample_v1", read_character_target: 3000, sample_page_guard: 12,
  progress_started_ms: 1, elapsed_seconds: 294, files_per_second: 13 / 294,
  enrichment_files_done: 13, enrichment_files_total: 100,
  current_file: "generated-report.pdf", current_page: 3, current_page_total: 20, current_file_characters: 3100,
  activity: "Summarizing the collected sample…", pdf_pages_processed: 3, ocr_units_completed: 1,
}).text();
for (const visible of ["Files finished", "13 of 100", "0.04 files/s", "Current file: generated-report.pdf",
  "Page 3 of 20", "3,100 / 3,000 target characters", "Summarizing the collected sample", "Reading target: 3,000", "Pause", "Cancel"])
  assert.ok(liveCard.includes(visible), `card missing ${visible}`);
assert.doesNotMatch(liveCard, /Reader pass|0\.0 files\/s|Save budget|Now:/);
assert.match(cardFixture({ id: "legacy", state: "building", current_file: "previous.pdf", progress_started_ms: 1, scan_text_artifacts: 900 }).text(), /Last completed file: previous.pdf/);
assert.match(cardFixture({ id: "paused", state: "paused_user" }).text(), /reuses cached samples/);
const parallelCard = cardFixture({ id: "parallel", state: "building", content_reading_policy: "opening_sample_v1",
  extraction_workers: 4, active_workers: 2, summary_queue: 1, progress_started_ms: 1,
  pdf_pages_processed: 2,
  active_files_json: JSON.stringify([
    { file: "native.pdf", activity: "Summarizing the collected sample", page: 2, pages: 20, characters: 3400 },
    { file: "scan.pdf", activity: "Recognizing page text", page: 1, pages: 9, characters: 0 },
  ]),
}).text();
for (const label of ["4 native workers", "2 active files", "1 waiting for summary", "File", "Activity", "Characters", "native.pdf", "scan.pdf", "2 / 20"])
  assert.ok(parallelCard.includes(label), `parallel card missing ${label}`);

// Folder memory must not expand a saved file task's evidence scope. Execute
// the shipped action handler with a completed and an interrupted chat switch.
const actionBegin = app.indexOf("      async function chutniAction(scope, action)");
const actionEnd = app.indexOf("      function openChutniForget(scope)", actionBegin);
assert.ok(actionBegin >= 0 && actionEnd > actionBegin);
const scopeFixture = eval(`(() => {
  const original = { id: "selected-conversation", workflow_job_id: "selected-task", directory_context: null };
  const chats = [original]; let current = original; let interrupted = true;
  const activeChat = () => current;
  const ensureChat = () => current;
  const newChat = async () => {
    if (interrupted) return;
    current = { id: "memory-conversation", directory_context: null }; chats.push(current);
  };
  const saveState = () => {}; const renderChutni = () => {}; const updateActiveMemoryContext = () => {};
  const showView = () => {}; const els = { prompt: { focus() {} } };
  ${app.slice(actionBegin, actionEnd)}
  return { ask: () => chutniAction({ id: "folder-memory" }, "ask"),
    retry: () => { interrupted = false; }, inspect: () => ({ original, chats, current }) };
})()`);
await scopeFixture.ask();
assert.equal(scopeFixture.inspect().chats.length, 1);
assert.equal(scopeFixture.inspect().original.directory_context, null);
scopeFixture.retry();
await scopeFixture.ask();
assert.equal(scopeFixture.inspect().chats.length, 2);
assert.equal(scopeFixture.inspect().original.workflow_job_id, "selected-task");
assert.equal(scopeFixture.inspect().original.directory_context, null);
assert.deepEqual(scopeFixture.inspect().current.directory_context, { scope_id: "folder-memory" });
assert.equal(scopeFixture.inspect().current.workflow_job_id, undefined);

console.log("test_chutni_ui.mjs: PASS");

const filterModelContent = eval(`(${extractFunction("filterModelContent")})`);
const visibleModelContent = eval(`(${extractFunction("visibleModelContent")})`);
assert.equal(visibleModelContent('<think>private reasoning</think>Final answer'), 'Final answer');
assert.equal(visibleModelContent('<think>unfinished private reasoning'), '');
assert.equal(visibleModelContent('Normal <b>markup</b> and 2 < 3'), 'Normal <b>markup</b> and 2 < 3');
for (let split = 0; split <= 52; split++) {
  const raw = '<think>private reasoning</think>The grounded answer.';
  const state = {};
  const pieces = [raw.slice(0, split), raw.slice(split)];
  let answer = '';
  for (const piece of pieces) answer += filterModelContent(state, piece);
  answer += filterModelContent(state, '', true);
  assert.equal(answer, 'The grounded answer.', `SSE boundary ${split}`);
  assert.doesNotMatch(answer, /private|think/);
}
const characterState = {};
let characterAnswer = '';
for (const char of '<think>secret</think>Safe <em>answer</em>')
  characterAnswer += filterModelContent(characterState, char);
characterAnswer += filterModelContent(characterState, '', true);
assert.equal(characterAnswer, 'Safe <em>answer</em>');
