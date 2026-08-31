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
STALE_BACKEND_PID=

cleanup() {
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    sh "$ROOT/dist/samosa" serve --stop >/dev/null 2>&1 || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$BACKEND_PID" ] || kill "$BACKEND_PID" 2>/dev/null || true
  [ -z "$STALE_BACKEND_PID" ] || kill "$STALE_BACKEND_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

grep -q 'window.addEventListener("pagehide", closeAppLifecycle)' "$ROOT/assets/app.html"
grep -q 'navigator.sendBeacon("/v1/app/lifecycle"' "$ROOT/assets/app.html"

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

# Reproduce launchd's asynchronous bootout/bootstrap race deterministically:
# the first bootstrap reports failure, while the retry delegates to the real
# launchctl. The launcher must still bring up a healthy persistent service.
if [ "$(uname -s)" = Darwin ] && command -v launchctl >/dev/null 2>&1; then
  REAL_LAUNCHCTL=$(command -v launchctl)
  cat >"$TMP/launchctl-retry" <<EOF
#!/bin/sh
if [ "\${1:-}" = bootstrap ] && [ ! -e "$TMP/bootstrap-failed-once" ]; then
  : >"$TMP/bootstrap-failed-once"
  exit 5
fi
exec "$REAL_LAUNCHCTL" "\$@"
EOF
  chmod +x "$TMP/launchctl-retry"
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    SAMOSA_LAUNCHCTL="$TMP/launchctl-retry" \
    sh "$ROOT/dist/samosa" serve >/dev/null
  [ -f "$TMP/bootstrap-failed-once" ]
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    SAMOSA_LAUNCHCTL="$TMP/launchctl-retry" \
    sh "$ROOT/dist/samosa" serve --stop >/dev/null
fi

# The user-facing app default must be browser-owned and must not register a
# launchd KeepAlive job. Activity Monitor-style termination of its gateway
# must also stop the model and stay stopped.
SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
  SAMOSA_OPEN="$TMP/open" sh "$ROOT/dist/samosa" app >/dev/null
DEFAULT_HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
printf '%s' "$DEFAULT_HEALTH" | grep -q '"app_owned":true'
DEFAULT_GW_PID=$(tr -d '\r\n' <"$HOME_DIR/server.pid")
DEFAULT_BACKEND_PID=$(printf '%s' "$DEFAULT_HEALTH" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$DEFAULT_GW_PID" ] && [ -n "$DEFAULT_BACKEND_PID" ]
kill -TERM "$DEFAULT_GW_PID"
i=0
while kill -0 "$DEFAULT_GW_PID" 2>/dev/null && [ "$i" -lt 200 ]; do
  sleep 0.05
  i=$((i + 1))
done
kill -0 "$DEFAULT_GW_PID" 2>/dev/null && { echo "FAIL: default app gateway survived SIGTERM" >&2; exit 1; }
kill -0 "$DEFAULT_BACKEND_PID" 2>/dev/null && { echo "FAIL: default app backend survived gateway SIGTERM" >&2; exit 1; }
sleep 0.3
curl -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && {
  echo "FAIL: launchd respawned the default app gateway" >&2; exit 1;
}

# The browser-owned default must transition cleanly from an ordinary
# launchd-owned `serve` process without racing its stale server.pid.
if [ "$(uname -s)" = Darwin ] && command -v launchctl >/dev/null 2>&1; then
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    sh "$ROOT/dist/samosa" serve >/dev/null
  SERVICE_PID=$(tr -d '\r\n' <"$HOME_DIR/server.pid")
  SERVICE_HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
  printf '%s' "$SERVICE_HEALTH" | grep -q '"app_owned":false'

  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    SAMOSA_OPEN="$TMP/open" sh "$ROOT/dist/samosa" app >/dev/null
  i=0
  while [ "$i" -lt 100 ]; do
    TRANSITION_HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
    printf '%s' "$TRANSITION_HEALTH" | grep -q '"app_owned":true' && break
    sleep 0.05
    i=$((i + 1))
  done
  [ "$i" -lt 100 ] || { echo "FAIL: launchd-to-app ownership transition failed" >&2; exit 1; }
  TRANSITION_PID=$(tr -d '\r\n' <"$HOME_DIR/server.pid")
  [ "$TRANSITION_PID" != "$SERVICE_PID" ] || {
    echo "FAIL: app mode reused the launchd-owned gateway" >&2; exit 1;
  }
  SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
    sh "$ROOT/dist/samosa" serve --stop >/dev/null
fi

# Recreate an abnormal prior exit: the gateway is gone but its exact model
# process and durable PID marker remain. App startup must recover that stale
# child before launching the newly supervised backend on the same port.
mkdir -p "$HOME_DIR/run"
"$RELEASE_DIR/bin/qwen36b" --serve --port $((PORT + 1)) >"$TMP/stale-backend.log" 2>&1 &
STALE_BACKEND_PID=$!
printf '%s\n' "$STALE_BACKEND_PID" >"$HOME_DIR/run/backend.pid"
i=0
while [ "$i" -lt 100 ]; do
  curl -fsS "http://127.0.0.1:$((PORT + 1))/healthz" >/dev/null 2>&1 && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "FAIL: stale backend fixture did not start" >&2; exit 1; }

if ! SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
  SAMOSA_OPEN="$TMP/open" SAMOSA_VOICE_TRACE_AUTO=0 \
  sh "$ROOT/dist/samosa" app >/dev/null; then
  echo "FAIL: app-owned gateway startup log:" >&2
  sed -n '1,240p' "$HOME_DIR/server.log" >&2 || true
  exit 1
fi

i=0
while [ "$i" -lt 100 ]; do
  stale_state=$(ps -p "$STALE_BACKEND_PID" -o stat= 2>/dev/null | tr -d '[:space:]' || true)
  case "$stale_state" in ''|Z*) break ;; esac
  sleep 0.05
  i=$((i + 1))
