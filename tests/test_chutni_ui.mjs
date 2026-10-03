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
    ["Reader pass", "12 of 35"],
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
    ["Plain-text files read", "8"],
    ["File records written", "4"],
    ["PDF pages read", "19"],
    ["OCR outputs", "3"],
    ["Image captions", "2"],
    ["Summaries", "10"],
    ["Errors", "3"],
  ],
);
assert.equal(chutniActionPayload({ active_job_id: "job-123" }), '{"job_id":"job-123"}');
assert.equal(chutniActionPayload({}), '{"job_id":null}');
assert.match(app, /Live activity/);
assert.match(
  app,
  /id="chutniSummaryBudget" min="128" max="16384" step="128" value="3000"/,
);
assert.match(app, /summary_token_budget: summaryTokenBudget/);
assert.match(app, /\/summary-budget`/);
assert.match(app, /token_budget: tokenBudget/);
assert.match(app, /Used on the next Refresh/);
assert.match(app, /limits summary input only; searchable content extraction remains separate/);

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
