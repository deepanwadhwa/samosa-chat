/* Sidebar state and conversation-management regression coverage. The app is
 * a dependency-free static page, so this keeps the offline test dependency-
 * free too and evaluates the exact shipped pure deletion helper. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const scripts = [...app.matchAll(/<script>\s*([\s\S]*?)<\/script>/g)];
assert.ok(scripts.length > 0, "the shipped app must contain its startup script");
assert.doesNotThrow(() => new Function(scripts.at(-1)[1]),
  "the complete shipped startup script must parse before any feature fixture runs");

for (const id of [
  "collapseSidebar", "expandSidebar", "railNewChat", "selectChats",
  "chatSelectionBar", "selectAllChats", "deleteSelectedChats",
  "cancelChatSelection", "deleteChatsScrim", "deleteChatsDialog",
  "deleteChatsConfirm", "deleteChatsCancel",
]) {
  assert.match(app, new RegExp(`id=["']${id}["']`), `${id} must be present in the shipped UI`);
}

assert.match(app, /SIDEBAR_COLLAPSED_KEY\s*=\s*"samosa-sidebar-collapsed-v1"/,
  "the collapsed preference must use its own durable local-storage key");
assert.match(app, /localStorage\.setItem\(SIDEBAR_COLLAPSED_KEY, "1"\)/,
  "collapsing the sidebar must persist across reloads");
assert.match(app, /localStorage\.getItem\(SIDEBAR_COLLAPSED_KEY\) === "1"/,
  "startup must restore the persisted sidebar state");
assert.match(app, /\.shell\.sidebar-collapsed\s*\{\s*grid-template-columns:\s*64px/,
  "collapsed desktop mode must leave a compact rail");
assert.match(app, /\.shell\.sidebar-collapsed \.sidebar-rail\s*\{\s*display:\s*flex/,
  "the compact rail must expose its controls");
assert.match(app, /@media \(max-width: 760px\)[\s\S]*?\.shell\.sidebar-collapsed \.sidebar-body\s*\{\s*display:\s*flex/,
  "desktop collapse state must not hide the mobile drawer contents");

assert.match(app, /class="chat-checkbox" type="checkbox"/,
  "selection mode must render real accessible checkboxes");
assert.match(app, /selectedChatIds\s*=\s*new Set\(\)/,
  "bulk selection must de-duplicate conversation IDs");
assert.match(app, /requestDeleteChats\(\[\.\.\.selectedChatIds\]/,
  "the bulk delete action must use the selected conversation set");
assert.match(app, /class="chooser-dialog delete-dialog"[^>]*role="dialog"[^>]*aria-modal="true"/,
  "deletion must use an accessible in-app modal");

const deleteChatStart = app.indexOf("      function deleteChat(id, opener)");
const deleteChatEnd = app.indexOf("      function selectChat", deleteChatStart);
assert.ok(deleteChatStart >= 0 && deleteChatEnd > deleteChatStart, "deleteChat must remain inspectable");
assert.doesNotMatch(app.slice(deleteChatStart, deleteChatEnd), /\bconfirm\s*\(/,
  "chat deletion must never use the browser-native confirm box");
assert.match(app.slice(deleteChatStart, deleteChatEnd), /requestDeleteChats\(\[id\], opener\)/,
  "single-chat deletion must share the custom confirmation flow");

const newChatStart = app.indexOf("      async function newChat()");
const newChatEnd = app.indexOf("      function deleteChat", newChatStart);
assert.ok(newChatStart >= 0 && newChatEnd > newChatStart, "New conversation must remain inspectable");
const newChatBlock = app.slice(newChatStart, newChatEnd);
assert.doesNotMatch(newChatBlock, /if \(generating\) return/,
  "New conversation must not silently ignore clicks during generation");
assert.match(newChatBlock, /await Promise\.race\(\[cancel\(\)/,
  "New conversation must stop an active response before switching");
assert.match(newChatBlock, /requestAnimationFrame\(resolve\)/,
  "New conversation must yield a paint before replacing the transcript");

const helperStart = app.indexOf("      function removeChatsById");
const helperEnd = app.indexOf("      function deleteDialogFocusableControls", helperStart);
assert.ok(helperStart >= 0 && helperEnd > helperStart, "the pure deletion helper must remain extractable");
const removeChatsById = eval(`(() => {${app.slice(helperStart, helperEnd)}; return removeChatsById;})()`);
const chats = [{ id: "a" }, { id: "b" }, { id: "c" }];
assert.deepEqual(removeChatsById(chats, ["a", "c"]), [{ id: "b" }],
  "bulk deletion must remove every selected chat and preserve unselected chats");
assert.deepEqual(removeChatsById(chats, ["missing"]), chats,
  "unknown IDs must not remove healthy conversations");

// The home file action opens the existing file workflow without sending a
// message or replacing a draft. Prompt suggestions remain editable, and a
// missing model still opens model settings rather than promising a response.
{
  const welcomeStart = app.indexOf("      function welcomeHTML() {");
  const welcomeEnd = app.indexOf("      // A document attachment", welcomeStart);
  const renderStart = app.indexOf("      function renderMessages() {");
  const renderEnd = app.indexOf("      function appendMessageNode", renderStart);
  assert.ok(welcomeStart >= 0 && welcomeEnd > welcomeStart && renderStart >= 0 && renderEnd > renderStart);
  const controls = [];
  const messages = {
    set innerHTML(html) {
      this.html = html;
      controls.length = 0;
      for (const match of html.matchAll(/<button\b([^>]*)>/g)) {
        const attrs = match[1];
        controls.push({ attrs, dataset: { prompt: attrs.match(/data-prompt="([^"]*)"/)?.[1] },
          addEventListener(_event, handler) { this.onclick = handler; } });
      }
    },
    querySelectorAll(selector) { return controls.filter(control => control.attrs.includes(selector.slice(1, -1))); },
    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; },
  };
  const prompt = { value: "My unfinished draft", focus() { this.focused = true; } };
  const views = [];
  const backend = { ready: true };
  let modelSettingsOpened = false;
  const renderHome = eval(`(() => {
    const profileName = "A&B";
    const escapeHTML = text => text.replaceAll("&", "&amp;");
    const els = { messages, prompt };
    const activeChat = () => null;
    const showView = view => views.push(view);
    const resizePrompt = () => {};
    const openModelSettings = () => { modelSettingsOpened = true; };
    ${app.slice(welcomeStart, welcomeEnd)}
    ${app.slice(renderStart, renderEnd)}
    return renderMessages;
  })()`);
  renderHome();
  assert.ok(messages.html.includes("A&amp;B"), "profile names must remain escaped");
  messages.querySelector("[data-open-files]").onclick();
  assert.deepEqual(views, ["jobs"]);
  assert.equal(prompt.value, "My unfinished draft");
  const suggestion = messages.querySelector("[data-prompt]");
  suggestion.onclick();
  assert.equal(prompt.value, suggestion.dataset.prompt);
  assert.equal(prompt.focused, true);
  backend.ready = false;
  renderHome();
  assert.equal(messages.querySelector("[data-open-files]"), null);
  messages.querySelector("[data-choose-model]").onclick();
  assert.equal(modelSettingsOpened, true);
}

console.log("sidebar UI fixtures: PASS");
