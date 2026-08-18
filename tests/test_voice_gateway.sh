#!/bin/sh
set -eu

# Exercises the real localhost STT boundary without downloading Whisper.cpp:
# a tiny argv-safe fake CLI stands in for the pinned binary, while the model
# file is sparse but exact-size so the same readiness check production uses is
# exercised. The gateway must authenticate, validate PCM WAV, clean temporary
# recordings, and return only the local CLI's text.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
RUNTIME_SCRIPT="$ROOT/tools/samosa_voice_runtime.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/voice_gateway_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18993
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

# The runtime builder stages only one executable, so its Whisper dependency
# graph must be static; a dynamic default build would point at a temporary
# build directory that is intentionally removed after installation.
grep -F -- '-DBUILD_SHARED_LIBS=OFF' "$RUNTIME_SCRIPT" >/dev/null
grep -F -- 'f049fff95a089aa9969deb009cdd4892b3e74916' "$RUNTIME_SCRIPT" >/dev/null
grep -F -- 'd8cd961352377b1cc612224016a9ebdfe0ae508dc2b2f9ef514b341d672e3fdc' "$RUNTIME_SCRIPT" >/dev/null

make samosa-gateway >/dev/null 2>&1 || true
mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Samosa voice fixture</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

python3 -c "from pathlib import Path; Path('$TMP/ggml-base.en.bin').write_bytes(b''); Path('$TMP/ggml-base.en.bin').open('r+b').truncate(147964211)"
python3 -c "from pathlib import Path; Path('$TMP/ggml-tiny.en.bin').write_bytes(b''); Path('$TMP/ggml-tiny.en.bin').open('r+b').truncate(77704715)"
cat >"$TMP/whisper-cli" <<'EOF'
#!/bin/sh
set -eu
out=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -of) out=$2; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$out" ]
printf 'hello from local whisper\n' >"$out.txt"
EOF
chmod 700 "$TMP/whisper-cli"

python3 -c "
import struct
samples = [0, 500, -500, 0] * 400
data = struct.pack('<%dh' % len(samples), *samples)
wav = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 16000, 32000, 2, 16) + b'data' + struct.pack('<I', len(data)) + data
open('$TMP/voice.wav', 'wb').write(wav)
"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_MODELS_CATALOG="$ROOT/assets/models.json" \
SAMOSA_VOICE_BROWSER_ROOT="$ROOT/assets/voice/browser" \
SAMOSA_WHISPER_CLI="$TMP/whisper-cli" \
SAMOSA_WHISPER_MODEL="$TMP/ggml-base.en.bin" \
SAMOSA_WHISPER_TINY_MODEL="$TMP/ggml-tiny.en.bin" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!
i=0
while [ "$i" -lt 80 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done
[ -s "$HOME_DIR/run/ui-token" ] || { cat "$TMP/stderr.log"; exit 1; }
TOKEN=$(cat "$HOME_DIR/run/ui-token")

STATUS=$(curl -sS -o "$TMP/moss-host.html" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/assets/voice/tts/moss/browser_onnx_host.html")
[ "$STATUS" = 200 ] || { cat "$TMP/moss-host.html"; exit 1; }
grep -q 'browser_onnx_host.js' "$TMP/moss-host.html" || exit 1

STATUS=$(curl -sS -o "$TMP/status.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/voice/status")
[ "$STATUS" = 200 ] || { cat "$TMP/status.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/status.json'))
assert d['stt_model_id'] == 'voice-stt-whisper-base-en'
assert d['stt_runtime_ready'] is True and d['stt_model_downloaded'] is True and d['stt_ready'] is True, d
"

STATUS=$(curl -sS -o "$TMP/select-tiny.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' --data '{"kind":"stt","model_id":"voice-stt-whisper-tiny-en"}' "http://127.0.0.1:$PORT/v1/voice/select")
[ "$STATUS" = 200 ] || { cat "$TMP/select-tiny.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/select-tiny.json'))
assert d['stt_model_id'] == 'voice-stt-whisper-tiny-en'
assert d['stt_ready'] is True and d['stt_model_downloaded'] is True, d
"

STATUS=$(curl -sS -o "$TMP/select-browser.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' --data '{"kind":"tts","model_id":"voice-tts-browser"}' "http://127.0.0.1:$PORT/v1/voice/select")
[ "$STATUS" = 200 ] || { cat "$TMP/select-browser.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/select-browser.json'))
assert d['tts_model_id'] == 'voice-tts-browser'
assert d['tts_neural_ready'] is False, d
"

STATUS=$(curl -sS -o "$TMP/select-moss.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' --data '{"kind":"tts","model_id":"voice-tts-moss-nano"}' "http://127.0.0.1:$PORT/v1/voice/select")
[ "$STATUS" = 409 ] || { cat "$TMP/select-moss.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/select-moss.json'))
assert d['error']['code'] == 'voice_model_not_ready', d
"

STATUS=$(curl -sS -o "$TMP/select-moss-browser.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' --data '{"kind":"tts","model_id":"voice-tts-moss-nano","browser_ready":true}' "http://127.0.0.1:$PORT/v1/voice/select")
[ "$STATUS" = 200 ] || { cat "$TMP/select-moss-browser.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/select-moss-browser.json'))
assert d['tts_model_id'] == 'voice-tts-moss-nano'
assert d['tts_neural_ready'] is True and d['tts_engine'] == 'moss_browser', d
"

STATUS=$(curl -sS -o "$TMP/browser-speech.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' --data '{"text":"one two four four three three three what what"}' "http://127.0.0.1:$PORT/v1/voice/speech/stream")
[ "$STATUS" = 409 ] || { cat "$TMP/browser-speech.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/browser-speech.json'))
assert d['error']['code'] == 'browser_tts_only', d
"

STATUS=$(curl -sS -o "$TMP/prepare-moss.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' --data '{"model_id":"voice-tts-moss-nano"}' "http://127.0.0.1:$PORT/v1/voice/tts/runtime")
[ "$STATUS" = 409 ] || { cat "$TMP/prepare-moss.json"; exit 1; }

STATUS=$(curl -sS -o "$TMP/unauth.json" -w '%{http_code}' -X POST -H 'Content-Type: audio/wav' --data-binary @"$TMP/voice.wav" "http://127.0.0.1:$PORT/v1/voice/transcriptions")
[ "$STATUS" = 401 ] || { cat "$TMP/unauth.json"; exit 1; }

STATUS=$(curl -sS -o "$TMP/bad.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: audio/wav' --data-binary 'not-a-wav' "http://127.0.0.1:$PORT/v1/voice/transcriptions")
[ "$STATUS" = 415 ] || { cat "$TMP/bad.json"; exit 1; }

STATUS=$(curl -sS -o "$TMP/transcript.json" -w '%{http_code}' -X POST -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: audio/wav' --data-binary @"$TMP/voice.wav" "http://127.0.0.1:$PORT/v1/voice/transcriptions")
[ "$STATUS" = 200 ] || { cat "$TMP/transcript.json"; exit 1; }
python3 -c "
import json
d=json.load(open('$TMP/transcript.json'))
assert d == {'text':'hello from local whisper','duration_seconds':0.1,'engine':'whisper.cpp'}, d
"

[ ! -d "$HOME_DIR/voice/tmp" ] || [ -z "$(find "$HOME_DIR/voice/tmp" -type f -print -quit)" ] || {
  echo 'FAIL: local voice recording was not removed' >&2
  exit 1
}

echo "test_voice_gateway.sh: PASS"
