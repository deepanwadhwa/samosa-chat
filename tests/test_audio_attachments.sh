#!/bin/sh
set -eu

# End-to-end Auto audio slice: byte-sniffed PCM WAV upload, selected local
# Whisper provider, timestamped durable evidence, cache reuse, and synthesis
# through the ordinary selected chat model. No real model download is needed.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
AUDIO_DECODER="${SAMOSA_AUDIO_DECODER:-./$BUILD_DIR/samosa-audio-decode}"
ACTIVE_AUDIO_DECODER="$AUDIO_DECODER"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
checks() { node "$ROOT/tests/audio_attachment_checks.mjs" "$@"; }
TMP=$(mktemp -d "${TMPDIR:-/tmp}/audio_attachments_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=19024
GW_PID=""

stop_gateway() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  GW_PID=""
}

cleanup() {
  stop_gateway
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make samosa-gateway test_fake_openai_backend >/dev/null
COMPRESSED_AUDIO=0
if [ "$(uname -s)" = Darwin ]; then
  make samosa-audio-decode >/dev/null
  [ -x "$AUDIO_DECODER" ] || {
    echo "FAIL: macOS compressed-audio decoder was not built" >&2
    exit 1
  }
  COMPRESSED_AUDIO=1
fi
mkdir -p "$HOME_DIR/qwen-model"
printf '<!doctype html><title>Samosa audio fixture</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"
checks model "$TMP/ggml-base.en.bin"

cat >"$TMP/whisper-cli" <<'EOF'
#!/bin/sh
set -eu
out=""
format=""
input=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -of) out=$2; shift 2 ;;
    -f) input=$2; shift 2 ;;
    -osrt) format=srt; shift ;;
    -otxt) format=txt; shift ;;
    *) shift ;;
  esac
done
[ -n "$out" ]
printf 'run\n' >>"$SAMOSA_FAKE_WHISPER_COUNT"
run_count=$(wc -l <"$SAMOSA_FAKE_WHISPER_COUNT" | tr -d ' ')
if [ -n "${SAMOSA_FAKE_WHISPER_FAIL_ONCE_MARKER:-}" ] &&
   [ "$run_count" = 2 ] &&
   [ ! -e "$SAMOSA_FAKE_WHISPER_FAIL_ONCE_MARKER" ]; then
  : >"$SAMOSA_FAKE_WHISPER_FAIL_ONCE_MARKER"
  exit 9
fi
if [ -n "${SAMOSA_FAKE_WHISPER_BLOCK_AT_FILE:-}" ] &&
   [ -s "$SAMOSA_FAKE_WHISPER_BLOCK_AT_FILE" ] &&
   [ "$(cat "$SAMOSA_FAKE_WHISPER_BLOCK_AT_FILE")" = "$run_count" ]; then
  : >"$SAMOSA_FAKE_WHISPER_BLOCK_READY"
  trap 'exit 143' TERM INT
  while [ ! -e "$SAMOSA_FAKE_WHISPER_RELEASE" ]; do sleep 0.02; done
fi
if [ "$format" = srt ]; then
  input_bytes=0
  [ -z "$input" ] || input_bytes=$(wc -c <"$input" | tr -d ' ')
  if [ -s "${SAMOSA_FAKE_WHISPER_RANKING:-}" ]; then
    set -- $(cat "$SAMOSA_FAKE_WHISPER_RANKING")
    ranking_index=$((run_count - $1))
    if [ "$ranking_index" = 1 ]; then
      ranking_text='The early ranking sentinel is cedar.'
    elif [ "$ranking_index" = "$2" ]; then
      ranking_text='The late ranking sentinel is quartz.'
    else
      ranking_text='The middle ranking sentinel is mango.'
    fi
    cat >"$out.srt" <<SRT
1
00:00:01,000 --> 00:00:02,000
$ranking_text
SRT
  elif [ "$input_bytes" -ge 90000 ] && [ "$input_bytes" -le 100000 ]; then
    # whisper.cpp timestamps very short final chunks against its 30-second
    # decode frame. The gateway must bound this cue to the actual tail.
    cat >"$out.srt" <<'SRT'
1
00:00:00,000 --> 00:00:30,000
The bounded trailing sentinel appears here.
SRT
  elif [ "$input_bytes" -gt 0 ] && [ "$input_bytes" -lt 70000 ]; then
    cat >"$out.srt" <<'SRT'
1
00:00:00,000 --> 00:00:00,800
The podcast sentinel appears here.
SRT
  else
    cat >"$out.srt" <<'SRT'
1
00:00:00,000 --> 00:00:01,000
Welcome to the local podcast.

2
00:00:01,000 --> 00:00:02,500
The podcast sentinel appears here.

