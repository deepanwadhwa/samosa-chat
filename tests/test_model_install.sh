#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T2.2 (docs/TASKS_UI_CHUTNI.md section 5.3): resumable server-owned model
# downloads. Exercises the real compiled gateway against
# tests/fake_model_download_server.c (T0.1), which already implements every
# documented failure mode this task's acceptance list names. Never touches
# a real multi-gigabyte artifact or the network.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
SERVER="${SAMOSA_FAKE_DOWNLOAD_SERVER:-./$BUILD_DIR/fake_model_download_server}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/model_install_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18992
SERVER_PORT=18993
GW_PID=""
SERVER_PID=""

cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  [ -z "$SERVER_PID" ] || kill "$SERVER_PID" 2>/dev/null || true
  [ -z "$SERVER_PID" ] || wait "$SERVER_PID" 2>/dev/null || true
  rm -rf "$TMP"
}

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    echo "FAIL: neither shasum nor sha256sum is installed" >&2
    return 127
  fi
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

# A real, non-trivial fixture artifact with a real, independently-computed
# SHA-256 -- exercises the same read_cache_key_file() verification path a
# real Qwen/Bonsai/Ornith artifact would.
python3 -c "
import hashlib
data = bytes((i * 37 + 11) % 256 for i in range(3000000))
open('$TMP/artifact.bin', 'wb').write(data)
open('$TMP/meta.txt', 'w').write(hashlib.sha256(data).hexdigest() + ' ' + str(len(data)) + '\n')
"
read FIXTURE_SHA FIXTURE_SIZE < "$TMP/meta.txt"

cat > "$TMP/catalog.json" <<EOF
{
  "schema_version": 1, "catalog_revision": "test", "runtime_abi": "samosa-model-runtime-v1",
  "models": [
    {
      "id": "testmodel", "version": "v1", "preferred_for_backend": true, "label": "Test",
      "description": "test", "capabilities": ["text"], "backend_kind": "qwen_native",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1", "minimum_ram_bytes": 0,
      "launch_profile_id": "x", "runtime_dependencies": [],
      "license": { "name": "x", "url": "https://huggingface.co/x" },
      "artifacts": [
        { "name": "artifact.bin", "role": "weights", "required": true,
          "url": "http://127.0.0.1:$SERVER_PORT/artifact", "install_path": "test/artifact.bin",
          "file_mode": "0600", "bytes": $FIXTURE_SIZE, "sha256": "$FIXTURE_SHA" }
      ]
    },
    {
      "id": "hugefixture", "version": "v1", "preferred_for_backend": true, "label": "Huge",
      "description": "Declares an artifact far larger than any real free disk space, to exercise the preflight ENOSPC check without needing a real constrained filesystem.",
      "capabilities": ["text"], "backend_kind": "qwen_native",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1", "minimum_ram_bytes": 0,
      "launch_profile_id": "x", "runtime_dependencies": [],
      "license": { "name": "x", "url": "https://huggingface.co/x" },
      "artifacts": [
        { "name": "huge.bin", "role": "weights", "required": true,
          "url": "http://127.0.0.1:$SERVER_PORT/artifact", "install_path": "test/huge.bin",
          "file_mode": "0600", "bytes": 100000000000000, "sha256": "$(printf '%064d' 0)" }
      ]
    }
  ]
}
EOF

start_server() { # start_server <mode> <fixture-file>
  SAMOSA_FAKE_DOWNLOAD_MODE="$1" SAMOSA_FAKE_DOWNLOAD_FILE="$2" \
    "$SERVER" --port "$SERVER_PORT" >"$TMP/server.log" 2>&1 &
  SERVER_PID=$!
  i=0
  while [ "$i" -lt 50 ]; do
    curl -fsS "http://127.0.0.1:$SERVER_PORT/health" >/dev/null 2>&1 && break
    sleep 0.05; i=$((i + 1))
  done
}
stop_server() {
  [ -z "$SERVER_PID" ] || kill "$SERVER_PID" 2>/dev/null || true
  [ -z "$SERVER_PID" ] || wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
}

