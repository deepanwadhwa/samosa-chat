import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      let UI_TOKEN =");
const end = app.indexOf("      // App mode owns its gateway/model tree.");
assert.ok(begin >= 0 && end > begin, "session-token helpers must remain extractable");

const oldToken = "a".repeat(64);
const newToken = "b".repeat(64);
globalThis.document = {
  querySelector: selector => selector === 'meta[name="samosa-ui-token"]'
    ? { content: oldToken } : null,
};

const calls = [];
const invalid = {
  ok: false,
  status: 401,
  clone: () => ({ json: async () => ({ error: { code: "invalid_ui_token" } }) }),
};
const success = { ok: true, status: 201 };
globalThis.fetch = async (path, options = {}) => {
  calls.push({ path, options });
  if (calls.length === 1) return invalid;
  if (path === "/") {
    return {
      ok: true,
      status: 200,
      text: async () => `<meta name="samosa-ui-token" content="${newToken}">`,
    };
  }
  return success;
};

const api = eval(`(() => {${app.slice(begin, end)}
  return { authFetch, get token() { return UI_TOKEN; } };
})()`);
const body = new Blob(["video"]);
const response = await api.authFetch("/v1/attachments", {
  method: "POST",
  headers: { "Content-Type": "video/quicktime" },
  body,
});

assert.equal(response, success, "the original upload should be retried after refreshing the token");
assert.equal(calls.length, 3, "one failed request should cause one root refresh and one retry");
assert.equal(calls[0].options.headers.get("X-Samosa-Token"), oldToken);
assert.equal(calls[1].path, "/");
assert.equal(calls[1].options.cache, "no-store");
assert.equal(calls[2].options.headers.get("X-Samosa-Token"), newToken);
assert.equal(calls[2].options.body, body, "the selected File/Blob must survive the retry");
assert.equal(api.token, newToken);

console.log("test_session_token_ui.mjs: PASS");
