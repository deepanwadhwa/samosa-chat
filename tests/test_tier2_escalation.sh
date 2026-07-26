#!/bin/sh
set -eu

TMP=$(mktemp -d "${TMPDIR:-/tmp}/tier2_escalation_test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PORT_BE=18143
PORT_GW=18144

BUILD_DIR="${BUILD_DIR:-build}"

# Compile test fake backend if not built
make test_fake_openai_backend >/dev/null 2>&1 || true

# Start fake backend
"${BUILD_DIR}/test_fake_openai_backend" --port "$PORT_BE" &
BE_PID=$!
trap 'kill "$BE_PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT

# Wait for fake backend health
i=0
while [ "$i" -lt 50 ]; do
  if /usr/bin/curl -fsS "http://127.0.0.1:$PORT_BE/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
  i=$((i + 1))
done

# Prepare test files and mock fixtures
REAL_HOME="$HOME"
PACK="${SAMOSA_OCR_PACK:-$REAL_HOME/.samosa/models/ocr-pack-v1}"

mkdir -p "$TMP/files" "$TMP/home/models/ornith-9b" "$TMP/home/models/bonsai-27b-1bit" "$TMP/home/.samosa/cache/read"
printf 'fixture\n' >"$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'fixture\n' >"$TMP/home/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'mmproj-fixture\n' >"$TMP/home/bonsai-mmproj.gguf"
printf 'ornith\n' >"$TMP/home/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

# Copy tiny image fixture
cp tools/testdata/ocr/tiny.png "$TMP/files/cat-medical-note.png"

export HOME="$TMP/home"
export SAMOSA_HOME="$TMP/home"
export SAMOSA_READ_CACHE_DIR="$TMP/home/.samosa/cache/read"
export SAMOSA_OCR_PACK="$PACK"

# Start gateway with Ornith (text-only) active
SAMOSA_HOME="$TMP/home" \
SAMOSA_READ_CACHE_DIR="$TMP/home/.samosa/cache/read" \
SAMOSA_OCR_PACK="$PACK" \
SAMOSA_JOBS_ROOT="$TMP/jobs" \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_ORNITH_MODEL="$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_BONSAI_MMPROJ="$TMP/home/bonsai-mmproj.gguf" \
SAMOSA_OCR="$(pwd)/${BUILD_DIR}/samosa-ocr" \
SAMOSA_FS="$(pwd)/${BUILD_DIR}/samosa-fs" \
SAMOSA_EXTRACT="$(pwd)/${BUILD_DIR}/samosa-extract" \
SAMOSA_BONSAI_SERVER="$(pwd)/${BUILD_DIR}/test_fake_openai_backend" \
SAMOSA_BACKEND_PORT="$PORT_BE" \
SAMOSA_PORT="$PORT_GW" \
"${BUILD_DIR}/samosa-gateway" >"$TMP/gateway.log" 2>&1 &
GW_PID=$!
trap 'kill "$GW_PID" "$BE_PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT

# Wait for gateway healthz
i=0
while [ "$i" -lt 100 ]; do
  health=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT_GW/healthz" 2>/dev/null || true)
  if printf '%s' "$health" | /usr/bin/grep -q '"ready":true'; then
    break
  fi
  sleep 0.05
  i=$((i + 1))
done

# 1. Test Ornith (text-only backend): high-confidence printed scan reads cleanly
res1=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res1" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "Ornith run missing doc.read tool call" >&2; exit 1; }

# 2. Test switching backend to Bonsai (supports vision)
printf 'bonsai\n' >"$TMP/home/model-backend"

# Trigger a refresh doc.read call
res2=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res2" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "Bonsai run missing doc.read tool call" >&2; exit 1; }

kill "$GW_PID" "$BE_PID" 2>/dev/null || true
echo "tier2-escalation-test: PASS"
