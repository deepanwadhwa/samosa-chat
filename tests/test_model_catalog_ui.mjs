/* T2.1 (docs/TASKS_UI_CHUTNI.md sec5.3) DOM fixture coverage for the
 * catalog-driven model <select>. The production app is a dependency-free
 * static page (no browser test runner in this repo), so this evaluates the
 * exact shipped source block against a tiny hand-rolled DOM fixture instead
 * of introducing a real-browser dependency -- same pattern as
 * tests/test_chooser_ui.mjs. It cannot replace a real Safari/Chrome/Firefox
 * click-through. What it proves: the <option> list is rebuilt from whatever
 * GET /v1/models/catalog returns (ids, labels, compatibility, install
 * state), not from a hardcoded list baked into app.html. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.value = "";
    this.disabled = false;
    this.selected = false;
    this.textContent = "";
    this.attrs = {};
  }
  appendChild(child) { this.children.push(child); return child; }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; }
  get innerHTML() { return this._innerHTML || ""; }
  set innerHTML(value) { this._innerHTML = value; this.children = []; }
}

// Mimics real <select> value semantics closely enough for this test: the
// setter selects whichever option matches, or leaves nothing selected (and
// therefore value === "") if no option matches -- it does not retain
// whatever was previously selected.
class SelectElement extends Element {
  constructor() { super("select"); }
  get options() { return this.children; }
  get value() {
    const picked = this.children.find(o => o.selected);
    return picked ? picked.value : "";
  }
  set value(v) {
    for (const o of this.children) o.selected = (o.value === v);
  }
}

globalThis.document = {
  createElement: tag => new Element(tag),
};

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      function modelIsSelectable");
const end = app.indexOf("      function contextValue");
assert.ok(begin >= 0 && end > begin, "T2.1 model-catalog block must remain extractable");

function loadModelCatalogUi({ authFetch, activeBackend }) {
  globalThis.els = { modelBackend: new SelectElement() };
  globalThis.backend = activeBackend || { id: "qwen" };
  globalThis.modelCatalog = [];
  globalThis.authFetch = authFetch;
  const fns = eval(`(() => {${app.slice(begin, end)}
    return { modelIsSelectable, modelOptionLabel, renderModelOptions, refreshModels };
  })()`);
  return { fns, els: globalThis.els };
}

function jsonResponse(body, ok = true) { return { ok, json: async () => body }; }

const READY_QWEN = { id: "qwen", label: "Qwen3.6 35B A3B", compatible: true, install_state: "ready" };
const NOT_INSTALLED_BONSAI = { id: "bonsai", label: "Bonsai 27B 1-bit", compatible: true, install_state: "not_installed" };
const INCOMPATIBLE_ORNITH = { id: "ornith", label: "Ornith 1.0 9B", compatible: false, install_state: "unavailable" };

// --- modelIsSelectable / modelOptionLabel: pure per-model classification --
{
  const { fns } = loadModelCatalogUi({ authFetch: async () => jsonResponse({}) });
  assert.equal(fns.modelIsSelectable(READY_QWEN), true);
  assert.equal(fns.modelOptionLabel(READY_QWEN), "Qwen3.6 35B A3B", "a ready, compatible model's label is unchanged");
  assert.equal(fns.modelIsSelectable(NOT_INSTALLED_BONSAI), false);
  assert.equal(fns.modelOptionLabel(NOT_INSTALLED_BONSAI), "Bonsai 27B 1-bit (not installed)");
  assert.equal(fns.modelIsSelectable(INCOMPATIBLE_ORNITH), false);
  assert.equal(fns.modelOptionLabel(INCOMPATIBLE_ORNITH), "Ornith 1.0 9B (not compatible with this Mac)");
}

// --- renderModelOptions: builds one <option> per catalog entry, no hardcoded list ---
{
  const { fns, els } = loadModelCatalogUi({ authFetch: async () => jsonResponse({}) });
  globalThis.modelCatalog = [READY_QWEN, NOT_INSTALLED_BONSAI, INCOMPATIBLE_ORNITH];
  fns.renderModelOptions();
  assert.equal(els.modelBackend.options.length, 3, "the select must have exactly the catalog's models, nothing baked in");
  const [qwenOpt, bonsaiOpt, ornithOpt] = els.modelBackend.options;
  assert.equal(qwenOpt.value, "qwen"); assert.equal(qwenOpt.disabled, false);
  assert.equal(bonsaiOpt.value, "bonsai"); assert.equal(bonsaiOpt.disabled, true, "not_installed must not be selectable");
  assert.equal(bonsaiOpt.textContent, "Bonsai 27B 1-bit (not installed)");
  assert.equal(ornithOpt.value, "ornith"); assert.equal(ornithOpt.disabled, true, "incompatible must not be selectable");
  assert.equal(els.modelBackend.value, "qwen", "falls back to the active backend id when nothing was already selected");
}

// --- renderModelOptions: preserves the current selection across a rebuild --
{
  const { fns, els } = loadModelCatalogUi({ authFetch: async () => jsonResponse({}), activeBackend: { id: "qwen" } });
  globalThis.modelCatalog = [READY_QWEN, NOT_INSTALLED_BONSAI];
  fns.renderModelOptions();
  els.modelBackend.value = "bonsai"; // simulate an explicit prior selection unrelated to backend.id
  globalThis.modelCatalog = [READY_QWEN, { ...NOT_INSTALLED_BONSAI, install_state: "ready" }];
  fns.renderModelOptions();
  assert.equal(els.modelBackend.value, "bonsai", "a still-present selection must survive a catalog refresh");
}

// --- renderModelOptions: falls back to backend.id if the prior selection vanished ---
{
  const { fns, els } = loadModelCatalogUi({ authFetch: async () => jsonResponse({}), activeBackend: { id: "ornith" } });
  globalThis.modelCatalog = [READY_QWEN, NOT_INSTALLED_BONSAI];
  fns.renderModelOptions();
  els.modelBackend.value = "bonsai";
  globalThis.modelCatalog = [READY_QWEN, { ...INCOMPATIBLE_ORNITH, install_state: "ready", compatible: true }]; // bonsai dropped from the catalog entirely
  fns.renderModelOptions();
  assert.equal(els.modelBackend.value, "ornith", "must fall back to the active backend id, not silently keep a stale selection");
}

// --- refreshModels: fetches the real catalog endpoint with the session token, not /v1/backends ---
{
  let requestedPath = null, requestedOpts = null;
  const { fns, els } = loadModelCatalogUi({
    authFetch: async (path, opts) => { requestedPath = path; requestedOpts = opts; return jsonResponse({ models: [READY_QWEN, NOT_INSTALLED_BONSAI] }); },
    activeBackend: { id: "qwen" },
  });
  await fns.refreshModels();
  assert.equal(requestedPath, "/v1/models/catalog", "T2.1: the model list must come from the trusted catalog endpoint");
  assert.deepEqual(requestedOpts, { cache: "no-store" });
  assert.equal(els.modelBackend.options.length, 2);
  assert.equal(els.modelBackend.options[0].textContent, "Qwen3.6 35B A3B");
}

// --- refreshModels: a failed fetch leaves the existing options alone ------
{
  const { fns, els } = loadModelCatalogUi({ authFetch: async () => jsonResponse({}, false), activeBackend: { id: "qwen" } });
  globalThis.modelCatalog = [READY_QWEN];
  fns.renderModelOptions();
  await fns.refreshModels();
  assert.equal(els.modelBackend.options.length, 1, "a non-ok response must not clear or corrupt the current select");
}

process.stdout.write("model catalog UI DOM fixtures: PASS\n");
