#!/bin/sh
set -eu

# T0.1 (docs/TASKS_UI_CHUTNI.md): "record baseline behavior for existing Chat
# and Jobs routes before modifying the gateway."
#
# This freezes TODAY's zero-model startup behavior — the exact blocker T1.1
# ("Start the gateway with zero installed models") exists to remove. Per
# src/samosa_gateway.c main(), backend_start() failing today means the process
# exits(2) before the HTTP listener is ever created: no port, no /healthz, no
# UI at all. T1.1 must replace this with a listening control plane that
# distinguishes backend readiness from control-plane health.
#
# This test is EXPECTED TO BE REPLACED once T1.1 lands (its assertions
# describe the defect, not the target behavior) — do not "fix" it to make
# zero-model startup succeed; write T1.1's own test for that and retire this
# one, noting the supersession in docs/regressions/ui-chutni/.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/baseline_zero_model.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18981
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make samosa-gateway >/dev/null 2>&1 || true

mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
# Deliberately no model fixtures anywhere under $HOME_DIR: backend_available()
# must be false for bonsai, ornith, and qwen alike.

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!

# The process must exit on its own (today's defect); wait bounded, don't kill.
i=0
while [ "$i" -lt 50 ]; do
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  sleep 0.1; i=$((i + 1))
done
if kill -0 "$PID" 2>/dev/null; then
  echo "FAIL: gateway is still running with zero models installed (baseline says it should exit(2))" >&2
  echo "--- this may mean T1.1 already landed; retire this test and write T1.1's own coverage instead ---" >&2
  exit 1
fi

STATUS=0
wait "$PID" 2>/dev/null || STATUS=$?
PID=""

[ "$STATUS" = 2 ] || { echo "FAIL: expected exit(2), got $STATUS"; cat "$TMP/stderr.log" >&2; exit 1; }
grep -q "backend .* is not installed" "$TMP/stderr.log" || {
  echo "FAIL: expected 'backend ... is not installed' on stderr"; cat "$TMP/stderr.log" >&2; exit 1;
}

# The port must never have been bound — no listener, no /healthz, nothing.
if curl -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
  echo "FAIL: /healthz answered despite zero models installed (baseline says the port is never bound)" >&2
  exit 1
fi

echo "test_baseline_zero_model_startup.sh: PASS (today's behavior: exit(2), no listener, no UI at all)"
