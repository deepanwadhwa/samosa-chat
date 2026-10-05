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
    if (tag === "input") this.value = "";
  }
  appendChild(child) {
    this.children.push(child);
    if (this.tagName === "select" && child.tagName === "option" && this.value === undefined) this.value = child.value;
    return child;
  }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  replaceChildren(...children) { this.children = [...children]; }
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
  focus() { document.activeElement = this; }
  setAttribute() {}
}

globalThis.document = { createElement: tag => new Element(tag) };
globalThis.markdown = text => text;
globalThis.jobEls = {
  progress: new Element(), activity: new Element(), bar: new Element(), barText: new Element(), barActions: new Element(),
  result: new Element(), resultLabel: new Element(), resultText: new Element(), review: new Element(), reviewList: new Element(), reviewMeta: new Element(),
};
globalThis.lastJobId = null;
globalThis.bytesLabel = n => `${n} bytes`;
globalThis.showJobResult = text => { jobEls.resultText.innerHTML = text; jobEls.result.hidden = false; };

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
assert.match(app, /id="jobRecipe"[\s\S]*value="folder_report"/);
assert.match(app, /value="find_duplicates">Find duplicates/);
assert.match(app, /value="sort_by_type">Sort by file type/);
assert.match(app, /value="classify_inbox">Classify an inbox/);
assert.match(app, /value="watch_folder">Watch a folder/);
assert.match(app, /Recipe to run on added or changed files/);
assert.match(app, /v1\/jobs\/schedule\/stop/);
assert.match(app, /v1\/jobs\/history\/events\?job_id=/);
assert.match(app, /View saved run/);
assert.match(app, /id="chutniExcludeNames"/);
assert.match(app, /user_exclusions: userExclusions/);
assert.match(app, /Rebuild existing memory/);
assert.match(app, /confirm_rebuild: !!\(chutniPreflightId && chutniPreflightRequiresRebuild\)/);
assert.match(app, /This memory was built with an older or different inventory policy/);
assert.match(app, /v1\/jobs\/decision\/route/);
assert.match(app, /v1\/jobs\/decision\/start/);
assert.match(app, /v1\/jobs\/decision\/next/);
assert.match(app, /id="jobFollowupInput"/);
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
renderJobEvent({ type: "inventory_started", job_id: "report-ui", recipe: "folder_report" }, ctx);
assert.equal(ctx.jobId, "report-ui");
assert.match(jobEls.activity.children.at(-1).querySelector(".title").textContent, /Inspecting the folder/);
renderJobEvent({ type: "interrupted", phase: "inventory", resumable: false, message: "Run Folder report again to retry." }, ctx);
assert.match(jobEls.resultText.textContent, /Run Folder report again to retry/);
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

renderJobEvent({ type: "report", total: 3, bytes: 42, by_type: { ".txt": 2, ".csv": 1 }, age: [1, 1, 1, 0], size_bands: [3, 0, 0], duplicate_candidates: 2, duplicate_size_groups: 1, skip_reasons: {}, partial: true, limiting_reason: "maximum_files" }, ctx);
assert.match(jobEls.resultText.innerHTML, /Folder report/);
assert.match(jobEls.resultText.innerHTML, /2 \.txt/);
assert.match(jobEls.resultText.innerHTML, /size only; contents not compared/);
assert.match(jobEls.resultText.innerHTML, /Partial report.*maximum_files/);

renderJobEvent({ type: "duplicates", duplicate_groups: 1, candidate_files: 3, hashed_files: 3, deferred_files: 0, skipped: 1, partial: false, groups: [{ size: 12, sha256: "a".repeat(64), files: ["first.txt", "copy.txt"] }] }, ctx);
assert.match(jobEls.resultText.innerHTML, /1 verified duplicate group/);
assert.match(jobEls.resultText.innerHTML, /first\.txt/);
assert.match(jobEls.resultText.innerHTML, /SHA-256/);
renderJobEvent({ type: "duplicates", duplicate_groups: 0, candidate_files: 129, hashed_files: 0, deferred_files: 129, skipped: 0, partial: true, limiting_reason: "hash_file_limit", groups: [] }, ctx);
assert.match(jobEls.resultText.innerHTML, /129 candidates deferred/);
assert.match(jobEls.resultText.innerHTML, /Partial duplicate search.*hash_file_limit/);