3
00:00:02,500 --> 00:00:04,000
Thanks for listening.
SRT
  fi
else
  printf 'short microphone transcript\n' >"$out.txt"
fi
EOF
chmod 700 "$TMP/whisper-cli"
: >"$TMP/whisper-count"

cat >"$TMP/audio-decode-wrapper" <<'EOF'
#!/bin/sh
set -eu
if [ "${1:-}" = "--decode-window" ] &&
   [ -n "${SAMOSA_FAKE_DECODE_BLOCK_FILE:-}" ] &&
   [ -e "$SAMOSA_FAKE_DECODE_BLOCK_FILE" ]; then
  : >"$SAMOSA_FAKE_DECODE_BLOCK_READY"
  trap 'exit 143' TERM INT
  while [ ! -e "$SAMOSA_FAKE_DECODE_RELEASE" ]; do sleep 0.02; done
fi
exec "$SAMOSA_REAL_AUDIO_DECODER" "$@"
EOF
chmod 700 "$TMP/audio-decode-wrapper"

# The gateway/media-router contract is tested independently of container
# generation tools. This wrapper gives one byte-sniffed MP4 fixture the same
# reviewed probe/decode contract as the native sidecar, while delegating every
# ordinary MP3/M4A operation to the real binary.
cat >"$TMP/video-audio-decode-wrapper" <<'EOF'
#!/bin/sh
set -eu
case "${2:-}" in
  *.mp4)
    if [ "${1:-}" = "--probe" ]; then
      printf '%s\n' '{"ok":true,"duration_seconds":2.000000,"audio_start_seconds":0.000000,"audio_end_seconds":2.000000,"audio_streams":1,"video_streams":1,"selected_stream":0,"codec_fourcc":"aac "}'
      exit 0
    fi
    if [ "${1:-}" = "--decode-window" ] &&
       [ "${3:-}" = "0.000000" ] && [ "${4:-}" = "2.000000" ] &&
       [ "${5:-}" = "--output" ] && [ -n "${6:-}" ]; then
      cp "$SAMOSA_FAKE_VIDEO_WAV" "$6"
      exit 0
    fi
    ;;
esac
exec "$SAMOSA_REAL_AUDIO_DECODER" "$@"
EOF
chmod 700 "$TMP/video-audio-decode-wrapper"

checks fixtures "$TMP"

# A small, deterministic ISO-BMFF-shaped source. The qualified media probe,
# not this prefix or the client MIME, is what grants transcript capability.
printf '\000\000\000\030ftypisom\000\000\002\000isomiso2avc1mp41' >"$TMP/talking.mp4"
printf 'samosa-video-audio-router-fixture\n' >>"$TMP/talking.mp4"
printf '\000\000\000\030ftypisom\000\000\002\000isomiso2' >"$TMP/visual-only.mp4"
printf 'not-a-qualified-audio-container\n' >>"$TMP/visual-only.mp4"

if [ "$COMPRESSED_AUDIO" = 1 ]; then
  # One second of a generated 440 Hz tone (mono 8 kHz, 8 kbps MP3). Keeping
  # this tiny synthetic fixture inline makes codec qualification independent
  # of ffmpeg, lame, network access, or host media files at test time.
  cp "$ROOT/tests/fixtures/audio/tone-440hz.mp3" "$TMP/tone.mp3"
  /usr/bin/afconvert "$TMP/compressed-source.wav" "$TMP/podcast.m4a" \
    -f m4af -d aac -b 64000
  /usr/bin/afconvert "$TMP/compressed-cancel-source.wav" "$TMP/cancel.m4a" \
    -f m4af -d aac -b 64000
  "$AUDIO_DECODER" --probe "$TMP/podcast.m4a" >"$TMP/m4a-probe.json"
  checks probe-aac "$TMP/m4a-probe.json"
  "$AUDIO_DECODER" --decode-window "$TMP/podcast.m4a" 9 14 \
    --output "$TMP/decoded-window.wav"
  checks decoded-window "$TMP/decoded-window.wav"
  "$AUDIO_DECODER" --probe "$TMP/tone.mp3" >"$TMP/mp3-probe.json"
  checks probe-mp3 "$TMP/mp3-probe.json"
  "$AUDIO_DECODER" --decode-window "$TMP/tone.mp3" 0 1 \
    --output "$TMP/decoded-mp3.wav"
  checks decoded-mp3 "$TMP/decoded-mp3.wav"
  if command -v ffmpeg >/dev/null 2>&1; then
    # Optional host integration: when ffmpeg is present, generate a real tiny
    # video+AAC container and exercise AVFoundation's actual video demux path.
    ffmpeg -hide_banner -loglevel error \
      -f lavfi -i color=c=black:s=16x16:r=1:d=2 \
      -f lavfi -i sine=frequency=440:sample_rate=16000:duration=2 \
      -c:v mpeg4 -q:v 10 -c:a aac -b:a 24000 -shortest \
      "$TMP/native-video-audio.mp4"
    "$AUDIO_DECODER" --probe "$TMP/native-video-audio.mp4" \
      >"$TMP/native-video-probe.json"
    checks probe-video "$TMP/native-video-probe.json"
    "$AUDIO_DECODER" --decode-window "$TMP/native-video-audio.mp4" 0 2 \
      --output "$TMP/native-video-audio.wav"
    checks decoded-video "$TMP/native-video-audio.wav"
  fi
