#!/bin/sh
set -eu

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

make samosa-gateway test_fake_openai_backend >/dev/null 2>&1 || true

mkdir -p "$HOME_DIR/qwen-model"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"

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

echo "test_settings_compact_proxy.sh: PASS"
