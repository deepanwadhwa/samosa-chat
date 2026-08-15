#!/bin/sh
set -eu

# App mode is deliberately different from `samosa serve`: it owns the
# gateway lifetime and must not register a KeepAlive launchd job.
BUILD_DIR="${BUILD_DIR:-build}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-app-lifecycle.XXXXXX")
HOME_DIR="$TMP/home"
RELEASE_DIR="$TMP/release"
PORT=19372
GW_PID=
BACKEND_PID=

cleanup() {
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    sh "$ROOT/dist/samosa" serve --stop >/dev/null 2>&1 || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$BACKEND_PID" ] || kill "$BACKEND_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

grep -q 'window.addEventListener("pagehide", shutDownOwnedApp)' "$ROOT/assets/app.html"
grep -q 'appLifecycleOwned = !!h.app_owned' "$ROOT/assets/app.html"

mkdir -p "$HOME_DIR/models/qwen" "$RELEASE_DIR/bin"
cp "$ROOT/$BUILD_DIR/samosa-gateway" "$RELEASE_DIR/bin/samosa-gateway"
cp "$ROOT/$BUILD_DIR/test_fake_openai_backend" "$RELEASE_DIR/bin/qwen36b"
cp "$ROOT/$BUILD_DIR/chutni-mcp" "$RELEASE_DIR/bin/chutni-mcp"
cp "$ROOT/assets/app.html" "$RELEASE_DIR/app.html"
cp "$ROOT/assets/samosa-chat.png" "$RELEASE_DIR/samosa-chat.png"
cp "$ROOT/assets/models.json" "$RELEASE_DIR/models.json"
printf 'fixture\n' >"$HOME_DIR/models/qwen/experts.bin"
printf 'fixture\n' >"$HOME_DIR/models/qwen/tokenizer_qwen36.json"
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
  printf '%s' "$HEALTH" | grep -q '"app_owned":true' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "FAIL: app-owned gateway did not become healthy" >&2; exit 1; }
printf '%s' "$HEALTH" | grep -q '"app_owned":true'
BACKEND_PID=$(printf '%s' "$HEALTH" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$BACKEND_PID" ] && kill -0 "$BACKEND_PID" 2>/dev/null

# Activity Monitor-style termination must stay terminated; a launchd KeepAlive
# parent would recreate this process and make the assertion fail below.
kill -TERM "$GW_PID"
i=0
while kill -0 "$GW_PID" 2>/dev/null && [ "$i" -lt 100 ]; do
  sleep 0.05
  i=$((i + 1))
done
if kill -0 "$GW_PID" 2>/dev/null; then
  echo "FAIL: app-owned gateway did not terminate" >&2
  exit 1
fi
if [ -n "$BACKEND_PID" ] && kill -0 "$BACKEND_PID" 2>/dev/null; then
  echo "FAIL: model backend survived gateway termination" >&2
  exit 1
fi
sleep 0.3
if curl -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
  echo "FAIL: app-owned gateway was respawned after termination" >&2
  exit 1
fi

echo "test_samosa_app_lifecycle.sh: PASS"