fi

start_gateway() {
  SAMOSA_HOME="$HOME_DIR" \
  SAMOSA_PORT="$PORT" \
  SAMOSA_BACKEND_PORT=$((PORT + 1)) \
  SAMOSA_APP_HTML="$TMP/app.html" \
  SAMOSA_APP_LOGO="$TMP/logo.png" \
  SAMOSA_QWEN_ENGINE="$BACKEND" \
  SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
  SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
  SAMOSA_MODELS_CATALOG="$ROOT/assets/models.json" \
  SAMOSA_WHISPER_CLI="$TMP/whisper-cli" \
  SAMOSA_WHISPER_MODEL="$TMP/ggml-base.en.bin" \
  SAMOSA_AUDIO_DECODE="$ACTIVE_AUDIO_DECODER" \
  SAMOSA_REAL_AUDIO_DECODER="$AUDIO_DECODER" \
  SAMOSA_FAKE_DECODE_BLOCK_FILE="$TMP/decode-block" \
  SAMOSA_FAKE_DECODE_BLOCK_READY="$TMP/decode-block-ready" \
  SAMOSA_FAKE_DECODE_RELEASE="$TMP/decode-release" \
  SAMOSA_FAKE_VIDEO_PATH="$TMP/talking.mp4" \
  SAMOSA_FAKE_VIDEO_WAV="$TMP/video-audio.wav" \
  SAMOSA_FAKE_WHISPER_COUNT="$TMP/whisper-count" \
  SAMOSA_FAKE_WHISPER_FAIL_ONCE_MARKER="$TMP/whisper-failed-once" \
  SAMOSA_FAKE_WHISPER_BLOCK_AT_FILE="$TMP/whisper-block-at" \
  SAMOSA_FAKE_WHISPER_BLOCK_READY="$TMP/whisper-block-ready" \
  SAMOSA_FAKE_WHISPER_RELEASE="$TMP/whisper-release" \
  SAMOSA_FAKE_WHISPER_RANKING="$TMP/whisper-ranking" \
  SAMOSA_AUDIO_WINDOW_SECONDS=10 \
  SAMOSA_DOCUMENT_FULL_TOKEN_LIMIT=5 \
    "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  GW_PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true' && break
    sleep 0.05
    i=$((i + 1))
  done
  [ -s "$HOME_DIR/run/ui-token" ] || { cat "$TMP/stderr.log"; exit 1; }
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}

audio_question() {
  IDS=""
  [ "${1:-}" != "attached" ] || IDS=",\"attachment_ids\":[\"$AUDIO_ID\"]"
  curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"audio-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}]$IDS,\"stream\":false}"
}

start_gateway

CAPS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/capabilities/sources")
printf '%s' "$CAPS" | checks caps-audio "$COMPRESSED_AUDIO"

if [ "$COMPRESSED_AUDIO" = 1 ]; then
  VISUAL_ONLY_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    -H 'X-Samosa-Media-Type: audio/mp4' \
    -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    --data-binary @"$TMP/visual-only.mp4")
  VISUAL_ONLY_ID=$(printf '%s' "$VISUAL_ONLY_UPLOAD" | checks visual-only-id)
  WHISPER_BEFORE_UNAVAILABLE=$(wc -l <"$TMP/whisper-count" | tr -d ' ')
  STATUS=$(curl -sS -o "$TMP/video-audio-unavailable.json" -w '%{http_code}' \
    -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"video-audio-unavailable\",\"messages\":[{\"role\":\"user\",\"content\":\"What is being said in this video?\"}],\"attachment_ids\":[\"$VISUAL_ONLY_ID\"],\"stream\":false}")
  [ "$STATUS" = 422 ] || {
    cat "$TMP/video-audio-unavailable.json"
    echo "FAIL: a speech-only question on an unqualified video should be 422"
    exit 1
  }
  grep -q 'video_audio_unavailable' "$TMP/video-audio-unavailable.json" || exit 1
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = "$WHISPER_BEFORE_UNAVAILABLE" ] || {
    echo "FAIL: an unqualified video launched Whisper"
    exit 1
  }
  STATUS=$(curl -sS -o /dev/null -w '%{http_code}' \
    -H "X-Samosa-Token: $TOKEN" -X DELETE \
    "http://127.0.0.1:$PORT/v1/attachments/$VISUAL_ONLY_ID")
  [ "$STATUS" = 200 ] || { echo "FAIL: unused visual-only video was not deletable"; exit 1; }
