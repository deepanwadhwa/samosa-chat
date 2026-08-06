#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T2.4 (docs/TASKS_UI_CHUTNI.md section 5.1/4): setup/status's next_step now
# resolves against real T2.1 catalog state and T2.2/T2.3 install/selection
# job state (setup_status_resolve() in src/samosa_gateway.c) instead of the
# old T1.2/T1.4 interim bridges. Exercises: persistence of selected_model_id/
# version at install-start and at backend-select time, the "download" step
# for a nonterminal or recoverable-failed install job, the non-recoverable
# artifact_not_downloadable exception, legacy-install adoption (an install
# already on disk before any onboarding ran), the real
# verified-but-not-yet-active transition, and install-job repair across a
# gateway restart. Runs against the real compiled gateway,
# tests/fake_model_download_server.c (T0.1), and tests/fake_openai_backend.c
# (T2.3) -- no real network artifact, no real multi-gigabyte model.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
SERVER="${SAMOSA_FAKE_DOWNLOAD_SERVER:-./$BUILD_DIR/fake_model_download_server}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/setup_flow_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18996
# Deliberately not PORT+1: that's SAMOSA_BACKEND_PORT, which a real backend
# process (the fake bonsai server started during backend selection below)
# binds to. Colliding here caused a real, reproduced EADDRINUSE failure.
SERVER_PORT=19010
GW_PID=""
SERVER_PID=""

