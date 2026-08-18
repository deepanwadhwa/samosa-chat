/* T3.2 (docs/TASKS_UI_CHUTNI.md sec4.4/sec5.8) DOM fixture coverage for the
 * rebuilt composer: the `+` menu's capability gating, attachment chips, and
 * the upload path that replaced Base64-in-localStorage with server-issued
 * content-addressed IDs.
 *
 * Same approach as tests/test_chooser_ui.mjs: the production app is a
 * dependency-free static page with no browser test runner in this repo, so
 * this evaluates the exact shipped source block against a small hand-rolled
 * DOM fixture rather than adding a real-browser dependency to the offline
 * gate. It cannot replace a real click-through in Safari/Chrome/Firefox --
 * see the T3.2 evidence doc for what was and wasn't verified interactively.
 */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class ClassList {
  constructor(owner) { this.owner = owner; }
  _set() { return new Set((this.owner.className || "").split(/\s+/).filter(Boolean)); }
  add(...v) { const s = this._set(); v.forEach(x => s.add(x)); this.owner.className = [...s].join(" "); }
  remove(...v) { const s = this._set(); v.forEach(x => s.delete(x)); this.owner.className = [...s].join(" "); }
  toggle(v, force) {
    const shouldAdd = force === undefined ? !this.contains(v) : !!force;
    if (shouldAdd) this.add(v); else this.remove(v);
    return shouldAdd;
  }
  contains(v) { return this._set().has(v); }
}

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.textContent = "";
    this.value = "";
    this.style = {};
    this.attrs = {};
    this.dataset = {};
    this.focusCount = 0;
    this.onclick = null;
    this.isConnected = true;
  }
  get classList() { return new ClassList(this); }
  appendChild(child) { this.children.push(child); child.parent = this; return child; }
  append(...kids) { kids.forEach(k => this.appendChild(k)); }
  set innerHTML(v) { this._innerHTML = v; if (v === "") this.children = []; }
  get innerHTML() { return this._innerHTML || ""; }
  setAttribute(n, v) { this.attrs[n] = String(v); }
  getAttribute(n) { return Object.prototype.hasOwnProperty.call(this.attrs, n) ? this.attrs[n] : null; }
  removeAttribute(n) { delete this.attrs[n]; }
  contains(node) {
    if (node === this) return true;
    return this.children.some(c => c.contains && c.contains(node));
  }
  replaceWith(node) {
    if (!this.parent) return;
    const i = this.parent.children.indexOf(this);
    if (i >= 0) { this.parent.children[i] = node; node.parent = this.parent; this.isConnected = false; }
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  querySelectorAll(sel) {
    // Supports ".cls", ".cls:not(:disabled)", and bare tag names -- the only
    // forms the composer block uses.
    const notDisabled = sel.includes(":not(:disabled)");
    const base = sel.replace(":not(:disabled)", "").trim();
    const byClass = base.startsWith(".");
    const wanted = byClass ? base.slice(1) : base.toLowerCase();
    const out = [];
    const walk = node => {
      for (const child of node.children) {
        const matches = byClass
          ? (child.className || "").split(/\s+/).includes(wanted)
          : child.tagName === wanted;
        if (matches && (!notDisabled || !child.disabled)) out.push(child);
        walk(child);
      }
    };
    walk(this);
    return out;
  }
  focus() { this.focusCount++; globalThis.document.activeElement = this; }
  click() { this.clicked = (this.clicked || 0) + 1; }
}

globalThis.document = {
  createElement: tag => new Element(tag),
  createElementNS: (_ns, tag) => new Element(tag),
  _listeners: {},
  addEventListener(t, fn) { (this._listeners[t] = this._listeners[t] || []).push(fn); },
  removeEventListener(t, fn) { this._listeners[t] = (this._listeners[t] || []).filter(f => f !== fn); },
  activeElement: null,
};
globalThis.URL.createObjectURL = () => "blob:fixture";
globalThis.URL.revokeObjectURL = () => {};
globalThis.btoa = s => Buffer.from(s, "binary").toString("base64");
// Real unescape() turns each %XX back into a single latin1 *byte*, which is
// exactly why btoa(unescape(encodeURIComponent(x))) is the UTF-8-safe btoa
// idiom. decodeURIComponent() would instead rebuild whole characters and
// hand btoa() something outside latin1, so the shim must not use it.
globalThis.unescape = s => s.replace(/%([0-9A-Fa-f]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16)));

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");

