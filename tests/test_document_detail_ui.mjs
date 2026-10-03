import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const start = app.indexOf('          button.onclick = async () => {', app.indexOf('button.textContent = "Check in more detail"'));
const end = app.indexOf('          actions.appendChild(button);', start);
assert.ok(start > 0 && end > start);

for (const job of [false, true]) {
  const button = {};
  const original = "Read this document carefully. OCR it.";
  const msg = { detailPrompt: original, detailAttachments: [{ id: "synthetic" }], canDeepen: true };
  const calls = [];
  const bind = new Function("button", "msg", "activeChat", "generating", "saveState", "updateAssistantNode", "sendPrompt", app.slice(start, end));
  bind(button, msg, () => job ? { workflow_job_id: "own-fixture" } : {}, false,
       () => {}, () => {}, async (...args) => calls.push(args));
  await button.onclick();
  assert.equal(calls.length, 1);
  const [prompt, , , options] = calls[0];
  assert.ok(prompt.includes(original), "The original reading request must survive the button");
  assert.match(prompt, /OCR for scanned text/);
  assert.match(prompt, /reading gaps/);
  assert.doesNotMatch(prompt, /visual model|both exact text and visual evidence/,
                      "Detail must not force a text document into a visual specialist");
  assert.equal(options.analysisDepth, "detailed");
  assert.equal(options.displayText, "Check in more detail");
  assert.equal(msg.canDeepen, false);
}
console.log("test_document_detail_ui.mjs: PASS");
