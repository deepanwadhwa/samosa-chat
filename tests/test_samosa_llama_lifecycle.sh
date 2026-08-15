#!/bin/sh
set -eu

# Bonsai/Ornith use llama-server rather than the Qwen executable. This test
# exercises the browser-owned app path with that exact backend shape and then
# closes it through the same shutdown endpoint used by pagehide/sendBeacon.
BUILD_DIR="${BUILD_DIR:-build}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-llama-lifecycle.XXXXXX")
HOME_DIR="$TMP/home"
RELEASE_DIR="$TMP/release"
LLAMA_DIR="$HOME_DIR/backends/prism-llama.cpp/build/bin"
PORT=19373
GW_PID=
LLAMA_PID=

cleanup() {
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    sh "$ROOT/dist/samosa" serve --stop >/dev/null 2>&1 || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$LLAMA_PID" ] || kill "$LLAMA_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

grep -q 'window.addEventListener("pagehide", shutDownOwnedApp)' "$ROOT/assets/app.html"
grep -q 'navigator.sendBeacon("/v1/shutdown", "")' "$ROOT/assets/app.html"

mkdir -p "$HOME_DIR/models/bonsai-27b-1bit" "$LLAMA_DIR" "$RELEASE_DIR/bin"
cp "$ROOT/$BUILD_DIR/samosa-gateway" "$RELEASE_DIR/bin/samosa-gateway"
cp "$ROOT/$BUILD_DIR/test_fake_openai_backend" "$RELEASE_DIR/bin/qwen36b"
cp "$ROOT/$BUILD_DIR/test_fake_openai_backend" "$LLAMA_DIR/llama-server"
cp "$ROOT/$BUILD_DIR/chutni-mcp" "$RELEASE_DIR/bin/chutni-mcp"
cp "$ROOT/assets/app.html" "$RELEASE_DIR/app.html"
cp "$ROOT/assets/samosa-chat.png" "$RELEASE_DIR/samosa-chat.png"
cp "$ROOT/assets/models.json" "$RELEASE_DIR/models.json"
printf 'fixture\n' >"$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'bonsai\n' >"$HOME_DIR/model-backend"
printf '#!/bin/sh\nexit 0\n' >"$TMP/open"
chmod +x "$TMP/open"

SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
  SAMOSA_OPEN="$TMP/open" sh "$ROOT/dist/samosa" app >/dev/null

GW_PID=$(tr -d '\n' <"$HOME_DIR/server.pid")
[ -n "$GW_PID" ]
HEALTH=
i=0
while [ "$i" -lt 100 ]; do
  HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$HEALTH" | grep -q '"app_owned":true' && \
    printf '%s' "$HEALTH" | grep -q '"backend":"bonsai"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "FAIL: app-owned llama gateway did not become healthy" >&2; exit 1; }
printf '%s' "$HEALTH" | grep -q '"app_owned":true'
printf '%s' "$HEALTH" | grep -q '"backend":"bonsai"'
LLAMA_PID=$(printf '%s' "$HEALTH" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$LLAMA_PID" ] && kill -0 "$LLAMA_PID" 2>/dev/null

# This is the request sent by the pagehide beacon when the browser app closes.
curl -fsS --max-time 5 -X POST "http://127.0.0.1:$PORT/v1/shutdown" >/dev/null
i=0
while kill -0 "$GW_PID" 2>/dev/null && [ "$i" -lt 100 ]; do
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "FAIL: app-owned gateway did not shut down" >&2; exit 1; }

i=0
while kill -0 "$LLAMA_PID" 2>/dev/null && [ "$i" -lt 100 ]; do
  sleep 0.05
  i=$((i + 1))
done
if kill -0 "$LLAMA_PID" 2>/dev/null; then
  echo "FAIL: llama-server survived browser app shutdown" >&2
  exit 1
fi
sleep 0.3
if curl -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
  echo "FAIL: app-owned llama gateway was respawned after shutdown" >&2
  exit 1
fi

echo "test_samosa_llama_lifecycle.sh: PASS"
