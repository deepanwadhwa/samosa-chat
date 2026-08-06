#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T1.1 (docs/TASKS_UI_CHUTNI.md): "Start the gateway with zero installed
# models." Supersedes tests/test_baseline_zero_model_startup.sh (T0.1's
# frozen record of the pre-fix defect: the process used to exit(2) before
# ever binding the HTTP listener). This asserts the target behavior: the
# control plane binds and serves setup/health/diagnostics with no model
# installed anywhere, and Chat requests fail closed with a stable,
# structured 409 model_required instead of a dropped connection.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/zero_model_startup.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18984
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
# Deliberately no model fixtures anywhere under $HOME_DIR.

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!

i=0
while [ "$i" -lt 50 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done

# The process must still be running -- the old defect was exit(2) here.
kill -0 "$PID" 2>/dev/null || {
  echo "FAIL: gateway exited instead of serving the control plane with zero models" >&2
  cat "$TMP/stderr.log" >&2
  exit 1
}

HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
printf '%s' "$HEALTH" | grep -q '"ready":false' || { echo "FAIL: healthz claims ready with no model"; echo "$HEALTH"; exit 1; }
printf '%s' "$HEALTH" | grep -q '"installed":false' || { echo "FAIL: healthz claims installed with no model"; echo "$HEALTH"; exit 1; }
printf '%s' "$HEALTH" | grep -q '"backend_state":"none"' || { echo "FAIL: healthz backend_state should be none"; echo "$HEALTH"; exit 1; }

# Root HTML still renders -- setup must not depend on a model.
HTML_STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$HTML_STATUS" = "200" ] || { echo "FAIL: root HTML did not render (status $HTML_STATUS)"; exit 1; }

# Chat fails closed with a stable, structured error -- never a dropped
# connection, never a crash.
CHAT_STATUS=$(curl -sS -o "$TMP/chat.json" -w '%{http_code}' -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" -d '{"messages":[{"role":"user","content":"hi"}]}')
[ "$CHAT_STATUS" = "409" ] || { echo "FAIL: expected 409, got $CHAT_STATUS"; cat "$TMP/chat.json"; exit 1; }
grep -q '"code":"model_required"' "$TMP/chat.json" || { echo "FAIL: expected model_required error code"; cat "$TMP/chat.json"; exit 1; }

echo "test_zero_model_startup.sh: PASS (control plane serves setup/health/chat-409 with zero models)"
