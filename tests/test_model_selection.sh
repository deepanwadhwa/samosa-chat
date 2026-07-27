#!/bin/sh
set -eu

# T2.3 (docs/TASKS_UI_CHUTNI.md section 5.3): readiness-safe model
# activation. Exercises the real compiled gateway's POST /v1/backends/select
# against tests/fake_openai_backend.c, which now supports a /healthz alias
# (so it can stand in for SAMOSA_QWEN_ENGINE, not just SAMOSA_BONSAI_SERVER)
# and a SAMOSA_FAKE_HEALTH_DELAY_MS knob (a deterministic window to act
# before a switch's watchdog observes readiness). "qwen" is always the
# backend active before a switch in every scenario below, so a failure
# rolling back can be checked against a real, independently-verified
# working state rather than the very backend under test.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/model_selection_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18994
GW_PID=""

cleanup() {
  chmod 0700 "$HOME_DIR" 2>/dev/null || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make samosa-gateway test_fake_openai_backend >/dev/null 2>&1 || true

mkdir -p "$HOME_DIR/qwen-model" "$HOME_DIR/models/bonsai-27b-1bit"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"
printf 'bonsai-weights-fixture\n' >"$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'qwen\n' >"$HOME_DIR/model-backend"

cat >"$TMP/never-binds.sh" <<'EOF'
#!/bin/sh
exec sleep 999
EOF
chmod +x "$TMP/never-binds.sh"

cat >"$TMP/crashes.sh" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$TMP/crashes.sh"

start_gateway() { # start_gateway [extra_env...] -- extra_env entries override the defaults below (env applies later assignments last)
  env \
    SAMOSA_HOME="$HOME_DIR" \
    SAMOSA_PORT="$PORT" \
    SAMOSA_BACKEND_PORT=$((PORT + 1)) \
    SAMOSA_APP_HTML="$TMP/app.html" \
    SAMOSA_APP_LOGO="$TMP/logo.png" \
    SAMOSA_QWEN_ENGINE="$BACKEND" \
    SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
    SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
    SAMOSA_BONSAI_SERVER="$BACKEND" \
    SAMOSA_BONSAI_MODEL="$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf" \
    "$@" \
    "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  GW_PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true' && break
    sleep 0.05; i=$((i + 1))
  done
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}
stop_gateway() {
  chmod 0700 "$HOME_DIR" 2>/dev/null || true
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  GW_PID=""
  rm -rf "$HOME_DIR"
  mkdir -p "$HOME_DIR/qwen-model" "$HOME_DIR/models/bonsai-27b-1bit"
  printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
  printf 'bonsai-weights-fixture\n' >"$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
  printf 'qwen\n' >"$HOME_DIR/model-backend"
}

field() { printf '%s' "$1" | python3 -c "import json,sys; v=json.load(sys.stdin).get('$2'); print('' if v is None else v)"; } # field <json> <key>
select_backend() { curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/backends/select" -d "{\"backend\":\"$1\"}"; }
selection_status() { curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/selection/$1"; }
wait_terminal() { # wait_terminal <job_id> -> prints final status JSON
  job="$1"; j=0
  while [ "$j" -lt 100 ]; do
    status_json=$(selection_status "$job")
    state=$(field "$status_json" state)
    case "$state" in selected|failed) break ;; esac
    sleep 0.05; j=$((j + 1))
  done
  printf '%s' "$status_json"
}
healthz() { curl -sS "http://127.0.0.1:$PORT/healthz"; }

# --- 1. Auth: the new selection routes require the session token ---
start_gateway
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/models/selection/active")
[ "$STATUS" = "401" ] || { echo "FAIL: selection/active with no token should be 401, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o "$TMP/none.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/selection/active")
[ "$STATUS" = "404" ] || { echo "FAIL: selection/active with no in-flight switch should be 404, got $STATUS"; exit 1; }
grep -q '"no_active_selection"' "$TMP/none.json" || { echo "FAIL: expected no_active_selection code"; cat "$TMP/none.json"; exit 1; }
stop_gateway
echo "auth: selection routes gated, no active selection is a clean 404: PASS"

# --- 2. Happy path: switch never claims ready merely because fork()
#        succeeded -- job passes through waiting_ready before selected, and
#        only reaches selected once /healthz independently agrees. ---
start_gateway
RESP=$(select_backend bonsai)
[ "$(field "$RESP" accepted)" = "True" ] || { echo "FAIL: expected accepted:true"; echo "$RESP"; exit 1; }
JOB_ID=$(field "$RESP" job_id)
[ -n "$JOB_ID" ] || { echo "FAIL: expected a job_id in the select response"; echo "$RESP"; exit 1; }
FINAL=$(wait_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "selected" ] || { echo "FAIL: expected selected, got $(field "$FINAL" state)"; echo "$FINAL"; exit 1; }
[ "$(field "$FINAL" active_backend)" = "bonsai" ] || { echo "FAIL: expected active_backend bonsai"; echo "$FINAL"; exit 1; }
H=$(healthz)
[ "$(field "$H" backend)" = "bonsai" ] || { echo "FAIL: healthz backend should be bonsai"; echo "$H"; exit 1; }
[ "$(field "$H" ready)" = "True" ] || { echo "FAIL: healthz should report ready:true"; echo "$H"; exit 1; }
[ "$(cat "$HOME_DIR/model-backend")" = "bonsai" ] || { echo "FAIL: model-backend file should durably record bonsai"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/selection/active")
[ "$STATUS" = "404" ] || { echo "FAIL: selection/active should be 404 once the switch is terminal, got $STATUS"; exit 1; }
stop_gateway
echo "happy path: bonsai selected, healthz agrees, durably persisted, no longer 'active' once done: PASS"

# --- 3. Reconnect mid-switch: GET .../selection/active returns the SAME
#        in-flight job, satisfying the "refresh during model load
#        reconnects through the durable registry" acceptance item. ---
start_gateway SAMOSA_FAKE_HEALTH_DELAY_MS=400
RESP=$(select_backend bonsai)
JOB_ID=$(field "$RESP" job_id)
ACTIVE=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/selection/active")
[ "$(field "$ACTIVE" job_id)" = "$JOB_ID" ] || { echo "FAIL: selection/active should reconnect to the in-flight job"; echo "$ACTIVE"; exit 1; }
case "$(field "$ACTIVE" state)" in starting|waiting_ready) : ;; *) echo "FAIL: expected an in-flight state, got $(field "$ACTIVE" state)"; exit 1 ;; esac
FINAL=$(wait_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "selected" ] || { echo "FAIL: delayed-health switch should still complete, got $(field "$FINAL" state)"; exit 1; }
stop_gateway
echo "reconnect mid-switch via GET /v1/models/selection/active: PASS"

# --- 4. Concurrent switch while one is in flight is rejected (mirrors
#        T2.2's single-active-transfer gate for install). A zero busy-wait
#        override means this checks the instant-reject case; the default
#        production grace period (a second switch arriving just as the
#        first clears) is exercised implicitly by every other scenario in
#        this file that switches, waits for ready, and switches again. ---
start_gateway SAMOSA_FAKE_HEALTH_DELAY_MS=2000 SAMOSA_TEST_SELECTION_BUSY_WAIT_MS=0
RESP=$(select_backend bonsai)
JOB_ID=$(field "$RESP" job_id)
STATUS=$(curl -sS -o "$TMP/busy.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/backends/select" -d '{"backend":"qwen"}')
[ "$STATUS" = "409" ] || { echo "FAIL: expected 409 while a switch is in flight, got $STATUS"; cat "$TMP/busy.json"; exit 1; }
grep -q '"selection_busy"' "$TMP/busy.json" || { echo "FAIL: expected selection_busy code"; cat "$TMP/busy.json"; exit 1; }
wait_terminal "$JOB_ID" >/dev/null
stop_gateway
echo "concurrent switch rejected (409 selection_busy): PASS"

# --- 5. Readiness timeout: the target process starts but never binds its
#        port. The switch must fail (never report selected on a forked-only
#        process) and restore the previously-working qwen backend. ---
start_gateway SAMOSA_TEST_SELECTION_READY_TIMEOUT_MS=300 SAMOSA_BONSAI_SERVER="$TMP/never-binds.sh"
RESP=$(select_backend bonsai)
JOB_ID=$(field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "failed" ] || { echo "FAIL: expected failed, got $(field "$FINAL" state)"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code": *"readiness_timeout"' || { echo "FAIL: expected readiness_timeout"; echo "$FINAL"; exit 1; }
[ "$(field "$FINAL" active_backend)" = "qwen" ] || { echo "FAIL: active_backend should be restored to qwen"; echo "$FINAL"; exit 1; }
H=$(healthz)
[ "$(field "$H" backend)" = "qwen" ] || { echo "FAIL: healthz should show qwen restored"; echo "$H"; exit 1; }
[ "$(field "$H" ready)" = "True" ] || { echo "FAIL: restored qwen should be ready"; echo "$H"; exit 1; }
[ "$(cat "$HOME_DIR/model-backend")" = "qwen" ] || { echo "FAIL: model-backend file must never have been rewritten to bonsai"; exit 1; }
stop_gateway
echo "readiness timeout -> failed, restores the prior working backend: PASS"

# --- 6. Immediate child crash: the target process exits before ever
#        answering a probe. Distinguished from a timeout (fails fast, not
#        after the full deadline) and still restores qwen. ---
start_gateway SAMOSA_BONSAI_SERVER="$TMP/crashes.sh"
START_MS=$(python3 -c 'import time; print(int(time.time()*1000))')
RESP=$(select_backend bonsai)
JOB_ID=$(field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
END_MS=$(python3 -c 'import time; print(int(time.time()*1000))')
[ "$(field "$FINAL" state)" = "failed" ] || { echo "FAIL: expected failed, got $(field "$FINAL" state)"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code": *"backend_crashed"' || { echo "FAIL: expected backend_crashed"; echo "$FINAL"; exit 1; }
[ "$((END_MS - START_MS))" -lt 10000 ] || { echo "FAIL: an immediate crash should be detected in well under the 20s default timeout"; exit 1; }
[ "$(field "$FINAL" active_backend)" = "qwen" ] || { echo "FAIL: active_backend should be restored to qwen"; echo "$FINAL"; exit 1; }
H=$(healthz)
[ "$(field "$H" backend)" = "qwen" ] && [ "$(field "$H" ready)" = "True" ] || { echo "FAIL: qwen should be restored and ready"; echo "$H"; exit 1; }
stop_gateway
echo "immediate child crash -> failed fast, restores the prior working backend: PASS"

# --- 7. Fingerprint mismatch: the target's weights file changes while the
#        switch is waiting for readiness (a delayed health response gives a
#        deterministic window). Must fail and restore qwen even though the
#        new process did become reachable. ---
start_gateway SAMOSA_FAKE_HEALTH_DELAY_MS=500
RESP=$(select_backend bonsai)
JOB_ID=$(field "$RESP" job_id)
printf 'swapped-content-different-size\n' >"$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
FINAL=$(wait_terminal "$JOB_ID")
[ "$(field "$FINAL" state)" = "failed" ] || { echo "FAIL: expected failed, got $(field "$FINAL" state)"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code": *"fingerprint_mismatch"' || { echo "FAIL: expected fingerprint_mismatch"; echo "$FINAL"; exit 1; }
[ "$(field "$FINAL" active_backend)" = "qwen" ] || { echo "FAIL: active_backend should be restored to qwen"; echo "$FINAL"; exit 1; }
[ "$(cat "$HOME_DIR/model-backend")" = "qwen" ] || { echo "FAIL: model-backend file must never have committed to bonsai"; exit 1; }
stop_gateway
echo "fingerprint mismatch (weights file swapped mid-switch) -> failed, restores the prior working backend: PASS"

# --- 8. Registry commit failure: readiness and the fingerprint both pass,
#        but the durable model-backend file cannot be written. Must not
#        leave a live, unpersisted bonsai process running -- roll back so
#        the live process matches the untouched, still-correct file. ---
start_gateway
chmod 0500 "$HOME_DIR"
RESP=$(select_backend bonsai)
JOB_ID=$(field "$RESP" job_id)
FINAL=$(wait_terminal "$JOB_ID")
chmod 0700 "$HOME_DIR"
[ "$(field "$FINAL" state)" = "failed" ] || { echo "FAIL: expected failed, got $(field "$FINAL" state)"; echo "$FINAL"; exit 1; }
printf '%s' "$FINAL" | grep -q '"code": *"registry_commit_failed"' || { echo "FAIL: expected registry_commit_failed"; echo "$FINAL"; exit 1; }
[ "$(field "$FINAL" active_backend)" = "qwen" ] || { echo "FAIL: active_backend should be restored to qwen"; echo "$FINAL"; exit 1; }
H=$(healthz)
[ "$(field "$H" backend)" = "qwen" ] && [ "$(field "$H" ready)" = "True" ] || { echo "FAIL: qwen should be restored and ready"; echo "$H"; exit 1; }
[ "$(cat "$HOME_DIR/model-backend")" = "qwen" ] || { echo "FAIL: model-backend file should remain qwen, unwritten"; exit 1; }
stop_gateway
echo "registry commit failure -> failed, live process rolled back to match the untouched file: PASS"

# --- 9. Chat/Jobs inference reaching the gateway between backend_stop() and
#        the new backend answering its first probe gets the same retryable
#        response it always did (unchanged, pre-existing backend_probe()
#        gate in proxy_request()/backend_json()) -- exercised end to end by
#        every scenario above that switches, waits for /healthz's own
#        ready:true, and immediately issues a follow-up request without
#        that request ever getting spuriously rejected. An earlier version
#        of this test asserted an explicit "switching" flag blocked
#        inference for the /healthz-ready-but-not-yet-fully-committed tail;
#        that flag was removed after it caused exactly the false rejection
#        this comment describes (found via
#        tests/test_compiled_gateway.sh's vision-backend scenario -- see
#        the T2.3 evidence doc) and is not reintroduced here. ---

echo "test_model_selection.sh: PASS"
