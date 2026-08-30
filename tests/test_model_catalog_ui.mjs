/* T2.1/Settings-models DOM fixture coverage for refreshSettingsModels(), the
 * function that lets a returning user (past first-run setup) reach the same
 * catalog-driven Download/Pause/Resume/Cancel/Retry/Use cards from Settings
 * that setupEls.modelList already offered during onboarding. The production
 * app is a dependency-free static page (no browser test runner in this
 * repo), so this evaluates the exact shipped source block against a tiny
 * hand-rolled fixture instead of introducing a real-browser dependency --
 * same pattern as tests/test_chooser_ui.mjs and tests/test_setup_flow_ui.mjs.
 * It cannot replace a real Safari/Chrome/Firefox click-through.
 *
 * What it proves: the fetch orchestration (setup/status, then catalog +
 * installs in parallel), the module-scope modelCatalog/setupInstalls
 * assignment, the settingsModelError show/hide, and the exact container/
 * errorEl/status arguments handed to renderModelCards -- not
 * renderModelCards' own rendering, which tests/test_setup_flow_ui.mjs
 * already covers directly and this file deliberately does not re-test.
 *
 * This file previously covered the catalog-driven <select id="modelBackend">
 * (modelIsSelectable/modelOptionLabel/renderModelOptions/refreshModels).
 * That dropdown-only UI is gone -- a not-installed model in Settings now
 * gets the same card-based Download action the setup flow always had,
 * closing the "no way to add a model after onboarding" gap. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      async function refreshSettingsModels() {");
const end = app.indexOf("      function contextValue() {");
assert.ok(begin >= 0 && end > begin, "the refreshSettingsModels block must remain extractable");

function loadSettingsModelsUi({ fetchSetupStatus, authFetch }) {
  globalThis.els = {
    settingsModelList: { tag: "settingsModelList" },
    settingsVisionModelList: { tag: "settingsVisionModelList" },
    settingsModelError: { hidden: true, textContent: "" },
  };
  globalThis.modelCatalog = [];
  globalThis.setupInstalls = [];
  globalThis.fetchSetupStatus = fetchSetupStatus;
  globalThis.authFetch = authFetch;
  const renderCalls = [];
  globalThis.renderModelCards = (container, errorEl, status, customCatalog) => {
    renderCalls.push({ container, errorEl, status, customCatalog });
  };
  const fns = eval(`(() => {${app.slice(begin, end)}
    return { refreshSettingsModels };
  })()`);
  return { fns, els: globalThis.els, renderCalls };
}

function jsonResponse(body, ok = true) { return { ok, json: async () => body }; }

const READY_QWEN = { id: "qwen", label: "Qwen3.6 35B A3B", compatible: true, install_state: "ready" };
const NOT_INSTALLED_BONSAI = { id: "bonsai", label: "Bonsai 27B 1-bit", compatible: true, install_state: "not_installed" };
const VISIONPSY_MODEL = { id: "visionpsy-nano-460m-mlx-bf16", label: "VisionPsy-Nano 460M", category: "vision", role: "auxiliary", compatible: true, install_state: "ready" };
const MOLMO_DIRECT_MODEL = { id: "molmo2-4b-mlx-q4-v1", label: "Molmo2 4B Native Q4", family: "chat", category: "chat", role: "primary", compatible: true, install_state: "ready", load_policy: "on_demand_per_turn" };
const STATUS = { active_install_job_id: null, active_selection_operation_id: null, active_model_ready: true };

// --- happy path: setup/status, then catalog + installs, then separated render calls ---
{
  const requested = [];
  const { fns, els, renderCalls } = loadSettingsModelsUi({
    fetchSetupStatus: async () => STATUS,
    authFetch: async (path, opts) => {
      requested.push(path);
      if (path === "/v1/models/catalog") return jsonResponse({ models: [READY_QWEN, NOT_INSTALLED_BONSAI, MOLMO_DIRECT_MODEL, VISIONPSY_MODEL] });
      if (path === "/v1/models/installs") return jsonResponse({ jobs: [{ job_id: "j1", model_id: "bonsai" }] });
      throw new Error(`unexpected path ${path}`);
    },
  });
  await fns.refreshSettingsModels();
  assert.deepEqual(requested.sort(), ["/v1/models/catalog", "/v1/models/installs"]);
  // modelCatalog should only have text LLMs, excluding visionpsy
  assert.deepEqual(globalThis.modelCatalog, [READY_QWEN, NOT_INSTALLED_BONSAI, MOLMO_DIRECT_MODEL], "direct Molmo must be selectable while auxiliary-only vision models stay separated");
  assert.deepEqual(globalThis.setupInstalls, [{ job_id: "j1", model_id: "bonsai" }]);
  assert.equal(els.settingsModelError.hidden, true);
  // Two render calls: one for Text LLMs (settingsModelList), one for Vision (settingsVisionModelList)
  assert.equal(renderCalls.length, 2);
  assert.equal(renderCalls[0].container, els.settingsModelList, "must render text models into the Text container");
  assert.deepEqual(renderCalls[0].customCatalog, [READY_QWEN, NOT_INSTALLED_BONSAI, MOLMO_DIRECT_MODEL]);
  assert.equal(renderCalls[1].container, els.settingsVisionModelList, "must render vision models into the Vision container");
  assert.deepEqual(renderCalls[1].customCatalog, [VISIONPSY_MODEL]);
}

// --- a failed catalog fetch shows an error and never calls renderModelCards ---
{
  const { fns, els, renderCalls } = loadSettingsModelsUi({
    fetchSetupStatus: async () => STATUS,
    authFetch: async (path) => (path === "/v1/models/catalog" ? jsonResponse({}, false) : jsonResponse({ jobs: [] })),
  });
  await fns.refreshSettingsModels();
  assert.equal(els.settingsModelError.hidden, false);
  assert.match(els.settingsModelError.textContent, /couldn't load/i);
  assert.equal(renderCalls.length, 0, "a failed catalog fetch must not render a stale or empty card list silently");
}

// --- setup/status failing must not abort the catalog refresh (Settings works even if that call fails) ---
{
  const { fns, renderCalls } = loadSettingsModelsUi({
    fetchSetupStatus: async () => { throw new Error("network down"); },
    authFetch: async (path) => (path === "/v1/models/catalog" ? jsonResponse({ models: [READY_QWEN, VISIONPSY_MODEL] }) : jsonResponse({ jobs: [] })),
  });
  await fns.refreshSettingsModels();
  assert.equal(renderCalls.length, 2, "a setup/status failure is non-fatal -- the model list still renders");
  assert.equal(renderCalls[0].status, null, "status is null, not a stale or fabricated value, when setup/status could not be read");
}

// --- an unavailable installs list degrades to no jobs, not a fatal error ---
{
  const { fns, renderCalls } = loadSettingsModelsUi({
    fetchSetupStatus: async () => STATUS,
    authFetch: async (path) => (path === "/v1/models/catalog" ? jsonResponse({ models: [READY_QWEN] }) : jsonResponse({}, false)),
  });
  await fns.refreshSettingsModels();
  assert.deepEqual(globalThis.setupInstalls, [], "installs endpoint failing must not block rendering the catalog itself");
  assert.equal(renderCalls.length, 2);
}

process.stdout.write("settings model catalog UI DOM fixtures: PASS\n");
