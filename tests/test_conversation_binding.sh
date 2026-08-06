#!/bin/sh
set -eu

# T1.4 (docs/TASKS_UI_CHUTNI.md sec5.2): conversation schema v2 and
# gateway-enforced model binding. Exercises the canonical GET/PUT
# /v1/conversations/<id>/binding endpoints directly, then the binding
# validation and implicit create-once binding wrapped around
# /v1/chat/completions -- against a real gateway process and the repo's
# fake OpenAI-shaped backend standing in for a "ready" model (no 24 GB
# model needed; only the binding logic under test is real).

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/conversation_binding_test.XXXXXX")
HOME_DIR="$TMP/samosahome"
PORT=18995
BACKEND_PORT=18996
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make samosa-gateway test_fake_openai_backend >/dev/null 2>&1 || true

mkdir -p "$HOME_DIR/models/ornith-9b"
ORNITH_MODEL="$HOME_DIR/models/ornith-9b/Ornith-Fixture-Q4_K_M.gguf"
printf 'fixture\n' >"$ORNITH_MODEL"
EXPECTED_VERSION=$(basename "$ORNITH_MODEL")
printf 'ornith\n' >"$HOME_DIR/model-backend"
printf '<!doctype html><title>Compiled Samosa</title><meta name="samosa-ui-token" content="__SAMOSA_UI_TOKEN__">\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT="$BACKEND_PORT" \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_BONSAI_SERVER="$BACKEND" \
SAMOSA_ORNITH_MODEL="$ORNITH_MODEL" \
  "$GATEWAY" >"$TMP/gateway.log" 2>&1 &
PID=$!

i=0
while [ "$i" -lt 100 ]; do
  health=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$health" | grep -q '"ready":true' && break
  kill -0 "$PID" 2>/dev/null || { cat "$TMP/gateway.log" >&2; echo "FAIL: gateway exited before becoming ready"; exit 1; }
  sleep 0.05; i=$((i + 1))
done
printf '%s' "$health" | grep -q '"ready":true' || { echo "FAIL: gateway never became ready"; cat "$TMP/gateway.log" >&2; exit 1; }
printf '%s' "$health" | grep -q "\"model_version\":\"$EXPECTED_VERSION\"" || { echo "FAIL: /healthz did not report the expected interim model_version"; echo "$health"; exit 1; }

TOKEN=$(cat "$HOME_DIR/run/ui-token")
ORIGIN="http://127.0.0.1:$PORT"
AUTH_H="-H X-Samosa-Token:$TOKEN -H Origin:$ORIGIN"

# --- auth: the binding endpoint requires the UI token ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding")
[ "$STATUS" = "401" ] || { echo "FAIL: GET binding without a token should be 401, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding" \
  -H 'Content-Type: application/json' --data '{"model_id":"ornith","model_version":"x"}')
[ "$STATUS" = "401" ] || { echo "FAIL: PUT binding without a token should be 401, got $STATUS"; exit 1; }

# --- GET on a conversation with no recorded binding: 404, not a crash ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-never-bound/binding")
[ "$STATUS" = "404" ] || { echo "FAIL: GET on an unbound conversation should be 404, got $STATUS"; exit 1; }

# --- an id outside the letters/digits/dash/underscore charset is rejected ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/bad%2fid/binding")
[ "$STATUS" = "400" ] || { echo "FAIL: a conversation_id containing a slash should be 400, got $STATUS"; exit 1; }

# --- PUT missing model_id/model_version: 400 ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding" \
  -H 'Content-Type: application/json' --data '{"model_id":"ornith"}')
[ "$STATUS" = "400" ] || { echo "FAIL: PUT without model_version should be 400, got $STATUS"; exit 1; }

# --- PUT malformed JSON body: 400, not a crash ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding" \
  -H 'Content-Type: application/json' --data '{not json')
[ "$STATUS" = "400" ] || { echo "FAIL: PUT with malformed JSON should be 400, got $STATUS"; exit 1; }

