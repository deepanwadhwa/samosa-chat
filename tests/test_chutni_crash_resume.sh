#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-crash-resume.XXXXXX")
PORT=$((21800 + $$ % 500))
GW_PID=
BACK_PID=
SCAN_PID=
cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$SCAN_PID" ] || kill "$SCAN_PID" 2>/dev/null || true
  [ -z "$BACK_PID" ] || kill "$BACK_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  [ -z "$BACK_PID" ] || wait "$BACK_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$TMP/home" "$TMP/source"
i=1
while [ "$i" -le 30 ]; do
  printf 'CHUTNI_REAL_CRASH_SENTINEL_%s\n' "$i" >"$TMP/source/file-$i.txt"
  i=$((i + 1))
done
SAMOSA_FAKE_MODEL_FILE="$TMP/model.gguf" \
  "$ROOT/$BUILD_DIR/test_fake_openai_backend" "$((PORT + 1))" >"$TMP/backend.log" 2>&1 &
BACK_PID=$!

launch_gateway() {
  SAMOSA_HOME="$TMP/home" SAMOSA_PORT="$PORT" \
  SAMOSA_BACKEND_PORT="$((PORT + 1))" \
  SAMOSA_CHUTNI_SERVICE="$ROOT/tests/chutni_real_slow_wrapper.sh" \
  SAMOSA_REAL_CHUTNI_SERVICE="$ROOT/$BUILD_DIR/chutni-mcp" \
  SAMOSA_TEST_CHUTNI_PID_FILE="$TMP/chutni-worker.pid" \
  SAMOSA_FS="$ROOT/$BUILD_DIR/samosa-fs" \
  SAMOSA_APP_HTML="$ROOT/assets/app.html" \
  CHUTNI_TEST_SCAN_DELAY_US=40000 \
  "$ROOT/$BUILD_DIR/samosa-gateway" >>"$TMP/gateway.log" 2>&1 &
  GW_PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" >"$TMP/health.json" 2>/dev/null && break
    sleep 0.05
    i=$((i + 1))
  done
  [ "$i" -lt 100 ] || { cat "$TMP/gateway.log" >&2; return 1; }
  TOKEN=$(curl -fsS "http://127.0.0.1:$PORT/" |
    sed -n 's/.*name="samosa-ui-token" content="\([^"]*\)".*/\1/p')
  [ -n "$TOKEN" ]
}

launch_gateway
PREFLIGHT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}]}")
PREFLIGHT_ID=$(printf '%s' "$PREFLIGHT" | sed -n 's/.*"preflight_id":"\([^"]*\)".*/\1/p')
POLICY=$(printf '%s' "$PREFLIGHT" | sed -n 's/.*"policy_fingerprint":"\([^"]*\)".*/\1/p' | head -1)
[ -n "$PREFLIGHT_ID" ] && [ -n "$POLICY" ]
CREATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT_ID\",\"policy_fingerprint\":\"$POLICY\",\"display_name\":\"Real scan crash fixture\"}")
SCOPE=$(printf '%s' "$CREATED" | sed -n 's/.*"scope_id":"\([^"]*\)".*/\1/p')
JOB=$(printf '%s' "$CREATED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$SCOPE" ] && [ -n "$JOB" ]

# Wait for the real scanner to report an observed source, then stop both the
# gateway and its in-flight scanner without graceful cleanup.
i=0
while [ "$i" -lt 200 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"scan_files_seen":[1-9]' && break
  kill -0 "$GW_PID" 2>/dev/null || { cat "$TMP/gateway.log" >&2; exit 1; }
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 200 ] || { printf '%s\n' "$STATUS" >&2; cat "$TMP/gateway.log" >&2; exit 1; }
[ -f "$TMP/chutni-worker.pid" ]
SCAN_PID=$(cat "$TMP/chutni-worker.pid")
kill -KILL "$GW_PID" 2>/dev/null || true
wait "$GW_PID" 2>/dev/null || true
GW_PID=
kill -KILL "$SCAN_PID" 2>/dev/null || true
SCAN_PID=
sleep 0.05

launch_gateway
i=0
while [ "$i" -lt 100 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"paused_user"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { printf '%s\n' "$STATUS" >&2; cat "$TMP/gateway.log" >&2; exit 1; }
RESUMED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/resume" \
  --data-binary "{\"job_id\":\"$JOB\"}")
printf '%s' "$RESUMED" | grep -q '"state":"queued"'
i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { printf '%s\n' "$STATUS" >&2; cat "$TMP/gateway.log" >&2; exit 1; }
printf '%s' "$STATUS" | grep -q '"files_indexed":30'
printf '%s\n' 'test_chutni_crash_resume.sh: PASS (real scanner killed mid-index; restart resumed to ready)'
