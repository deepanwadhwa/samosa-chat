/* Focused DOM-fixture coverage for Settings > Advanced. The production UI is
 * one dependency-free HTML file, so this evaluates the exact shipped runtime
 * settings block against a deliberately small DOM instead of duplicating the
 * behavior in a test-only implementation. Gateway persistence/restart safety
 * remains covered by the compiled-gateway integration tests. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class ClassList {
  constructor(owner) { this.owner = owner; }
  _values() { return new Set((this.owner.className || "").split(/\s+/).filter(Boolean)); }
  toggle(value, force) {
    const values = this._values();
    const add = force === undefined ? !values.has(value) : !!force;
    if (add) values.add(value); else values.delete(value);
    this.owner.className = [...values].join(" ");
    return add;
  }
  contains(value) { return this._values().has(value); }
}

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.dataset = {};
    this.attrs = {};
    this.className = "";
    this.disabled = false;
    this.hidden = false;
    this.checked = false;
    this.value = "";
    this._textContent = "";
  }
  get classList() { return new ClassList(this); }
  get options() { return this.children; }
  get textContent() { return this._textContent; }
  set textContent(value) { this._textContent = String(value); this.children = []; }
  appendChild(child) { this.children.push(child); return child; }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; }
  focus() {}
}

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");

function occurrences(source, pattern) { return [...source.matchAll(pattern)].length; }

// The pane is a first-class member of the existing Settings shell, and the
// old General pane no longer owns machine-runtime controls.
const settingsMarkup = app.slice(app.indexOf('<aside class="settings"'), app.indexOf("  <script>"));
assert.equal(occurrences(settingsMarkup, /data-settings-pane="advanced"/g), 1);
assert.equal(occurrences(settingsMarkup, /data-settings-pane-content="advanced"/g), 1);
for (const id of ["cpuThreads", "cpuEffective", "contextTokens", "contextCustom", "contextEffective",
  "autoCompact", "compactThreshold", "compactNow", "runtimeApply", "runtimeSettingsStatus"]) {
  assert.match(app, new RegExp(`id="${id}"`), `${id} must exist in Advanced settings`);
}
assert.match(app, /id="runtimeSettingsStatus"[^>]*role="status"[^>]*aria-live="polite"/);
const generalMarkup = app.slice(
  app.indexOf('data-settings-pane-content="general"'),
  app.indexOf('data-settings-pane-content="models"'),
);
assert.doesNotMatch(generalMarkup, /id="(?:cpuThreads|contextTokens|autoCompact|compactThreshold|compactNow)"/,
  "General must not retain a second copy of machine-runtime controls");
assert.doesNotMatch(app, /authFetch\("\/v1\/settings"/,
  "the browser must not call the legacy Qwen-only runtime route");
const localSettingsBlock = app.slice(app.indexOf("      function defaultSettings()"), app.indexOf("      function activeChat()"));
assert.doesNotMatch(localSettingsBlock, /contextTokens|contextConfigured|autoCompact|compactThreshold|cpuThreads/,
  "localStorage must not own machine-runtime settings");

// Exercise the actual pane router so the nav item cannot silently fall back
// to General when its name is omitted from the allow-list.
{
  const panes = ["general", "models", "advanced", "voice", "privacy"].map(name => {
    const element = new Element("section"); element.dataset.settingsPaneContent = name; element.hidden = name !== "general"; return element;
  });
  const buttons = ["general", "models", "advanced", "voice", "privacy"].map(name => {
    const element = new Element("button"); element.dataset.settingsPane = name; return element;
  });
  globalThis.runtimeSettings = {}; // prevents the optional first-open refresh in this isolated router fixture
  globalThis.refreshRuntimeSettings = () => { throw new Error("an already-hydrated pane must not refetch"); };
  globalThis.refreshDeveloperTrace = () => {};
  globalThis.document = {
    querySelectorAll(selector) {
      if (selector === "[data-settings-pane-content]") return panes;
      if (selector === "[data-settings-pane]") return buttons;
      return [];
    },
  };
  const begin = app.indexOf("      function selectSettingsPane(name)");
  const end = app.indexOf("      function applyAppearance(value)");
  assert.ok(begin >= 0 && end > begin, "settings pane router must remain extractable");
  const { selectSettingsPane } = eval(`(() => {${app.slice(begin, end)} return {selectSettingsPane};})()`);
  selectSettingsPane("advanced");
  assert.equal(panes.find(pane => pane.dataset.settingsPaneContent === "advanced").hidden, false);
  assert.ok(panes.filter(pane => !pane.hidden).length === 1, "only Advanced should be visible");
  assert.ok(buttons.find(button => button.dataset.settingsPane === "advanced").classList.contains("active"));
  assert.equal(buttons.find(button => button.dataset.settingsPane === "advanced").getAttribute("aria-current"), "page");
  selectSettingsPane("not-a-pane");
  assert.equal(panes.find(pane => pane.dataset.settingsPaneContent === "general").hidden, false, "unknown pane names fall back safely");
}

function freshEls() {
  const compactThreshold = new Element("select");
  for (const value of [70, 75, 80, 85, 90]) {
    const option = new Element("option"); option.value = String(value); option.textContent = `At ${value}% projected use`; compactThreshold.appendChild(option);
  }
  compactThreshold.value = "80";
  const els = {
    cpuThreads: new Element("select"), cpuThreadsHelp: new Element("small"), cpuEffective: new Element("span"),
    contextTokens: new Element("select"), contextCustom: new Element("input"), contextEffective: new Element("span"), contextHelp: new Element("small"),
    autoCompact: new Element("input"), compactThreshold, compactNow: new Element("button"), compactHelp: new Element("small"),
    runtimeApply: new Element("button"), runtimeApplyTitle: new Element("strong"), runtimeSettingsStatus: new Element("span"),
    closure: new Element("span"), prompt: new Element("textarea"), context: new Element("span"),
  };
  for (const control of [els.cpuThreads, els.contextTokens, els.contextCustom, els.autoCompact, els.compactThreshold, els.compactNow, els.runtimeApply]) control.disabled = true;
  els.contextTokens.value = "auto"; els.contextCustom.hidden = true; els.autoCompact.checked = true;
  return els;
}

function response(body, ok = true) { return { ok, json: async () => body }; }
function settingsBody(overrides = {}) {
  return {
    backend: "qwen", label: "Qwen 35B",
    cpu_threads: { requested: 3, effective: 3, maximum: 4, source: "configured", locked: false },
    context: { requested: 24576, effective: 24576, maximum: 131072, source: "configured", locked: false },
    compaction: { supported: true, manual_supported: true, auto: true, threshold_percent: 80, reason: "" },
    apply_requires_restart: true,
    ...overrides,
  };
}

const runtimeBegin = app.indexOf("      // --- Advanced runtime settings BEGIN");
const runtimeEnd = app.indexOf("      // --- Advanced runtime settings END");
assert.ok(runtimeBegin >= 0 && runtimeEnd > runtimeBegin, "Advanced runtime block must remain extractable");
const runtimeSource = app.slice(runtimeBegin, runtimeEnd);

function loadRuntime(responder) {
  const requests = [];
  const els = freshEls();
  let healthCalls = 0;
  globalThis.document = { createElement: tag => new Element(tag) };
  globalThis.els = els;
  globalThis.generating = false;
  globalThis.authFetch = async (path, options = {}) => {
    requests.push({ path, options });
    return responder(path, options, requests.length);
  };
  globalThis.activeChat = () => null;
  globalThis.setGenerating = value => { globalThis.generating = value; };
  globalThis.health = () => { healthCalls++; };
  const fns = eval(`(() => {${runtimeSource}
    return {refreshRuntimeSettings, renderRuntimeSettings, markRuntimeSettingsDirty, updateRuntimeControlAvailability,
      applyRuntimeSettings, syncContextCustom, contextValue,
      getSettings: () => runtimeSettings, isDirty: () => runtimeSettingsDirty};
  })()`);
  return { fns, els, requests, healthCalls: () => healthCalls };
}

// GET hydrates model-aware CPU/context choices and server capabilities.
{
  const { fns, els, requests } = loadRuntime(async () => response(settingsBody()));
  await fns.refreshRuntimeSettings({ force: true });
  assert.equal(requests[0].path, "/v1/runtime/settings");
  assert.equal(requests[0].options.cache, "no-store");
  assert.deepEqual(els.cpuThreads.options.map(option => option.value), ["auto", "1", "2", "3", "4"]);
  assert.equal(els.cpuThreads.value, "3");
  assert.equal(els.cpuThreads.disabled, false);
  assert.equal(els.contextTokens.value, "24576");
  assert.ok(els.contextTokens.options.some(option => option.value === "131072"));
  assert.ok(!els.contextTokens.options.some(option => option.value === "262144"), "presets above the model maximum must be omitted");
  assert.equal(els.autoCompact.checked, true);
  assert.equal(els.compactThreshold.value, "80");
  assert.equal(els.compactNow.disabled, false);
  assert.equal(els.runtimeApply.disabled, true, "a freshly hydrated form has nothing to apply");
}

// A routine same-backend refresh must not erase edits the user is making.
{
  let get = 0;
  const { fns, els } = loadRuntime(async () => response(get++ ? settingsBody({
    cpu_threads: { requested: "auto", effective: 2, maximum: 4, source: "auto", locked: false },
  }) : settingsBody()));
  await fns.refreshRuntimeSettings({ force: true });
  els.cpuThreads.value = "4"; fns.markRuntimeSettingsDirty();
  await fns.refreshRuntimeSettings();
  assert.equal(els.cpuThreads.value, "4", "background hydration must preserve a dirty control");
  assert.equal(fns.isDirty(), true);
}

// Unsupported features stay visible but truthfully unavailable, with the
// reason supplied by the runtime rather than a guessed frontend string.
{
  const ornith = settingsBody({
    backend: "ornith", label: "Ornith 9B",
    compaction: { supported: false, manual_supported: false, auto: false, threshold_percent: 80, reason: "Ornith does not support session compaction." },
  });
  const { fns, els } = loadRuntime(async () => response(ornith));
  await fns.refreshRuntimeSettings({ force: true });
  assert.equal(els.cpuThreads.disabled, false);
  assert.equal(els.contextTokens.disabled, false);
  assert.equal(els.autoCompact.disabled, true);
  assert.equal(els.compactThreshold.disabled, true);
  assert.equal(els.compactNow.disabled, true);
  assert.equal(els.compactHelp.textContent, ornith.compaction.reason);
}

// Environment-owned values cannot masquerade as editable settings.
{
  const managed = settingsBody({ cpu_threads: { requested: 4, effective: 4, maximum: 4, source: "environment", locked: true } });
  const { fns, els } = loadRuntime(async () => response(managed));
  await fns.refreshRuntimeSettings({ force: true });
  assert.equal(els.cpuThreads.disabled, true);
  assert.match(els.cpuThreadsHelp.textContent, /environment override/i);
  assert.equal(els.contextTokens.disabled, false, "an unrelated unlocked control must remain editable");
}

// Apply sends one exact gateway-owned PATCH, disables controls while pending,
// and reports the restart state returned by the server.
{
  const initial = settingsBody({ context: { requested: 32768, effective: 32768, maximum: 131072, source: "configured", locked: false } });
  const applied = settingsBody({
    cpu_threads: { requested: 4, effective: 4, maximum: 4, source: "configured", locked: false },
    context: { requested: 65536, effective: 65536, maximum: 131072, source: "configured", locked: false },
    compaction: { supported: true, manual_supported: true, auto: false, threshold_percent: 75, reason: "" },
    restarted: true, loading: true,
  });
  let releasePatch;
  const { fns, els, requests, healthCalls } = loadRuntime(async (_path, options) => {
    if (!options.method) return response(initial);
    return new Promise(resolve => { releasePatch = () => resolve(response(applied)); });
  });
  await fns.refreshRuntimeSettings({ force: true });
  els.cpuThreads.value = "4"; els.contextTokens.value = "65536";
  els.autoCompact.checked = false; els.compactThreshold.value = "75";
  fns.markRuntimeSettingsDirty();
  const pending = fns.applyRuntimeSettings();
  await Promise.resolve();
  assert.equal(els.runtimeApply.disabled, true, "Apply must lock while the restart request is pending");
  releasePatch();
  assert.equal(await pending, true);
  const patch = requests.find(request => request.options.method === "PATCH");
  assert.ok(patch, "Apply must use PATCH");
  assert.equal(patch.path, "/v1/runtime/settings");
  assert.deepEqual(JSON.parse(patch.options.body), {
    cpu_threads: 4, context_tokens: 65536, auto_compact: false, compact_threshold_percent: 75,
  });
  assert.equal(healthCalls(), 1);
  assert.match(els.runtimeSettingsStatus.textContent, /restarting/i);
  assert.equal(fns.isDirty(), false);
}

// Client validation makes no PATCH and a gateway error leaves the chosen
// values intact so the user can correct or retry them.
{
  const requestsBeforePatch = [];
  const { fns, els, requests } = loadRuntime(async (_path, options) => {
    requestsBeforePatch.push(options.method || "GET");
    if (!options.method) return response(settingsBody());
    return response({ error: { message: "Stop the current response before restarting the model." } }, false);
  });
  await fns.refreshRuntimeSettings({ force: true });
  els.contextTokens.value = "custom"; els.contextCustom.value = ""; fns.syncContextCustom(); fns.markRuntimeSettingsDirty();
  assert.equal(await fns.applyRuntimeSettings(), false);
  assert.equal(requests.filter(request => request.options.method === "PATCH").length, 0);
  assert.match(els.runtimeSettingsStatus.textContent, /2 or more/i);
  els.contextCustom.value = "1"; fns.markRuntimeSettingsDirty();
  assert.equal(await fns.applyRuntimeSettings(), false);
  assert.equal(requests.filter(request => request.options.method === "PATCH").length, 0, "a one-token context must fail client validation");
  assert.match(els.runtimeSettingsStatus.textContent, /2 or more/i);
  els.contextCustom.value = "65536"; els.cpuThreads.value = "4"; fns.markRuntimeSettingsDirty();
  assert.equal(await fns.applyRuntimeSettings(), false);
  assert.equal(els.contextCustom.value, "65536");
  assert.equal(els.cpuThreads.value, "4");
  assert.match(els.runtimeSettingsStatus.textContent, /Stop the current response/i);
}

console.log("runtime settings UI DOM fixtures: PASS");