const begin = app.indexOf("      const MAX_ATTACHMENTS = 6;");
const end = app.indexOf("      // --- Jobs (Models -> Tools -> Jobs)");
assert.ok(begin >= 0 && end > begin, "T3.2 composer attachment block must remain extractable");

function attachItem() {
  const b = new Element("button");
  const label = new Element("span");
  label.className = "attach-item-label";
  b.className = "attach-item";
  b.appendChild(label);
  return b;
}

function loadComposer({ authFetch, storage = new Map() } = {}) {
  const els = {
    attachMenu: new Element("div"),
    attachBtn: new Element("button"),
    attachChips: new Element("div"),
    attachError: new Element("div"),
    attachImage: attachItem(), attachDocument: attachItem(),
    attachWeb: attachItem(), attachWebSearch: attachItem(), attachDirectory: attachItem(),
    attachImageReason: new Element("span"), attachDocumentReason: new Element("span"),
    attachWebReason: new Element("span"), attachWebSearchReason: new Element("span"),
    attachDirectoryReason: new Element("span"),
    imageInput: new Element("input"), documentInput: new Element("input"),
  };
  els.attachImage.querySelector(".attach-item-label").textContent = "Image";
  els.attachDocument.querySelector(".attach-item-label").textContent = "Document";
  els.attachWeb.querySelector(".attach-item-label").textContent = "Web page";
  els.attachWebSearch.querySelector(".attach-item-label").textContent = "Web search";
  els.attachDirectory.querySelector(".attach-item-label").textContent = "Directory";
  els.attachMenu.append(els.attachImage, els.attachDocument, els.attachWeb,
                        els.attachWebSearch, els.attachDirectory);
  els.attachMenu.hidden = true;

  globalThis.els = els;
  globalThis.backend = { ready: true, supports_images: true, supports_documents: true };
  // Phase W (docs/TASKS_WEB_SEARCH.md W6): what GET /v1/web/config reported.
  // Defaults to fully closed, matching the app's own initial value.
  globalThis.web = { offline: false, fetch_available: false, search_configured: false,
                     available: false, consent: "granted", provider: "", reason: "" };
  globalThis.pendingAttachments = [];
  globalThis.localStorage = {
    getItem: key => storage.has(key) ? storage.get(key) : null,
    setItem: (key, value) => storage.set(key, String(value)),
    removeItem: key => storage.delete(key),
  };
  globalThis.authFetch = authFetch || (async () => ({ ok: true, json: async () => ({}) }));
  globalThis.resizePrompt = () => {};

  const fns = eval(`(() => {${app.slice(begin, end)}
    return { applyCapabilities, setAttachItem, openAttachMenu, closeAttachMenu,
             attachMenuKeydown, setAttachError, removeAttachment, renderAttachments,
             uploadAttachment, pickAttachment, clearAttachments,
             addWebPage, addWebSearch, applyPendingWebContext, applyAutomaticWebContext,
             shouldContinueWebResearch, setWebSearchEnabled, toggleWebSearch,
             get webSearchEnabled() { return webSearchEnabled; },
             preferenceKey: WEB_SEARCH_PREFERENCE_KEY,
             get pending() { return pendingAttachments; },
             set pending(v) { pendingAttachments = v; },
             get pendingWebContext() { return pendingWeb; },
             set pendingWebContext(v) { pendingWeb = v; },
             set readyMemoryCount(v) { readyChutniMemoryCount = v; } };
  })()`);
  return { fns, els };
}

