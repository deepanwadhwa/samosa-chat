/* T1.3 (docs/TASKS_UI_CHUTNI.md sec5.4) DOM fixture coverage for the browser
 * directory chooser. The production app is a dependency-free static page (no
 * browser test runner in this repo), so this evaluates the exact shipped
 * source block against a tiny hand-rolled DOM fixture instead of introducing
 * a real-browser dependency into the offline gate. It cannot replace a real
 * Safari/Chrome/Firefox click-through -- see the T1.3 evidence doc for what
 * was and wasn't verified interactively. */
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
    this.textContent = "";
    this.style = {};
    this.attrs = {};
    this.focusCount = 0;
    this.onclick = null;
  }
  get classList() { return new ClassList(this); }
  appendChild(child) { this.children.push(child); return child; }
  append(...kids) { kids.forEach(k => this.appendChild(k)); }
  set innerHTML(value) {
    this._innerHTML = value; this.children = [];
    if (value === `<svg aria-hidden="true"><use href="#icon-folder"></use></svg><span></span>`) {
      const svg = new Element("svg"), span = new Element("span");
      this.append(svg, span);
    }
  }
  get innerHTML() { return this._innerHTML || ""; }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; }
  removeAttribute(name) { delete this.attrs[name]; }
  querySelector(selector) {
    const wanted = selector.startsWith(".") ? selector.slice(1) : selector.toLowerCase();
    const byClass = selector.startsWith(".");
    const walk = node => {
      for (const child of node.children) {
        if (byClass ? (child.className || "").split(/\s+/).includes(wanted) : child.tagName === wanted) return child;
        const found = walk(child); if (found) return found;
      }
      return null;
    };
    return walk(this);
  }
  querySelectorAll(selector) {
    const wanted = selector.startsWith(".") ? selector.slice(1) : selector.toLowerCase();
    const byClass = selector.startsWith(".");
    const out = [];
    const walk = node => {
      for (const child of node.children) {
        if (byClass ? (child.className || "").split(/\s+/).includes(wanted) : child.tagName === wanted) out.push(child);
        walk(child);
      }
    };
    walk(this);
    return out;
  }
  scrollIntoView() {}
  focus() { this.focusCount++; globalThis.__lastFocused = this; }
}

globalThis.document = {
  createElement: tag => new Element(tag),
  _listeners: {},
  addEventListener(type, fn) { this._listeners[type] = fn; },
  removeEventListener(type, fn) { if (this._listeners[type] === fn) delete this._listeners[type]; },
  activeElement: null,
};

function freshEls() {
  return {
    scrim: new Element("div"), dialog: new Element("div"), close: new Element("button"),
    roots: new Element("div"), path: new Element("div"), list: new Element("div"),
    up: new Element("button"), select: new Element("button"),
    openBtn: new Element("button"), clearBtn: new Element("button"), selectedPath: new Element("div"),
  };
}

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      function chooserFocusableEls");
const end = app.indexOf("      function openSettings");
assert.ok(begin >= 0 && end > begin, "T1.3 chooser block must remain extractable");

function loadChooser({ authFetch }) {
  globalThis.chooserEls = freshEls();
  globalThis.chooser = { roots: [], rootId: null, path: null, parent: null, entries: [], focusIndex: -1, opener: null, onSelect: null };
  globalThis.authFetch = authFetch;
  const fns = eval(`(() => {${app.slice(begin, end)}
    return { chooserFocusableEls, chooserKeydown, chooserMoveFocus, chooserActivateFocused,
             chooserRenderRoots, chooserRenderPath, chooserRenderList, chooserLoadRoots,
             chooserOpenPath, openChooser, closeChooser, chooserConfirmSelection, chooserClearSelection };
  })()`);
  return { fns, els: globalThis.chooserEls, chooser: globalThis.chooser };
}

function jsonResponse(body) { return { ok: true, json: async () => body }; }

