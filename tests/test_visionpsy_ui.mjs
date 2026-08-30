import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf('      const GENERATION_LABEL = "Working through it";');
const end = app.indexOf("      function welcomeHTML() {");
assert.ok(begin >= 0 && end > begin, "VisionPsy continuation UI block must remain extractable");

class ClassList {
  constructor() { this.names = new Set(); }
  add(name) { this.names.add(name); }
  remove(name) { this.names.delete(name); }
}

class FakeElement {
  constructor() {
    this.hidden = true;
    this.disabled = false;
    this.textContent = "";
    this.style = {};
    this.classList = new ClassList();
    this.onclick = null;
  }
}

function response(body, ok = true) {
  return { ok, json: async () => body };
}

function loadHarness({ installResponse, jobResponses }) {
  const button = new FakeElement();
  const status = new FakeElement();
  const progress = new FakeElement();
  const fill = new FakeElement();
  const selectors = new Map([
    [".vision-download-btn", button],
    [".vision-install-status", status],
    [".vision-progress-bar", progress],
    [".model-progress-fill", fill],
  ]);
  const node = {
    innerHTML: "",
    querySelector(selector) { return selectors.get(selector) || null; },
  };
  const calls = [];
  let jobIndex = 0;
  globalThis.authFetch = async (path, options) => {
    calls.push({ path, options });
    if (path === "/v1/models/install") return response(installResponse);
    return response(jobResponses[Math.min(jobIndex++, jobResponses.length - 1)]);
  };
  const continuations = [];
  globalThis.sendPrompt = async (...args) => { continuations.push(args); };
  globalThis.formatBytes = value => `${value} B`;
  globalThis.markdown = text => text;
  let intervalCallback = null;
  let cleared = 0;
  globalThis.setInterval = callback => { intervalCallback = callback; return 17; };
  globalThis.clearInterval = id => { assert.equal(id, 17); cleared++; };
  const fns = eval(`(() => {${app.slice(begin, end)} return { renderAssistantResponse }; })()`);
  return {
    ...fns, node, button, status, progress, fill, calls, continuations,
    interval: () => intervalCallback,
    cleared: () => cleared,
  };
}

{
  const pending = { messages: [{ role: "user", content: "inspect it" }], attachment_ids: ["a".repeat(64)] };
  const msg = { id: "assistant-1", visionRequired: true, _pendingPayload: pending, content: "", streaming: false };
  const h = loadHarness({
    installResponse: { job_id: "job-1", status_url: "/v1/models/installs/job-1" },
    jobResponses: [
      { state: "verifying", completed_bytes: 50, total_bytes: 100 },
      { state: "installed", completed_bytes: 100, total_bytes: 100 },
    ],
  });
  h.renderAssistantResponse(h.node, msg);
  assert.equal(typeof h.button.onclick, "function");
  await h.button.onclick();
  const installBody = JSON.parse(h.calls[0].options.body);
  assert.equal(installBody.model_id, "visionpsy-nano-460m-mlx-bf16");
  assert.match(installBody.client_request_id, /^vision-resume-assistant-1-/);
  assert.equal(h.calls[0].path, "/v1/models/install");

  // Concurrent timer delivery must collapse to one status fetch.
  await Promise.all([h.interval()(), h.interval()()]);
  assert.equal(h.calls.filter(call => call.path === "/v1/models/installs/job-1").length, 1);
  assert.match(h.status.textContent, /50 B \/ 100 B/);

  await h.interval()();
  assert.equal(h.cleared(), 1);
  assert.equal(msg.visionRequired, false);
  assert.equal(h.continuations.length, 1, "the preserved turn resumes exactly once");
  assert.deepEqual(h.continuations[0], [null, pending, msg]);
  await h.interval()();
  assert.equal(h.continuations.length, 1, "late timer callbacks cannot duplicate continuation");
}

{
  const msg = { id: "assistant-2", visionRequired: true, _pendingPayload: {}, content: "", streaming: false };
  const h = loadHarness({ installResponse: { job_id: "job-without-url" }, jobResponses: [] });
  h.renderAssistantResponse(h.node, msg);
  await h.button.onclick();
  assert.equal(h.button.disabled, false);
  assert.equal(h.button.textContent, "Retry download");
  assert.match(h.status.textContent, /status URL/i);
  assert.equal(h.continuations.length, 0);
}

process.stdout.write("VisionPsy download-and-continue UI fixtures: PASS\n");