renderJobEvent({ type: "await_apply", job_id: "sort-job", moves: 4 }, ctx);
assert.equal(ctx.awaitingApply, true);
assert.equal(jobEls.barText.textContent, "4 files ready to move.");
assert.equal(jobEls.barActions.children[0].textContent, "Apply moves");
renderJobEvent({ type: "inbox_classification", counts: [1, 1, 0, 0, 1], items: [
  { path: "invoice.pdf", category: "billing", signal: "invoice" },
  { path: "medical-invoice.pdf", category: "review", signal: "conflicting filename clues" },
] }, ctx);
assert.match(jobEls.resultText.innerHTML, /filename clues only/);
assert.match(jobEls.resultText.innerHTML, /invoice\.pdf/);
assert.match(jobEls.resultText.innerHTML, /conflicting filename clues/);

// The production form routes plain language, shows a shortlist, then sends a
// plain-language follow-up without asking the user to pick a recipe.
Object.assign(jobEls, {
  folder: Object.assign(new Element("input"), { value: "/tmp/ui-folder" }),
  goal: Object.assign(new Element("input"), { value: "Find Titli's medical records" }),
  followup: new Element(),
  followupInput: Object.assign(new Element("input"), { value: "Filter to vaccinations" }),
  followupRun: Object.assign(new Element("button"), { disabled: false, textContent: "Continue" }),
  checkSelected: new Element("button"), selectionStatus: new Element(),
  auto: Object.assign(new Element("input"), { checked: false }),
  run: Object.assign(new Element("button"), { disabled: false, textContent: "Run job" }),
});
const uiRequests = [];
let failSelectionSave = false;
const shortlist = { ok: true, job_id: "decision-ui", result: {
  schema_version: 7,
  checked_files: 2, total_files: 2, remaining_files: 0, partial: false,
  shortlist_count: 1, items: [
    { path: "Titli-vaccination.txt", excerpt: "Titli feline vaccination", source: "text", decision: "match", score: 0.96,
      model_choice: "plausible", model_scores: { plausible: 0.96, unrelated: 0.04 } },
    { path: "groceries.txt", excerpt: "Apples and bananas", source: "text", decision: "rejected", score: 0.12,
      model_choice: "unrelated", model_scores: { plausible: 0.12, unrelated: 0.88 } },
  ],
} };
globalThis.authFetch = async (path, options = {}) => {
  if (path === "/v1/jobs/review") return { ok: true, json: async () => ({ ok: true, items: [] }) };
  uiRequests.push({ path, options });
  if (path === "/v1/jobs/selection") {
    if (failSelectionSave) return { ok: false };
    shortlist.selected_paths = JSON.parse(options.body).selected_paths;
    return { ok: true, json: async () => ({ ok: true }) };
  }
  if (path === "/v1/jobs/selection/plan") {
    const request = JSON.parse(options.body);
    return { ok: true, json: async () => ({ ok: true, job_id: "copy-ui-action",
      selection_job_id: request.job_id, plan_id: "reviewable-plan-token", operation: request.operation,
      moves: [{ src: "/tmp/ui-folder/Titli-vaccination.txt", dst: `/tmp/ui-folder/${request.destination}/Titli-vaccination.txt` }] }) };
  }
  const data = path.endsWith("/route") ? { ok: true, action: "find" } : structuredClone(shortlist);
  return { ok: true, json: async () => data };
};
globalThis.clearJobOutput = () => {
  jobEls.activity.children = [];
  jobEls.result.hidden = true;
  jobEls.resultText.textContent = "";
  jobEls.resultText.children = [];
  jobEls.bar.hidden = true;
  jobEls.followup.hidden = true;
};
globalThis.jobLine = (dot, title) => {
  const line = new Element(); line.innerHTML = '<span class="dot"></span><div class="body"><div class="title"></div></div>';
  line.querySelector(".dot").textContent = dot;
  line.querySelector(".title").textContent = title;
  jobEls.activity.appendChild(line); return line;
};
globalThis.baseName = path => path.split("/").at(-1);
const runStart = app.indexOf("      function saveDecisionSelection() {");
const runEnd = app.indexOf("      async function applyJob", runStart);
assert.ok(runStart >= 0 && runEnd > runStart);
const { runUiJob, runUiFollowup, reopen, flushSelection, openFiles, reviewFiles, previewFiles, workflowState } = eval(`(() => {
  let jobRunning = false;
  let lastJobId = null;
  let currentId = null;
  const state = { chats: [] };
  const renderMoveReview = (plan, ctx) => { state.review = plan; state.reviewContext = ctx; };
  const showBar = (text, actions) => { state.bar = text; state.actions = actions; };
  const applyJob = async (jobId, ctx) => { state.approved = { jobId, planId: ctx.planId, selectionJobId: ctx.selectionJobId }; };
  const generating = false;
  const els = { prompt: new Element("input"), closure: new Element() };
  const activeChat = () => state.chats.find(chat => chat.id === currentId);
  const makeId = () => "workflow-ui-chat";
  const titleFor = text => text;
  const saveState = () => {};
  const render = () => {};
  const showView = view => { state.view = view; };
  const decisionSelectedPaths = new Set();
  let decisionSelectionSave = Promise.resolve(true);
  const decisionLiveRows = new Map();
  const jobEls = globalThis.jobEls;
  const clearJobOutput = globalThis.clearJobOutput;
  const refreshJobHistory = async () => {};
  const renderJobEvent = globalThis.renderJobEvent;
  const authFetch = globalThis.authFetch;
  const trackDecisionProgress = async (_id, request) => request;
  const jobLine = globalThis.jobLine;
  const showJobResult = globalThis.showJobResult;
  const streamJob = async () => {};
  const baseName = globalThis.baseName;
  const localStorage = { getItem: () => null, setItem() {}, removeItem() {} };
  ${app.slice(runStart, runEnd)}
  return { runUiJob: runJob, runUiFollowup: runDecisionFollowup,
    reopen: renderDecisionShortlist, flushSelection: () => decisionSelectionSave,
    openFiles: openFileConversation, reviewFiles: reopenConversationFiles, previewFiles: previewSelectedOrganization,
    workflowState: () => ({ state, currentId, question: els.prompt.value }) };
})()`);
await runUiJob();
assert.deepEqual(uiRequests.map(request => request.path), ["/v1/jobs/decision/route", "/v1/jobs/decision/start"]);
assert.deepEqual(JSON.parse(uiRequests[0].options.body), { goal: "Find Titli's medical records" });
const startBody = JSON.parse(uiRequests[1].options.body);
assert.equal(startBody.goal, "Find Titli's medical records");
assert.equal(startBody.folder, "/tmp/ui-folder");
assert.match(startBody.job_id, /^job-/);
assert.equal(jobEls.resultLabel.textContent, "Search results", jobEls.resultText.textContent);
assert.match(jobEls.resultText.children[1].children[0].textContent, /1 likely matches/);
assert.equal(jobEls.followup.hidden, false);
const firstPanel = jobEls.resultText.children[1];
assert.equal(firstPanel.querySelector(".job-decision-list").children.length, 2, "Every checked file and its status should be visible initially");
firstPanel.children[1].children[1].value = "all";
firstPanel.children[1].children[1].onchange();
const checkedRows = jobEls.resultText.children[1].querySelector(".job-decision-list").children;
assert.equal(checkedRows.length, 2, "a low relevance estimate hid a checked file by default");
assert.equal(checkedRows[1].children[1].children[1].children[1].textContent, "Apples and bananas");
await runUiFollowup();
assert.equal(uiRequests[2].path, "/v1/jobs/selection");
assert.deepEqual(JSON.parse(uiRequests[2].options.body), { job_id: "decision-ui", selected_paths: [] });
assert.equal(uiRequests[3].path, "/v1/jobs/decision/next");
assert.deepEqual(JSON.parse(uiRequests[3].options.body), { job_id: "decision-ui", followup: "Filter to vaccinations" });
const list = jobEls.resultText.children[1].querySelector(".job-decision-list");
const checkbox = list.children[0].children[0].children[0];
checkbox.checked = true;
checkbox.onchange();
await flushSelection();
assert.deepEqual(shortlist.selected_paths, ["Titli-vaccination.txt"]);
assert.equal(jobEls.checkSelected.hidden, false);
// Reopening restores server state, including deliberate deselection. Opening
// another job must not inherit the previous job's selected paths.
reopen(structuredClone(shortlist));
assert.equal(jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0].checked, true);
const other = structuredClone(shortlist);
other.job_id = "other-job"; other.selected_paths = [];
reopen(other);
assert.equal(jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0].checked, false);
reopen(structuredClone(shortlist));
jobEls.followupInput.value = "Find vaccination records";
await runUiFollowup();
assert.deepEqual(JSON.parse(uiRequests.at(-1).options.body), {
  job_id: "decision-ui", followup: "Find vaccination records",
});
assert.equal(jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0].checked, true);
assert.equal(jobEls.run.disabled, false);
// Rapid checkbox edits are serialized and the final state is durable.
const savedCheckbox = jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0];
savedCheckbox.checked = false; savedCheckbox.onchange();
savedCheckbox.checked = true; savedCheckbox.onchange();
savedCheckbox.checked = false; savedCheckbox.onchange();
await flushSelection();
assert.deepEqual(shortlist.selected_paths, []);
reopen(structuredClone(shortlist));
assert.equal(jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0].checked, false);

