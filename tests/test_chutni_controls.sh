#!/bin/sh
set -eu

fail() {
  echo "test_chutni_controls.sh: FAIL: $1" >&2
  exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-chutni-controls.$$
PORT=$((21000 + $$ % 2000))
GW_PID=""
BACK_PID=""
cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$BACK_PID" ] || kill "$BACK_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  [ -z "$BACK_PID" ] || wait "$BACK_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$TMP/home" "$TMP/source"
printf 'repository-only pause fixture\n' >"$TMP/source/one.txt"

BUILD_DIR="${BUILD_DIR:-build}"

SAMOSA_FAKE_MODEL_FILE="$TMP/model.gguf" \
  "$ROOT/$BUILD_DIR/test_fake_openai_backend" "$((PORT + 1))" >"$TMP/backend.log" 2>&1 &
BACK_PID=$!

REAL_CHUTNI_MCP="$ROOT/$BUILD_DIR/chutni-mcp" \
SAMOSA_HOME="$TMP/home" SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_URL="http://127.0.0.1:$((PORT + 1))" \
SAMOSA_CHUTNI_SERVICE="$ROOT/tests/fake_chutni_slow.sh" \
SAMOSA_APP_HTML="$ROOT/assets/app.html" \
  "$ROOT/$BUILD_DIR/samosa-gateway" >"$TMP/gateway.log" 2>&1 &
GW_PID=$!

i=0
while [ "$i" -lt 100 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >"$TMP/health.json" 2>/dev/null && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { cat "$TMP/gateway.log" >&2; fail "gateway never became ready"; }
TOKEN=$(curl -fsS "http://127.0.0.1:$PORT/" |
  sed -n 's/.*name="samosa-ui-token" content="\([^"]*\)".*/\1/p')
[ -n "$TOKEN" ] || fail "failed to get token"

PREFLIGHT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}]}")
PREFLIGHT_ID=$(printf '%s' "$PREFLIGHT" | sed -n 's/.*"preflight_id":"\([^"]*\)".*/\1/p')
[ -n "$PREFLIGHT_ID" ] || fail "failed to get preflight id"
CREATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT_ID\",\"display_name\":\"Control fixture\"}")
SCOPE=$(printf '%s' "$CREATED" | sed -n 's/.*"scope_id":"\([^"]*\)".*/\1/p')
JOB=$(printf '%s' "$CREATED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$SCOPE" ] && [ -n "$JOB" ] || fail "scope ID missing from create response"

i=0
while [ "$i" -lt 100 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"scan_files_seen":[2-9]' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "$STATUS" >&2; cat "$TMP/gateway.log" >&2; fail "scope never reached scan phase"; }
printf '%s' "$STATUS" | grep -q "\"active_job_id\":\"$JOB\"" || fail "missing active_job_id"
printf '%s' "$STATUS" | grep -q '"phase":"scan"' || fail "missing phase:scan"
printf '%s' "$STATUS" | grep -q '"elapsed_seconds":' || fail "missing elapsed_seconds"
printf '%s' "$STATUS" | grep -q '"files_per_second":' || fail "missing files_per_second"

PAUSED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/pause" \
  --data-binary "{\"job_id\":\"$JOB\"}")
printf '%s' "$PAUSED" | grep -q '"state":"paused_user"' || fail "expected state:paused_user"
# Simulate a scope created by the pre-monitoring release. The live status
# overlay must add new fields instead of requiring the user to forget/re-add.
printf '{"id":"%s","schema_version":2,"kind":"folder","display_name":"Control fixture","canonical_root":"%s","state":"unbuilt","evidence_generation":0}\n' \
  "$SCOPE" "$TMP/source" >"$TMP/home/chutni/scopes/$SCOPE/scope.json"
STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
printf '%s' "$STATUS" | grep -q '"state":"paused_user"' || fail "expected state:paused_user"
printf '%s' "$STATUS" | grep -q "\"active_job_id\":\"$JOB\"" || fail "missing active_job_id"
printf '%s' "$STATUS" | grep -q '"scan_files_seen":' || fail "missing scan_files_seen"

RESUMED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/resume" \
  --data-binary "{\"job_id\":\"$JOB\"}")
printf '%s' "$RESUMED" | grep -q '"state":"queued"' || fail "expected state:queued"

i=0
while [ "$i" -lt 100 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"building"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "$STATUS" >&2; cat "$TMP/gateway.log" >&2; fail "scope never reached building state"; }

CANCELED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/cancel" \
  --data-binary "{\"job_id\":\"$JOB\"}")
printf '%s' "$CANCELED" | grep -q '"state":"canceling"' || fail "expected state:canceling"
STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
printf '%s' "$STATUS" | grep -q '"state":"canceled_initial"' || fail "expected state:canceled_initial"

echo "test_chutni_controls.sh: PASS"