# --- create a binding, defaulting model_binding_source to "explicit" ---
PUT1=$(curl -fsS -X PUT $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding" \
  -H 'Content-Type: application/json' --data "{\"model_id\":\"ornith\",\"model_version\":\"$EXPECTED_VERSION\"}")
python3 -c "
import json, sys
b = json.loads(sys.argv[1])
assert b['schema_version'] == 2, b
assert b['model_id'] == 'ornith', b
assert b['model_version'] == sys.argv[2], b
assert b['model_binding_source'] == 'explicit', b
assert b['created_at'] and b['updated_at'], b
" "$PUT1" "$EXPECTED_VERSION"
CREATED_AT=$(printf '%s' "$PUT1" | python3 -c "import json,sys; print(json.load(sys.stdin)['created_at'])")

# --- GET now returns the same record ---
GET1=$(curl -fsS $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding")
python3 -c "
import json, sys
b = json.loads(sys.argv[1])
assert b['model_id'] == 'ornith' and b['model_version'] == sys.argv[2], b
" "$GET1" "$EXPECTED_VERSION"

# --- PUT idempotent when the requested binding is identical ---
PUT2=$(curl -fsS -X PUT $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding" \
  -H 'Content-Type: application/json' --data "{\"model_id\":\"ornith\",\"model_version\":\"$EXPECTED_VERSION\"}")
python3 -c "
import json, sys
b = json.loads(sys.argv[1])
assert b['created_at'] == sys.argv[2], f\"idempotent PUT must not change created_at: {b['created_at']!r} != {sys.argv[2]!r}\"
" "$PUT2" "$CREATED_AT"

# --- PUT with a different model on an already-bound conversation: 409 ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-a/binding" \
  -H 'Content-Type: application/json' --data '{"model_id":"qwen","model_version":"other"}')
[ "$STATUS" = "409" ] || { echo "FAIL: PUT with a conflicting binding should be 409, got $STATUS"; exit 1; }

# --- a corrupt metadata.json is treated as unbound, not a crash ---
mkdir -p "$HOME_DIR/chats/conv-corrupt"
printf 'not json at all' >"$HOME_DIR/chats/conv-corrupt/metadata.json"
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-corrupt/binding")
[ "$STATUS" = "404" ] || { echo "FAIL: a corrupt binding file should read back as unbound (404), got $STATUS"; exit 1; }

# =====================================================================
# /v1/chat/completions binding validation and implicit create-once bind
# =====================================================================

# --- stateless request (no conversation_id): unaffected, forwarded as before ---
REPLY=$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data '{"messages":[{"role":"user","content":"hello"}],"stream":false}')
printf '%s' "$REPLY" | grep -q 'compiled reply' || { echo "FAIL: stateless chat request regressed"; echo "$REPLY"; exit 1; }

# --- conversation_id present but model_id/model_version missing: 400 ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data '{"messages":[{"role":"user","content":"hi"}],"stream":false,"conversation_id":"conv-b"}')
[ "$STATUS" = "400" ] || { echo "FAIL: conversation_id without model identity should be 400, got $STATUS"; exit 1; }

# --- a direct request naming the wrong active model: 409, never forwarded ---
OUT=$(curl -sS -w '\n%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data '{"messages":[{"role":"user","content":"hi"}],"stream":false,"conversation_id":"conv-c","model_id":"qwen","model_version":"whatever"}')
STATUS=$(printf '%s' "$OUT" | tail -n1)
REPLY=$(printf '%s' "$OUT" | sed '$d')
[ "$STATUS" = "409" ] || { echo "FAIL: wrong active model should be 409, got $STATUS"; exit 1; }
printf '%s' "$REPLY" | grep -q 'compiled reply' && { echo "FAIL: a mismatched request must never reach the backend"; exit 1; }

# --- a fresh conversation's first turn, naming the correct active model,
# implicitly creates its canonical binding ---
REPLY=$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":false,\"conversation_id\":\"conv-d\",\"model_id\":\"ornith\",\"model_version\":\"$EXPECTED_VERSION\"}")
printf '%s' "$REPLY" | grep -q 'compiled reply' || { echo "FAIL: a matching first turn should reach the backend"; echo "$REPLY"; exit 1; }
BOUND=$(curl -fsS $AUTH_H "http://127.0.0.1:$PORT/v1/conversations/conv-d/binding")
python3 -c "
import json, sys
b = json.loads(sys.argv[1])
assert b['model_id'] == 'ornith' and b['model_version'] == sys.argv[2], b
assert b['model_binding_source'] == 'explicit', b
" "$BOUND" "$EXPECTED_VERSION"

# --- a second matching turn on the same conversation succeeds without conflict ---
REPLY=$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data "{\"messages\":[{\"role\":\"user\",\"content\":\"again\"}],\"stream\":false,\"conversation_id\":\"conv-d\",\"model_id\":\"ornith\",\"model_version\":\"$EXPECTED_VERSION\"}")
printf '%s' "$REPLY" | grep -q 'compiled reply' || { echo "FAIL: a second matching turn should still reach the backend"; exit 1; }

# --- reopening that same conversation under a different model: 409, not forwarded ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data '{"messages":[{"role":"user","content":"hi"}],"stream":false,"conversation_id":"conv-d","model_id":"qwen","model_version":"other"}')
[ "$STATUS" = "409" ] || { echo "FAIL: a bound conversation continued under a different model should be 409, got $STATUS"; exit 1; }

# --- the gateway must still be alive after all of the above (no crash) ---
curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null || { echo "FAIL: gateway did not survive the conversation binding test"; exit 1; }

echo "test_conversation_binding.sh: PASS"