// --- chooserRenderRoots: disabled/current state ---------------------------
{
  const { fns, els, chooser } = loadChooser({ authFetch: async () => jsonResponse({}) });
  chooser.roots = [
    { chooser_root_id: "home", label: "Home", path: "/h", readable: true, connected: true },
    { chooser_root_id: "volume-1", label: "Backup", path: "/v", readable: false, connected: true },
  ];
  chooser.rootId = "home";
  fns.chooserRenderRoots();
  assert.equal(els.roots.children.length, 2);
  assert.ok(els.roots.children[0].className.includes("current"), "current root should be marked");
  assert.equal(els.roots.children[0].disabled, false);
  assert.equal(els.roots.children[1].disabled, true, "unreadable root must be disabled, not just unlabeled");
}

// --- chooserRenderList: readable + denied rows, empty state ----------------
{
  const { fns, els, chooser } = loadChooser({ authFetch: async () => jsonResponse({}) });
  chooser.entries = [
    { name: "Research", path: "/h/Research", readable: true },
    { name: "NoPerm", path: "/h/NoPerm", readable: false },
  ];
  chooser.focusIndex = 0;
  fns.chooserRenderList();
  assert.equal(els.list.children.length, 2);
  assert.ok(els.list.children[0].className.includes("focused"));
  assert.ok(!els.list.children[0].className.includes("denied"));
  assert.ok(els.list.children[1].className.includes("denied"), "an unreadable entry must render as denied, not omitted");
  assert.equal(els.list.children[1].getAttribute("aria-disabled"), "true");
  assert.equal(els.list.children[0].querySelector("span").textContent, "Research");
  assert.equal(els.list.getAttribute("aria-activedescendant"), "chooser-row-0");

  chooser.entries = []; chooser.focusIndex = -1;
  fns.chooserRenderList();
  assert.equal(els.list.children.length, 1);
  assert.equal(els.list.children[0].className, "chooser-empty");
  assert.equal(els.list.getAttribute("aria-activedescendant"), null, "empty list must not claim an active descendant");
}

// --- chooserMoveFocus: clamps to bounds in both directions -----------------
{
  const { fns, els, chooser } = loadChooser({ authFetch: async () => jsonResponse({}) });
  chooser.entries = [{ name: "a", path: "/a", readable: true }, { name: "b", path: "/b", readable: true }, { name: "c", path: "/c", readable: true }];
  chooser.focusIndex = 0;
  fns.chooserMoveFocus(1);
  assert.equal(chooser.focusIndex, 1);
  assert.equal(els.list.getAttribute("aria-activedescendant"), "chooser-row-1");
  fns.chooserMoveFocus(-5);
  assert.equal(chooser.focusIndex, 0, "must clamp at the first entry, not go negative");
  fns.chooserMoveFocus(50);
  assert.equal(chooser.focusIndex, 2, "must clamp at the last entry");
}

// --- chooserActivateFocused: denied entries never trigger navigation ------
{
  let requestedPaths = [];
  const { fns, chooser } = loadChooser({
    authFetch: async (path) => { requestedPaths.push(path); return jsonResponse({ path: "/h/Research", chooser_root_id: "home", parent: "/h", directories: [] }); },
  });
  chooser.entries = [{ name: "NoPerm", path: "/h/NoPerm", readable: false }, { name: "Research", path: "/h/Research", readable: true }];
  chooser.focusIndex = 0;
  await fns.chooserActivateFocused();
  assert.equal(requestedPaths.length, 0, "activating a denied row must not call the server at all");
  assert.equal(chooser.path, null);

  chooser.focusIndex = 1;
  await fns.chooserActivateFocused();
  assert.equal(requestedPaths.length, 1);
  assert.match(requestedPaths[0], /Research/);
  assert.equal(chooser.path, "/h/Research", "activating a readable row must navigate into it");
}