start_gateway() { # start_gateway [extra_env...]
  env "$@" \
    SAMOSA_HOME="$HOME_DIR" \
    SAMOSA_PORT="$PORT" \
    SAMOSA_BACKEND_PORT=$((PORT + 1)) \
    SAMOSA_APP_HTML="$TMP/app.html" \
    SAMOSA_APP_LOGO="$TMP/logo.png" \
    SAMOSA_MODELS_CATALOG="$TMP/catalog.json" \
    SAMOSA_TEST_ALLOW_LOOPBACK_ARTIFACTS=1 \
    "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  GW_PID=$!
  i=0
  while [ "$i" -lt 50 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
    sleep 0.05; i=$((i + 1))
  done
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}
stop_gateway() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  GW_PID=""
  rm -rf "$HOME_DIR"
  mkdir -p "$HOME_DIR"
}

install_request() { # install_request <model_id> <version> [client_request_id]
  if [ -n "${3:-}" ]; then
    curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/install" \
      -d "{\"model_id\":\"$1\",\"version\":\"$2\",\"client_request_id\":\"$3\"}"
  else
    curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/install" \
      -d "{\"model_id\":\"$1\",\"version\":\"$2\"}"
  fi
}

wait_terminal() { # wait_terminal <job_id> -> prints final status JSON
  job="$1"
  j=0
  while [ "$j" -lt 100 ]; do
    status_json=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/installs/$job")
    state=$(printf '%s' "$status_json" | python3 -c "import json,sys; print(json.load(sys.stdin).get('state','?'))" 2>/dev/null || echo "?")
    case "$state" in installed|failed|canceled) break ;; esac
    sleep 0.1; j=$((j + 1))
  done
  printf '%s' "$status_json"
}

job_field() { printf '%s' "$1" | python3 -c "import json,sys; print(json.load(sys.stdin).get('$2'))"; } # job_field <json> <key>

# --- 1. Auth: no token on any new route ---
start_server normal "$TMP/artifact.bin"
start_gateway
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/models/install" -d '{"model_id":"testmodel","version":"v1"}')
[ "$STATUS" = "401" ] || { echo "FAIL: install with no token should be 401, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/models/installs")
[ "$STATUS" = "401" ] || { echo "FAIL: installs list with no token should be 401, got $STATUS"; exit 1; }

# --- 2. Normal full install: completes, verified, activated at install_path with 0600 ---
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
STATE=$(job_field "$FINAL" state)
[ "$STATE" = "installed" ] || { echo "FAIL: normal install expected installed, got $STATE"; echo "$FINAL"; exit 1; }
COMPLETED=$(job_field "$FINAL" completed_bytes)
TOTAL=$(job_field "$FINAL" total_bytes)
[ "$COMPLETED" = "$TOTAL" ] || { echo "FAIL: completed_bytes ($COMPLETED) != total_bytes ($TOTAL)"; exit 1; }
[ -f "$HOME_DIR/test/artifact.bin" ] || { echo "FAIL: artifact was not activated at install_path"; exit 1; }
ACTUAL_SHA=$(sha256_file "$HOME_DIR/test/artifact.bin")
[ "$ACTUAL_SHA" = "$FIXTURE_SHA" ] || { echo "FAIL: activated file hash mismatch"; exit 1; }
PERMS=$(stat -f "%Lp" "$HOME_DIR/test/artifact.bin" 2>/dev/null || stat -c "%a" "$HOME_DIR/test/artifact.bin")
[ "$PERMS" = "600" ] || { echo "FAIL: expected mode 600 on activated artifact, got $PERMS"; exit 1; }

stop_gateway
stop_server
echo "normal install + activation + permissions + hash: PASS"

# --- 3. Duplicate install request for the same model/version, made while
#        the first is still nonterminal, dedups to the same job instead of
#        starting a second transfer; a repeated client_request_id dedups
#        the same way. ---
start_server normal "$TMP/artifact.bin"
start_gateway SAMOSA_TEST_LIMIT_RATE=200K
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
sleep 0.3
DUP=$(install_request testmodel v1)
JOB_ID_DUP=$(job_field "$DUP" job_id)
[ "$JOB_ID_DUP" = "$JOB_ID" ] || { echo "FAIL: duplicate model/version install should return the existing nonterminal job, got a different id"; echo "$RESP"; echo "$DUP"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/install" -d '{"model_id":"testmodel","version":"v1"}')
[ "$STATUS" = "202" ] || { echo "FAIL: dedup response should still be 202, got $STATUS"; exit 1; }
curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/cancel" >/dev/null
stop_gateway
stop_server
echo "duplicate model/version install dedups to the existing nonterminal job: PASS"

# client_request_id dedup, tested in isolation from the model/version dedup
# above (both paths independently return an existing nonterminal job, but
# firing them in the same window would make it ambiguous which path a
# given dedup actually exercised).
start_server normal "$TMP/artifact.bin"
start_gateway SAMOSA_TEST_LIMIT_RATE=200K
CRID=$(install_request testmodel v1 same-client-request-id)
CRID_JOB=$(job_field "$CRID" job_id)
sleep 0.3
CRID2=$(install_request testmodel v1 same-client-request-id)
CRID2_JOB=$(job_field "$CRID2" job_id)
[ "$CRID2_JOB" = "$CRID_JOB" ] || { echo "FAIL: same client_request_id should return the same job"; echo "$CRID"; echo "$CRID2"; exit 1; }
curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$CRID_JOB/cancel" >/dev/null
stop_gateway
stop_server
echo "repeated client_request_id dedups to the existing job: PASS"

# --- 4. Truncate: fails, never activates ---
start_server truncate "$TMP/artifact.bin"
start_gateway
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
[ "$(job_field "$FINAL" state)" = "failed" ] || { echo "FAIL: truncate mode should fail"; echo "$FINAL"; exit 1; }
[ ! -f "$HOME_DIR/test/artifact.bin" ] || { echo "FAIL: truncate mode must never activate"; exit 1; }
stop_gateway
stop_server
echo "truncate -> failed, not activated: PASS"

# --- 5. Corrupt: checksum_mismatch, never activates ---
start_server corrupt "$TMP/artifact.bin"
start_gateway
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
[ "$(job_field "$FINAL" state)" = "failed" ] || { echo "FAIL: corrupt mode should fail"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code":"checksum_mismatch"' || { echo "FAIL: expected checksum_mismatch"; echo "$FINAL"; exit 1; }
[ ! -f "$HOME_DIR/test/artifact.bin" ] || { echo "FAIL: corrupt mode must never activate"; exit 1; }
stop_gateway
stop_server
echo "corrupt -> checksum_mismatch, not activated: PASS"

# --- 6. Redirect to an untrusted (non-https) target: rejected, never activates ---
start_server redirect "$TMP/artifact.bin"
start_gateway
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
[ "$(job_field "$FINAL" state)" = "failed" ] || { echo "FAIL: redirect-to-untrusted-target should fail"; echo "$FINAL"; exit 1; }
[ ! -f "$HOME_DIR/test/artifact.bin" ] || { echo "FAIL: redirect mode must never activate"; exit 1; }
stop_gateway
stop_server
echo "redirect to untrusted target -> rejected, not activated: PASS"

# --- 7. Oversize: declared bytes smaller than what the server actually sent -> size_mismatch ---
python3 -c "
data = bytes((i * 11 + 3) % 256 for i in range(3500000))
open('$TMP/bigger.bin', 'wb').write(data)
"
start_server oversize "$TMP/bigger.bin"
start_gateway
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
[ "$(job_field "$FINAL" state)" = "failed" ] || { echo "FAIL: oversize mode should fail"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code":"size_mismatch"' || { echo "FAIL: expected size_mismatch"; echo "$FINAL"; exit 1; }
stop_gateway
stop_server
echo "oversize -> size_mismatch: PASS"

# --- 7b. ignore_range (server responds 200 to a Range request instead of
#         206) and bad_content_range (a 206 with a Content-Range header that
#         doesn't match what was actually sent) are server quirks curl
#         itself absorbs, not attacks -- both must still result in a
#         correct, fully-verified install rather than a silently corrupt
#         one, since this downloader's own size+hash check is the actual
#         backstop regardless of what curl or the server believed
#         mid-transfer about ranges. ---
for MODE in ignore_range bad_content_range; do
  start_server "$MODE" "$TMP/artifact.bin"
  start_gateway
  RESP=$(install_request testmodel v1)
  JOB_ID=$(job_field "$RESP" job_id)
  FINAL=$(wait_terminal "$JOB_ID")
  [ "$(job_field "$FINAL" state)" = "installed" ] || { echo "FAIL: $MODE should still result in a correct install, got $(job_field "$FINAL" state)"; echo "$FINAL"; exit 1; }
  ACTUAL_SHA=$(sha256_file "$HOME_DIR/test/artifact.bin")
  [ "$ACTUAL_SHA" = "$FIXTURE_SHA" ] || { echo "FAIL: $MODE activated a file with the wrong hash"; exit 1; }
  stop_gateway
  stop_server
  echo "$MODE -> still installs correctly (verified by hash, not by trusting the header): PASS"
done

# --- 7c. A working installed version survives a failed re-install attempt:
#         the already-published artifact is never touched by a later job
#         that fails, since activation only happens after that job's own
#         full verification succeeds. ---
start_server normal "$TMP/artifact.bin"
start_gateway
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
wait_terminal "$JOB_ID" >/dev/null
[ -f "$HOME_DIR/test/artifact.bin" ] || { echo "FAIL: setup for 7c did not install the first copy"; exit 1; }
ORIGINAL_SHA=$(sha256_file "$HOME_DIR/test/artifact.bin")
ORIGINAL_MTIME=$(stat -f "%m" "$HOME_DIR/test/artifact.bin" 2>/dev/null || stat -c "%Y" "$HOME_DIR/test/artifact.bin")
stop_server
start_server corrupt "$TMP/artifact.bin"
RESP2=$(install_request testmodel v1)
JOB_ID2=$(job_field "$RESP2" job_id)
[ "$JOB_ID2" != "$JOB_ID" ] || { echo "FAIL: the first job is already terminal (installed), a fresh request must get a new job"; exit 1; }
FINAL2=$(wait_terminal "$JOB_ID2")
[ "$(job_field "$FINAL2" state)" = "failed" ] || { echo "FAIL: the corrupt re-install attempt should fail, got $(job_field "$FINAL2" state)"; exit 1; }
SURVIVING_SHA=$(sha256_file "$HOME_DIR/test/artifact.bin")
SURVIVING_MTIME=$(stat -f "%m" "$HOME_DIR/test/artifact.bin" 2>/dev/null || stat -c "%Y" "$HOME_DIR/test/artifact.bin")
[ "$SURVIVING_SHA" = "$ORIGINAL_SHA" ] || { echo "FAIL: the working installed file was corrupted by a failed re-install"; exit 1; }
[ "$SURVIVING_MTIME" = "$ORIGINAL_MTIME" ] || { echo "FAIL: the working installed file was rewritten by a failed re-install (mtime changed)"; exit 1; }
stop_gateway
stop_server
echo "a working installed version survives a failed re-install attempt: PASS"

# --- 8. Preflight ENOSPC: an artifact declaring far more bytes than any real
#        free disk space fails immediately, before any curl invocation ---
start_server normal "$TMP/artifact.bin"
start_gateway
RESP=$(install_request hugefixture v1)
JOB_ID=$(job_field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
[ "$(job_field "$FINAL" state)" = "failed" ] || { echo "FAIL: huge fixture should fail preflight"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code":"insufficient_space"' || { echo "FAIL: expected insufficient_space"; echo "$FINAL"; exit 1; }
ERROR_MSG=$(printf '%s' "$FINAL" | python3 -c "import json,sys; print(json.load(sys.stdin)['error']['message'])")
printf '%s' "$ERROR_MSG" | grep -qE '[0-9]+ bytes.*[0-9]+ available' || { echo "FAIL: insufficient_space error should report required vs available bytes, got: $ERROR_MSG"; exit 1; }
stop_gateway
stop_server
echo "preflight insufficient_space (reports required vs available bytes): PASS"

# --- 9. Pause mid-transfer (rate-limited so there is a real window), resume,
#        completes with the correct final bytes; second distinct model can
#        install while the first sits paused (paused does not hold the slot) ---
start_server normal "$TMP/artifact.bin"
start_gateway SAMOSA_TEST_LIMIT_RATE=200K
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
sleep 1
MID=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID")
[ "$(job_field "$MID" state)" = "downloading" ] || { echo "FAIL: expected downloading mid-transfer, got $(job_field "$MID" state)"; exit 1; }
MID_COMPLETED=$(job_field "$MID" completed_bytes)
[ "$MID_COMPLETED" -gt 0 ] || { echo "FAIL: expected nonzero mid-transfer progress"; exit 1; }
[ "$MID_COMPLETED" -lt "$FIXTURE_SIZE" ] || { echo "FAIL: expected incomplete mid-transfer progress"; exit 1; }

PAUSED=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/pause")
[ "$(job_field "$PAUSED" state)" = "paused" ] || { echo "FAIL: expected paused, got $(job_field "$PAUSED" state)"; echo "$PAUSED"; exit 1; }
PAUSED_COMPLETED=$(job_field "$PAUSED" completed_bytes)
[ "$PAUSED_COMPLETED" -gt 0 ] || { echo "FAIL: pause lost all progress"; exit 1; }

# Idempotent: pausing an already-paused job just returns it, no error.
PAUSED_AGAIN=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/pause")
[ "$(job_field "$PAUSED_AGAIN" state)" = "paused" ] || { echo "FAIL: idempotent pause should stay paused"; exit 1; }

# A different model can install now -- the paused job doesn't hold the slot.
BUSY_CHECK=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/install" -d '{"model_id":"hugefixture","version":"v1"}')
[ "$BUSY_CHECK" != "503" ] || { echo "FAIL: a different model should be installable while another sits paused"; exit 1; }

RESUMED=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/resume")
[ "$(job_field "$RESUMED" state)" = "queued" ] || { echo "FAIL: expected queued immediately after resume, got $(job_field "$RESUMED" state)"; exit 1; }
FINAL=$(wait_terminal "$JOB_ID")
[ "$(job_field "$FINAL" state)" = "installed" ] || { echo "FAIL: resumed job should complete to installed, got $(job_field "$FINAL" state)"; echo "$FINAL"; exit 1; }
[ "$(job_field "$FINAL" completed_bytes)" = "$FIXTURE_SIZE" ] || { echo "FAIL: resumed completed_bytes should equal the full fixture size"; exit 1; }
stop_gateway
stop_server
echo "pause mid-transfer + idempotent pause + slot freed for a different model + resume to installed: PASS"

# --- 10. Cancel mid-transfer, then retry reuses partial bytes and completes ---
start_server normal "$TMP/artifact.bin"
start_gateway SAMOSA_TEST_LIMIT_RATE=200K
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
sleep 1
CANCELED=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/cancel")
[ "$(job_field "$CANCELED" state)" = "canceled" ] || { echo "FAIL: expected canceled, got $(job_field "$CANCELED" state)"; echo "$CANCELED"; exit 1; }
CANCELED_AGAIN=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/cancel")
[ "$(job_field "$CANCELED_AGAIN" state)" = "canceled" ] || { echo "FAIL: idempotent cancel should stay canceled"; exit 1; }

RETRY=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/retry")
NEW_JOB_ID=$(job_field "$RETRY" job_id)
[ -n "$NEW_JOB_ID" ] && [ "$NEW_JOB_ID" != "None" ] || { echo "FAIL: retry did not return a new job_id"; echo "$RETRY"; exit 1; }
[ "$NEW_JOB_ID" != "$JOB_ID" ] || { echo "FAIL: retry must create a NEW job id, not reuse the old one"; exit 1; }
REUSED=$(job_field "$RETRY" reused_bytes)
[ "$REUSED" -gt 0 ] || { echo "FAIL: retry should report nonzero reused_bytes after a mid-transfer cancel"; exit 1; }
FINAL=$(wait_terminal "$NEW_JOB_ID")
[ "$(job_field "$FINAL" state)" = "installed" ] || { echo "FAIL: retried job should complete to installed, got $(job_field "$FINAL" state)"; echo "$FINAL"; exit 1; }
stop_gateway
stop_server
echo "cancel mid-transfer + idempotent cancel + retry reuses bytes + completes: PASS"

# --- 11. A second distinct-model install while one is actively transferring
#         is rejected (single active transfer, no FIFO queue in this pass) ---
start_server normal "$TMP/artifact.bin"
start_gateway SAMOSA_TEST_LIMIT_RATE=200K
RESP=$(install_request testmodel v1)
JOB_ID=$(job_field "$RESP" job_id)
sleep 0.3
STATUS=$(curl -sS -o "$TMP/busy.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/install" -d '{"model_id":"hugefixture","version":"v1"}')
[ "$STATUS" = "503" ] || { echo "FAIL: expected 503 install_busy for a second concurrent distinct model, got $STATUS"; cat "$TMP/busy.json"; exit 1; }
grep -q '"code":"install_busy"' "$TMP/busy.json" || { echo "FAIL: expected install_busy code"; cat "$TMP/busy.json"; exit 1; }
curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/cancel" >/dev/null
stop_gateway
stop_server
echo "concurrent distinct-model install rejected (503 install_busy): PASS"

echo "test_model_install.sh: PASS"