cleanup() {
  chmod 0700 "$HOME_DIR" 2>/dev/null || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  [ -z "$SERVER_PID" ] || kill "$SERVER_PID" 2>/dev/null || true
  [ -z "$SERVER_PID" ] || wait "$SERVER_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

# Real fixture bytes + independently-computed SHA-256 for the "bonsai" catalog
# entry -- this is the one model_id in this test that resolve_installed_artifact()
# actually knows how to find (via SAMOSA_BONSAI_MODEL), so it is the only id
# that can ever report "verified" through the real catalog path.
python3 -c "
import hashlib
data = bytes((i * 13 + 7) % 256 for i in range(2000000))
open('$TMP/bonsai.bin', 'wb').write(data)
open('$TMP/bonsai.meta', 'w').write(hashlib.sha256(data).hexdigest() + ' ' + str(len(data)) + '\n')
"
read BONSAI_SHA BONSAI_SIZE < "$TMP/bonsai.meta"

# An arbitrary, non-backend model_id -- fine for exercising install-job
# state transitions (queued/downloading/failed), but artifact_is_present()
# can never report it "verified" (resolve_installed_artifact only knows
# qwen/bonsai/ornith), which is by design and not under test here.
python3 -c "
import hashlib
data = bytes((i * 41 + 3) % 256 for i in range(3000000))
open('$TMP/testmodel.bin', 'wb').write(data)
open('$TMP/testmodel.meta', 'w').write(hashlib.sha256(data).hexdigest() + ' ' + str(len(data)) + '\n')
"
read TESTMODEL_SHA TESTMODEL_SIZE < "$TMP/testmodel.meta"

cat > "$TMP/catalog.json" <<EOF
{
  "schema_version": 1, "catalog_revision": "test", "runtime_abi": "samosa-model-runtime-v1",
  "models": [
    {
      "id": "bonsai", "version": "test-v1", "preferred_for_backend": true, "label": "Test Bonsai",
      "description": "test", "capabilities": ["text"], "backend_kind": "llama_cpp",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1", "minimum_ram_bytes": 0,
      "launch_profile_id": "x", "runtime_dependencies": [],
      "license": { "name": "x", "url": "https://huggingface.co/x" },
      "artifacts": [
        { "name": "Bonsai-27B-Q1_0.gguf", "role": "weights", "required": true,
          "url": "http://127.0.0.1:$SERVER_PORT/artifact", "install_path": "models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf",
          "file_mode": "0600", "bytes": $BONSAI_SIZE, "sha256": "$BONSAI_SHA" }
      ]
    },
    {
      "id": "testmodel", "version": "v1", "preferred_for_backend": false, "label": "Test",
      "description": "test", "capabilities": ["text"], "backend_kind": "qwen_native",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1", "minimum_ram_bytes": 0,
      "launch_profile_id": "x", "runtime_dependencies": [],
      "license": { "name": "x", "url": "https://huggingface.co/x" },
      "artifacts": [
        { "name": "testmodel.bin", "role": "weights", "required": true,
          "url": "http://127.0.0.1:$SERVER_PORT/artifact", "install_path": "test/testmodel.bin",
          "file_mode": "0600", "bytes": $TESTMODEL_SIZE, "sha256": "$TESTMODEL_SHA" }
      ]
    },
    {
      "id": "brokenmodel", "version": "v1", "preferred_for_backend": false, "label": "Broken",
      "description": "an artifact with no download host yet, matching Qwen's own real state ahead of a public release host",
      "capabilities": ["text"], "backend_kind": "qwen_native",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1", "minimum_ram_bytes": 0,
      "launch_profile_id": "x", "runtime_dependencies": [],
      "license": { "name": "x", "url": "https://huggingface.co/x" },
      "artifacts": [
        { "name": "broken.bin", "role": "weights", "required": true,
          "url": "", "install_path": "test/broken.bin",
          "file_mode": "0600", "bytes": 100, "sha256": "$(printf '%064d' 0)" }
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
    SAMOSA_BONSAI_SERVER="$BACKEND" \
    SAMOSA_BONSAI_MODEL="$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf" \
    "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  GW_PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
    sleep 0.05; i=$((i + 1))
  done
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}
stop_gateway() {
  chmod 0700 "$HOME_DIR" 2>/dev/null || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  GW_PID=""
}
fresh_home() {
  rm -rf "$HOME_DIR"
  mkdir -p "$HOME_DIR"
}

field() { printf '%s' "$1" | python3 -c "import json,sys; v=json.load(sys.stdin).get('$2'); print('' if v is None else v)"; }
# GET /v1/profile nests onboarding fields; setup/status reports them flat.
profile_field() { printf '%s' "$1" | python3 -c "import json,sys; v=json.load(sys.stdin).get('onboarding',{}).get('$2'); print('' if v is None else v)"; }
setup_status() { curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/setup/status"; }
profile() { curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/profile"; }
complete_name_and_welcome() {
  curl -fsS -X PUT -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
    -d '{"name":"Tester"}' "http://127.0.0.1:$PORT/v1/profile" >/dev/null
  curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/setup/welcome/complete" >/dev/null
}
install_request() { # install_request <model_id> <version>
  curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/install" \
    -d "{\"model_id\":\"$1\",\"version\":\"$2\"}"
}
install_status() { curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/installs/$1"; }
wait_install_terminal() { # -> prints final status JSON
  job="$1"; j=0
  while [ "$j" -lt 300 ]; do
    status_json=$(install_status "$job")
    state=$(field "$status_json" state)
    case "$state" in installed|failed|canceled) break ;; esac
    sleep 0.1; j=$((j + 1))
  done
  printf '%s' "$status_json"
}
select_backend() { # select_backend <backend> [version]
  if [ -n "${2:-}" ]; then
    curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/backends/select" \
      -d "{\"backend\":\"$1\",\"model_version\":\"$2\"}"
  else
    curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/backends/select" -d "{\"backend\":\"$1\"}"
  fi
}
selection_status() { curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/selection/$1"; }
wait_selection_terminal() {
  job="$1"; j=0
  while [ "$j" -lt 100 ]; do
    status_json=$(selection_status "$job")
    state=$(field "$status_json" state)
    case "$state" in selected|failed) break ;; esac
    sleep 0.05; j=$((j + 1))
  done
  printf '%s' "$status_json"
}

# --- 1. No selection at all, nothing legacy-installed -> next_step "model" ---
fresh_home
start_server normal "$TMP/testmodel.bin"
start_gateway
complete_name_and_welcome
STATUS_JSON=$(setup_status)
[ "$(field "$STATUS_JSON" next_step)" = "model" ] || { echo "FAIL: expected next_step model with nothing selected"; echo "$STATUS_JSON"; exit 1; }
[ "$(field "$STATUS_JSON" selected_model_id)" = "" ] || { echo "FAIL: expected no selected_model_id yet"; echo "$STATUS_JSON"; exit 1; }
stop_gateway
echo "1. nothing selected, nothing legacy-installed -> next_step model: PASS"

# --- 2. Starting an install persists selected_model_id/version immediately,
#        and a nonterminal job forces next_step "download" ---
fresh_home
start_gateway SAMOSA_TEST_LIMIT_RATE=200K
complete_name_and_welcome
RESP=$(install_request testmodel v1)
JOB_ID=$(field "$RESP" job_id)
PROFILE_JSON=$(profile)
[ "$(profile_field "$PROFILE_JSON" selected_model_id)" = "testmodel" ] || { echo "FAIL: install should immediately persist selected_model_id"; echo "$PROFILE_JSON"; exit 1; }
[ "$(profile_field "$PROFILE_JSON" selected_model_version)" = "v1" ] || { echo "FAIL: install should immediately persist selected_model_version"; echo "$PROFILE_JSON"; exit 1; }
STATUS_JSON=$(setup_status)
[ "$(field "$STATUS_JSON" next_step)" = "download" ] || { echo "FAIL: expected next_step download mid-transfer"; echo "$STATUS_JSON"; exit 1; }
[ "$(field "$STATUS_JSON" active_install_job_id)" = "$JOB_ID" ] || { echo "FAIL: setup/status should report the live active_install_job_id"; echo "$STATUS_JSON"; exit 1; }
curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/cancel" >/dev/null
stop_gateway
stop_server
echo "2. install persists selection immediately; nonterminal job -> next_step download (with live job id): PASS"

# --- 3. A recoverable failure (checksum_mismatch) keeps next_step "download" ---
fresh_home
start_server corrupt "$TMP/testmodel.bin"
start_gateway
complete_name_and_welcome
RESP=$(install_request testmodel v1)
JOB_ID=$(field "$RESP" job_id)
FINAL=$(wait_install_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "failed" ] || { echo "FAIL: corrupt download should fail"; echo "$FINAL"; exit 1; }
STATUS_JSON=$(setup_status)
[ "$(field "$STATUS_JSON" next_step)" = "download" ] || { echo "FAIL: a recoverable failed job should keep next_step download (retry is offered)"; echo "$STATUS_JSON"; exit 1; }
stop_gateway
stop_server
echo "3. recoverable failed job (checksum_mismatch) -> next_step stays download: PASS"

# --- 4. A non-recoverable failure (artifact_not_downloadable, no URL at all)
#        does NOT hold next_step at "download" -- falls through to "model" ---
fresh_home
start_gateway
complete_name_and_welcome
RESP=$(install_request brokenmodel v1)
JOB_ID=$(field "$RESP" job_id)
FINAL=$(wait_install_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "failed" ] || { echo "FAIL: no-url artifact should fail"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code": *"artifact_not_downloadable"' || { echo "FAIL: expected artifact_not_downloadable"; echo "$FINAL"; exit 1; }
STATUS_JSON=$(setup_status)
[ "$(field "$STATUS_JSON" next_step)" = "model" ] || { echo "FAIL: artifact_not_downloadable is not recoverable by retry -- expected next_step model"; echo "$STATUS_JSON"; exit 1; }
stop_gateway
echo "4. artifact_not_downloadable is not treated as recoverable -> next_step model: PASS"

# --- 5. Legacy-install adoption: a bonsai install already on disk before any
#        onboarding ran is adopted as the implicit selection, using the real
#        catalog version string (not a filename basename), and reaches
#        next_step "chat" directly. ---
fresh_home
mkdir -p "$HOME_DIR/models/bonsai-27b-1bit"
cp "$TMP/bonsai.bin" "$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
start_gateway
complete_name_and_welcome
# backend_start() forks and returns immediately (see src/samosa_gateway.c) --
# the gateway's own /healthz can already answer before the forked bonsai
# process has bound its port, so poll setup/status rather than asserting on
# the very first response.
i=0
while [ "$i" -lt 100 ]; do
  STATUS_JSON=$(setup_status)
  [ "$(field "$STATUS_JSON" next_step)" = "chat" ] && break
  sleep 0.05; i=$((i + 1))
done
[ "$(field "$STATUS_JSON" next_step)" = "chat" ] || { echo "FAIL: a legacy-ready bonsai install should reach next_step chat"; echo "$STATUS_JSON"; exit 1; }
PROFILE_JSON=$(profile)
[ "$(profile_field "$PROFILE_JSON" selected_model_id)" = "bonsai" ] || { echo "FAIL: legacy install should be adopted as selected_model_id"; echo "$PROFILE_JSON"; exit 1; }
[ "$(profile_field "$PROFILE_JSON" selected_model_version)" = "test-v1" ] || { echo "FAIL: adopted version should be the real catalog version, not a filename basename"; echo "$PROFILE_JSON"; exit 1; }
stop_gateway
echo "5. legacy-ready install is adopted (real catalog version, not a filename) -> next_step chat: PASS"

# --- 6. A verified-but-not-yet-active model: full real install of "bonsai"
#        (through the fake download server, matching SAMOSA_BONSAI_MODEL's
#        real path) with the gateway never having switched to it -- next_step
#        must be "download" (activation still owed), then POST
#        /v1/backends/select (with an explicit model_version) both switches
#        AND persists the real catalog version, reaching next_step "chat". ---
fresh_home
start_server normal "$TMP/bonsai.bin"
start_gateway
complete_name_and_welcome
RESP=$(install_request bonsai test-v1)
JOB_ID=$(field "$RESP" job_id)
FINAL=$(wait_install_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "installed" ] || { echo "FAIL: bonsai install should complete"; echo "$FINAL"; exit 1; }
[ -f "$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf" ] || { echo "FAIL: bonsai artifact was not activated"; exit 1; }
STATUS_JSON=$(setup_status)
[ "$(field "$STATUS_JSON" next_step)" = "download" ] || { echo "FAIL: verified-but-not-active bonsai should keep next_step download"; echo "$STATUS_JSON"; exit 1; }
[ "$(field "$STATUS_JSON" active_model_id)" = "" ] || { echo "FAIL: no backend should be live/ready yet (qwen has no files)"; echo "$STATUS_JSON"; exit 1; }

SELECT_RESP=$(select_backend bonsai test-v1)
SEL_JOB=$(field "$SELECT_RESP" job_id)
SEL_FINAL=$(wait_selection_terminal "$SEL_JOB")
[ "$(field "$SEL_FINAL" state)" = "selected" ] || { echo "FAIL: bonsai selection should succeed"; echo "$SEL_FINAL"; exit 1; }
STATUS_JSON=$(setup_status)
[ "$(field "$STATUS_JSON" next_step)" = "chat" ] || { echo "FAIL: verified + active + ready bonsai should reach next_step chat"; echo "$STATUS_JSON"; exit 1; }
PROFILE_JSON=$(profile)
[ "$(profile_field "$PROFILE_JSON" selected_model_version)" = "test-v1" ] || { echo "FAIL: explicit model_version on backends/select should be persisted"; echo "$PROFILE_JSON"; exit 1; }
stop_gateway
stop_server
echo "6. real install (verified, not yet active) -> download; explicit backends/select -> chat, version persisted: PASS"

# --- 7. Gateway restart mid-transfer: a job stuck "downloading" from a dead
#        process is repaired to "paused" (not left claiming forever-frozen
#        progress), keeps its partial bytes, and can still be resumed. ---
fresh_home
start_server normal "$TMP/testmodel.bin"
start_gateway SAMOSA_TEST_LIMIT_RATE=100K
complete_name_and_welcome
RESP=$(install_request testmodel v1)
JOB_ID=$(field "$RESP" job_id)
sleep 1
MID=$(install_status "$JOB_ID")
[ "$(field "$MID" state)" = "downloading" ] || { echo "FAIL: expected downloading mid-transfer before the crash"; echo "$MID"; exit 1; }
MID_BYTES=$(field "$MID" completed_bytes)
[ "$MID_BYTES" -gt 0 ] || { echo "FAIL: expected nonzero progress before the crash"; exit 1; }

# Simulate a crash/restart: kill the gateway process, then start a fresh one
# against the SAME home directory (not wiped).
kill "$GW_PID" 2>/dev/null || true
wait "$GW_PID" 2>/dev/null || true
GW_PID=""
start_gateway SAMOSA_TEST_LIMIT_RATE=100K

AFTER_RESTART=$(install_status "$JOB_ID")
[ "$(field "$AFTER_RESTART" state)" = "paused" ] || { echo "FAIL: a job stuck downloading across a restart should be repaired to paused, got $(field "$AFTER_RESTART" state)"; echo "$AFTER_RESTART"; exit 1; }
AFTER_BYTES=$(field "$AFTER_RESTART" completed_bytes)
[ "$AFTER_BYTES" -ge "$MID_BYTES" ] || { echo "FAIL: restart repair should not lose already-downloaded bytes"; exit 1; }

RESUMED=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/models/installs/$JOB_ID/resume")
[ "$(field "$RESUMED" state)" = "queued" ] || { echo "FAIL: a restart-repaired paused job should still resume normally"; echo "$RESUMED"; exit 1; }
FINAL=$(wait_install_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "installed" ] || { echo "FAIL: resumed job after restart-repair should complete, got $(field "$FINAL" state)"; echo "$FINAL"; exit 1; }
stop_gateway
stop_server
echo "7. gateway restart mid-transfer repairs a stuck job to paused, preserves bytes, still resumable: PASS"

echo "test_setup_flow.sh: PASS"