fi

if [ "$COMPRESSED_AUDIO" = 1 ]; then
  STATUS=$(curl -sS -o "$TMP/malformed-m4a.json" -w '%{http_code}' \
    -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/attachments" \
    --data-binary @"$TMP/malformed.m4a")
  [ "$STATUS" = 415 ] || {
    cat "$TMP/malformed-m4a.json"
    echo "FAIL: malformed M4A should be rejected"
    exit 1
  }
  grep -q 'unsupported_audio_codec' "$TMP/malformed-m4a.json" || exit 1
fi

STATUS=$(curl -sS -o "$TMP/stereo.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/attachments" --data-binary @"$TMP/stereo.wav")
[ "$STATUS" = 415 ] || { cat "$TMP/stereo.json"; echo "FAIL: stereo WAV should be rejected"; exit 1; }

STATUS=$(curl -sS -o "$TMP/short-riff.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/attachments" --data-binary @"$TMP/podcast.wav.short-riff")
[ "$STATUS" = 415 ] || { cat "$TMP/short-riff.json"; echo "FAIL: PCM outside RIFF should be rejected"; exit 1; }

LONG_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  -H "X-Samosa-Filename-B64: $(printf 'long.wav' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary @"$TMP/long.wav")
LONG_ID=$(printf '%s' "$LONG_UPLOAD" | checks duration-id 180)
LONG_META=$(find "$HOME_DIR/attachments" -name "$LONG_ID.json" -print -quit)
LONG_WINDOW="$(dirname "$LONG_META")/$LONG_ID.transcript.window-0000.json"
printf '{"fixture":true}\n' >"$LONG_WINDOW"
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" \
  -X DELETE "http://127.0.0.1:$PORT/v1/attachments/$LONG_ID")
[ "$STATUS" = 200 ] || { echo "FAIL: unused streamed WAV should be deletable"; exit 1; }
[ ! -e "$LONG_WINDOW" ] || { echo "FAIL: audio window checkpoints survived attachment deletion"; exit 1; }

UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  -H 'X-Samosa-Media-Type: application/octet-stream' \
  -H "X-Samosa-Filename-B64: $(printf 'episode.mp3' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary @"$TMP/podcast.wav")
AUDIO_ID=$(printf '%s' "$UPLOAD" | checks id)
printf '%s' "$UPLOAD" | checks wav-upload

# The second bounded window fails once. Window 0 must already be durable, and
# the final evidence must not be published until every window is complete.
STATUS=$(curl -sS -o "$TMP/first-audio-question.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"audio-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$AUDIO_ID\"],\"stream\":false}")
[ "$STATUS" = 500 ] || {
  cat "$TMP/first-audio-question.json"
  echo "FAIL: the deliberate second-window Whisper failure should surface as 500"
  exit 1
}
grep -q 'audio_transcription_failed' "$TMP/first-audio-question.json" || exit 1
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 2 ] || {
  echo "FAIL: the first attempt should reach and fail window 2"; exit 1;
}
FIRST_WINDOW=$(find "$HOME_DIR/attachments" -name "$AUDIO_ID.transcript.window-0000.json" -print -quit)
[ -n "$FIRST_WINDOW" ] && [ -s "$FIRST_WINDOW" ] || {
  echo "FAIL: completed window 1 was not checkpointed before window 2 failed"; exit 1;
}
[ -z "$(find "$HOME_DIR/attachments" -name "$AUDIO_ID.transcript.json" -print -quit)" ] || {
  echo "FAIL: final transcript was published after an incomplete window set"; exit 1;
}

# Resume through the durable checkpoint, not process memory.
stop_gateway
start_gateway
RESPONSE=$(audio_question attached)
printf '%s' "$RESPONSE" | grep -q 'saw durable audio transcript' || {
  echo "FAIL: timestamped transcript did not reach the answering model: $RESPONSE"; exit 1;
}
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 3 ] || {
  echo "FAIL: retry should reuse window 1 and invoke only missing window 2"; exit 1;
}
EVIDENCE=$(find "$HOME_DIR/attachments" -name "$AUDIO_ID.transcript.json" -print -quit)
[ -n "$EVIDENCE" ] && [ -s "$EVIDENCE" ] || { echo "FAIL: durable transcript evidence was not published"; exit 1; }
checks evidence "$EVIDENCE" "$AUDIO_ID" "$HOME_DIR/attachments"

