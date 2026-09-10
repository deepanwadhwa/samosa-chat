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
    this.onclick = null; this.isConnected = true; this.currentTime = 0; this.paused = true;
  }
  appendChild(c) { this.children.push(c); c.parent = this; return c; }
  append(...k) { k.forEach(x => this.appendChild(x)); }
  set innerHTML(v) {
    this._innerHTML = v;
    this.children = [];
    // The renderer reads back .avatar/.bubble/.thinking-body/.response/
    // .error-note from markup it just set, so the fixture materializes those.
    for (const cls of ["avatar", "bubble", "thinking", "thinking-body", "response", "error-note", "generation-status", "generation-workmark", "generation-status-label"]) {
      if (v.includes(`class="${cls}`)) { const e = new Element("div"); e.className = cls; this.appendChild(e); }
    }
  }
  get innerHTML() { return this._innerHTML || ""; }
  setAttribute(n, v) { this.attrs[n] = String(v); }
  removeAttribute(n) { delete this.attrs[n]; }
  getAttribute(n) { return this.attrs[n] ?? null; }
  get classList() { return { toggle: (name, active) => {
    const classes = new Set(this.className.split(/\s+/).filter(Boolean));
    if (active) classes.add(name); else classes.delete(name);
    this.className = [...classes].join(" ");
  } }; }
  addEventListener() {}
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
  globalThis.renderFileActivity = () => {};
  globalThis.renderWebActivity = () => {};
  globalThis.authFetch = async () => ({ ok: false });
  globalThis.voicePlaybackReady = () => false;
  const fns = eval(`(() => {${block}
    return { renderMessages, appendMessageNode, welcomeHTML, escapeHTML,
             generationStatusHTML, renderAssistantResponse,
             parseMolmoGrounding, renderMolmoGrounding,
             parseMolmoTracking, renderMolmoTracking,
             get renderLimit() { return renderLimit; },
             set renderLimit(v) { renderLimit = v; },
             RENDER_WINDOW };
  })()`);
  return { fns, els };
}

// --- File decisions remain readable, safe, and distinct from filenames ----
{
  const source = extract("      function renderFileActivity(row, msg)", "      function updateAssistantNode(msg)");
  const render = eval(`(() => {${source}; return renderFileActivity;})()`);
  const row = new Element(), box = new Element(); box.className = "file-activity"; row.appendChild(box);
  const activity = { filename: '<img src=x onerror=alert(1)>' + "long-name".repeat(60) + ".pdf",
    message: "Reading PDF pages 1–9 to find the first chapter…", progress: 0, indeterminate: true };
  render(row, { streaming: true, content: "", fileActivity: activity });
  assert.equal(box.querySelector(".file-activity-label").textContent, activity.message);
  assert.equal(box.querySelector(".file-activity-source").textContent, activity.filename);
  assert.equal(box.querySelectorAll("img").length, 0, "source labels must remain text, not markup");
  assert.equal(box.querySelector("span").style.width, "100%", "unknown duration must remain visible");
  assert.equal(box.getAttribute("aria-live"), "polite");
  activity.message = "Read PDF pages 1–9. Checking whether they answer your question…";
  render(row, { streaming: true, content: "", fileActivity: activity });
  assert.equal(box.querySelector(".file-activity-label").textContent, activity.message);
  render(row, { streaming: true, content: "The first chapter", fileActivity: activity });
  assert.equal(box.hidden, true, "live file status must clear when the answer begins");
}

// --- Molmo video tracks become a synchronized overlay, not visible XML ----
{
  const { fns } = load();
  const tracking = fns.parseMolmoTracking(
    '<tracks coords="0.0 1 730 615;0.5 1 720 600;1.0 1 710 590">cat</tracks>');
  assert.equal(tracking.points.length, 3, "each timestamped Molmo2 track point must be decoded");
  assert.deepEqual(tracking.points.map(p => [p.time, p.objectId, p.x, p.y]), [
    [0, "1", 73, 61.5], [0.5, "1", 72, 60], [1, "1", 71, 59]
  ]);
  assert.equal(tracking.cleanText, "", "tracking coordinate markup must not leak into prose");
  const assistant = { id: "tracked-a", role: "assistant", content: "" };
  globalThis.activeChat = () => ({ messages: [
    { id: "tracked-u", role: "user", content: "track my cat", attachments: [
      { id: "b".repeat(64), kind: "video", name: "cat.mov", bytes: 1024 }
    ] },
    assistant
  ] });
  const trackedNode = new Element("div");
  assert.equal(fns.renderMolmoTracking(trackedNode, assistant, tracking), true,
    "a valid video track must render as an annotated video");
  assert.equal(trackedNode.querySelectorAll(".molmo-tracking-marker").length, 1,
    "one stable object id must reuse one synchronized marker across timestamps");
  assert.match(trackedNode.querySelector("figcaption").textContent, /3 sampled times · up to 2 FPS/,
    "the UI must disclose sparse local sampling rather than imply original-frame tracking");
  assert.ok(app.includes("video.requestVideoFrameCallback") && app.includes("right.x - left.x"),
    "playback must update and interpolate the marker between adjacent sampled predictions");
}