// Showing all files must reveal unselected candidates without changing the
// saved selection. Otherwise a refinement can leave the user trapped in it.
const showAll = structuredClone(shortlist);
showAll.selected_paths = ["Titli-vaccination.txt"];
showAll.result.selected_action = "show_all";
reopen(showAll);
const allRows = jobEls.resultText.children[1].querySelector(".job-decision-list").children;
assert.equal(allRows.length, 2);
assert.equal(allRows[0].children[0].children[0].checked, true);
assert.equal(allRows[1].children[0].children[0].checked, false);

// Deselecting with the keyboard keeps focus in the selected-file list, then
// returns to the filter when the last row disappears.
const keyboardSelection = structuredClone(shortlist);
keyboardSelection.selected_paths = ["Titli-vaccination.txt", "groceries.txt"];
reopen(keyboardSelection);
const keyboardPanel = jobEls.resultText.children[1];
const keyboardList = keyboardPanel.querySelector(".job-decision-list");
const firstSelected = keyboardList.children[0].children[0].children[0];
firstSelected.focus(); firstSelected.checked = false; firstSelected.onchange();
assert.equal(keyboardList.children.length, 1);
const lastSelected = keyboardList.children[0].children[0].children[0];
assert.equal(document.activeElement, lastSelected);
assert.equal(lastSelected.checked, true);
lastSelected.checked = false; lastSelected.onchange();
assert.equal(document.activeElement, keyboardPanel.children[1].children[1]);
await flushSelection();
assert.deepEqual(shortlist.selected_paths, []);
reopen(structuredClone(shortlist));

