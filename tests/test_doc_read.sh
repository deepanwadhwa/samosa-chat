#!/bin/sh
set -eu

TMP=$(mktemp -d "${TMPDIR:-/tmp}/doc_read_test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PORT_BE=18123
PORT_GW=18124

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

REAL_HOME="$HOME"
PACK="${SAMOSA_OCR_PACK:-$REAL_HOME/.samosa/models/ocr-pack-v1}"

# Prepare files folder, home structure, and mock fixtures
mkdir -p "$TMP/files" "$TMP/home/models/ornith-9b" "$TMP/home/models/bonsai-27b-1bit" "$TMP/home/.samosa/cache/read"
printf 'fixture\n' >"$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'fixture\n' >"$TMP/home/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'mmproj-fixture\n' >"$TMP/home/bonsai-mmproj.gguf"
printf 'ornith\n' >"$TMP/home/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
cp tools/testdata/ocr/tiny.png "$TMP/files/cat-medical-note.png"

export HOME="$TMP/home"
export SAMOSA_HOME="$TMP/home"
export SAMOSA_READ_CACHE_DIR="$TMP/home/.samosa/cache/read"
export SAMOSA_OCR_PACK="$PACK"

# Start gateway pointing to fake backend
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
if ! printf '%s' "$health" | /usr/bin/grep -q '"ready":true'; then
  echo "Gateway failed to start:" >&2
  cat "$TMP/gateway.log" >&2
  exit 1
fi

# 1. First run: cold cache miss -> run samosa-ocr -> return result -> write to cache
res1=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res1" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "doc.read tool not recorded in job stream" >&2; exit 1; }
printf '%s' "$res1" | /usr/bin/grep -q '"path":"cat-medical-note.png"' || { echo "Path missing from tool result event" >&2; exit 1; }

# Verify cache entry was created and contains OCR text
CACHE_BASE="$TMP/home/.samosa/cache/read"
[ -d "$CACHE_BASE" ] || { echo "Cache directory not created: $CACHE_BASE" >&2; exit 1; }

CACHE_FILE=$(find "$CACHE_BASE" -name "*.json" | head -n 1)
[ -n "$CACHE_FILE" ] && [ -f "$CACHE_FILE" ] || { echo "No cache JSON file found in $CACHE_BASE" >&2; exit 1; }
grep -q 'Poličar 2019' "$CACHE_FILE" || { echo "Cache file content missing OCR text" >&2; exit 1; }

# 2. Second run: cache hit -> returns cached result
res2=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res2" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "Cache hit run missing doc.read tool call" >&2; exit 1; }

kill "$GW_PID" "$BE_PID" 2>/dev/null || true
echo "doc-read-test: PASS"
