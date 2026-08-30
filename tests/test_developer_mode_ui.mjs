import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const markup = app.slice(app.indexOf('<section class="preferences-pane" data-settings-pane-content="advanced"'),
                         app.indexOf('<section class="preferences-pane" data-settings-pane-content="voice"'));
for (const id of ["developerTraceEnabled", "developerTraceCopy", "developerTraceClear",
                  "developerTraceStatus", "developerTracePath"])
  assert.match(markup, new RegExp(`id="${id}"`), `${id} must be exposed in Advanced settings`);
assert.match(markup, /prompts, replies, routing decisions, OCR text, raw VisionPsy observations/i);
assert.match(markup, /private document contents/i);
assert.match(markup, /Authentication tokens are never logged/i);

class ClassList {
  constructor() { this.values = new Set(); }
  toggle(name, force) { if (force) this.values.add(name); else this.values.delete(name); }
}
class Element {
  constructor() {
    this.disabled = false; this.checked = false; this.hidden = false;
    this.textContent = ""; this.classList = new ClassList();
  }
}
const els = {
  developerTraceEnabled: new Element(), developerTraceCopy: new Element(),
  developerTraceClear: new Element(), developerTraceStatus: new Element(),
  developerTracePath: new Element(),
};
const requests = [];
let current = {
  enabled: false, captures_sensitive_data: true, authentication_tokens_logged: false,
  persistent: true, directory: "/private/logs/developer", bytes: 0, events: 0,
};
globalThis.els = els;
globalThis.confirm = () => true;
let copied = "";
Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  value: { clipboard: { writeText: async value => { copied = value; } } },
});
globalThis.authFetch = async (path, options = {}) => {
  requests.push({ path, options });
  if (path === "/v1/developer/trace" && options.method === "PUT") {
    const { enabled } = JSON.parse(options.body);
    current = { ...current, enabled, path: "/private/logs/developer/developer-trace-id.jsonl", events: enabled ? 1 : 12 };
    return { ok: true, json: async () => current };
  }
  if (path === "/v1/developer/trace/clear") {
    current = { ...current, path: undefined, bytes: 0, events: 0 };
    return { ok: true, json: async () => ({ removed: 1 }) };
  }
  return { ok: true, json: async () => current };
};

const begin = app.indexOf("      // Developer mode is gateway-owned and persistent.");
const end = app.indexOf("      // --- Advanced runtime settings BEGIN");
assert.ok(begin >= 0 && end > begin, "Developer mode UI block must remain extractable");
const api = eval(`(() => {${app.slice(begin, end)} return {
  refreshDeveloperTrace, setDeveloperTrace, copyDeveloperTracePath, clearDeveloperTraces,
  state: () => developerTraceState
};})()`);

await api.refreshDeveloperTrace();
assert.equal(requests[0].path, "/v1/developer/trace");
assert.equal(els.developerTraceEnabled.checked, false);
assert.equal(els.developerTraceClear.disabled, false);
assert.match(els.developerTracePath.textContent, /Logs folder/);

await api.setDeveloperTrace(true);
assert.equal(JSON.parse(requests.at(-1).options.body).enabled, true);
assert.equal(els.developerTraceEnabled.checked, true);
assert.equal(els.developerTraceClear.disabled, true, "active logs cannot be cleared");
assert.match(els.developerTraceStatus.textContent, /Recording/);
await api.copyDeveloperTracePath();
assert.equal(copied, current.path);

const beforeActiveClear = requests.length;
await api.clearDeveloperTraces();
assert.equal(requests.length, beforeActiveClear, "clear must be inert while tracing is active");
await api.setDeveloperTrace(false);
await api.clearDeveloperTraces();
assert.ok(requests.some(request => request.path === "/v1/developer/trace/clear"));
assert.equal(api.state().enabled, false);
assert.equal(els.developerTraceEnabled.disabled, false);

process.stdout.write("Developer mode Settings UI fixtures: PASS\n");