// A failed save blocks the follow-up while leaving its scope and question
// visible. Retrying succeeds without requiring another checkbox selection.
const retryCheckbox = jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0];
failSelectionSave = true;
retryCheckbox.checked = true; retryCheckbox.onchange();
await flushSelection();
assert.match(jobEls.selectionStatus.textContent, /not saved/);
jobEls.followupInput.value = "Keep this question";
const followupsBeforeFailure = uiRequests.filter(request => request.path.endsWith("/next")).length;
await runUiFollowup();
assert.equal(uiRequests.filter(request => request.path.endsWith("/next")).length, followupsBeforeFailure);
assert.equal(jobEls.result.hidden, false);
assert.equal(retryCheckbox.checked, true);
assert.equal(jobEls.followupInput.value, "Keep this question");
failSelectionSave = false;
await runUiFollowup();
assert.deepEqual(shortlist.selected_paths, ["Titli-vaccination.txt"]);
assert.equal(uiRequests.filter(request => request.path.endsWith("/next")).length, followupsBeforeFailure + 1);

// Selection handoff creates one durable conversation and reopening keeps its
// question, exact selected set, and original search without another inventory.
jobEls.followupInput.value = "What does the vaccination record say?";
await openFiles();
assert.equal(workflowState().state.chats.length, 1);
assert.equal(workflowState().state.chats[0].workflow_job_id, "decision-ui");
assert.equal(workflowState().state.view, "chat");
assert.equal(workflowState().question, "What does the vaccination record say?");
assert.ok(uiRequests.some(request => request.path === "/v1/conversations/workflow-ui-chat/work"));
await openFiles();
assert.equal(workflowState().state.chats.length, 1);
await reviewFiles();
assert.equal(workflowState().state.view, "jobs");
assert.equal(jobEls.resultText.children[1].querySelector(".job-decision-list").children[0].children[0].children[0].checked, true);

