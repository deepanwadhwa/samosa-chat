#!/bin/sh
set -eu

if [ "$(uname -s)" != "Darwin" ]; then
  echo "test_kokoro_native_gateway.sh: SKIP (macOS dylib contract test)" >&2
  exit 0
fi

# End-to-end contract for the native neural voice boundary. The gateway is
# given a C dylib, not a process or interpreter: it must load the documented
# Sherpa C symbols, pass Kokoro's paths/speaker ID, and respond with PCM WAV.

BUILD_DIR=${BUILD_DIR:-build}
GATEWAY=${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kokoro_native_gateway.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18994
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  [ "${SAMOSA_TEST_KEEP:-0}" = 1 ] || rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/voice/kokoro/runtime/lib" "$HOME_DIR/voice/kokoro/model/espeak-ng-data"
cc -dynamiclib -O2 -Wall -Wextra -Werror -std=c11 -I"$ROOT/src" \
  "$ROOT/tests/fake_kokoro_native.c" \
  -o "$HOME_DIR/voice/kokoro/runtime/lib/libsherpa-onnx-c-api.dylib"
printf 'model\n' >"$HOME_DIR/voice/kokoro/model/model.int8.onnx"
printf 'voices\n' >"$HOME_DIR/voice/kokoro/model/voices.bin"
printf 'tokens\n' >"$HOME_DIR/voice/kokoro/model/tokens.txt"
printf 'native fixture\n' >"$HOME_DIR/voice/kokoro/ready"
printf '<!doctype html><title>Samosa Kokoro fixture</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_MODELS_CATALOG="$ROOT/assets/models.json" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!
i=0
while [ "$i" -lt 80 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done
[ -s "$HOME_DIR/run/ui-token" ] || { cat "$TMP/stderr.log"; exit 1; }
TOKEN=$(cat "$HOME_DIR/run/ui-token")

STATUS=$(curl -sS -o "$TMP/status.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/voice/status")
[ "$STATUS" = 200 ] || { cat "$TMP/status.json"; exit 1; }
grep -q '"tts_neural_ready":true' "$TMP/status.json"
grep -q '"tts_engine":"kokoro_native"' "$TMP/status.json"

STATUS=$(curl -sS -o "$TMP/speech.wav" -w '%{http_code}' -X POST \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  --data '{"text":"Hello from native Kokoro.","voice":"sarah"}' \
  "http://127.0.0.1:$PORT/v1/voice/speech")
[ "$STATUS" = 200 ] || { cat "$TMP/stderr.log"; exit 1; }
[ "$(dd if="$TMP/speech.wav" bs=1 count=4 2>/dev/null)" = RIFF ] || {
  echo 'FAIL: native Kokoro endpoint did not return a WAV' >&2; exit 1;
}
grep -F 'Python' "$ROOT/tools/samosa_kokoro_runtime.sh" >/dev/null &&
  grep -F 'No Python or pip' "$ROOT/tools/samosa_kokoro_runtime.sh" >/dev/null || {
  echo 'FAIL: native installer does not state its no-Python contract' >&2; exit 1;
}
grep -F 'codesign --force --sign -' "$ROOT/tools/samosa_kokoro_runtime.sh" >/dev/null || {
  echo 'FAIL: native installer does not re-sign macOS dylibs before dlopen' >&2; exit 1;
}

echo 'test_kokoro_native_gateway.sh: PASS'
