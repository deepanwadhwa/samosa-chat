/* Exact shipped-source fixture for live web source rendering. Remote titles
 * and URLs stay data: this test proves validation, deduplication, keyed DOM
 * updates, and safe link attributes without adding a browser dependency. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toLowerCase();
    this.children = [];
    this.className = "";
    this.dataset = {};
    this.hidden = false;
    this.attrs = {};
    this._text = "";
  }
  appendChild(child) {
    if (child.parent) child.parent.children = child.parent.children.filter(item => item !== child);
    this.children.push(child); child.parent = this; return child;
  }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text; }
  setAttribute(name, value) { this.attrs[name.toLowerCase()] = String(value); }
  getAttribute(name) { return this.attrs[name.toLowerCase()] ?? null; }
  removeAttribute(name) { delete this.attrs[name.toLowerCase()]; }
  set href(value) { this.setAttribute("href", value); }
  get href() { return this.getAttribute("href") || ""; }
  set target(value) { this.setAttribute("target", value); }
  get target() { return this.getAttribute("target") || ""; }
  set rel(value) { this.setAttribute("rel", value); }
  get rel() { return this.getAttribute("rel") || ""; }
  set referrerPolicy(value) { this.setAttribute("referrerpolicy", value); }
  get referrerPolicy() { return this.getAttribute("referrerpolicy") || ""; }
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
  querySelectorAll(selector) {
    const wanted = selector.startsWith(".") ? selector.slice(1) : selector.toLowerCase();
    const byClass = selector.startsWith(".");
    const found = [];
    const walk = node => {
      for (const child of node.children) {
        const matches = byClass
          ? child.className.split(/\s+/).includes(wanted)
          : child.tagName === wanted;
        if (matches) found.push(child);
        walk(child);
      }
    };
    walk(this); return found;
  }
}

globalThis.document = { createElement: tag => new Element(tag) };

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
assert.match(app, /const qwenHistory=researching \? history\.slice\(-5\)/,
  "Qwen web turns must carry bounded recent context for referential query planning");
assert.match(app, /: \[\{role:"user",content:user\.content\}\]/,
  "plain Qwen turns must retain the minimal system-plus-current-user payload");
const helpersBegin = app.indexOf("      function privateWebHostname(hostname) {");
const helpersEnd = app.indexOf("      function consumeEvent(raw, assistant) {");
const renderBegin = app.indexOf("      function renderWebActivity(row, msg) {");
const renderEnd = app.indexOf("      function updateAssistantNode(msg) {");
assert.ok(helpersBegin >= 0 && helpersEnd > helpersBegin, "web source helpers must remain extractable");
assert.ok(renderBegin >= 0 && renderEnd > renderBegin, "web activity renderer must remain extractable");

const fns = eval(`(() => {
  ${app.slice(helpersBegin, helpersEnd)}
  ${app.slice(helpersEnd, renderBegin)}
  ${app.slice(renderBegin, renderEnd)}
  return { privateWebHostname, normaliseWebSource, mergeWebSource, consumeEvent, renderWebActivity };
})()`);

globalThis.sendPrompt = function () {};
sendPrompt.voiceRun = null;
globalThis.els = { speed: new Element(), rss: new Element(), closure: new Element() };
globalThis.updateAssistantNode = () => {};
globalThis.scrollBottom = () => {};
globalThis.setVoiceStage = () => {};

const streamed = { content: "", reasoning: "", sources: [] };
assert.equal(fns.consumeEvent('{"choices":[{"delta":{"content":"hello"}}]}', streamed), false,
  "content chunks are not terminal");
assert.equal(streamed.content, "hello");
assert.equal(fns.consumeEvent('{"choices":[{"delta":{},"finish_reason":"stop"}]}', streamed), true,
  "finish_reason marks a completed response even before the DONE sentinel");
assert.equal(fns.consumeEvent("[DONE]", streamed), true, "the DONE sentinel marks a completed response");
assert.equal(fns.consumeEvent("not-json", streamed), false, "malformed chunks cannot mark a response complete");
assert.equal(fns.consumeEvent('{"error":{"message":"generation failed"}}', streamed), true,
  "structured backend errors terminate the stream visibly");
assert.equal(streamed.error, "generation failed");

for (const url of [
  "javascript:alert(1)", "data:text/html,hello", "file:///etc/passwd", "/relative",
  "http://user:pass@example.com/", "https://example.com:8443/x",
  "http://127.0.0.1/x", "http://192.168.1.4/x", "http://[::1]/x"
]) {
  assert.equal(fns.normaliseWebSource({ url, kind: "search_result" }), null,
    `unsafe web activity URL must be rejected: ${url}`);
}

const message = { streaming: true, activity: "Working out what needs checking…\n", sources: [] };
assert.equal(fns.mergeWebSource(message, {
  id: "result-1", kind: "search_result", state: "found",
  title: '<img src=x onerror="boom">Careers', url: "https://example.com/jobs#team"
}), true);
assert.equal(fns.mergeWebSource(message, {
  id: "result-1", kind: "search_result", state: "found",
  title: "Careers at Example", url: "https://example.com/jobs"
}), true);
assert.equal(message.sources.length, 1, "repeated results update one source row");
assert.equal(message.sources[0].url, "https://example.com/jobs", "fragments are removed from source URLs");

const duplicateUrl = { sources: [] };
fns.mergeWebSource(duplicateUrl, {
  id: "search:one", kind: "search_result", state: "found",
  title: "Result title", url: "https://example.com/same"
});
fns.mergeWebSource(duplicateUrl, {
  id: "page:one", kind: "page", state: "read",
  title: "Fetched title", url: "https://example.com/same#content"
});
assert.equal(duplicateUrl.sources.length, 1,
  "a search result and fetched page with the same URL render as one source");
assert.equal(duplicateUrl.sources[0].kind, "page");
assert.equal(duplicateUrl.sources[0].state, "read");
assert.equal(duplicateUrl.sources[0].title, "Fetched title");

assert.equal(fns.mergeWebSource(message, {
  id: "page-1", kind: "page", state: "checking", title: "", url: "https://docs.example.org/start"
}), true);
assert.equal(fns.mergeWebSource(message, {
  id: "page-1", kind: "page", state: "read", title: "Documentation", url: "https://docs.example.org/final"
}), true);
assert.equal(message.sources.length, 2, "a redirect updates the attempted page instead of adding a duplicate");
assert.equal(message.sources[1].state, "read");

const row = new Element("article");
const activity = new Element("div"); activity.className = "web-activity"; row.appendChild(activity);
fns.renderWebActivity(row, message);
const sourcesDetails = activity.querySelector(".web-sources");
assert.equal(sourcesDetails.tagName, "details", "the source list is collapsible");
assert.equal(sourcesDetails.querySelector(".web-sources-head").tagName, "summary");
assert.equal(sourcesDetails.open, true, "sources stay expanded while research is streaming");
const sourceRows = activity.querySelectorAll(".web-source");
assert.equal(sourceRows.length, 2);
assert.equal(sourceRows[0].tagName, "a");
assert.equal(sourceRows[0].href, "https://example.com/jobs");
assert.equal(sourceRows[0].target, "_blank");
assert.match(sourceRows[0].rel, /noopener/);
assert.match(sourceRows[0].rel, /noreferrer/);
assert.equal(sourceRows[0].referrerPolicy, "no-referrer");
assert.match(sourceRows[0].getAttribute("aria-label"), /opens in a new tab/);
assert.equal(sourceRows[0].querySelector(".web-source-title").textContent, "Careers at Example");
assert.equal(sourceRows[1].querySelector(".web-source-state").textContent, "Read");

const hostile = { streaming: true, activity: "Found a result.\n", sources: [] };
const hostileTitle = '<img src=x onerror="boom">Not markup';
fns.mergeWebSource(hostile, { id: "hostile", kind: "search_result", state: "found", title: hostileTitle, url: "https://safe.example.net/x" });
const hostileRow = new Element("article");
const hostileActivity = new Element("div"); hostileActivity.className = "web-activity"; hostileRow.appendChild(hostileActivity);
fns.renderWebActivity(hostileRow, hostile);
assert.equal(hostileActivity.querySelector(".web-source-title").textContent, hostileTitle);
assert.equal(hostileActivity.querySelectorAll("img").length, 0, "remote titles remain literal text");

const originalResultRow = sourceRows[0];
message.activity += "Looking up 1 of 3…\n";
fns.renderWebActivity(row, message);
assert.equal(activity.querySelectorAll(".web-source")[0], originalResultRow,
  "activity updates preserve existing link nodes and keyboard focus");
message.streaming = false;
fns.renderWebActivity(row, message);
assert.equal(sourcesDetails.open, false, "sources collapse when the completed answer arrives");
sourcesDetails.open = true;
fns.renderWebActivity(row, message);
assert.equal(sourcesDetails.open, true, "completed renders preserve the user's open/closed choice");

const failed = { streaming: true, activity: "Could not read that page.\n", sources: [] };
fns.mergeWebSource(failed, { id: "bad-page", kind: "page", state: "failed", url: "https://example.net/missing" });
const failedRow = new Element("article");
const failedActivity = new Element("div"); failedActivity.className = "web-activity"; failedRow.appendChild(failedActivity);
fns.renderWebActivity(failedRow, failed);
const failedSource = failedActivity.querySelector(".web-source");
assert.equal(failedSource.getAttribute("href"), null, "failed attempts are visible but never clickable");
assert.equal(failedSource.querySelector(".web-source-state").textContent, "Failed");

process.stdout.write("web activity UI fixtures: PASS\n");