MANIFEST="$HOME_DIR/chats/audio-source-probe/documents.json"
[ -s "$MANIFEST" ] || { echo "FAIL: conversation source binding was not persisted"; exit 1; }
checks manifest "$MANIFEST" "$AUDIO_ID"

RESPONSE=$(audio_question)
printf '%s' "$RESPONSE" | grep -q 'saw durable audio transcript' || exit 1
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 3 ] || {
  echo "FAIL: repeat question retranscribed cached audio"; exit 1;
}

# The evidence is attachment-addressed rather than process memory: a fresh
# gateway must reuse it without launching Whisper again.
stop_gateway
start_gateway
RESPONSE=$(audio_question)
printf '%s' "$RESPONSE" | grep -q 'saw durable audio transcript' || exit 1
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 3 ] || {
  echo "FAIL: gateway restart retranscribed durable audio evidence"; exit 1;
}

# Streaming turns expose exact window progress. Stop during window 2, verify
# window 1 remains durable, then retry and observe a cache-hit update followed
# by only the two missing Whisper windows.
CANCEL_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  -H "X-Samosa-Filename-B64: $(printf 'cancel.wav' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary @"$TMP/cancel.wav")
CANCEL_ID=$(printf '%s' "$CANCEL_UPLOAD" | checks duration-id 25)
printf '5\n' >"$TMP/whisper-block-at"
rm -f "$TMP/whisper-block-ready" "$TMP/whisper-release"
curl -sS -m 15 -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"audio-cancel-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$CANCEL_ID\"],\"stream\":true}" \
  >"$TMP/cancel-stream.txt" &
STREAM_PID=$!
i=0
while [ "$i" -lt 250 ] && [ ! -e "$TMP/whisper-block-ready" ]; do
  sleep 0.02
  i=$((i + 1))
done
[ -e "$TMP/whisper-block-ready" ] || {
  cat "$TMP/stderr.log"
  echo "FAIL: cancellation fixture never reached Whisper window 2"
  exit 1
}
CANCEL_RESPONSE=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/cancel")
printf '%s' "$CANCEL_RESPONSE" | grep -q '"cancelled":true' || exit 1
wait "$STREAM_PID" || true
grep -q 'Transcribed window 1 of 3' "$TMP/cancel-stream.txt" || {
  cat "$TMP/cancel-stream.txt"
  echo "FAIL: streaming audio progress omitted completed window 1"
  exit 1
}
grep -q 'audio_transcription_cancelled' "$TMP/cancel-stream.txt" || {
  cat "$TMP/cancel-stream.txt"
  echo "FAIL: stopped audio turn omitted its typed cancellation error"
  exit 1
}
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 5 ] || {
  echo "FAIL: cancellation should stop the blocked second window"; exit 1;
}
CANCEL_WINDOW_0=$(find "$HOME_DIR/attachments" -name "$CANCEL_ID.transcript.window-0000.json" -print -quit)
[ -n "$CANCEL_WINDOW_0" ] && [ -s "$CANCEL_WINDOW_0" ] || {
  echo "FAIL: cancellation discarded the completed first window"; exit 1;
}
[ -z "$(find "$HOME_DIR/attachments" -name "$CANCEL_ID.transcript.window-0001.json" -print -quit)" ] || {
  echo "FAIL: interrupted Whisper window was published as complete"; exit 1;
}
[ -z "$(find "$HOME_DIR/attachments" -name "$CANCEL_ID.transcript.json" -print -quit)" ] || {
  echo "FAIL: cancellation published incomplete final evidence"; exit 1;
}

: >"$TMP/whisper-block-at"
RETRY_STREAM=$(curl -fsS -m 15 -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"audio-cancel-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$CANCEL_ID\"],\"stream\":true}")
printf '%s' "$RETRY_STREAM" | grep -q 'Reused transcript window 1 of 3' || {
  echo "FAIL: retry did not report its durable window cache hit"; exit 1;
}
printf '%s' "$RETRY_STREAM" | grep -q 'Transcribed window 2 of 3' || exit 1
printf '%s' "$RETRY_STREAM" | grep -q 'Transcribed window 3 of 3' || exit 1
printf '%s' "$RETRY_STREAM" | grep -q 'saw durable audio transcript' || exit 1
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 7 ] || {
  echo "FAIL: retry did not transcribe exactly the two missing windows"; exit 1;
}
CANCEL_EVIDENCE=$(find "$HOME_DIR/attachments" -name "$CANCEL_ID.transcript.json" -print -quit)
[ -n "$CANCEL_EVIDENCE" ] && [ -s "$CANCEL_EVIDENCE" ] || {
  echo "FAIL: retry did not publish final evidence"; exit 1;
}