// --- Molmo image grounding remains structured instead of visible XML ------
{
  const { fns } = load();
  const legacy = fns.parseMolmoGrounding(
    'Counting the <points x1="10.5" y1="20.5" x2="80.0" y2="75.0" alt="charts">charts</points> shows a total of 2.');
  assert.equal(legacy.points.length, 2, "legacy Molmo xN/yN points must all be decoded");
  assert.deepEqual(legacy.points.map(p => [p.x, p.y]), [[10.5, 20.5], [80, 75]]);
  assert.equal(legacy.cleanText, "Counting the charts shows a total of 2.");

  const molmo2 = fns.parseMolmoGrounding(
    '<points coords="1 1 098 629 2 500 250 3 901 875">charts</points>');
  assert.equal(molmo2.points.length, 3, "Molmo2 frame-prefixed 0..1000 coordinates must be decoded");
  assert.deepEqual(molmo2.points.map(p => [p.x, p.y]), [[9.8, 62.9], [50, 25], [90.1, 87.5]]);
  assert.equal(molmo2.cleanText, "charts", "coordinate markup must not leak into visible prose");

  const multiGroup = fns.parseMolmoGrounding(
    '<points coords="1 1 100 200 2 300 400;2 3 500 600">items</points>');
  assert.equal(multiGroup.points.length, 3, "semicolon-delimited image/frame groups must retain every point");
  const assistant = { id: "grounded-a", role: "assistant", content: molmo2.cleanText };
  globalThis.activeChat = () => ({ messages: [
    { id: "grounded-u", role: "user", content: "count them", attachments: [
      { id: "a".repeat(64), kind: "image", name: "charts.png" }
    ] },
    assistant
  ] });
  const groundedNode = new Element("div");
  fns.renderMolmoGrounding(groundedNode, assistant, molmo2);
  assert.equal(groundedNode.querySelectorAll(".molmo-grounding-marker").length, 3,
    "the response DOM must contain one visible marker per decoded coordinate");
  assert.equal(groundedNode.querySelector("figcaption").textContent, "charts (3)",
    "the overlay caption must expose the grounded count");
  assert.ok(app.includes(".molmo-grounding-marker") && app.includes("background: #ef4a9b"),
    "grounded locations must render as the requested pink markers");
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

// --- Waiting for the first token feels active without a terminal cursor ---
{
  const { fns } = load();
  const response = new Element("div");
  fns.renderAssistantResponse(response, { streaming: true, content: "" });
  assert.match(response.innerHTML, /class="generation-status"/,
    "a pending assistant turn should show the live generation status");
  assert.match(response.innerHTML, /Working through it/,
    "the waiting state should have a readable label");
  assert.match(response.innerHTML, /generation-workmark/,
    "the waiting state should use the active-work mark");
  assert.doesNotMatch(response.innerHTML, /typing/,
    "the retired terminal cursor class must not return");
  const visionResponse = new Element("div");
  fns.renderAssistantResponse(visionResponse, { streaming: true, content: "", fileActivity: {
    message: "Using Molmo2 4B vision model to process this image; Maple remains selected…"
  }});
  assert.equal(visionResponse.innerHTML, "",
    "file activity already has a live panel and must not be duplicated below it");
  const originalStatus = response.querySelector(".generation-status");
  fns.renderAssistantResponse(response, { streaming: true, content: "", reasoning: "A later SSE update" });
  assert.equal(response.querySelector(".generation-status"), originalStatus,
    "pre-token SSE updates must not recreate and restart the working mark");

  fns.renderAssistantResponse(response, { streaming: true, content: "The first token" });
  assert.doesNotMatch(response.innerHTML, /generation-status/,
    "real response text becomes the progress signal as soon as it arrives");
  assert.match(response.innerHTML, /The first token/);
  assert.doesNotMatch(app, /\.typing::after|@keyframes\s+blink/,
    "the old blinking pipe CSS must remain removed");
  assert.doesNotMatch(app, /generation-sweep|background-clip|drop-shadow/,
    "the rejected text-shimmer implementation must remain removed");
  assert.match(app, /@keyframes work-tile-a/,
    "the active-work mark should rearrange its tiles instead of animating text");

  const fastAnswer = new Element("div");
  fns.renderAssistantResponse(fastAnswer, {
    streaming: false, content: "Fast OCR answer", canDeepen: true,
    detailPrompt: "What does this say?",
    detailAttachments: [{ id: "a".repeat(64), kind: "image", name: "note.png" }]
  });
  assert.equal(fastAnswer.querySelector(".detail-check")?.textContent, "Check in more detail",
    "a completed fast file answer must offer an explicit deeper visual pass");
  const detailedAnswer = new Element("div");
  fns.renderAssistantResponse(detailedAnswer, {
    streaming: false, content: "Detailed visual answer", analysisDepth: "detailed"
  });
  assert.equal(detailedAnswer.querySelector(".detail-check"), null,
    "a detailed answer must not offer an endless chain of deeper passes");
  assert.doesNotMatch(app, /@keyframes\s+file-progress-slide/,
    "file progress must not use the moving orange sweep");
  assert.doesNotMatch(app, /is answering from the local .* analysis/,
    "the removed model-answering status sentence must not return");
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
  const personalized = fns.welcomeHTML();
  assert.ok(personalized.includes("Welcome, Deepan"), "a normal name personalizes the heading");
  assert.ok(!personalized.includes(globalThis.backend.label),
    "the welcome copy must not bake the active model into its prose");
  assert.ok(personalized.includes("Your conversations are stored on this computer."),
    "the local-storage promise should remain clear without naming a model");
  globalThis.profileName = "";
  assert.ok(fns.welcomeHTML().includes("Your model. Your machine."), "no name falls back to the generic heading");
}

console.log("test_composer_perf.mjs: PASS");
