/* T3.2 acceptance (docs/TASKS_UI_CHUTNI.md): "A 1,000-message synthetic
 * conversation remains usable within the agreed browser performance budget."
 *
 * The shipped renderer builds a DOM node per message and runs the markdown
 * pass over every assistant turn, so rendering an entire long conversation
 * eagerly is the thing that makes it slow. renderMessages() therefore only
 * materializes the most recent RENDER_WINDOW turns and reveals older ones on
 * demand. This measures the two properties that follow from that:
 *
 *   1. node count is bounded by the window, not by conversation length; and
 *   2. render time does not grow with conversation length.
 *
 * Node is not a browser and this fixture has no layout, style, or paint --
 * so the absolute milliseconds here are NOT a browser frame budget and must
 * not be quoted as one. What it does prove is the algorithmic property: the
 * work per render is O(window), not O(conversation). The budget below is
 * deliberately loose so this fails on a complexity regression (dropping the
 * window and rendering everything) rather than on machine speed.
 */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.className = ""; this.hidden = false; this.disabled = false;
    this.textContent = ""; this.style = {}; this.attrs = {}; this.dataset = {};
    this.scrollHeight = 0; this.scrollTop = 0; this.clientHeight = 0;
    this.onclick = null; this.isConnected = true;
  }
  appendChild(c) { this.children.push(c); c.parent = this; return c; }
  append(...k) { k.forEach(x => this.appendChild(x)); }
  set innerHTML(v) {
    this._innerHTML = v;
    this.children = [];
    // The renderer reads back .avatar/.bubble/.thinking-body/.response/
    // .error-note from markup it just set, so the fixture materializes those.
    for (const cls of ["avatar", "bubble", "thinking", "thinking-body", "response", "error-note"]) {
      if (v.includes(`class="${cls}`)) { const e = new Element("div"); e.className = cls; this.appendChild(e); }
    }
  }
  get innerHTML() { return this._innerHTML || ""; }
  setAttribute(n, v) { this.attrs[n] = String(v); }
  getAttribute(n) { return this.attrs[n] ?? null; }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  querySelectorAll(sel) {
    const byClass = sel.startsWith(".");
    const wanted = byClass ? sel.slice(1) : sel.toLowerCase();
    const out = [];
    const walk = n => { for (const c of n.children) { if (byClass ? (c.className || "").split(/\s+/).includes(wanted) : c.tagName === wanted) out.push(c); walk(c); } };
    walk(this); return out;
  }
  scrollTo() {}
  focus() {}
}

globalThis.document = { createElement: t => new Element(t), createElementNS: (_n, t) => new Element(t) };
globalThis.requestAnimationFrame = () => {};

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
function extract(startMarker, endMarker) {
  const b = app.indexOf(startMarker), e = app.indexOf(endMarker);
  assert.ok(b >= 0 && e > b, `block must remain extractable: ${startMarker.trim()}`);
  return app.slice(b, e);
}
// escapeHTML + markdown + welcomeHTML + attachment nodes + renderMessages +
// appendMessageNode, exactly as shipped.
const block = extract("      function escapeHTML(text)", "      function renderList()");

function load() {
  const els = { messages: new Element("section"), prompt: new Element("textarea"), jump: new Element("button"), send: new Element("button") };
  globalThis.els = els;
  globalThis.followOutput = true;
  globalThis.profileName = "Deepan";
  globalThis.backend = { label: "Qwen3.6 35B A3B" };
  globalThis.generating = false;
  globalThis.resizePrompt = () => {};
  globalThis.scrollBottom = () => {};
  globalThis.authFetch = async () => ({ ok: false });
  const fns = eval(`(() => {${block}
    return { renderMessages, appendMessageNode, welcomeHTML, escapeHTML,
             get renderLimit() { return renderLimit; },
             set renderLimit(v) { renderLimit = v; },
             RENDER_WINDOW };
  })()`);
  return { fns, els };
}