# A truncated ID3 header must take the typed compressed-audio failure path,
# never fall through to document text or generic video handling.
printf 'ID3' >"$TMP/malformed.mp3"
STATUS=$(curl -sS -o "$TMP/mp3.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary @"$TMP/malformed.mp3")
[ "$STATUS" = 415 ] || { cat "$TMP/mp3.json"; exit 1; }
if [ "$COMPRESSED_AUDIO" = 1 ]; then
  grep -q 'unsupported_audio_codec' "$TMP/mp3.json" || exit 1
else
  grep -q 'compressed_audio_decoder_unavailable' "$TMP/mp3.json" || exit 1
fi

if [ "$COMPRESSED_AUDIO" = 1 ]; then
  M4A_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    -H 'X-Samosa-Media-Type: video/mp4' \
    -H "X-Samosa-Filename-B64: $(printf 'field-recording.m4a' | base64)" \
    -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    --data-binary @"$TMP/podcast.m4a")
  M4A_ID=$(printf '%s' "$M4A_UPLOAD" | checks id)
  printf '%s' "$M4A_UPLOAD" | checks m4a-upload

  M4A_RESPONSE=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"m4a-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$M4A_ID\"],\"stream\":false}")
  printf '%s' "$M4A_RESPONSE" | grep -q 'saw durable audio transcript' || {
    echo "FAIL: decoded M4A transcript did not reach the model: $M4A_RESPONSE"
    exit 1
  }
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 9 ] || {
    echo "FAIL: 14-second M4A should decode/transcribe as two bounded windows"
    exit 1
  }
  M4A_EVIDENCE=$(find "$HOME_DIR/attachments" -name "$M4A_ID.transcript.json" -print -quit)
  [ -n "$M4A_EVIDENCE" ] && [ -s "$M4A_EVIDENCE" ] || {
    echo "FAIL: M4A transcript evidence was not published"
    exit 1
  }
  checks m4a-evidence "$M4A_EVIDENCE" "$M4A_ID"

  # The compressed source, decoder fingerprint, and transcript evidence are
  # all durable; a process restart must not invoke either decode/transcribe
  # path again for the same attachment.
  stop_gateway
  start_gateway
  M4A_CACHED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"m4a-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$M4A_ID\"],\"stream\":false}")
  printf '%s' "$M4A_CACHED" | grep -q 'saw durable audio transcript' || exit 1
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 9 ] || {
    echo "FAIL: gateway restart retranscribed cached M4A evidence"
    exit 1
  }

  MP3_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    -H 'X-Samosa-Media-Type: application/octet-stream' \
    -H "X-Samosa-Filename-B64: $(printf 'voice-note.mp3' | base64)" \
    -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    --data-binary @"$TMP/tone.mp3")
  MP3_ID=$(printf '%s' "$MP3_UPLOAD" | checks id)
  printf '%s' "$MP3_UPLOAD" | checks mp3-upload
  STATUS=$(curl -sS -o "$TMP/mp3-question.json" -w '%{http_code}' \
    -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"mp3-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$MP3_ID\"],\"stream\":false}")
  [ "$STATUS" = 200 ] || {
    sed -n '1,80p' "$TMP/mp3-question.json"
    sed -n '1,120p' "$TMP/stderr.log"
    echo "FAIL: decoded MP3 question returned HTTP $STATUS"
    exit 1
  }
  MP3_RESPONSE=$(sed -n '1,$p' "$TMP/mp3-question.json")
  printf '%s' "$MP3_RESPONSE" | grep -q 'saw durable audio transcript' || {
    echo "FAIL: decoded MP3 transcript did not reach the model: $MP3_RESPONSE"
    exit 1
  }
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 10 ] || {
    echo "FAIL: short MP3 should invoke Whisper exactly once"
    exit 1
  }
  MP3_EVIDENCE=$(find "$HOME_DIR/attachments" -name "$MP3_ID.transcript.json" -print -quit)
  [ -n "$MP3_EVIDENCE" ] && [ -s "$MP3_EVIDENCE" ] || {
    echo "FAIL: MP3 transcript evidence was not published"
    exit 1
  }
  MP3_CACHED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"mp3-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$MP3_ID\"],\"stream\":false}")
  printf '%s' "$MP3_CACHED" | grep -q 'saw durable audio transcript' || exit 1
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 10 ] || {
    echo "FAIL: repeat question retranscribed cached MP3 evidence"
    exit 1
  }

  # Stop must also own the decode child that runs before Whisper. A wrapper
  # delegates real probes but pauses the first bounded decode until SIGTERM.
  stop_gateway
  ACTIVE_AUDIO_DECODER="$TMP/audio-decode-wrapper"
  : >"$TMP/decode-block"
  rm -f "$TMP/decode-block-ready" "$TMP/decode-release"
  start_gateway
  DECODE_CANCEL_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    -H "X-Samosa-Filename-B64: $(printf 'decode-cancel.m4a' | base64)" \
    -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    --data-binary @"$TMP/cancel.m4a")
  DECODE_CANCEL_ID=$(printf '%s' "$DECODE_CANCEL_UPLOAD" | checks duration-id 24.9 25.1)
  curl -sS -m 15 -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"m4a-decode-cancel\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$DECODE_CANCEL_ID\"],\"stream\":true}" \
    >"$TMP/decode-cancel-stream.txt" &
  STREAM_PID=$!
  i=0
  while [ "$i" -lt 250 ] && [ ! -e "$TMP/decode-block-ready" ]; do
    sleep 0.02
    i=$((i + 1))
  done
  [ -e "$TMP/decode-block-ready" ] || {
    echo "FAIL: compressed cancellation fixture never entered the decoder"
    exit 1
  }
  CANCEL_RESPONSE=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/cancel")
  printf '%s' "$CANCEL_RESPONSE" | grep -q '"cancelled":true' || exit 1
  wait "$STREAM_PID" || true
  grep -q 'audio_transcription_cancelled' "$TMP/decode-cancel-stream.txt" || {
    sed -n '1,100p' "$TMP/decode-cancel-stream.txt"
    echo "FAIL: cancelling the decoder omitted the typed audio error"
    exit 1
  }
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 10 ] || {
    echo "FAIL: cancellation during decode should not launch Whisper"
    exit 1
  }
  [ -z "$(find "$HOME_DIR/attachments" -name "$DECODE_CANCEL_ID.transcript.window-*.json" -print -quit)" ] || {
    echo "FAIL: interrupted decode produced a transcript checkpoint"
    exit 1
  }
  [ -z "$(find "$HOME_DIR/attachments" -name "$DECODE_CANCEL_ID.transcript.json" -print -quit)" ] || {
    echo "FAIL: interrupted decode produced final transcript evidence"
    exit 1
  }

  # Retry with the packaged decoder and prove the interrupted temporary WAV
  # left no false cache state: all three windows must run and publish.
  stop_gateway
  ACTIVE_AUDIO_DECODER="$AUDIO_DECODER"
  rm -f "$TMP/decode-block"
  start_gateway
  DECODE_RETRY=$(curl -fsS -m 20 -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"m4a-decode-cancel\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$DECODE_CANCEL_ID\"],\"stream\":false}")
  printf '%s' "$DECODE_RETRY" | grep -q 'saw durable audio transcript' || exit 1
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = 13 ] || {
    echo "FAIL: compressed retry did not transcribe exactly three windows"
    exit 1
  }

  # A video with a probe-qualified AAC track is still one video source, but
  # Auto can select speech transcription without loading Molmo for a dialogue
  # question. The transcript then follows the same durable conversation and
  # compaction path as a podcast.
  stop_gateway
  ACTIVE_AUDIO_DECODER="$TMP/video-audio-decode-wrapper"
  start_gateway
  CAPS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/capabilities/sources")
  printf '%s' "$CAPS" | checks caps-video

  VIDEO_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
    -H 'X-Samosa-Media-Type: audio/mp4' \
    -H "X-Samosa-Filename-B64: $(printf 'talking-clip.mp4' | base64)" \
    -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    --data-binary @"$TMP/talking.mp4")
  VIDEO_AUDIO_ID=$(printf '%s' "$VIDEO_UPLOAD" | checks video-audio-id)
  WHISPER_BEFORE=$(wc -l <"$TMP/whisper-count" | tr -d ' ')
  VIDEO_AUDIO_RESPONSE=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"video-audio-source-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment video audio probe: what is being said?\"}],\"attachment_ids\":[\"$VIDEO_AUDIO_ID\"],\"stream\":false}")
  printf '%s' "$VIDEO_AUDIO_RESPONSE" | grep -q 'saw routed video audio transcript' || {
    echo "FAIL: Auto did not route the video dialogue question through Whisper: $VIDEO_AUDIO_RESPONSE"
    exit 1
  }
  WHISPER_AFTER=$(wc -l <"$TMP/whisper-count" | tr -d ' ')
  [ "$WHISPER_AFTER" = "$((WHISPER_BEFORE + 1))" ] || {
    echo "FAIL: the short video audio track should invoke Whisper exactly once"
    exit 1
  }
  VIDEO_EVIDENCE=$(find "$HOME_DIR/attachments" -name "$VIDEO_AUDIO_ID.transcript.json" -print -quit)
  [ -n "$VIDEO_EVIDENCE" ] && [ -s "$VIDEO_EVIDENCE" ] || {
    echo "FAIL: video audio transcript evidence was not durable"
    exit 1
  }
  VIDEO_MANIFEST="$HOME_DIR/chats/video-audio-source-probe/documents.json"
  checks video-manifest "$VIDEO_MANIFEST" "$VIDEO_AUDIO_ID"

  VIDEO_AUDIO_FOLLOWUP=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"model":"qwen3.6-35b-a3b","model_id":"qwen","model_version":"qwen-model","conversation_id":"video-audio-source-probe","messages":[{"role":"user","content":"attachment video audio probe: what is being said?"}],"stream":false}')
  printf '%s' "$VIDEO_AUDIO_FOLLOWUP" | grep -q 'saw routed video audio transcript' || {
    echo "FAIL: bound video transcript was not reused on follow-up"
    exit 1
  }
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = "$WHISPER_AFTER" ] || {
    echo "FAIL: follow-up retranscribed durable video evidence"
    exit 1
  }
  COMPACT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
    "http://127.0.0.1:$PORT/v1/compact" \
    -H 'Content-Type: application/json' \
    -d '{"conversation_id":"video-audio-source-probe"}')
  printf '%s' "$COMPACT" | grep -q 'saw pinned compaction context' || {
    echo "FAIL: compaction did not retain the bound video transcript"
    exit 1
  }
  [ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = "$WHISPER_AFTER" ] || {
    echo "FAIL: compaction retranscribed durable video evidence"
    exit 1
  }
