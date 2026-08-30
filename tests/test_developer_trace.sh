#!/bin/sh
set -eu

GATEWAY=${SAMOSA_COMPILED_GATEWAY:-./build/samosa-gateway}
BACKEND=${SAMOSA_FAKE_BACKEND:-./build/test_fake_openai_backend}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-developer-trace.XXXXXX")
HOME_DIR="$TMP/home"
PORT=19054
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/qwen-model"
printf 'experts-fixture\n' > "$HOME_DIR/qwen-model/experts.bin"
printf '{"version":"fixture"}\n' > "$TMP/tokenizer.json"
printf '<!doctype html><title>Samosa test</title>\n' > "$TMP/app.html"
printf 'logo\n' > "$TMP/logo.png"

start_gateway() {
  SAMOSA_HOME="$HOME_DIR" \
  SAMOSA_PORT="$PORT" \
  SAMOSA_BACKEND_PORT=$((PORT + 1)) \
  SAMOSA_APP_HTML="$TMP/app.html" \
  SAMOSA_APP_LOGO="$TMP/logo.png" \
  SAMOSA_QWEN_ENGINE="$BACKEND" \
  SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
  SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
    "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
    sleep 0.05
    i=$((i + 1))
  done
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}

stop_gateway() {
  [ -z "$PID" ] || kill -TERM "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  PID=""
}

start_gateway

STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/developer/trace")
[ "$STATUS" = "401" ] || { echo "FAIL: Developer status without auth returned $STATUS"; exit 1; }

BODY=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace")
printf '%s' "$BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["enabled"] is False and d["captures_sensitive_data"] is True and d["authentication_tokens_logged"] is False, d'

STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"enabled":"yes"}' "http://127.0.0.1:$PORT/v1/developer/trace")
[ "$STATUS" = "400" ] || { echo "FAIL: invalid Developer setting returned $STATUS"; exit 1; }

BODY=$(curl -fsS -X PUT -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"enabled":true}' "http://127.0.0.1:$PORT/v1/developer/trace")
TRACE=$(printf '%s' "$BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["enabled"] is True; print(d["path"])')
python3 - "$TRACE" <<'PY'
import os, stat, sys
assert stat.S_IMODE(os.stat(sys.argv[1]).st_mode) == 0o600
PY
[ "$(cat "$HOME_DIR/developer-mode")" = enabled ] || { echo "FAIL: enabled state was not persisted"; exit 1; }

STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/developer/trace/clear")
[ "$STATUS" = "409" ] || { echo "FAIL: active trace clear returned $STATUS"; exit 1; }

curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"stream":false,"messages":[{"role":"user","content":"DEVELOPER_TRACE_CORRELATION_PROBE"}]}' \
  "http://127.0.0.1:$PORT/v1/chat/completions" >/dev/null

python3 - "$TRACE" "$TOKEN" <<'PY'
import json, sys
path, token = sys.argv[1:]
raw = open(path, encoding="utf-8").read()
assert token not in raw
events = [json.loads(line) for line in raw.splitlines() if line]
names = [event["event"] for event in events]
for required in ["chat_request_received", "chat_turn_started", "backend_request",
                 "backend_response", "backend_complete", "chat_turn_completed"]:
    assert required in names, (required, names)
assert "DEVELOPER_TRACE_CORRELATION_PROBE" in raw
turns = {event.get("turn_id") for event in events if event["event"].startswith("chat_") or event["event"].startswith("backend_")}
turns.discard(None)
assert len(turns) == 1, turns
PY

stop_gateway
start_gateway
BODY=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace")
TRACE_AFTER_RESTART=$(printf '%s' "$BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["enabled"] is True; print(d["path"])')
[ "$TRACE_AFTER_RESTART" != "$TRACE" ] || { echo "FAIL: restart did not create a new trace session"; exit 1; }

curl -fsS -X PUT -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"enabled":false}' "http://127.0.0.1:$PORT/v1/developer/trace" >/dev/null
[ "$(cat "$HOME_DIR/developer-mode")" = disabled ] || { echo "FAIL: disabled state was not persisted"; exit 1; }
curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"stream":false,"messages":[{"role":"user","content":"DEVELOPER_TRACE_DISABLED_SECRET"}]}' \
  "http://127.0.0.1:$PORT/v1/chat/completions" >/dev/null
! grep -q 'DEVELOPER_TRACE_DISABLED_SECRET' "$TRACE_AFTER_RESTART" || { echo "FAIL: disabled mode logged a later prompt"; exit 1; }

BODY=$(curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace/clear")
printf '%s' "$BODY" | python3 -c 'import json,sys; assert json.load(sys.stdin)["removed"] >= 2'
[ ! -e "$TRACE" ] && [ ! -e "$TRACE_AFTER_RESTART" ] || { echo "FAIL: clear left Developer trace files behind"; exit 1; }

echo "test_developer_trace.sh: PASS"