done
case "$stale_state" in
  ''|Z*) wait "$STALE_BACKEND_PID" 2>/dev/null || true; STALE_BACKEND_PID= ;;
  *) echo "FAIL: app startup did not stop the stale backend" >&2; exit 1 ;;
esac

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
TOKEN=$(tr -d '\r\n' <"$HOME_DIR/run/ui-token")
[ -n "$TOKEN" ]

# The browser troubleshooting button uses this authenticated kill route. It
# must terminate the same app-owned gateway/backend tree without a respawn.
curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/kill" >/dev/null
i=0
while kill -0 "$GW_PID" 2>/dev/null && [ "$i" -lt 200 ]; do
  sleep 0.05
  i=$((i + 1))
done
kill -0 "$GW_PID" 2>/dev/null && { echo "FAIL: browser kill left gateway running" >&2; exit 1; }
kill -0 "$BACKEND_PID" 2>/dev/null && { echo "FAIL: browser kill left backend running" >&2; exit 1; }
sleep 0.3
curl -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && {
  echo "FAIL: browser-killed gateway was respawned" >&2; exit 1;
}

# Start a fresh default app instance for refresh-versus-final-tab-close checks.
SAMOSA_HOME="$HOME_DIR" SAMOSA_RELEASE_DIR="$RELEASE_DIR" SAMOSA_PORT="$PORT" \
  SAMOSA_OPEN="$TMP/open" SAMOSA_VOICE_TRACE_AUTO=1 \
  sh "$ROOT/dist/samosa" app >/dev/null
GW_PID=$(tr -d '\r\n' <"$HOME_DIR/server.pid")
HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
printf '%s' "$HEALTH" | grep -q '"app_owned":true'
BACKEND_PID=$(printf '%s' "$HEALTH" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
TOKEN=$(tr -d '\r\n' <"$HOME_DIR/run/ui-token")
[ -n "$GW_PID" ] && [ -n "$BACKEND_PID" ] && [ -n "$TOKEN" ]

# Refresh closes the old document and opens a replacement. The grace period
# must preserve both the gateway and its already-loaded backend.
curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"action":"open","client_id":"page-old"}' \
  "http://127.0.0.1:$PORT/v1/app/lifecycle" >/dev/null
curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"action":"close","client_id":"page-old"}' \
  "http://127.0.0.1:$PORT/v1/app/lifecycle" >/dev/null
curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"action":"open","client_id":"page-new"}' \
  "http://127.0.0.1:$PORT/v1/app/lifecycle" >/dev/null
sleep 3
kill -0 "$GW_PID" 2>/dev/null
kill -0 "$BACKEND_PID" 2>/dev/null
curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null

# Closing the final browser client must stop the app-owned gateway and model.
curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -d '{"action":"close","client_id":"page-new"}' \
  "http://127.0.0.1:$PORT/v1/app/lifecycle" >/dev/null
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
grep -q '"event":"browser_clients_closed"' "$HOME_DIR/logs/gateway-lifecycle.jsonl"
grep -q '"event":"gateway_exiting".*"mode":"app_closed".*"signal_number":0' \
  "$HOME_DIR/logs/gateway-lifecycle.jsonl"
TRACE=$(find "$HOME_DIR/logs/voice" -name 'voice-trace-*.jsonl' -type f | head -1)
[ -n "$TRACE" ]
grep -q '"event":"gateway_shutdown_observed".*"mode":"app_closed".*"signal_number":0' "$TRACE"

echo "test_samosa_app_lifecycle.sh: PASS"