fi

# Window counting must use the same one-second overlap as window traversal.
# With ten-second windows, a precisely thirty-second source needs four
# windows (0-10, 9-19, 18-28, 27-30), not the naïve three. This is the shape
# that caught incomplete tail coverage in the one-hour real-model gate.
EXACT_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  -H "X-Samosa-Filename-B64: $(printf 'exact-window.wav' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary @"$TMP/exact-window.wav")
EXACT_ID=$(printf '%s' "$EXACT_UPLOAD" | checks duration-id 30)
WHISPER_BEFORE=$(wc -l <"$TMP/whisper-count" | tr -d ' ')
EXACT_RESPONSE=$(curl -fsS -m 20 -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"audio-exact-window-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment audio probe: where is the podcast sentinel?\"}],\"attachment_ids\":[\"$EXACT_ID\"],\"stream\":false}")
printf '%s' "$EXACT_RESPONSE" | grep -q 'saw durable audio transcript' || {
  echo "FAIL: exact-window transcript did not reach the model: $EXACT_RESPONSE"
  exit 1
}
WHISPER_AFTER=$(wc -l <"$TMP/whisper-count" | tr -d ' ')
[ "$WHISPER_AFTER" = "$((WHISPER_BEFORE + 4))" ] || {
  echo "FAIL: 30 seconds with 10-second windows and overlap must use four windows"
  exit 1
}
EXACT_EVIDENCE=$(find "$HOME_DIR/attachments" -name "$EXACT_ID.transcript.json" -print -quit)
checks exact-evidence "$EXACT_EVIDENCE" "$EXACT_ID" "$HOME_DIR/attachments"

