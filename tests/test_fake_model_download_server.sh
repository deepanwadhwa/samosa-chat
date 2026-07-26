#!/bin/sh
set -eu

# T0.1 (docs/TASKS_UI_CHUTNI.md): the fake model-download server must exercise
# every failure mode T2.2's future resumable downloader will need to survive,
# deterministically and offline. This test proves the fixture server itself is
# correct before anything depends on it.

BUILD_DIR="${BUILD_DIR:-build}"
SERVER="${SAMOSA_FAKE_DOWNLOAD_SERVER:-./$BUILD_DIR/fake_model_download_server}"
PORT=18979
TMP=$(mktemp -d "${TMPDIR:-/tmp}/fake_download_server.XXXXXX")
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make fake_model_download_server >/dev/null 2>&1 || true

FIXTURE="$TMP/artifact.bin"
# 5000 deterministic bytes, not all-zero, so a single flipped byte is detectable.
python3 -c "
import sys
data = bytes((i * 37 + 11) % 256 for i in range(5000))
open('$FIXTURE', 'wb').write(data)
"
EXPECTED_SHA=$(shasum -a 256 "$FIXTURE" 2>/dev/null | awk '{print $1}' || sha256sum "$FIXTURE" | awk '{print $1}')

start_server() {
  mode="$1"
  file="$2"
  SAMOSA_FAKE_DOWNLOAD_MODE="$mode" SAMOSA_FAKE_DOWNLOAD_FILE="$file" \
    "$SERVER" --port "$PORT" &
  PID=$!
  i=0
  while [ "$i" -lt 50 ]; do
    if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then return 0; fi
    sleep 0.05; i=$((i + 1))
  done
  echo "server did not become healthy (mode=$mode)" >&2; exit 1
}

stop_server() {
  curl -fsS -X POST "http://127.0.0.1:$PORT/shutdown" >/dev/null 2>&1 || true
  wait "$PID" 2>/dev/null || true
  PID=""
}

# --- normal: full GET ---
start_server normal "$FIXTURE"
curl -fsS "http://127.0.0.1:$PORT/artifact" -o "$TMP/full.bin"
[ "$(wc -c <"$TMP/full.bin" | tr -d ' ')" = 5000 ] || { echo "FAIL: normal full GET size"; exit 1; }
GOT_SHA=$(shasum -a 256 "$TMP/full.bin" 2>/dev/null | awk '{print $1}' || sha256sum "$TMP/full.bin" | awk '{print $1}')
[ "$GOT_SHA" = "$EXPECTED_SHA" ] || { echo "FAIL: normal full GET checksum"; exit 1; }

# --- normal: valid ranged GET (206 + Content-Range) ---
HEADERS=$(curl -fsS -D - -o "$TMP/range.bin" -H "Range: bytes=100-199" "http://127.0.0.1:$PORT/artifact")
echo "$HEADERS" | grep -q "^HTTP/1.1 206" || { echo "FAIL: ranged GET did not return 206"; echo "$HEADERS"; exit 1; }
echo "$HEADERS" | grep -qi "^Content-Range: bytes 100-199/5000" || { echo "FAIL: bad Content-Range"; echo "$HEADERS"; exit 1; }
[ "$(wc -c <"$TMP/range.bin" | tr -d ' ')" = 100 ] || { echo "FAIL: ranged GET size"; exit 1; }
stop_server

# --- ignore_range: server returns 200 with full body despite Range header ---
start_server ignore_range "$FIXTURE"
HEADERS=$(curl -fsS -D - -o "$TMP/ignored.bin" -H "Range: bytes=100-199" "http://127.0.0.1:$PORT/artifact")
echo "$HEADERS" | grep -q "^HTTP/1.1 200" || { echo "FAIL: ignore_range did not return 200"; exit 1; }
[ "$(wc -c <"$TMP/ignored.bin" | tr -d ' ')" = 5000 ] || { echo "FAIL: ignore_range body was not full"; exit 1; }
stop_server

# --- truncate: Content-Length promises 5000, connection yields fewer bytes ---
start_server truncate "$FIXTURE"
HEADERS=$(curl -s -D - -o "$TMP/truncated.bin" "http://127.0.0.1:$PORT/artifact" || true)
echo "$HEADERS" | grep -qi "^Content-Length: 5000" || { echo "FAIL: truncate did not declare full length"; exit 1; }
GOT_LEN=$(wc -c <"$TMP/truncated.bin" | tr -d ' ')
[ "$GOT_LEN" -lt 5000 ] || { echo "FAIL: truncate delivered the full body ($GOT_LEN bytes)"; exit 1; }
stop_server

# --- corrupt: correct length, wrong checksum ---
start_server corrupt "$FIXTURE"
curl -fsS "http://127.0.0.1:$PORT/artifact" -o "$TMP/corrupt.bin"
[ "$(wc -c <"$TMP/corrupt.bin" | tr -d ' ')" = 5000 ] || { echo "FAIL: corrupt mode changed length"; exit 1; }
GOT_SHA=$(shasum -a 256 "$TMP/corrupt.bin" 2>/dev/null | awk '{print $1}' || sha256sum "$TMP/corrupt.bin" | awk '{print $1}')
[ "$GOT_SHA" != "$EXPECTED_SHA" ] || { echo "FAIL: corrupt mode did not change checksum"; exit 1; }
stop_server

# --- bad_content_range: 206 status but a Content-Range header that cannot be
#     reconciled with the request (a real client must reject this, not resume). ---
start_server bad_content_range "$FIXTURE"
HEADERS=$(curl -fsS -D - -o "$TMP/badrange.bin" -H "Range: bytes=100-199" "http://127.0.0.1:$PORT/artifact")
echo "$HEADERS" | grep -q "^HTTP/1.1 206" || { echo "FAIL: bad_content_range did not return 206"; exit 1; }
echo "$HEADERS" | grep -qi "^Content-Range: bytes 0-0/0" || { echo "FAIL: bad_content_range header not reproduced"; echo "$HEADERS"; exit 1; }
stop_server

# --- redirect: 302 to a guaranteed non-routable (RFC 5737 TEST-NET-1) host ---
start_server redirect "$FIXTURE"
HEADERS=$(curl -fsS -D - -o /dev/null "http://127.0.0.1:$PORT/artifact")
echo "$HEADERS" | grep -q "^HTTP/1.1 302" || { echo "FAIL: redirect mode did not return 302"; exit 1; }
echo "$HEADERS" | grep -qi "^Location: http://192.0.2.1" || { echo "FAIL: redirect Location missing/unexpected"; echo "$HEADERS"; exit 1; }
stop_server

# --- oversize: fixture larger than a hypothetical catalog-declared size ---
BIGGER="$TMP/bigger.bin"
python3 -c "
data = bytes((i * 37 + 11) % 256 for i in range(6000))
open('$BIGGER', 'wb').write(data)
"
start_server oversize "$BIGGER"
curl -fsS "http://127.0.0.1:$PORT/artifact" -o "$TMP/oversize.bin"
GOT_LEN=$(wc -c <"$TMP/oversize.bin" | tr -d ' ')
[ "$GOT_LEN" -gt 5000 ] || { echo "FAIL: oversize fixture was not larger than the nominal catalog size"; exit 1; }
stop_server

echo "test_fake_model_download_server.sh: PASS (normal/ranged, ignore_range, truncate, corrupt, bad_content_range, redirect, oversize)"