jobEls.destination = new Element("input"); jobEls.destination.value = "selected-records";
jobEls.operation = new Element("select"); jobEls.operation.value = "copy";
await previewFiles();
assert.equal(workflowState().state.review.moves.length, 1);
assert.equal(workflowState().state.reviewContext.selectionJobId, "decision-ui");
assert.equal(workflowState().state.approved, undefined, "preview applied without approval");
await workflowState().state.actions[0][2]();
assert.deepEqual(workflowState().state.approved, { jobId: "copy-ui-action", planId: "reviewable-plan-token", selectionJobId: "decision-ui" });

// Actual live progress renderer: one table row per file, updates replace
// provisional status, and counts derive from those rows rather than event totals.
const progressBegin = app.indexOf("      const decisionLiveRows = new Map(), decisionProgressSeen = new Map();");
const progressEnd = app.indexOf("      async function trackDecisionProgress", progressBegin);
const progressFixture = eval(`(() => {${app.slice(progressBegin, progressEnd)}; return { renderDecisionProgress };})()`);
jobEls.activity.replaceChildren();
progressFixture.renderDecisionProgress({type: "search_intent", target: "a purchase order"});
progressFixture.renderDecisionProgress({type: "file_checking", path: "<example>.pdf"});
progressFixture.renderDecisionProgress({type: "file_checking", path: "<example>.pdf"});
progressFixture.renderDecisionProgress({type: "file_decision", path: "<example>.pdf", decision: "needs_check", excerpt_chars_actual: 1200});
progressFixture.renderDecisionProgress({type: "file_reading", path: "<example>.pdf", message: "Reading more evidence"});
assert.match(jobEls.activity.querySelector(".job-progress-count").textContent, /0 checked.*1 checking/);
progressFixture.renderDecisionProgress({type: "file_decision", path: "<example>.pdf", decision: "match", excerpt_chars_actual: 1800});
progressFixture.renderDecisionProgress({type: "file_decision", path: "unrelated.pdf", decision: "rejected", excerpt_chars_actual: 600});
const progressTable = jobEls.activity.querySelector(".job-progress-table");
assert.equal(progressTable.tagName, "table");
assert.equal(progressTable.children[1].children.length, 2);
assert.equal(progressTable.children[1].children[0].children[0].textContent, "<example>.pdf");
assert.equal(progressTable.children[1].children[0].children[1].textContent, "Likely match");
assert.equal(progressTable.children[1].children[0].children[2].textContent, "1800 characters");
assert.match(jobEls.activity.querySelector(".job-progress-count").textContent, /2 checked.*1 likely matches.*0 need review.*0 checking/);

