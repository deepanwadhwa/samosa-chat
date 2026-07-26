#!/bin/sh
set -eu

BUILD_DIR="build"
SERVER="./$BUILD_DIR/fake_model_download_server"
GATEWAY="./$BUILD_DIR/samosa-gateway"

TMP=$(mktemp -d "/tmp/samosa_dl_test.XXXXXX")
PID_SERVER=""
PID_GW=""

cleanup() {
  [ -z "$PID_SERVER" ] || kill "$PID_SERVER" 2>/dev/null || true
  [ -z "$PID_GW" ] || kill "$PID_GW" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make fake_model_download_server samosa-gateway >/dev/null

FIXTURE="$TMP/artifact.bin"
python3 -c "
import sys
data = bytes((i * 37 + 11) % 256 for i in range(5000))
open('$FIXTURE', 'wb').write(data)
"

SAMOSA_FAKE_DOWNLOAD_MODE="normal" SAMOSA_FAKE_DOWNLOAD_FILE="$FIXTURE" \
    "$SERVER" --port 18979 &
PID_SERVER=$!

SAMOSA_HOME="$TMP/home" SAMOSA_PORT="18980" "$GATEWAY" &
PID_GW=$!

sleep 1

TOKEN=$(cat "$TMP/home/run/ui-token")
RES=$(curl -sS -X POST "http://127.0.0.1:18980/v1/models/install" -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" -d '{"model_id":"qwen"}')
echo "Install Response: $RES"
JOB_ID=$(echo "$RES" | grep -o '"job_id":"[^"]*"' | cut -d'"' -f4)

# Wait for events
sleep 2

cat "$TMP/home/models/qwen/.partial/$JOB_ID/events.jsonl" || true

echo "Done"
