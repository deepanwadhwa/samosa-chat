/* DOM fixture coverage for the Jobs SSE renderer.  The production app is a
 * dependency-free static page, so this deliberately uses a tiny DOM fixture
 * instead of introducing a browser/runtime dependency into the offline gate. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class ClassList {
  constructor() { this.values = new Set(); }
  add(...values) { values.forEach(value => this.values.add(value)); }
  remove(...values) { values.forEach(value => this.values.delete(value)); }
  toggle(value, force) { const on = force === undefined ? !this.values.has(value) : force; if (on) this.add(value); else this.remove(value); return on; }
  contains(value) { return this.values.has(value); }
}

class Element {
  constructor(tag = "div") {
    this.tagName = tag; this.children = []; this.className = ""; this.classList = new ClassList();
    this.hidden = false; this.textContent = ""; this.style = {}; this.dataset = {}; this.scrollTop = this.scrollHeight = 0;
  }
  appendChild(child) { this.children.push(child); return child; }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  set innerHTML(value) {
    this._innerHTML = value; this.children = [];
    if (value === '<span class="dot"></span><div class="body"><div class="title"></div></div>') {
      const dot = new Element("span"), body = new Element(), title = new Element();
      dot.className = "dot"; body.className = "body"; title.className = "title";
      body.appendChild(title); this.append(dot, body);
    }
  }
  get innerHTML() { return this._innerHTML || ""; }
  querySelector(selector) {
    const wanted = selector.startsWith(".") ? selector.slice(1) : selector;
    const walk = node => {
      if ((node.className || "").split(/\s+/).includes(wanted)) return node;
      for (const child of node.children) { const found = walk(child); if (found) return found; }
      return null;
    };
    return walk(this);
  }
  querySelectorAll(selector) { const one = this.querySelector(selector); return one ? [one] : []; }
  scrollIntoView() {}
  focus() {}
  setAttribute() {}
}

globalThis.document = { createElement: tag => new Element(tag) };
globalThis.markdown = text => text;
globalThis.jobEls = {
  progress: new Element(), activity: new Element(), bar: new Element(), barText: new Element(), barActions: new Element(),
  result: new Element(), resultLabel: new Element(), resultText: new Element(), review: new Element(), reviewList: new Element(), reviewMeta: new Element(),
};
globalThis.lastJobId = null;

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      const baseName =");
const end = app.indexOf("      async function streamJob");
assert.ok(begin >= 0 && end > begin, "Jobs renderer block must remain extractable");
// The extracted source is exactly the shipped event renderer and helpers.
globalThis.renderJobEvent = eval(`(() => {${app.slice(begin, end)}; return renderJobEvent;})()`);

// Progress copy stays coupled to a concrete SSE event and its mechanical
// field; this catches a tempting but dishonest static status string.
for (const [event, field] of [["index_complete", "evt.total"], ["triage_progress", "evt.done"], ["skim_progress", "evt.done"], ["classify_progress", "evt.done"]]) {
  const from = app.indexOf(`case "${event}"`), to = app.indexOf("break;", from);
  assert.ok(from >= 0 && to > from && app.slice(from, to).includes(field), `${event} progress must use ${field}`);
}

const ctx = { folder: "/tmp/folder" };
renderJobEvent({ type: "indexing", total: 50 }, ctx);
renderJobEvent({ type: "index_complete", total: 50, checked: 50, batches: 4 }, ctx);
assert.equal(jobEls.activity.children.at(-1).querySelector(".title").textContent, "Checked 50 filenames");

renderJobEvent({ type: "triage_progress", done: 16, total: 50 }, ctx);
renderJobEvent({ type: "triage_progress", done: 50, total: 50 }, ctx);
assert.equal(jobEls.activity.children.at(-1).querySelector(".title").textContent, "Triaging filenames (50/50)");

renderJobEvent({ type: "skim_progress", done: 50, total: 50, current: "CamScanner_03-15-2024.png", confidence: "medium", source: "ocr" }, ctx);
assert.equal(jobEls.activity.children.at(-1).querySelector(".title").textContent, "Skimmed 50 files");
renderJobEvent({ type: "skim_progress", done: 1, total: 50, current: "CamScanner_03-15-2024.png", confidence: "medium", source: "ocr", expected_remaining_seconds: 57 }, ctx);
assert.match(jobEls.activity.children.at(-1).querySelector(".title").textContent, /first skim ~1 min remaining/);

renderJobEvent({ type: "classify_progress", done: 50, total: 50, shortlist: 2 }, ctx);
assert.equal(jobEls.activity.children.at(-1).querySelector(".title").textContent, "Classified 50 files (shortlisted 2)");

renderJobEvent({ type: "tool_call", tool: "doc.read", path: "CamScanner_03-15-2024.png" }, ctx);
renderJobEvent({ type: "tool_result", tool: "doc.read", path: "CamScanner_03-15-2024.png", source: "OCR" }, ctx);
assert.equal(jobEls.activity.children.at(-1).querySelector(".title").textContent, "Read page 1 (OCR) — CamScanner_03-15-2024.png");

renderJobEvent({ type: "await_continue", job_id: "job-ui", skimmed: 300, remaining: 17 }, ctx);
assert.equal(jobEls.barText.textContent, "Skimmed 300 files; 17 remain.");
assert.equal(jobEls.barActions.children[0].textContent, "Continue");

renderJobEvent({ type: "result", matches: [{ path: "Titli/certificate.pdf", evidence: "Rabies vaccination", page: 1, confidence: "high" }], rejected_count: 47, unreadable: [{ path: "scan.png", reason: "ocr_unavailable" }] }, ctx);
assert.match(jobEls.resultText.innerHTML, /certificate\.pdf/);
assert.match(jobEls.resultText.innerHTML, /Rabies vaccination/);
assert.match(jobEls.resultText.innerHTML, /scan\.png/);

process.stdout.write("jobs UI DOM fixtures: PASS\n");
