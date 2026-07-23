#!/bin/sh
set -eu

TMP=$(mktemp -d "${TMPDIR:-/tmp}/motto_scenario_test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PORT_BE=18133
PORT_GW=18134

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

# Prepare 20-file fixture folder
REAL_HOME="$HOME"
PACK="${SAMOSA_OCR_PACK:-$REAL_HOME/.samosa/models/ocr-pack-v1}"

mkdir -p "$TMP/files" "$TMP/home/models/ornith-9b" "$TMP/home/models/bonsai-27b-1bit" "$TMP/home/.samosa/cache/read"
printf 'fixture\n' >"$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'fixture\n' >"$TMP/home/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'mmproj-fixture\n' >"$TMP/home/bonsai-mmproj.gguf"
printf 'ornith\n' >"$TMP/home/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

# Populate 15 plain text files
i=1
while [ "$i" -le 15 ]; do
  if [ "$i" -eq 7 ]; then
    printf "Motto: Titli rabies booster record 2026.\n" >"$TMP/files/file_$i.txt"
  else
    printf "General document contents file %d.\n" "$i" >"$TMP/files/file_$i.txt"
  fi
  i=$((i + 1))
done

# Populate 4 image files with tiny printed OCR fixture
cp tools/testdata/ocr/tiny.png "$TMP/files/cat-medical-note.png"
cp tools/testdata/ocr/tiny.png "$TMP/files/scan_17.png"
cp tools/testdata/ocr/tiny.png "$TMP/files/scan_18.png"
cp tools/testdata/ocr/tiny.png "$TMP/files/scan_19.png"

# Planted low-confidence / image guard file
cp assets/samosa-chat.png "$TMP/files/uncertain_20.png"

export HOME="$TMP/home"
export SAMOSA_HOME="$TMP/home"
export SAMOSA_READ_CACHE_DIR="$TMP/home/.samosa/cache/read"
export SAMOSA_OCR_PACK="$PACK"

# Start gateway
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

# 1. Run 1: Read fixture folder -> populates cache for scans
res1=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res1" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "Run 1 missing doc.read" >&2; exit 1; }

# Check that cache files exist in cache dir
CACHE_BASE="$TMP/home/.samosa/cache/read"
[ -d "$CACHE_BASE" ] || { echo "Cache dir missing" >&2; exit 1; }
CACHE_COUNT=$(find "$CACHE_BASE" -name "*.json" | wc -l | tr -d ' ')
[ "$CACHE_COUNT" -ge 1 ] || { echo "No cache files written" >&2; exit 1; }

# 2. Run 2: Read same fixture folder -> 100% cache hits
res2=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find cat image document with doc.read\",\"folder\":\"$TMP/files\"}")

printf '%s' "$res2" | /usr/bin/grep -q '"tool":"doc.read"' || { echo "Run 2 missing doc.read" >&2; exit 1; }

# 3. Verify image definition guard / low confidence review parking
def_payload="{\"job\":{\"job_id\":\"motto-definition-guard\",\"input\":{\"folder\":\"$TMP/files\"},\"instruction\":\"Extract people count.\",\"output_schema\":{\"type\":\"object\",\"properties\":{\"people\":{\"type\":\"integer\"}}},\"output\":{\"dir\":\"$TMP/def-out\"}}}"

run_def=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT_GW/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$def_payload")

printf '%s' "$run_def" | /usr/bin/grep -q '"type":"item_complete"' || { echo "Definition run missing item_complete" >&2; exit 1; }

# Check output.jsonl for review_required parking on image units when text-only backend is active
[ -f "$TMP/def-out/output.jsonl" ] || { echo "output.jsonl not created" >&2; exit 1; }
grep -q '"status":"review_required"' "$TMP/def-out/output.jsonl" || { echo "Planted image unit did not land in review_required" >&2; exit 1; }
grep -q '"reasons":\["vision_backend_required"\]' "$TMP/def-out/output.jsonl" || { echo "Expected reason vision_backend_required missing" >&2; exit 1; }

kill "$GW_PID" "$BE_PID" 2>/dev/null || true
echo "motto-scenario-test: PASS"
