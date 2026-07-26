/* T1.4 (docs/TASKS_UI_CHUTNI.md sec5.2) DOM fixture coverage for the browser
 * conversation schema v1 -> v2 migration and the model-mismatch/"reopen
 * under another model" dialog. Same rationale as tests/test_chooser_ui.mjs:
 * this evaluates the exact shipped source block against a tiny hand-rolled
 * DOM/localStorage fixture rather than a real browser. It cannot replace a
 * real interactive click-through -- see the T1.4 evidence doc. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class ClassList {
  constructor(owner) { this.owner = owner; }
  _set() { return new Set((this.owner.className || "").split(/\s+/).filter(Boolean)); }
  add(...values) { const s = this._set(); values.forEach(v => s.add(v)); this.owner.className = [...s].join(" "); }
  remove(...values) { const s = this._set(); values.forEach(v => s.delete(v)); this.owner.className = [...s].join(" "); }
  contains(value) { return this._set().has(value); }
}

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = "";
    this.textContent = "";
    this.style = {};
    this.attrs = {};
    this.focusCount = 0;
    this.clickCount = 0;
    this.onclick = null;
  }
  get classList() { return new ClassList(this); }
  appendChild(child) { this.children.push(child); return child; }
  append(...kids) { kids.forEach(k => this.appendChild(k)); }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; }
  removeAttribute(name) { delete this.attrs[name]; }
  focus() { this.focusCount++; globalThis.__lastFocused = this; }
  click() { this.clickCount++; if (this.onclick) this.onclick(); }
}

class FakeStorage {
  constructor(initial = {}) { this.data = { ...initial }; this.failSet = false; }
  getItem(k) { return Object.prototype.hasOwnProperty.call(this.data, k) ? this.data[k] : null; }
  setItem(k, v) { if (this.failSet) throw new Error("QuotaExceededError"); this.data[k] = String(v); }
  removeItem(k) { delete this.data[k]; }
}

globalThis.document = {
  createElement: tag => new Element(tag),
  activeElement: null,
};

function freshEls() {
  const scrim = new Element("div"), dialog = new Element("div"), banner = new Element("div");
  scrim.hidden = true; dialog.hidden = true; banner.hidden = true; // matches the real markup's `hidden` attribute
  return {
    thinking: { value: "off", options: [{ text: "Off" }], selectedIndex: 0 },
    maxTokens: { value: "2048" }, seed: { value: "" },
    contextTokens: { value: "auto" }, contextCustom: { value: "" },
    autoCompact: { checked: true }, compactThreshold: { value: "80" },
    mismatchScrim: scrim, mismatchDialog: dialog, mismatchDetail: new Element("div"),
    mismatchFork: new Element("button"), mismatchCancel: new Element("button"),
    migrationBanner: banner, migrationExport: new Element("button"), migrationReset: new Element("button"),
  };
}

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      // T1.4 (docs/TASKS_UI_CHUTNI.md sec5.2): schema v2 adds gateway-enforced");
const end = app.indexOf("      function titleFor");
assert.ok(begin >= 0 && end > begin, "T1.4 migration/binding block must remain extractable");

let idCounter = 0;
function load({ storage = {}, confirmResult = true } = {}) {
  globalThis.els = freshEls();
  globalThis.localStorage = new FakeStorage(storage);
  globalThis.render = () => { globalThis.__renderCalls = (globalThis.__renderCalls || 0) + 1; };
  globalThis.closePanels = () => { globalThis.__closePanelsCalls = (globalThis.__closePanelsCalls || 0) + 1; };
  globalThis.self = globalThis;
  globalThis.confirm = () => confirmResult;
  globalThis.__reloaded = false;
  globalThis.location = { reload: () => { globalThis.__reloaded = true; } };
  const blobs = [];
  globalThis.Blob = class { constructor(parts, opts) { blobs.push({ parts, opts }); } };
  globalThis.URL = { createObjectURL: () => "blob:stub", revokeObjectURL: () => {} };
  globalThis.__renderCalls = 0; globalThis.__closePanelsCalls = 0;
  // `backend` and `generating` are declared *inside* the sliced block (they
  // sit between the migration code and selectChat in the real file), so a
  // pre-set globalThis.backend would just be shadowed. Mutate the real
  // binding through this accessor instead, matching the module's own id
  // ("qwen") and giving it a known version so mismatch checks are meaningful.
  const fns = eval(`(() => {${app.slice(begin, end)}
    return { migrateStateV1ToV2, migrateLegacyChat, chatNeedsExplicitAction, forkChatFrom,
             openMismatchDialog, closeMismatchDialog, confirmForkMismatch,
             showMigrationBanner, hideMigrationBanner, exportMigrationRecovery, resetMigrationRecovery,
             ensureChat, selectChat,
             getState: () => state, getCurrentId: () => currentId,
             getBackend: () => backend,
             getMigrationRecoveryRaw: () => migrationRecoveryRaw };
  })()`);
  Object.assign(fns.getBackend(), { id: "qwen", label: "Qwen", model: "qwen3.6-35b-a3b", version: "v1", ready: true });
  return { fns, els: globalThis.els, storage: globalThis.localStorage, blobs };
}

// --- migrateStateV1ToV2: missing/null input -> fresh empty state ----------
{
  const { fns } = load();
  const result = fns.migrateStateV1ToV2(null);
  assert.equal(result.ok, true);
  assert.deepEqual(result.state.chats, []);
}

// --- migrateStateV1ToV2: malformed top-level JSON -> ok:false --------------
{
  const { fns } = load();
  assert.equal(fns.migrateStateV1ToV2("{not json").ok, false);
  assert.equal(fns.migrateStateV1ToV2(JSON.stringify({ notChats: 1 })).ok, false);
}

// --- migrateStateV1ToV2: valid v1 -> v2 fields added, malformed record skipped, healthy ones kept ---
{
  const { fns } = load();
  const v1 = {
    currentId: "chat-a",
    chats: [
      { id: "chat-a", title: "Hello", created: 1000, updated: 2000, messages: [{ id: "m1", role: "user", content: "hi" }] },
      { id: "chat-bad", title: "Bad" }, // missing messages: must be skipped, not crash the whole migration
    ],
    settings: { thinking: "off" },
  };
  const result = fns.migrateStateV1ToV2(JSON.stringify(v1));
  assert.equal(result.ok, true);
  assert.equal(result.state.chats.length, 1, "the malformed record must be dropped without erasing the healthy one");
  const migrated = result.state.chats[0];
  assert.equal(migrated.id, "chat-a");
  assert.equal(migrated.created, 1000, "the original numeric created timestamp must be preserved for existing sort/render code");
  assert.equal(migrated.schema_version, 2);
  assert.equal(migrated.model_id, null, "a pre-T1.4 conversation's model cannot be established and must not get a false binding");
  assert.equal(migrated.model_binding_source, "unknown");
  assert.equal(migrated.directory_context, null);
  assert.ok(/^\d{4}-\d{2}-\d{2}T/.test(migrated.created_at), "created_at must be an ISO-8601 string");
  assert.deepEqual(migrated.messages, v1.chats[0].messages);
}

// --- loadState (via module init): no v1, no v2 -> fresh state, nothing recorded as needing recovery ---
{
  const { fns } = load({ storage: {} });
  assert.deepEqual(fns.getState().chats, []);
  assert.equal(fns.getMigrationRecoveryRaw(), null);
}

// --- loadState: valid v1 migrates into v2 and leaves v1 untouched ----------
{
  const v1 = { currentId: null, chats: [{ id: "c1", title: "T", created: 1, updated: 1, messages: [] }], settings: {} };
  const v1Raw = JSON.stringify(v1);
  const { fns, storage } = load({ storage: { "samosa-chat-v1": v1Raw } });
  assert.equal(fns.getState().chats.length, 1);
  assert.equal(fns.getState().chats[0].schema_version, 2);
  assert.equal(storage.getItem("samosa-chat-v1"), v1Raw, "the v1 key must survive migration unchanged");
  assert.ok(storage.getItem("samosa-chat-v2"), "a successful migration must persist the v2 state");
}

// --- loadState: malformed v1 JSON preserves the original key and flags recovery ---
{
  const badRaw = "{not json at all";
  const { fns, storage } = load({ storage: { "samosa-chat-v1": badRaw } });
  assert.deepEqual(fns.getState().chats, [], "a malformed v1 must fall back to a safe empty state, not crash");
  assert.equal(storage.getItem("samosa-chat-v1"), badRaw, "malformed top-level JSON must preserve the original localStorage key verbatim");
  assert.equal(fns.getMigrationRecoveryRaw(), badRaw);
}

// --- loadState: storage quota failure on write preserves the original v1 key ---
{
  const v1Raw = JSON.stringify({ currentId: null, chats: [{ id: "c1", title: "T", created: 1, updated: 1, messages: [] }], settings: {} });
  globalThis.els = freshEls();
  const storage = new FakeStorage({ "samosa-chat-v1": v1Raw });
  storage.failSet = true;
  globalThis.localStorage = storage;
  globalThis.render = () => {}; globalThis.closePanels = () => {};
  globalThis.self = globalThis;
  const fns = eval(`(() => {${app.slice(begin, end)}
    return { getState: () => state, getMigrationRecoveryRaw: () => migrationRecoveryRaw };
  })()`);
  assert.deepEqual(fns.getState().chats, [], "a write/readback failure must fall back to a safe empty state");
  assert.equal(storage.getItem("samosa-chat-v1"), v1Raw, "a quota failure must preserve the original v1 key");
  assert.equal(fns.getMigrationRecoveryRaw(), v1Raw);
}

// --- loadState: an already-migrated v2 state short-circuits re-migration ---
{
  const v2 = { currentId: null, chats: [{ id: "c2", schema_version: 2, model_id: null, model_version: null, model_binding_source: "unknown", created: 1, updated: 1, created_at: "2026-01-01T00:00:00.000Z", updated_at: "2026-01-01T00:00:00.000Z", directory_context: null, title: "T2", messages: [] }], settings: {} };
  const { fns, storage } = load({ storage: { "samosa-chat-v2": JSON.stringify(v2), "samosa-chat-v1": "{should not be read" } });
  assert.equal(fns.getState().chats[0].id, "c2");
  assert.equal(fns.getMigrationRecoveryRaw(), null, "a healthy v2 state must never trigger the recovery path");
}

// --- chatNeedsExplicitAction: pure decision logic ---------------------------
{
  const { fns } = load();
  const backendA = { id: "qwen", version: "v1" };
  assert.equal(fns.chatNeedsExplicitAction({ model_id: null }, backendA), false, "an unbound chat is never a mismatch");
  assert.equal(fns.chatNeedsExplicitAction({ model_id: "qwen", model_version: "v1" }, backendA), false, "an exact match is never a mismatch");
  assert.equal(fns.chatNeedsExplicitAction({ model_id: "bonsai", model_version: "v9" }, backendA), true, "a different bound model must be a mismatch");
  assert.equal(fns.chatNeedsExplicitAction({ model_id: "qwen", model_version: "v1" }, { id: "qwen", version: "" }), false, "an unknown active version must never be treated as a mismatch");
}

// --- forkChatFrom: new id, copied messages, reset (unbound) model fields ---
{
  const { fns } = load();
  const state = fns.getState();
  const original = { id: "orig", title: "Original", messages: [{ id: "m1", role: "user", content: "hi" }], model_id: "bonsai", model_version: "v9" };
  const fork = fns.forkChatFrom(original);
  assert.notEqual(fork.id, original.id);
  assert.deepEqual(fork.messages, original.messages);
  assert.notEqual(fork.messages, original.messages, "messages must be a copy, not the same array reference");
  assert.equal(fork.model_id, null, "a fork starts unbound and binds explicitly to whatever model sends the next turn");
  assert.equal(fork.model_binding_source, "unknown");
  assert.equal(state.chats[0], fork, "the fork must be added to the conversation list");
}

// --- openMismatchDialog / closeMismatchDialog: visibility and focus restore ---
{
  const { fns, els } = load();
  const opener = new Element("button");
  const chat = { model_id: "bonsai", model_version: "v9" };
  fns.openMismatchDialog(chat, opener);
  assert.equal(els.mismatchDialog.hidden, false);
  assert.ok(els.mismatchDialog.classList.contains("open"));
  assert.match(els.mismatchDetail.textContent, /bonsai/);
  assert.match(els.mismatchDetail.textContent, /qwen/, "the detail text must also name the currently active model");
  assert.equal(els.mismatchFork.focusCount, 1, "opening must focus the primary action");
  fns.closeMismatchDialog();
  assert.equal(els.mismatchDialog.hidden, true);
  assert.equal(opener.focusCount, 1, "closing must return focus to whatever opened the dialog");
}

// --- confirmForkMismatch: forks, switches to the fork, and closes the dialog ---
{
  const { fns, els } = load();
  const state = fns.getState();
  state.chats.push({ id: "bound-chat", title: "Bound", messages: [{ id: "m1", role: "user", content: "hi" }], model_id: "bonsai", model_version: "v9" });
  fns.openMismatchDialog(state.chats[0]);
  fns.confirmForkMismatch();
  assert.equal(els.mismatchDialog.hidden, true);
  assert.equal(fns.getCurrentId(), state.chats[0].id, "the newly created fork must become current");
  assert.notEqual(fns.getCurrentId(), "bound-chat");
}

// --- selectChat: a mismatched conversation opens the dialog instead of switching ---
{
  const { fns, els } = load(); // backend defaults to {id:"qwen",version:"v1"}
  const state = fns.getState();
  state.chats.push({ id: "other-model-chat", title: "Other", messages: [], model_id: "bonsai", model_version: "v9" });
  const before = fns.getCurrentId();
  fns.selectChat("other-model-chat");
  assert.equal(els.mismatchDialog.hidden, false, "reopening a conversation bound to another model must prompt, not switch silently");
  assert.equal(fns.getCurrentId(), before, "currentId must not change until the user chooses fork or cancel");
}

// --- selectChat: an unbound (or matching) conversation switches normally ---
{
  const { fns, els } = load();
  const state = fns.getState();
  state.chats.push({ id: "plain-chat", title: "Plain", messages: [], model_id: null, model_version: null });
  fns.selectChat("plain-chat");
  assert.equal(els.mismatchDialog.hidden, true);
  assert.equal(fns.getCurrentId(), "plain-chat");
}

// --- ensureChat: a brand-new conversation is created unbound (v2 shape) ----
{
  const { fns } = load({ storage: {} });
  const state = fns.getState();
  state.chats = []; // loadState() already created one via bootstrap; start clean for this check
  const chat = fns.ensureChat();
  assert.equal(chat.schema_version, 2);
  assert.equal(chat.model_id, null);
  assert.equal(chat.model_binding_source, "unknown");
  assert.equal(chat.directory_context, null);
}

// --- migration banner + export/reset -----------------------------------
{
  const { fns, els } = load();
  fns.showMigrationBanner();
  assert.equal(els.migrationBanner.hidden, false);
  fns.hideMigrationBanner();
  assert.equal(els.migrationBanner.hidden, true);
}
{
  const badRaw = "{not json at all";
  const { fns, blobs } = load({ storage: { "samosa-chat-v1": badRaw } });
  fns.exportMigrationRecovery();
  assert.equal(blobs.length, 1, "export must package the raw recovery data into a downloadable blob");
  assert.equal(blobs[0].parts[0], badRaw);
}
{
  const badRaw = "{not json at all";
  const { fns, storage } = load({ storage: { "samosa-chat-v1": badRaw }, confirmResult: true });
  fns.resetMigrationRecovery();
  assert.equal(storage.getItem("samosa-chat-v1"), null, "confirmed reset must clear the v1 key");
  assert.equal(storage.getItem("samosa-chat-v2"), null, "confirmed reset must clear the v2 key");
  assert.equal(globalThis.__reloaded, true);
}
{
  const badRaw = "{not json at all";
  const { fns, storage } = load({ storage: { "samosa-chat-v1": badRaw }, confirmResult: false });
  fns.resetMigrationRecovery();
  assert.equal(storage.getItem("samosa-chat-v1"), badRaw, "declining the confirm dialog must leave storage untouched");
  assert.equal(globalThis.__reloaded, false);
}

process.stdout.write("conversation migration/binding UI DOM fixtures: PASS\n");
