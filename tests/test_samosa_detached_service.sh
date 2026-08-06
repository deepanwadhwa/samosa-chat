#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-build}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-detached-service.XXXXXX")
PORT=19371
LAUNCH_SHELL=

stop_test_server() {
  SAMOSA_HOME="$TMP/home" SAMOSA_RELEASE_DIR="$TMP/release" \
    SAMOSA_PORT="$PORT" SAMOSA_CURL="${SAMOSA_CURL:-curl}" \
    sh "$ROOT/dist/samosa" serve --stop >/dev/null 2>&1 || true
}
cleanup() {
  test -z "$LAUNCH_SHELL" || kill "$LAUNCH_SHELL" 2>/dev/null || true
  stop_test_server
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$TMP/home" "$TMP/release/bin" "$TMP/release/model"
cp "$ROOT/$BUILD_DIR/samosa-gateway" "$TMP/release/bin/samosa-gateway"
cp "$ROOT/$BUILD_DIR/test_fake_openai_backend" "$TMP/release/bin/qwen36b"
cp "$ROOT/$BUILD_DIR/chutni-mcp" "$TMP/release/bin/chutni-mcp"
cp "$ROOT/assets/app.html" "$TMP/release/app.html"
cp "$ROOT/assets/samosa-chat.png" "$TMP/release/samosa-chat.png"
printf 'fixture\n' >"$TMP/release/model/experts.bin"
printf '{}\n' >"$TMP/release/tokenizer_qwen36.json"

cat >"$TMP/launch-shell.sh" <<EOF
#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}
SAMOSA_HOME="$TMP/home" SAMOSA_RELEASE_DIR="$TMP/release" \
  SAMOSA_PORT="$PORT" sh "$ROOT/dist/samosa" serve
printf 'launcher-returned\n' >"$TMP/launcher-returned"
sleep 120
EOF
chmod +x "$TMP/launch-shell.sh"

"$TMP/launch-shell.sh" >"$TMP/launcher.out" 2>&1 &
LAUNCH_SHELL=$!

i=0
while [ "$i" -lt 240 ]; do
  [ -f "$TMP/launcher-returned" ] && break
  kill -0 "$LAUNCH_SHELL" 2>/dev/null || {
    sed -n '1,120p' "$TMP/launcher.out" >&2
    exit 1
  }
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 240 ] || {
  echo "FAIL: detached launcher did not return" >&2
  sed -n '1,120p' "$TMP/launcher.out" >&2
  exit 1
}

GATEWAY_PID=$(tr -d '\n' <"$TMP/home/server.pid")
[ -n "$GATEWAY_PID" ] && kill -0 "$GATEWAY_PID"

# Closing the launcher shell must not affect the launchd-owned Samosa service.
kill -HUP "$LAUNCH_SHELL" 2>/dev/null || true
wait "$LAUNCH_SHELL" 2>/dev/null || true
LAUNCH_SHELL=
sleep 0.1
kill -0 "$GATEWAY_PID"
if [ "$(uname -s)" = "Darwin" ]; then
  [ "$(ps -p "$GATEWAY_PID" -o ppid= | tr -d ' ')" = 1 ] || {
    echo "FAIL: Samosa is not owned by launchd" >&2
    exit 1
  }
fi

HEALTH=$(curl -fsS --max-time 5 "http://127.0.0.1:$PORT/healthz")
printf '%s' "$HEALTH" | grep -q '"gateway":true'
printf '%s' "$HEALTH" | grep -q '"chutni":{"available":true'
printf '%s' "$HEALTH" | grep -q '"managed_by":"samosa"'
printf '%s' "$HEALTH" | grep -q '"can_create_memory":true'

# Prove this surviving Samosa process can invoke its bundled Chutni runtime.
# Preflight reads only a repository fixture and does not create a memory.
TOKEN=$(tr -d '\n' <"$TMP/home/run/ui-token")
PREFLIGHT=$(curl -fsS --max-time 5 \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$ROOT/tests/fixtures/chutni_browser_e2e\"}]}")
printf '%s' "$PREFLIGHT" | grep -q '"preflight_id":'
printf '%s' "$PREFLIGHT" | grep -q '"action":"create_store"'

stop_test_server
if kill -0 "$GATEWAY_PID" 2>/dev/null; then
  echo "FAIL: detached Samosa service did not stop cleanly" >&2
  exit 1
fi

echo "test_samosa_detached_service.sh: PASS"