// --- chooserOpenPath: renders the server's response, sets Up availability -
{
  const { fns, els, chooser } = loadChooser({
    authFetch: async () => jsonResponse({
      path: "/h/Documents", chooser_root_id: "home", parent: "/h",
      directories: [{ name: "Research", path: "/h/Documents/Research", readable: true }],
    }),
  });
  await fns.chooserOpenPath("/h/Documents");
  assert.equal(chooser.path, "/h/Documents");
  assert.equal(chooser.parent, "/h");
  assert.equal(els.path.textContent, "/h/Documents");
  assert.equal(els.up.disabled, false, "a non-null parent must enable Up");
  assert.equal(els.list.children.length, 1);
}
{
  const { fns, els, chooser } = loadChooser({
    authFetch: async () => jsonResponse({ path: "/h", chooser_root_id: "home", parent: null, directories: [] }),
  });
  await fns.chooserOpenPath("/h");
  assert.equal(chooser.parent, null);
  assert.equal(els.up.disabled, true, "a root directory (parent:null) must disable Up rather than let it 403");
}

// --- chooserKeydown: Escape closes and restores focus to the opener -------
{
  const { fns, els, chooser } = loadChooser({ authFetch: async () => jsonResponse({ path: "/h", chooser_root_id: "home", parent: null, directories: [] }) });
  const opener = new Element("button");
  chooser.opener = opener;
  els.dialog.hidden = false; els.scrim.hidden = false;
  els.dialog.classList.add("open"); els.scrim.classList.add("show");
  fns.chooserKeydown({ key: "Escape", preventDefault() {} });
  assert.equal(els.dialog.hidden, true);
  assert.equal(els.scrim.hidden, true);
  assert.equal(opener.focusCount, 1, "closing must return focus to whatever opened the dialog");
}

// --- chooserKeydown: Tab cycles only through enabled focusable elements, wraps ---
{
  const { fns, els, chooser } = loadChooser({ authFetch: async () => jsonResponse({}) });
  chooser.roots = [{ chooser_root_id: "home", label: "Home", path: "/h", readable: true, connected: true }];
  chooser.rootId = "home";
  fns.chooserRenderRoots();
  els.up.disabled = true; // no parent yet, matching a freshly opened dialog at a root
  const focusable = fns.chooserFocusableEls();
  assert.deepEqual(focusable.map(el => el.tagName === "button" && el === els.up ? "up" : el), focusable, "sanity");
  assert.ok(!focusable.includes(els.up), "a disabled Up button must be excluded from the tab order");
  assert.ok(focusable.includes(els.close) && focusable.includes(els.list) && focusable.includes(els.select));

  document.activeElement = els.select; // last in order
  fns.chooserKeydown({ key: "Tab", shiftKey: false, preventDefault() {} });
  assert.equal(globalThis.__lastFocused, focusable[0], "Tab from the last focusable element must wrap to the first");

  document.activeElement = focusable[0];
  fns.chooserKeydown({ key: "Tab", shiftKey: true, preventDefault() {} });
  assert.equal(globalThis.__lastFocused, focusable[focusable.length - 1], "Shift+Tab from the first element must wrap to the last");
}

// --- chooserConfirmSelection: reusable callers receive the chosen path ----
{
  const { fns, els, chooser } = loadChooser({ authFetch: async () => jsonResponse({}) });
  let selected = null;
  chooser.path = "/h/Research";
  chooser.onSelect = path => { selected = path; };
  els.dialog.hidden = false; els.scrim.hidden = false;
  els.dialog.classList.add("open"); els.scrim.classList.add("show");
  fns.chooserConfirmSelection();
  assert.equal(selected, "/h/Research", "Chutni and Jobs callers must receive the browsed path");
  assert.equal(els.selectedPath.textContent, "", "a reusable selection must not leak into the old Settings preview");
  assert.equal(els.dialog.hidden, true);
}

process.stdout.write("chooser UI DOM fixtures: PASS\n");
