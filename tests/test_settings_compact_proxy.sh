#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T3.2 (docs/TASKS_UI_CHUTNI.md): POST /v1/settings and POST /v1/compact were
# always called by assets/app.html but the compiled gateway never routed
# them -- verified live (before this fix) that both 404'd unconditionally.
# qwen36b.c's own --serve HTTP server already implements both natively
# against its live KV cache/session state; the fix adds two route-table
# entries in src/samosa_gateway.c that reuse the existing generic
# proxy_request() (the same function GET /v1/models already uses) to reach
# that real implementation -- no new logic, just wiring. This test proves
# the wiring: a real compiled gateway, a fake "qwen" backend standing in for
# qwen36b.c (tests/fake_openai_backend.c, extended with these two routes for
# this task), and real HTTP round trips through the gateway's proxy path.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/settings_compact_proxy_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18998
GW_PID=""

cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR/qwen-model"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"
printf '%s\n' '{"offline":false,"search":{"provider":"fixture","providers":{"fixture":{"api_key":"keep-me"}}},"unrelated":{"keep":true}}' >"$HOME_DIR/config.json"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_QWEN_ENGINE="$BACKEND" \
SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
GW_PID=$!
i=0
while [ "$i" -lt 100 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true' && break
  sleep 0.05; i=$((i + 1))
done
TOKEN=$(cat "$HOME_DIR/run/ui-token")

# --- 1. No token -> 401 (a new route defaults to gated, per the T1.2 design) ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/settings" -d '{}')
[ "$STATUS" = "401" ] || { echo "FAIL: /v1/settings with no token should be 401, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/compact" -d '{}')
[ "$STATUS" = "401" ] || { echo "FAIL: /v1/compact with no token should be 401, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/runtime/settings")
[ "$STATUS" = "401" ] || { echo "FAIL: runtime settings with no token should be 401, got $STATUS"; exit 1; }

# --- 2. With a valid token, reaches the real backend's own /v1/settings ---
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/settings" \
  -H 'Content-Type: application/json' -d '{"context_tokens":"auto","auto_compact":true,"compact_threshold_percent":80}')
printf '%s' "$RESP" | grep -q '"context_limit_tokens":24576' || { echo "FAIL: expected the fake backend's real /v1/settings response, got: $RESP"; exit 1; }

# --- 3. With a valid token, reaches the real backend's own /v1/compact ---
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/compact" \
  -H 'Content-Type: application/json' -d '{"conversation_id":"chat-1"}')
printf '%s' "$RESP" | grep -q '"before_tokens":1000' || { echo "FAIL: expected the fake backend's real /v1/compact response, got: $RESP"; exit 1; }

# --- 4. GET is not accepted (both are POST-only, matching qwen36b.c's own routes) ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/settings")
[ "$STATUS" != "200" ] || { echo "FAIL: GET /v1/settings should not succeed"; exit 1; }

# --- 5. Advanced runtime settings are server-owned, validated, and durable ---
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/runtime/settings")
printf '%s' "$RESP" | grep -q '"backend":"qwen"' || { echo "FAIL: runtime settings omitted backend: $RESP"; exit 1; }
printf '%s' "$RESP" | grep -q '"requested":"auto"' || { echo "FAIL: runtime settings omitted Auto defaults: $RESP"; exit 1; }
printf '%s' "$RESP" | grep -q '"supported":true' || { echo "FAIL: Qwen compaction should be reported supported: $RESP"; exit 1; }

STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" \
  -X PATCH "http://127.0.0.1:$PORT/v1/runtime/settings" -H 'Content-Type: application/json' \
  -d '{"cpu_threads":99999}')
[ "$STATUS" = "400" ] || { echo "FAIL: out-of-range thread count should be 400, got $STATUS"; exit 1; }

RESP_FILE="$TMP/runtime-response.json"
STATUS=$(curl -sS -o "$RESP_FILE" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" \
  -X PATCH "http://127.0.0.1:$PORT/v1/runtime/settings" -H 'Content-Type: application/json' \
  -d '{"cpu_threads":1,"context_tokens":4096,"auto_compact":false,"compact_threshold_percent":75}')
[ "$STATUS" = "202" ] || { echo "FAIL: applying runtime settings should restart with 202, got $STATUS: $(cat "$RESP_FILE")"; exit 1; }
grep -q '"restarted":true' "$RESP_FILE" || { echo "FAIL: runtime response did not report restart"; exit 1; }
i=0
while [ "$i" -lt 100 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true' && break
  sleep 0.05; i=$((i + 1))
done
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/runtime/settings")
printf '%s' "$RESP" | grep -q '"requested":1,"effective":1' || { echo "FAIL: thread setting did not persist: $RESP"; exit 1; }
printf '%s' "$RESP" | grep -q '"requested":4096,"effective":4096' || { echo "FAIL: context setting did not persist: $RESP"; exit 1; }
printf '%s' "$RESP" | grep -q '"auto":false,"threshold_percent":75' || { echo "FAIL: compaction setting did not persist: $RESP"; exit 1; }
grep -q '"api_key":"keep-me"' "$HOME_DIR/config.json" || { echo "FAIL: runtime save dropped the Web provider secret"; exit 1; }
grep -q '"unrelated":{"keep":true}' "$HOME_DIR/config.json" || { echo "FAIL: runtime save dropped an unrelated config key"; exit 1; }

echo "test_settings_compact_proxy.sh: PASS"
