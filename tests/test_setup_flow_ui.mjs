/* T2.4 (docs/TASKS_UI_CHUTNI.md sec4.1-4.3) DOM fixture coverage for the
 * Name -> Welcome -> Model -> Chat setup flow's frontend logic. Same pattern
 * as tests/test_model_catalog_ui.mjs and tests/test_chooser_ui.mjs: no
 * browser-automation tool exists in this environment, so this evaluates the
 * exact shipped source block against a tiny hand-rolled DOM fixture instead.
 * It cannot replace a real interactive click-through -- what it proves is
 * that the pure view/state-derivation logic (which next_step maps to which
 * view, which action buttons a given install-job state offers, which error
 * codes are retryable) behaves correctly, and that the model-card renderer
 * produces the right structure for a representative state of each kind.
 * The real server-side transitions this logic reacts to (install progress,
 * selection success/failure, restart repair) are covered end to end against
 * the real compiled gateway by tests/test_setup_flow.sh -- this file does
 * not re-test that.
 */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.attrs = {};
    this.style = {};
    this._className = "";
    this._textContent = "";
    this.disabled = false;
    this.hidden = false;
  }
  appendChild(child) { this.children.push(child); return child; }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; }
  get className() { return this._className; }
  set className(v) { this._className = v || ""; }
  get classList() {
    const self = this;
    return {
      add(c) { const s = new Set(self._className.split(" ").filter(Boolean)); s.add(c); self._className = [...s].join(" "); },
      contains(c) { return self._className.split(" ").filter(Boolean).includes(c); },
    };
  }
  get textContent() { return this._textContent; }
  set textContent(v) { this._textContent = String(v); this.children = []; }
  get innerHTML() { return this._innerHTML || ""; }
  set innerHTML(value) { this._innerHTML = value; this.children = []; this._textContent = ""; }
  querySelectorAll(sel) {
    if (sel !== "button") return [];
    const found = [];
    const walk = node => { for (const child of node.children) { if (child.tagName === "button") found.push(child); walk(child); } };
    walk(this);
    return found;
  }
}