// A literal request to use the internet is explicit consent for that turn.
// Freshness follow-ups also continue a thread whose prior answer visibly used
// public sources; unrelated local questions do not start network activity.
{
  const { fns } = loadComposer();
  globalThis.web = { offline: false, fetch_available: true, search_configured: true,
                     available: true, consent: "granted", provider: "brave", reason: "" };
  const researched = { messages: [{ role: "assistant", sources: [{ url: "https://example.com" }] }] };
  assert.equal(fns.shouldContinueWebResearch({ messages: [] }, "check the internet dumbass"), true);
  assert.equal(fns.shouldContinueWebResearch(researched, "Is there any news from August 2026?"), true);
  assert.equal(fns.shouldContinueWebResearch(researched, "Rewrite that more clearly."), false);
  assert.equal(fns.shouldContinueWebResearch({ messages: [] }, "What is a closure?"), false);
}

// --- Phase W: Web search is a visible persistent preference ---------------
{
  const storage = new Map();
  const { fns } = loadComposer({ storage });
  // A configured provider does not grant automatic use until the user turns
  // the preference on.
  globalThis.web = { offline: false, fetch_available: true, search_configured: true,
                     available: true, consent: "granted", provider: "brave", reason: "" };
  assert.deepEqual(fns.applyPendingWebContext({ model: "test" }, []), { model: "test" });
  assert.deepEqual(
    fns.applyPendingWebContext({ model: "test" }, [{ kind: "search", url: null }]),
    { model: "test", web: true },
    "clicking Web search must explicitly request one search turn");
  assert.deepEqual(
    fns.applyPendingWebContext({ model: "test" }, [{ kind: "url", url: "https://example.com/a" }]),
    { model: "test", web_urls: ["https://example.com/a"] },
    "adding a page must not also enable Web search");
  assert.equal(fns.webSearchEnabled, false);
  const context = fns.applyAutomaticWebContext({}, "What is happening?", []);
  assert.deepEqual(context, [], "web search stays off until the user enables it");
  fns.toggleWebSearch();
  assert.equal(fns.webSearchEnabled, true);
  assert.equal(storage.get(fns.preferenceKey), "1", "the preference is persisted");
  assert.deepEqual(fns.applyAutomaticWebContext({}, "What is happening?", []), [{ kind: "search", url: null }]);
  // A new page load restores the preference and keeps it active for the next
  // message; this is the regression that the old one-turn chip lacked.
  const reloaded = loadComposer({ storage });
  assert.equal(reloaded.fns.webSearchEnabled, true, "the preference survives a reload");
  reloaded.fns.applyCapabilities();
  assert.equal(reloaded.els.attachWebSearch.getAttribute("aria-checked"), "true");
  reloaded.fns.toggleWebSearch();
  assert.equal(storage.get(fns.preferenceKey), undefined, "turning it off clears the stored preference");
}

const uploadOk = id => async () => ({ ok: true, json: async () => ({ id, filename: "photo.png" }) });
const fakeFile = (name, type) => ({ name, type, size: 4 });

// --- Capability gating: nothing is offered before the server confirms it ---
// sec5.8: the UI "must not display a working action for a route absent from
// the packaged gateway". Directory is backed by ready Chutni scopes. Web page
// and Web search are gated by GET /v1/web/config; nothing starts optimistic.
{
  const { fns, els } = loadComposer();
  fns.applyCapabilities();
  assert.equal(els.attachImage.disabled, false, "an image-capable ready model must enable Image");
  assert.equal(els.attachDocument.disabled, false, "a reported document reader must enable Document");
  assert.equal(els.attachWeb.disabled, true, "Web page must stay disabled until the server reports a web reader");
  assert.equal(els.attachWebSearch.disabled, true, "Web search must stay disabled until a provider is configured");
  assert.equal(els.attachDirectory.disabled, true, "Directory must stay disabled until a Chutni memory is ready");
  // A disabled item still states why, and says so to assistive tech too.
  assert.match(els.attachWebReason.textContent, /no web reader/i);
  assert.match(els.attachDirectoryReason.textContent, /Chutni/i);
  assert.match(els.attachWeb.getAttribute("aria-label"), /Web page — .*web reader/i);
  assert.match(els.attachDirectory.getAttribute("aria-label"), /Directory — /);
  fns.readyMemoryCount = 1;
  fns.applyCapabilities();
  assert.equal(els.attachDirectory.disabled, false, "a ready Chutni memory must make Directory available from Chat");
  assert.match(els.attachDirectoryReason.textContent, /folder memory/i);
}

