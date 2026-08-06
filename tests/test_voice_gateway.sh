#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

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


mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Samosa voice fixture</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

python3 -c "from pathlib import Path; Path('$TMP/ggml-base.en.bin').write_bytes(b''); Path('$TMP/ggml-base.en.bin').open('r+b').truncate(147964211)"
cat >"$TMP/whisper-cli" <<'EOF'
#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}
out=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -of) out=$2; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$out" ] || fail "expected non-empty value"
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
SAMOSA_WHISPER_CLI="$TMP/whisper-cli" \
SAMOSA_WHISPER_MODEL="$TMP/ggml-base.en.bin" \
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
python3 -c "
import json
d=json.load(open('$TMP/status.json'))
assert d['stt_model_id'] == 'voice-stt-whisper-base-en'
assert d['stt_runtime_ready'] is True and d['stt_model_downloaded'] is True and d['stt_ready'] is True, d
"

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
