#!/bin/sh
set -eu

TMP=$(mktemp -d "${TMPDIR:-/tmp}/r7_r6_test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PORT_BE=18153
PORT_GW=18154

BUILD_DIR="${BUILD_DIR:-build}"

# Compile binaries if needed
make samosa-ocr samosa-gateway test_fake_openai_backend >/dev/null 2>&1 || true

REAL_HOME="$HOME"
PACK="${SAMOSA_OCR_PACK:-$REAL_HOME/.samosa/models/ocr-pack-v1}"

export SAMOSA_OCR_PACK="$PACK"

# 1. Direct samosa-ocr test on printed fixture
out_print=$("${BUILD_DIR}/samosa-ocr" read tools/testdata/ocr/tiny.png)
printf '%s' "$out_print" | grep -q '"script":"printed"' || { echo "Printed fixture missing script:printed" >&2; exit 1; }
printf '%s' "$out_print" | grep -q '"reader":"rec_print"' || { echo "Printed fixture missing reader:rec_print" >&2; exit 1; }

# 2. Test mock handwriting pack creation & rec_hand reader path
mkdir -p "$TMP/mock_pack"
cp "$PACK"/* "$TMP/mock_pack/"
cp "$PACK/rec.bin" "$TMP/mock_pack/rec_hand.bin"

out_hand=$(SAMOSA_OCR_PACK="$TMP/mock_pack" "${BUILD_DIR}/samosa-ocr" read tools/testdata/ocr/tiny.png)
printf '%s' "$out_hand" | grep -q '"ok":true' || { echo "Mock handwriting pack read failed" >&2; exit 1; }

# 3. Start gateway and verify doc.read details mode
export HOME="$TMP/home"
export SAMOSA_HOME="$TMP/home"
export SAMOSA_READ_CACHE_DIR="$TMP/home/.samosa/cache/read"

mkdir -p "$TMP/files" "$TMP/home/.samosa" "$TMP/home/models/ornith-9b" "$TMP/home/models/bonsai-27b-1bit" "$TMP/home/.samosa/cache/read"
printf 'fixture\n' >"$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'fixture\n' >"$TMP/home/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'mmproj-fixture\n' >"$TMP/home/bonsai-mmproj.gguf"
printf 'ornith\n' >"$TMP/home/.samosa/model-backend"
printf 'ornith\n' >"$TMP/home/.samosa/active-backend"
printf 'ornith\n' >"$TMP/home/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
cp tools/testdata/ocr/tiny.png "$TMP/files/cat-medical-note.png"

# Start fake backend
"${BUILD_DIR}/test_fake_openai_backend" --port "$PORT_BE" &
BE_PID=$!
trap 'kill "$BE_PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT

SAMOSA_HOME="$TMP/home" \
SAMOSA_READ_CACHE_DIR="$TMP/home/.samosa/cache/read" \
SAMOSA_OCR_PACK="$PACK" \
SAMOSA_JOBS_ROOT="$TMP/jobs" \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_ORNITH_MODEL="$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_BONSAI_MMPROJ="$TMP/home/bonsai-mmproj.gguf" \
SAMOSA_BONSAI_SERVER="$(pwd)/${BUILD_DIR}/test_fake_openai_backend" \
SAMOSA_OCR="$(pwd)/${BUILD_DIR}/samosa-ocr" \
SAMOSA_FS="$(pwd)/${BUILD_DIR}/samosa-fs" \
SAMOSA_EXTRACT="$(pwd)/${BUILD_DIR}/samosa-extract" \
SAMOSA_BACKEND_PORT="$PORT_BE" \
SAMOSA_PORT="$PORT_GW" \
"${BUILD_DIR}/samosa-gateway" >"$TMP/gateway.log" 2>&1 &
GW_PID=$!
trap 'kill "$GW_PID" "$BE_PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT

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

res=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "Gateway missing doc.read" >&2; exit 1; }

kill "$GW_PID" "$BE_PID" 2>/dev/null || true
echo "r7-r6-test: PASS"