// setupEls (like the real file's els/jobEls/chooserEls) is built by calling
// $("someId") once at closure-eval time -- a registry keyed by id, reused
// per fixture, lets a test grab the exact same fake node back out (e.g.
// setupEls.modelList) to assert on afterward.
let fakeElementsById;
globalThis.document = {
  createElement: tag => new Element(tag),
  createTextNode: text => { const n = new Element("#text"); n.textContent = text; return n; },
  getElementById: id => { if (!fakeElementsById[id]) fakeElementsById[id] = new Element("div"); return fakeElementsById[id]; },
};
globalThis.$ = id => document.getElementById(id);

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      const setupEls = {");
const end = app.indexOf("      function showView(name) {");
assert.ok(begin >= 0 && end > begin, "T2.4 setup-flow block must remain extractable");

function loadSetupFlowUi() {
  fakeElementsById = {};
  globalThis.els = {};
  globalThis.state = {};
  globalThis.backend = { id: "qwen" };
  globalThis.modelCatalog = [];
  const fns = eval(`(() => {${app.slice(begin, end)}
    return { setupViewForNextStep, setupViewForStatus, formatBytes, installForModel, installErrorMessage, installErrorRetryable,
      selectionErrorMessage, modelCardState, modelCardActions, modelCardStatusText, buildModelCard, renderModelCards };
  })()`);
  return { fns, byId: fakeElementsById };
}

// --- once Chat exists, transient model lifecycle states stay inside Chat/Settings ---
{
  const { fns } = loadSetupFlowUi();
  assert.equal(fns.setupViewForStatus({ next_step: "download" }, true), "chat",
    "a Settings download must not reopen first-run model setup");
  assert.equal(fns.setupViewForStatus({ next_step: "model" }, true), "chat",
    "a Settings switch failure must stay in Chat where its error can be handled");
  assert.equal(fns.setupViewForStatus({ next_step: "download" }, false), "model",
    "the same state still renders the model step during genuine first-run setup");
}

// --- setupViewForNextStep: collapses "download" into "model", passes the rest through ---
{
  const { fns } = loadSetupFlowUi();
  assert.equal(fns.setupViewForNextStep("name"), "name");
  assert.equal(fns.setupViewForNextStep("welcome"), "welcome");
  assert.equal(fns.setupViewForNextStep("model"), "model");
  assert.equal(fns.setupViewForNextStep("download"), "model", "download is part of the Model view, not a 5th screen");
  assert.equal(fns.setupViewForNextStep("chat"), "chat");
}

// --- formatBytes ---
{
  const { fns } = loadSetupFlowUi();
  assert.equal(fns.formatBytes(0), "0 B");
  assert.equal(fns.formatBytes(500), "500 B");
  assert.equal(fns.formatBytes(2048), "2.0 KB");
  assert.equal(fns.formatBytes(20942159872), "19.5 GB", "real Qwen experts.bin size, sanity-checked against the actual catalog value");
}

// --- installForModel: prefers the setup/status-reported active job id, else the latest by updated_at ---
{
  const { fns } = loadSetupFlowUi();
  const installs = [
    { job_id: "old", model_id: "bonsai", state: "failed", updated_at: "2026-07-01T00:00:00Z" },
    { job_id: "new", model_id: "bonsai", state: "downloading", updated_at: "2026-07-02T00:00:00Z" },
    { job_id: "other", model_id: "ornith", state: "downloading", updated_at: "2026-07-03T00:00:00Z" },
  ];
  assert.equal(fns.installForModel("bonsai", installs, null).job_id, "new", "no active id -> most recently updated for this model");
  assert.equal(fns.installForModel("bonsai", installs, "old").job_id, "old", "an explicit active_install_job_id wins even if not the latest");
  assert.equal(fns.installForModel("qwen", installs, null), null, "no jobs for this model -> null");
}

// --- install error copy: artifact_not_downloadable is the one non-recoverable code ---
{
  const { fns } = loadSetupFlowUi();
  assert.equal(fns.installErrorRetryable("download_failed"), true);
  assert.equal(fns.installErrorRetryable("checksum_mismatch"), true);
  assert.equal(fns.installErrorRetryable("insufficient_space"), true);
  assert.equal(fns.installErrorRetryable("artifact_not_downloadable"), false, "no download host exists at all -- retrying cannot help");
  assert.match(fns.installErrorMessage("insufficient_space"), /space/i);
  assert.equal(fns.installErrorMessage("some_unknown_code"), "The download failed.", "an unrecognized code still gets a safe, honest fallback");
}
{
  const { fns } = loadSetupFlowUi();
  assert.match(fns.selectionErrorMessage("readiness_timeout"), /ready in time/i);
  assert.equal(fns.selectionErrorMessage("totally_unknown"), "This model could not be started.");
}

// --- modelCardState: incompatible short-circuits before any job/install_state check ---
{
  const { fns } = loadSetupFlowUi();
  const incompatible = { compatible: false, compatibility_reason: "arm64 only", install_state: "unavailable" };
  assert.deepEqual(fns.modelCardState(incompatible, { state: "downloading" }, false), { kind: "incompatible", reason: "arm64 only" });
}
// --- modelCardState: job state takes priority over the catalog's coarse install_state ---
{
  const { fns } = loadSetupFlowUi();
  const compatibleModel = { compatible: true, install_state: "not_installed" };
  assert.equal(fns.modelCardState(compatibleModel, { state: "downloading" }, false).kind, "downloading");
  assert.equal(fns.modelCardState(compatibleModel, { state: "paused" }, false).kind, "paused");
  const failedState = fns.modelCardState(compatibleModel, { state: "failed", error: { code: "checksum_mismatch" } }, false);
  assert.equal(failedState.kind, "failed");
  assert.equal(failedState.retryable, true);
  assert.match(failedState.message, /verify/);
  const noDownload = fns.modelCardState(compatibleModel, { state: "failed", error: { code: "artifact_not_downloadable" } }, false);
  assert.equal(noDownload.retryable, false);
}
// --- modelCardState: no job -> ready/active/activating/not_installed from the catalog + live flags ---
{
  const { fns } = loadSetupFlowUi();
  assert.equal(fns.modelCardState({ compatible: true, install_state: "not_installed" }, null, false).kind, "not_installed");
  assert.equal(fns.modelCardState({ compatible: true, install_state: "ready", active: false }, null, false).kind, "ready");
  assert.equal(fns.modelCardState({ compatible: true, install_state: "ready", active: true }, null, false).kind, "active");
  assert.equal(fns.modelCardState({ compatible: true, install_state: "ready", active: true }, null, false, false).kind, "activating",
    "a catalog-active model whose backend is still loading must show progress instead of looking inert");
  assert.equal(fns.modelCardState({ compatible: true, install_state: "ready", active: false }, null, true).kind, "activating",
    "a ready-but-not-yet-active model while a selection is in flight reads as activating, not a plain 'ready' invite to click again");
}

// --- modelCardActions: exactly the actions each state should offer ---
{
  const { fns } = loadSetupFlowUi();
  assert.deepEqual(fns.modelCardActions({ kind: "incompatible" }).map(a => a.action), []);
  assert.deepEqual(fns.modelCardActions({ kind: "not_installed" }).map(a => a.action), ["install"]);
  assert.deepEqual(fns.modelCardActions({ kind: "downloading", job: { state: "downloading" } }).map(a => a.action), ["pause", "cancel"]);
  assert.deepEqual(fns.modelCardActions({ kind: "downloading", job: { state: "verifying" } }).map(a => a.action), ["cancel"],
    "verifying/installing cannot be paused server-side, so no Pause button is offered");
  assert.deepEqual(fns.modelCardActions({ kind: "paused" }).map(a => a.action), ["resume", "cancel"]);
  assert.deepEqual(fns.modelCardActions({ kind: "failed", retryable: true }).map(a => a.action), ["retry"]);
  assert.deepEqual(fns.modelCardActions({ kind: "failed", retryable: false }).map(a => a.action), [], "a non-recoverable failure offers no Retry");
  assert.deepEqual(fns.modelCardActions({ kind: "ready" }).map(a => a.action), ["select"]);
  assert.deepEqual(fns.modelCardActions({ kind: "active" }).map(a => a.action), []);
  assert.deepEqual(fns.modelCardActions({ kind: "activating" }).map(a => a.action), []);
}

// --- buildModelCard: a downloading card shows a determinate progress bar and Pause/Cancel ---
{
  const { fns } = loadSetupFlowUi();
  const model = { id: "bonsai", label: "Bonsai", description: "test", version: "v1", download_bytes: 1000, required_free_bytes: 2000, capabilities: ["text"], license: { name: "MIT", url: "https://example.test/license" }, compatible: true, install_state: "not_installed" };
  const job = { state: "downloading", completed_bytes: 250, total_bytes: 1000, current_artifact: "weights.bin" };
  const card = fns.buildModelCard(model, job, false);
  const buttons = card.querySelectorAll("button");
  assert.deepEqual(buttons.map(b => b.textContent), ["Pause", "Cancel"]);
  const progressWrap = card.children.find(c => c.className === "model-card-progress");
  assert.ok(progressWrap, "a downloading card must render its progress block");
  const fill = progressWrap.children[0].children[0];
  assert.equal(fill.style.width, "25.0%");
  assert.equal(fill.classList.contains("indeterminate"), false);
}

// --- buildModelCard: an incompatible card shows the reason and no action buttons ---
{
  const { fns } = loadSetupFlowUi();
  const model = { id: "ornith", label: "Ornith", version: "v1", compatible: false, compatibility_reason: "Needs arm64.", install_state: "unavailable", capabilities: [] };
  const card = fns.buildModelCard(model, null, false);
  assert.equal(card.querySelectorAll("button").length, 0);
  const note = card.children.find(c => c.className === "model-card-incompatible");
  assert.equal(note.textContent, "Needs arm64.");
}

// --- buildModelCard: a ready, already-installed model offers "Use this model" ---
{
  const { fns } = loadSetupFlowUi();
  const model = { id: "qwen", label: "Qwen", version: "v1", compatible: true, install_state: "ready", active: false, capabilities: ["text", "image"] };
  const card = fns.buildModelCard(model, null, false);
  const buttons = card.querySelectorAll("button");
  assert.deepEqual(buttons.map(b => b.textContent), ["Use this model"]);
}

// --- selecting from Settings returns to a fresh chat; it never invokes the onboarding router ---
{
  fakeElementsById = {};
  let newChatCalls = 0, healthCalls = 0;
  globalThis.els = { settings: { classList: { contains: () => false } } };
  globalThis.state = {};
  globalThis.backend = { id: "qwen" };
  globalThis.modelCatalog = [];
  globalThis.newChat = () => { newChatCalls++; };
  globalThis.health = async () => { healthCalls++; };
  globalThis.authFetch = async (path) => {
    assert.equal(path, "/v1/backends/select", "a completed Settings selection must not fetch setup/status again");
    return { ok: true, json: async () => ({ accepted: true, job_id: "selection-1" }) };
  };
  const { runModelCardAction } = eval(`(() => {${app.slice(begin, end)}
    chatBootstrapped = true;
    return { runModelCardAction };
  })()`);
  const errorEl = { hidden: true, textContent: "" };
  await runModelCardAction("select", { id: "ornith", version: "v1" }, null, errorEl);
  assert.equal(errorEl.hidden, true);
  assert.equal(newChatCalls, 1, "a successful Settings selection should close Settings into a fresh conversation");
  assert.equal(healthCalls, 1, "the chat header should immediately refresh to Ready or Loading");
}

// --- renderModelCards: builds exactly one card per catalog entry, correlating jobs by model_id ---
{
  fakeElementsById = {};
  globalThis.els = {}; globalThis.state = {}; globalThis.backend = { id: "qwen" };
  globalThis.modelCatalog = [
    { id: "qwen", label: "Qwen", version: "v1", compatible: true, install_state: "not_installed", capabilities: [] },
    { id: "bonsai", label: "Bonsai", version: "v1", compatible: true, install_state: "not_installed", capabilities: [] },
  ];
  // renderModelCards reads the module-scope `setupInstalls`, populated here as part of the same eval'd
  // closure rather than assigned from outside it (a `let` in that closure isn't reachable any other way).
  const setupInstalls = [{ job_id: "j1", model_id: "bonsai", state: "downloading", completed_bytes: 1, total_bytes: 2, updated_at: "2026-07-01T00:00:00Z" }];
  const { renderModelCards } = eval(`(() => {${app.slice(begin, end)}
    setupInstalls = ${JSON.stringify(setupInstalls)};
    return { renderModelCards };
  })()`);
  const modelList = fakeElementsById["modelList"];
  const modelError = fakeElementsById["setupModelError"];
  renderModelCards(modelList, modelError, { active_install_job_id: null, active_selection_operation_id: null });
  assert.equal(modelList.children.length, 2, "one card per catalog model");
  const bonsaiCard = modelList.children.find(c => c.attrs["data-model-id"] === "bonsai");
  assert.ok(bonsaiCard.querySelectorAll("button").some(b => b.textContent === "Pause"), "the correlated downloading job must be reflected on its model's card");
}

process.stdout.write("setup flow UI DOM fixtures: PASS\n");
