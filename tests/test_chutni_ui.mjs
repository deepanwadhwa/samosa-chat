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
    ["Metadata-only at scan", "4"],
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

console.log("test_chutni_ui.mjs: PASS");