// Execute the shipped asynchronous model-fork handoff. A failed clone must
// leave the original conversation usable; retry preserves messages and binds
// only the fork to its independent server selection.
const forkBegin = app.indexOf("      function forkChatFrom(chat)");
const forkEnd = app.indexOf("      function showMigrationBanner()", forkBegin);
const forkFixture = eval(`(() => {
  const state = { chats: [] }; let currentId = "original";
  const els = { mismatchFork: new Element(), mismatchDetail: new Element(),
    mismatchScrim: new Element(), mismatchDialog: new Element() };
  const backend = { id: "qwen", version: "test-qwen" };
  const makeId = () => "independent-fork";
  let fail = true, saves = 0, requested;
  const authFetch = async (path, options) => {
    requested = { path, body: JSON.parse(options.body) };
    return { ok: !fail, json: async () => fail ? { error: { message: "clone failed" } } : { job_id: "cloned-task" } };
  };
  const saveState = () => { saves++; };
  const render = () => {}; const renderList = () => {}; const closePanels = () => {};
  ${app.slice(forkBegin, forkEnd)}
  const original = { id: "original", title: "Selected files", messages: [{ role: "user", content: "My question" }],
    workflow_job_id: "original-task", model_id: "ornith", model_version: "test-ornith" };
  state.chats.push(original);
  return { start: () => { openMismatchDialog(original); }, run: confirmForkMismatch,
    retry: () => { fail = false; }, inspect: () => ({ state, currentId, requested, saves, els }) };
})()`);
forkFixture.start();
await forkFixture.run();
assert.equal(forkFixture.inspect().state.chats.length, 1);
assert.equal(forkFixture.inspect().currentId, "original");
assert.match(forkFixture.inspect().els.mismatchDetail.textContent, /clone failed/);
forkFixture.retry();
await forkFixture.run();
const forked = forkFixture.inspect();
assert.equal(forked.state.chats.length, 2);
assert.equal(forked.currentId, "independent-fork");
assert.equal(forked.state.chats[0].workflow_job_id, "cloned-task");
assert.equal(forked.state.chats[1].workflow_job_id, "original-task");
assert.equal(forked.state.chats[0].messages[0].content, "My question");
assert.equal(forked.state.chats[0].model_id, null);
assert.deepEqual(forked.requested.body, { job_id: "original-task", clone: true });

const stoppedContext = { jobId: "copy-ui-action", planId: "reviewable-plan-token", operation: "copy" };
renderJobEvent({ type: "action_started", job_id: "copy-ui-action", run_id: "current-run" }, stoppedContext);
await jobEls.barActions.children[0].onclick();
const stopRequest = uiRequests.at(-1);
assert.equal(stopRequest.path, "/v1/jobs/action/stop");
assert.deepEqual(JSON.parse(stopRequest.options.body), { job_id: "copy-ui-action", run_id: "current-run" });
assert.match(jobEls.barText.textContent, /Stopping after the current file/);
renderJobEvent({ type: "applied", applied: 1, skipped: 0, pending: 1 }, stoppedContext);
renderJobEvent({ type: "done", stopped: true, pending: 1, summary: "Stopped after one copy." }, stoppedContext);
assert.equal(stoppedContext.skipped, 1);
assert.deepEqual(jobEls.barActions.children.map(button => button.textContent), ["Resume plan", "Undo completed files"]);
let resumed;
globalThis.applyJob = (job, context) => { resumed = { job, planId: context.planId }; };
await jobEls.barActions.children[0].onclick();
assert.deepEqual(resumed, { job: "copy-ui-action", planId: "reviewable-plan-token" });
renderJobEvent({ type: "undone", undone: 1, skipped: 0, pending: 1 }, stoppedContext);
assert.equal(stoppedContext.undone, false, "partial undo was marked complete");
renderJobEvent({ type: "done", stopped: true, undo: true, pending: 1, summary: "Undo stopped." }, stoppedContext);
assert.equal(jobEls.barActions.children[0].textContent, "Resume undo");

process.stdout.write("jobs UI DOM fixtures: PASS\n");