// --- Phase W: web items follow GET /v1/web/config, never optimism ---------
{
  const { fns, els } = loadComposer();
  globalThis.web = { offline: false, fetch_available: true, search_configured: true,
                     available: true, consent: "granted", provider: "brave", reason: "" };
  fns.applyCapabilities();
  assert.equal(els.attachWeb.disabled, false, "a reported web reader must enable Web page");
  assert.equal(els.attachWebSearch.disabled, false, "a configured provider must enable Web search");
  // The provider is named; a credential never is, because the server never sends one.
  assert.match(els.attachWebSearchReason.textContent, /brave/);
}
{
  // Fetch works, search does not: the server's own reason is shown verbatim
  // rather than a generic "unavailable".
  const { fns, els } = loadComposer();
  globalThis.web = { offline: false, fetch_available: true, search_configured: false,
                     available: true, consent: "granted", provider: "", reason: "no search provider is configured" };
  fns.applyCapabilities();
  assert.equal(els.attachWeb.disabled, false, "reading a page needs no credentials");
  assert.equal(els.attachWebSearch.disabled, true, "no provider means no Web search");
  assert.match(els.attachWebSearchReason.textContent, /no search provider is configured/);
}
{
  // Offline is a distinct reason from "not built", and closes both.
  const { fns, els } = loadComposer();
  globalThis.web = { offline: true, fetch_available: false, search_configured: false,
                     available: false, consent: "granted", provider: "", reason: "Samosa is in offline mode." };
  fns.applyCapabilities();
  assert.equal(els.attachWeb.disabled, true, "offline mode must disable Web page");
  assert.equal(els.attachWebSearch.disabled, true, "offline mode must disable Web search");
  assert.match(els.attachWebReason.textContent, /offline mode/i);
}

// --- Phase W: staged web context is dropped when the capability goes away --
{
  const { fns } = loadComposer();
  globalThis.web = { offline: false, fetch_available: true, search_configured: true,
                     available: true, consent: "granted", provider: "brave", reason: "" };
  fns.applyCapabilities();
  fns.addWebPage("https://example.com/a");
  fns.addWebSearch();
  assert.equal(fns.pendingWebContext.length, 2, "both kinds of web context stage");
  globalThis.web = { offline: true, fetch_available: false, search_configured: false,
                     available: false, consent: "granted", provider: "", reason: "Samosa is in offline mode." };
  fns.applyCapabilities();
  assert.equal(fns.pendingWebContext.length, 0,
    "going offline must drop staged web context rather than send a turn that cannot honour it");
}

// --- Phase W: URL validation, de-duplication, and the per-turn cap --------
{
  const { fns } = loadComposer();
  globalThis.web = { offline: false, fetch_available: true, search_configured: true,
                     available: true, consent: "granted", provider: "brave", reason: "" };
  assert.equal(fns.addWebPage("ftp://example.com/x"), false, "a non-http scheme is refused");
  assert.equal(fns.addWebPage("example.com"), false, "a scheme-less address is refused");
  assert.equal(fns.pendingWebContext.length, 0, "nothing invalid was staged");
  assert.equal(fns.addWebPage("https://example.com/a"), true);
  assert.equal(fns.addWebPage("https://example.com/a"), true, "re-adding the same URL is a no-op, not an error");
  assert.equal(fns.pendingWebContext.filter(w => w.kind === "url").length, 1, "duplicates are not staged twice");
  fns.addWebPage("https://example.com/b");
  fns.addWebPage("https://example.com/c");
  assert.equal(fns.addWebPage("https://example.com/d"), false, "the per-turn page cap holds");
  fns.addWebSearch(); fns.addWebSearch();
  assert.equal(fns.pendingWebContext.filter(w => w.kind === "search").length, 1, "Web search stages at most once");
  fns.clearAttachments();
  assert.equal(fns.pendingWebContext.length, 0, "sending clears web context with everything else");
}