# Repeated high-scoring middle evidence must not crowd a separately requested
# late fact out of the bounded transcript handoff.
RANKING_UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  -H "X-Samosa-Filename-B64: $(printf 'ranking.wav' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary @"$TMP/ranking.wav")
RANKING_ID=$(printf '%s' "$RANKING_UPLOAD" | checks duration-id 271)
RANKING_BEFORE=$(wc -l <"$TMP/whisper-count" | tr -d ' ')
printf '%s %s\n' "$RANKING_BEFORE" 30 >"$TMP/whisper-ranking"
RANKING_RESPONSE=$(curl -fsS -m 30 -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"conversation_id\":\"audio-balanced-retrieval-probe\",\"messages\":[{\"role\":\"user\",\"content\":\"audio balanced retrieval probe: report the early, middle, and late ranking sentinels\"}],\"attachment_ids\":[\"$RANKING_ID\"],\"stream\":false}")
rm -f "$TMP/whisper-ranking"
printf '%s' "$RANKING_RESPONSE" | grep -q 'saw balanced audio retrieval' || {
  echo "FAIL: term-balanced audio retrieval omitted a requested endpoint: $RANKING_RESPONSE"
  exit 1
}
[ "$(wc -l <"$TMP/whisper-count" | tr -d ' ')" = "$((RANKING_BEFORE + 30))" ] || {
  echo "FAIL: ranking fixture did not transcribe its expected 30 windows"
  exit 1
}

echo "test_audio_attachments.sh: PASS"