function synthesize(count) {
  const messages = [];
  for (let i = 0; i < count; i++) {
    messages.push(i % 2 === 0
      ? { id: `m${i}`, role: "user", content: `Question ${i} about a local model.` }
      : { id: `m${i}`, role: "assistant", content: `Answer ${i}.\n\n- point one\n- point two\n\n\`\`\`\ncode ${i}\n\`\`\`\n\n**bold** and \`inline\`.` });
  }
  return messages;
}

function timeRender(fns, els, messages) {
  globalThis.activeChat = () => ({ id: "c1", messages });
  fns.renderLimit = fns.RENDER_WINDOW;
  const t0 = process.hrtime.bigint();
  fns.renderMessages();
  const t1 = process.hrtime.bigint();
  return { ms: Number(t1 - t0) / 1e6, nodes: els.messages.children.length };
}

// --- 1. A 1,000-message conversation renders a bounded number of nodes ----
{
  const { fns, els } = load();
  const long = timeRender(fns, els, synthesize(1000));
  // window + the "show earlier" button
  assert.ok(long.nodes <= fns.RENDER_WINDOW + 1,
    `a 1,000-message conversation must not materialize every turn (rendered ${long.nodes} nodes)`);
  assert.ok(long.nodes >= 2, "the most recent turns must actually render");

  // --- 2. Render cost does not scale with conversation length -------------
  const { fns: fns2, els: els2 } = load();
  const short = timeRender(fns2, els2, synthesize(40));
  assert.equal(long.nodes > short.nodes, true, "a longer conversation still fills the window");

  // Re-measure both a few times and compare medians; a single sample is too
  // noisy to assert on.
  const median = arr => arr.slice().sort((a, b) => a - b)[Math.floor(arr.length / 2)];
  const longSamples = [], shortSamples = [];
  for (let i = 0; i < 7; i++) {
    longSamples.push(timeRender(fns, els, synthesize(1000)).ms);
    shortSamples.push(timeRender(fns2, els2, synthesize(40)).ms);
  }
  const longMs = median(longSamples), shortMs = median(shortSamples);
  // Windowed rendering means these should be the same order of magnitude.
  // Eager rendering would make the 1,000-message case ~25x the 40-message
  // one; 8x leaves generous headroom for fixture noise while still failing
  // loudly if the window is removed.
  assert.ok(longMs < Math.max(shortMs * 8, 5),
    `render time must not scale with conversation length (1000 msgs: ${longMs.toFixed(2)}ms vs 40 msgs: ${shortMs.toFixed(2)}ms)`);

  console.log(`  1,000 messages: ${long.nodes} nodes, ${longMs.toFixed(2)}ms (fixture DOM, no layout/paint)`);
  console.log(`     40 messages: ${short.nodes} nodes, ${shortMs.toFixed(2)}ms`);
}

// --- 3. Expanding the window reveals older turns, bounded each time -------
{
  const { fns, els } = load();
  const messages = synthesize(1000);
  globalThis.activeChat = () => ({ id: "c1", messages });
  fns.renderLimit = fns.RENDER_WINDOW;
  fns.renderMessages();
  const first = els.messages.children.length;
  fns.renderLimit = fns.RENDER_WINDOW * 2;
  fns.renderMessages();
  assert.ok(els.messages.children.length > first, "expanding must reveal more turns");
  assert.ok(els.messages.children.length <= fns.RENDER_WINDOW * 2 + 1, "and stay bounded by the expanded window");
}

// --- 4. The personalized empty state escapes the profile name ------------
{
  const { fns } = load();
  globalThis.profileName = '<img src=x onerror=alert(1)>';
  const html = fns.welcomeHTML();
  assert.ok(!html.includes("<img src=x onerror"), "a profile name must never reach the empty state as markup");
  assert.ok(html.includes("&lt;img src=x onerror=alert(1)&gt;"), "it should appear escaped instead");
  globalThis.profileName = "Deepan";
  assert.ok(fns.welcomeHTML().includes("Welcome, Deepan"), "a normal name personalizes the heading");
  globalThis.profileName = "";
  assert.ok(fns.welcomeHTML().includes("Your model. Your machine."), "no name falls back to the generic heading");
}

console.log("test_composer_perf.mjs: PASS");