// --- Capability gating reflects server truth, not optimism ----------------
{
  const { fns, els } = loadComposer();
  globalThis.backend = { ready: true, supports_images: false, supports_documents: false };
  fns.applyCapabilities();
  assert.equal(els.attachImage.disabled, true, "a model without vision must disable Image");
  assert.match(els.attachImageReason.textContent, /vision projector/i);
  assert.equal(els.attachDocument.disabled, true, "no reported reader must disable Document");
  assert.match(els.attachDocumentReason.textContent, /isn't installed/i);
}
{
  const { fns, els } = loadComposer();
  globalThis.backend = { ready: false, supports_images: true, supports_documents: true };
  fns.applyCapabilities();
  assert.equal(els.attachImage.disabled, true, "Image must stay disabled until a model is ready");
  assert.match(els.attachImageReason.textContent, /once a model is ready/i);
}

// --- Losing image support mid-session drops staged image attachments ------
{
  const { fns } = loadComposer();
  fns.pending = [{ clientId: "c1", kind: "image", name: "a.png", id: "x".repeat(64), status: "ready", thumb: null }];
  globalThis.backend = { ready: true, supports_images: false, supports_documents: true };
  fns.applyCapabilities();
  assert.equal(fns.pending.length, 0, "an image staged under a vision model must not survive a switch to a text-only one");
}

// --- Upload: sends raw bytes + headers, records the server's ID -----------
{
  let seen = null;
  const { fns, els } = loadComposer({
    authFetch: async (path, opts) => { seen = { path, opts }; return { ok: true, json: async () => ({ id: "a".repeat(64), filename: "photo.png" }) }; },
  });
  await fns.uploadAttachment(fakeFile("photo.png", "image/png"), "image");
  assert.equal(seen.path, "/v1/attachments");
  assert.equal(seen.opts.method, "POST");
  assert.equal(seen.opts.headers["X-Samosa-Media-Type"], "image/png");
  assert.equal(Buffer.from(seen.opts.headers["X-Samosa-Filename-B64"], "base64").toString(), "photo.png",
    "the display filename travels base64-encoded, not raw, in a header");
  assert.equal(fns.pending.length, 1);
  assert.equal(fns.pending[0].status, "ready");
  assert.equal(fns.pending[0].id, "a".repeat(64), "the server's content-addressed id is what gets stored");
  assert.equal(els.attachChips.children.length, 1, "a ready attachment renders one chip");
}

// --- A non-ASCII filename survives the header round trip ------------------
{
  let seen = null;
  const { fns } = loadComposer({
    authFetch: async (path, opts) => { seen = opts; return { ok: true, json: async () => ({ id: "b".repeat(64) }) }; },
  });
  await fns.uploadAttachment(fakeFile("reçu-café.pdf", "application/pdf"), "document");
  assert.equal(Buffer.from(seen.headers["X-Samosa-Filename-B64"], "base64").toString("utf8"), "reçu-café.pdf",
    "btoa() cannot take raw UTF-8 -- the name must be encoded before it is base64'd");
}

// --- A failed upload is visible as a failed chip, never silently dropped --
{
  const { fns, els } = loadComposer({
    authFetch: async () => ({ ok: false, json: async () => ({ error: { message: "That file could not be attached." } }) }),
  });
  await fns.uploadAttachment(fakeFile("bad.png", "image/png"), "image");
  assert.equal(fns.pending.length, 1);
  assert.equal(fns.pending[0].status, "failed");
  assert.match(els.attachChips.children[0].className, /failed/);
  assert.match(els.attachChips.children[0].querySelector(".attach-chip-name").textContent, /could not be attached/);
}

// --- The attachment cap is enforced and explained -------------------------
{
  const { fns, els } = loadComposer({ authFetch: uploadOk("c".repeat(64)) });
  for (let i = 0; i < 7; i++) await fns.uploadAttachment(fakeFile(`f${i}.png`, "image/png"), "image");
  assert.equal(fns.pending.length, 6, "no more than MAX_ATTACHMENTS may be staged");
  assert.match(els.attachError.textContent, /up to 6 files/i);
}

// --- Removing a chip also asks the server to drop the blob ----------------
{
  const deleted = [];
  const { fns, els } = loadComposer({
    authFetch: async (path, opts) => {
      if (opts && opts.method === "DELETE") { deleted.push(path); return { ok: true, json: async () => ({}) }; }
      return { ok: true, json: async () => ({ id: "d".repeat(64), filename: "x.png" }) };
    },
  });
  await fns.uploadAttachment(fakeFile("x.png", "image/png"), "image");
  const clientId = fns.pending[0].clientId;
  fns.removeAttachment(clientId);
  assert.equal(fns.pending.length, 0);
  assert.equal(els.attachChips.children.length, 0);
  assert.deepEqual(deleted, [`/v1/attachments/${"d".repeat(64)}`],
    "removing a staged attachment should release its server-side blob");
}

// --- Chip filenames are rendered as text, never as markup ----------------
// A filename is user-controlled input and reaches the DOM through
// textContent only; nothing in the chip path builds HTML from it.
{
  const { fns, els } = loadComposer({
    authFetch: async () => ({ ok: true, json: async () => ({ id: "e".repeat(64), filename: "<img src=x onerror=alert(1)>.png" }) }),
  });
  await fns.uploadAttachment(fakeFile("evil.png", "image/png"), "image");
  const nameEl = els.attachChips.children[0].querySelector(".attach-chip-name");
  assert.equal(nameEl.textContent, "<img src=x onerror=alert(1)>.png");
  assert.equal(els.attachChips.innerHTML, "", "chips are built as nodes; no innerHTML is assembled from a filename");
}

// --- Menu open/close and Escape ------------------------------------------
{
  const { fns, els } = loadComposer();
  fns.openAttachMenu();
  assert.equal(els.attachMenu.hidden, false);
  assert.equal(els.attachBtn.getAttribute("aria-expanded"), "true");
  assert.equal(document.activeElement, els.attachImage, "opening focuses the first enabled item");
  let prevented = false;
  fns.attachMenuKeydown({ key: "Escape", preventDefault: () => { prevented = true; } });
  assert.ok(prevented);
  assert.equal(els.attachMenu.hidden, true, "Escape closes the menu");
  assert.equal(els.attachBtn.getAttribute("aria-expanded"), "false");
}

// --- Tab wraps inside the open menu, skipping disabled items -------------
{
  const { fns, els } = loadComposer();
  fns.openAttachMenu();
  // Only Image and Document are enabled, so focus must cycle between them
  // and never land on the disabled Web page / Directory entries.
  document.activeElement = els.attachDocument;
  let prevented = false;
  fns.attachMenuKeydown({ key: "Tab", shiftKey: false, preventDefault: () => { prevented = true; } });
  assert.ok(prevented, "Tab past the last enabled item must be intercepted");
  assert.equal(document.activeElement, els.attachImage, "focus wraps to the first enabled item");
  document.activeElement = els.attachImage;
  fns.attachMenuKeydown({ key: "Tab", shiftKey: true, preventDefault: () => {} });
  assert.equal(document.activeElement, els.attachDocument, "Shift+Tab wraps to the last enabled item");
}

// --- Picking opens the right file input and closes the menu --------------
{
  const { fns, els } = loadComposer();
  fns.openAttachMenu();
  fns.pickAttachment("document");
  assert.equal(els.documentInput.clicked, 1, "Document opens the document picker");
  assert.equal(els.imageInput.clicked, undefined, "and not the image picker");
  assert.equal(els.attachMenu.hidden, true, "choosing an item closes the menu");
}

// --- clearAttachments resets everything after a send ---------------------
{
  const { fns, els } = loadComposer({ authFetch: uploadOk("f".repeat(64)) });
  await fns.uploadAttachment(fakeFile("a.png", "image/png"), "image");
  fns.setAttachError("something");
  fns.clearAttachments();
  assert.equal(fns.pending.length, 0);
  assert.equal(els.attachChips.children.length, 0);
  assert.equal(els.attachError.textContent, "");
  assert.equal(els.imageInput.value, "", "the file input is reset so the same file can be picked again");
}

console.log("test_composer_ui.mjs: PASS");
