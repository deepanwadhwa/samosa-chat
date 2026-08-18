#!/bin/sh
set -eu

if [ "$(uname -s)" != "Darwin" ]; then
  echo "test_kokoro_native_gateway.sh: SKIP (macOS dylib contract test)" >&2
  exit 0
fi

# End-to-end contract for the native neural voice boundary. The gateway is
# given a C dylib, not a process or interpreter: it must load the documented
# Sherpa C symbols, pass Kokoro's paths/speaker ID, and respond with PCM WAV.

BUILD_DIR=${BUILD_DIR:-build}
GATEWAY=${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kokoro_native_gateway.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18994
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  [ "${SAMOSA_TEST_KEEP:-0}" = 1 ] || rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/voice/kokoro/runtime/lib" "$HOME_DIR/voice/kokoro/model/espeak-ng-data"
cc -dynamiclib -O2 -Wall -Wextra -Werror -std=c11 -I"$ROOT/src" \
  "$ROOT/tests/fake_kokoro_native.c" \
  -o "$HOME_DIR/voice/kokoro/runtime/lib/libsherpa-onnx-c-api.dylib"
printf 'model\n' >"$HOME_DIR/voice/kokoro/model/model.int8.onnx"
printf 'voices\n' >"$HOME_DIR/voice/kokoro/model/voices.bin"
printf 'tokens\n' >"$HOME_DIR/voice/kokoro/model/tokens.txt"
printf 'native fixture\n' >"$HOME_DIR/voice/kokoro/ready"
printf '<!doctype html><title>Samosa Kokoro fixture</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_MODELS_CATALOG="$ROOT/assets/models.json" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!
i=0
while [ "$i" -lt 80 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done
[ -s "$HOME_DIR/run/ui-token" ] || { cat "$TMP/stderr.log"; exit 1; }
TOKEN=$(cat "$HOME_DIR/run/ui-token")

STATUS=$(curl -sS -o "$TMP/status.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/voice/status")
[ "$STATUS" = 200 ] || { cat "$TMP/status.json"; exit 1; }
grep -q '"tts_neural_ready":true' "$TMP/status.json"
grep -q '"tts_engine":"kokoro_native"' "$TMP/status.json"

STATUS=$(curl -sS -o "$TMP/speech.wav" -w '%{http_code}' -X POST \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  --data '{"text":"Hello from native Kokoro.","voice":"sarah"}' \
  "http://127.0.0.1:$PORT/v1/voice/speech")
[ "$STATUS" = 200 ] || { cat "$TMP/stderr.log"; exit 1; }
[ "$(dd if="$TMP/speech.wav" bs=1 count=4 2>/dev/null)" = RIFF ] || {
  echo 'FAIL: native Kokoro endpoint did not return a WAV' >&2; exit 1;
}

# Timing diagnostics correlate one turn and remain
# metadata-only. The streaming endpoint must return the fake runtime's two
# callback chunks as raw s16le PCM rather than waiting to build a WAV.
STATUS=$(curl -sS -o "$TMP/trace-start.json" -w '%{http_code}' -X POST \
  -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/voice/diagnostics/start")
[ "$STATUS" = 200 ] || { cat "$TMP/trace-start.json"; exit 1; }
grep -q '"active":true' "$TMP/trace-start.json"

STATUS=$(curl -sS -o "$TMP/speech.pcm" -w '%{http_code}' -X POST \
  -H "X-Samosa-Token: $TOKEN" -H 'X-Samosa-Voice-Turn: voice-fixture-turn' \
  -H 'Content-Type: application/json' \
  --data '{"text":"Hello from native Kokoro.","voice":"sarah"}' \
  "http://127.0.0.1:$PORT/v1/voice/speech/stream")
[ "$STATUS" = 200 ] || { cat "$TMP/stderr.log"; exit 1; }
[ "$(wc -c <"$TMP/speech.pcm" | tr -d ' ')" = 8 ] || {
  echo 'FAIL: streamed Kokoro PCM did not contain the callback samples' >&2; exit 1;
}

STATUS=$(curl -sS -o "$TMP/trace-stop.json" -w '%{http_code}' -X POST \
  -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/voice/diagnostics/stop")
[ "$STATUS" = 200 ] || { cat "$TMP/trace-stop.json"; exit 1; }
grep -q '"active":false' "$TMP/trace-stop.json"
TRACE=$(find "$HOME_DIR/logs/voice" -name 'voice-trace-*.jsonl' -type f -print -quit)
[ -n "$TRACE" ] || { echo 'FAIL: Voice diagnostics did not save JSONL' >&2; exit 1; }
python3 -c "
import json
rows=[json.loads(line) for line in open('$TRACE')]
events=[row['event'] for row in rows]
assert events[0]=='trace_started' and events[-1]=='trace_stopped', events
assert 'tts_generation_started' in events and 'tts_first_pcm_sent' in events and 'tts_generation_complete' in events, events
assert all(row['schema']=='samosa.voice.trace.v1' for row in rows)
assert all('text' not in row and 'audio' not in row for row in rows), rows
assert any(row.get('turn_id')=='voice-fixture-turn' for row in rows), rows
"
! grep -F 'Hello from native Kokoro' "$TRACE"
python3 "$ROOT/tools/analyze_voice_trace.py" "$TRACE" >"$TMP/trace-analysis.txt"
grep -q 'fixture-turn' "$TMP/trace-analysis.txt"
grep -q 'All durations are milliseconds' "$TMP/trace-analysis.txt"

# Install a Pocket-shaped fixture beside legacy Kokoro, restart, and prove the
# gateway prefers Pocket, supplies reference audio, and labels the PCM stream.
curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/shutdown" >/dev/null
wait "$PID"
PID=""
mkdir -p "$HOME_DIR/voice/pocket/runtime/lib" "$HOME_DIR/voice/pocket/model/voices"
cp "$HOME_DIR/voice/kokoro/runtime/lib/libsherpa-onnx-c-api.dylib" \
  "$HOME_DIR/voice/pocket/runtime/lib/libsherpa-onnx-c-api.dylib"
for file in lm_flow.int8.onnx lm_main.int8.onnx encoder.onnx decoder.int8.onnx text_conditioner.onnx vocab.json token_scores.json; do
  printf 'pocket fixture\n' >"$HOME_DIR/voice/pocket/model/$file"
done
python3 -c "import struct,wave; p='$HOME_DIR/voice/pocket/model/voices'; [(lambda w: (w.setparams((1,2,16000,4,'NONE','not compressed')), w.writeframes(struct.pack('<hhhh',0,1000,-1000,0)), w.close()))(wave.open(p+'/'+n,'wb')) for n in ('caro_davy.wav','stuart_bell.wav')]"
printf 'native pocket fixture\n' >"$HOME_DIR/voice/pocket/ready"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_MODELS_CATALOG="$ROOT/assets/models.json" \
  "$GATEWAY" >"$TMP/pocket-stdout.log" 2>"$TMP/pocket-stderr.log" &
PID=$!
i=0
while [ "$i" -lt 80 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done
TOKEN=$(cat "$HOME_DIR/run/ui-token")
curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/voice/status" >"$TMP/pocket-status.json"
grep -q '"tts_engine":"pocket_native"' "$TMP/pocket-status.json"
curl -fsS -D "$TMP/pocket-headers.txt" -o "$TMP/pocket-speech.pcm" -X POST \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  --data '{"text":"Hello from native Pocket.","voice":"caro"}' \
  "http://127.0.0.1:$PORT/v1/voice/speech/stream"
grep -qi '^X-Samosa-TTS-Engine: pocket_native' "$TMP/pocket-headers.txt"
[ "$(wc -c <"$TMP/pocket-speech.pcm" | tr -d ' ')" = 8 ] || {
  echo 'FAIL: streamed Pocket PCM did not contain both progress callbacks' >&2; exit 1;
}

grep -F 'Python' "$ROOT/tools/samosa_kokoro_runtime.sh" >/dev/null &&
  grep -F 'No Python or pip' "$ROOT/tools/samosa_kokoro_runtime.sh" >/dev/null || {
  echo 'FAIL: native installer does not state its no-Python contract' >&2; exit 1;
}
grep -F 'codesign --force --sign -' "$ROOT/tools/samosa_kokoro_runtime.sh" >/dev/null || {
  echo 'FAIL: native installer does not re-sign macOS dylibs before dlopen' >&2; exit 1;
}

echo 'test_kokoro_native_gateway.sh: PASS'
