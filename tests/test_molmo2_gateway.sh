#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
MM_HELPER="${SAMOSA_FAKE_MM_HELPER:-./$BUILD_DIR/fake-multimodal-helper}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/molmo2_gateway_test.XXXXXX")
HOME_DIR="$TMP/home"
MODEL_DIR="$TMP/molmo2"
VISIONPSY_MODEL_DIR="$TMP/visionpsy"
PORT=19024
GW_PID=""

cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/qwen-model" "$MODEL_DIR" "$VISIONPSY_MODEL_DIR"
printf '<!doctype html><title>Molmo fixture</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"
printf 'w\n' >"$MODEL_DIR/weights.safetensors"
printf 't\n' >"$MODEL_DIR/tokenizer.json"
printf '{}\n' >"$MODEL_DIR/config.json"
printf '{}\n' >"$MODEL_DIR/processor.json"
cat >"$MODEL_DIR/manifest.json" <<'JSON'
{"format":"samosa.molmo2.mlx.v1","package_id":"molmo2-4b-mlx-q4-v1","model_id":"allenai/Molmo2-4B","upstream_revision":"042abfa7a38879a376cec03d949eff0aefaa0600","processor_fingerprint":"808de9add76144a557348c5f5180a8408b12ca83592c7a6a257ae69c968e51df","estimated_resident_bytes":300000000,"quantization":{"mode":"affine","bits":4,"group_size":64},"files":[{"name":"weights.safetensors","role":"weights","bytes":2,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"},{"name":"tokenizer.json","role":"tokenizer","bytes":2,"sha256":"95e80901c901584f416b8fd4349fd60022774b89ba4377626511f0562cc599f7"},{"name":"config.json","role":"metadata","bytes":3,"sha256":"17e072e3c3b29d9be7a348c74b88658e67ccce094c31b21f87646f6cecd2a76f"},{"name":"processor.json","role":"metadata","bytes":3,"sha256":"808de9add76144a557348c5f5180a8408b12ca83592c7a6a257ae69c968e51df"}]}
JSON
printf '\000\000\000\030ftypisom\000\000\002\000isomiso2fixture-video' >"$TMP/clip.mp4"
printf '\211PNG\r\n\032\nfixture-a' >"$TMP/a.png"
printf '\211PNG\r\n\032\nfixture-b' >"$TMP/b.png"

# Make the smaller specialist look fully installed as well. Its executable is
# a tripwire: an installed Molmo package must win standard-image routing and
# this fallback must never be started.
dd if=/dev/zero of="$VISIONPSY_MODEL_DIR/model.safetensors" bs=1 count=1 seek=1014772919 2>/dev/null
for file in config.json preprocessor_config.json processor_config.json tokenizer.json tokenizer_config.json chat_template.jinja; do
  printf 'fixture\n' >"$VISIONPSY_MODEL_DIR/$file"
done
printf '#!/bin/sh\nprintf invoked >"%s"\nexit 91\n' "$TMP/visionpsy-invoked" >"$TMP/should-not-run-visionpsy"
chmod +x "$TMP/should-not-run-visionpsy"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_QWEN_ENGINE="$BACKEND" \
SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
SAMOSA_VISIONPSY_ENGINE="$TMP/should-not-run-visionpsy" \
SAMOSA_VISIONPSY_MODEL="$VISIONPSY_MODEL_DIR" \
SAMOSA_MOLMO2_ENGINE="$(CDPATH= cd -- "$(dirname "$MM_HELPER")" && pwd)/$(basename "$MM_HELPER")" \
SAMOSA_MOLMO2_MODEL="$MODEL_DIR" \
SAMOSA_FAKE_MM_FRAMED=1 \
SAMOSA_FAKE_MM_LOG="$TMP/molmo-commands.jsonl" \
SAMOSA_FAKE_MM_VISUAL_DELAY_MS=800 \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
GW_PID=$!
i=0
while [ "$i" -lt 120 ]; do
  HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$HEALTH" | grep -q '"ready":true' && break
  sleep 0.05; i=$((i + 1))
done
printf '%s' "$HEALTH" | grep -q '"supports_video_attachments":true' || {
  echo "FAIL: health did not expose the bundled Molmo video runtime: $HEALTH"
  sed -n '1,160p' "$TMP/stderr.log" >&2
  exit 1
}
printf '%s' "$HEALTH" | grep -q '"molmo2_model_ready":true' || {
  echo "FAIL: the exact fixture package was not admitted: $HEALTH"; exit 1;
}
TOKEN=$(cat "$HOME_DIR/run/ui-token")
UPLOAD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -X POST \
  "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@$TMP/clip.mp4")
VIDEO_ID=$(printf '%s' "$UPLOAD" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
[ ${#VIDEO_ID} = 64 ] || { echo "FAIL: video upload failed: $UPLOAD"; exit 1; }

REPLY=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment video probe: when does the visible event happen?\"}],\"attachment_ids\":[\"$VIDEO_ID\"],\"stream\":false}")
printf '%s' "$REPLY" | grep -q 'saw bounded Molmo video evidence' || {
  echo "FAIL: bounded Molmo evidence did not reach primary synthesis: $REPLY"; exit 1;
}

UPLOAD_A=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: image/png' \
  -H 'X-Filename: a.png' -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary "@$TMP/a.png")
UPLOAD_B=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: image/png' \
  -H 'X-Filename: b.png' -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  --data-binary "@$TMP/b.png")
IMAGE_A=$(printf '%s' "$UPLOAD_A" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
IMAGE_B=$(printf '%s' "$UPLOAD_B" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
[ ${#IMAGE_A} = 64 ] && [ ${#IMAGE_B} = 64 ] || {
  echo "FAIL: extended-image fixture upload failed: $UPLOAD_A $UPLOAD_B"; exit 1;
}
REPLY=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"system\",\"content\":\"ORIGINAL_UI_SYSTEM_CONTEXT\"},{\"role\":\"user\",\"content\":\"Molmo single image grounding probe: explain this image in great detail.\"}],\"attachment_ids\":[\"$IMAGE_A\"],\"stream\":false,\"thinking\":\"on\",\"temperature\":0.9,\"chat_template_kwargs\":{\"enable_thinking\":true}}")
printf '%s' "$REPLY" | grep -q 'There are 12 charts in a 3 by 4 grid' || {
  echo "FAIL: standard image did not use grounded Molmo synthesis: $REPLY"; exit 1;
}
grep -q 'transcribe legible text exactly' "$TMP/molmo-commands.jsonl" || {
  echo "FAIL: detailed image prompt did not require exact visible-text transcription"; exit 1;
}
[ ! -e "$TMP/visionpsy-invoked" ] || {
  echo "FAIL: installed VisionPsy was started instead of installed Molmo"; exit 1;
}
REPLY=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"system\",\"content\":\"ORIGINAL_UI_SYSTEM_CONTEXT\"},{\"role\":\"user\",\"content\":\"what is this?\"}],\"attachment_ids\":[\"$IMAGE_A\"],\"stream\":false}")
printf '%s' "$REPLY" | grep -q 'black-and-white circular mandala' || {
  echo "FAIL: vague image question received a visually poisoned specialist prompt: $REPLY"; exit 1;
}
REPLY=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"Molmo extended image probe: compare these images spatially.\"}],\"attachment_ids\":[\"$IMAGE_A\",\"$IMAGE_B\"],\"stream\":false}")
printf '%s' "$REPLY" | grep -q 'saw extended Molmo image evidence' || {
  echo "FAIL: extended multi-image work did not route through Molmo: $REPLY"; exit 1;
}

# A UI conversation that begins with one visual must use Molmo only as the
# evidence specialist, then hand the observation to the selected text model.
# Returning Molmo directly here bypasses the selected model's system prompt
# and can surface a generic "I cannot view images" answer in the app.
DELEGATED_PATH="$TMP/delegated-visual.stream"
curl -N -sS --max-time 10 -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"system\",\"content\":\"UI context\"},{\"role\":\"user\",\"content\":\"what is this image?\"}],\"attachment_ids\":[\"$IMAGE_A\"],\"conversation_id\":\"initial_visual_fixture\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"stream\":true}" \
  >"$DELEGATED_PATH" &
DELEGATED_PID=$!
SPECIALIST_HEALTH=""
i=0
while kill -0 "$DELEGATED_PID" 2>/dev/null && [ "$i" -lt 100 ]; do
  SPECIALIST_HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$SPECIALIST_HEALTH" | grep -q '"backend_state":"specialist"' && break
  sleep 0.02
  i=$((i + 1))
done
printf '%s' "$SPECIALIST_HEALTH" | grep -q '"backend":"qwen"' || {
  echo "FAIL: selected text model identity disappeared during visual handoff: $SPECIALIST_HEALTH"; exit 1;
}
printf '%s' "$SPECIALIST_HEALTH" | grep -q '"specialist_active":true' || {
  echo "FAIL: health did not expose active visual specialist: $SPECIALIST_HEALTH"; exit 1;
}
printf '%s' "$SPECIALIST_HEALTH" | grep -q '"specialist_model":"Molmo2 4B"' || {
  echo "FAIL: health did not name the active visual specialist: $SPECIALIST_HEALTH"; exit 1;
}
printf '%s' "$SPECIALIST_HEALTH" | grep -q '"installed":true' || {
  echo "FAIL: selected text model looked unconfigured during visual handoff: $SPECIALIST_HEALTH"; exit 1;
}
wait "$DELEGATED_PID"
DELEGATED=$(cat "$DELEGATED_PATH")
printf '%s' "$DELEGATED" | grep -q 'Preparing this image for local visual analysis before Qwen3.6 35B A3B answers' || {
  echo "FAIL: visual stream did not immediately explain the handoff: $DELEGATED"; exit 1;
}
printf '%s' "$DELEGATED" | grep -q 'Using Molmo2 4B vision model to process this image; Qwen3.6 35B A3B remains the selected answering model' || {
  echo "FAIL: visual stream did not name both model roles: $DELEGATED"; exit 1;
}
printf '%s' "$DELEGATED" | grep -q 'Vision analysis is complete. Loading Qwen3.6 35B A3B to answer' || {
  echo "FAIL: visual stream did not expose the specialist-to-text handoff: $DELEGATED"; exit 1;
}
printf '%s' "$DELEGATED" | grep -q 'Qwen3.6 35B A3B is answering from the local image analysis' || {
  echo "FAIL: visual stream did not identify the answering model: $DELEGATED"; exit 1;
}
printf '%s' "$DELEGATED" | grep -q 'This is a black-and-white geometric pattern' || {
  echo "FAIL: first visual turn did not reach primary-model synthesis: $DELEGATED"; exit 1;
}
if printf '%s' "$DELEGATED" | grep -q '"provider":"molmo2_4b"'; then
  echo "FAIL: first visual turn bypassed primary synthesis: $DELEGATED"; exit 1;
fi
if printf '%s' "$DELEGATED" | grep -qi 'cannot .*\(view\|access\).*image\|upload it again'; then
  echo "FAIL: first visual turn returned an image-access refusal: $DELEGATED"; exit 1;
fi
printf '%s' "$DELEGATED" | grep -q 'data: \[DONE\]' || {
  echo "FAIL: delegated visual stream did not terminate cleanly: $DELEGATED"; exit 1;
}
# The synthesis path waits for the selected text model to recover before it
# forwards the observation, so readiness must already be restored here.
HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
printf '%s' "$HEALTH" | grep -q '"ready":true' || {
  echo "FAIL: selected text model was not ready after visual synthesis: $HEALTH"; exit 1;
}
DELEGATED_VIDEO=$(curl -sS --max-time 10 -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"What happens in this video?\"}],\"attachment_ids\":[\"$VIDEO_ID\"],\"conversation_id\":\"initial_video_fixture\",\"model_id\":\"qwen\",\"model_version\":\"qwen-model\",\"stream\":false}")
printf '%s' "$DELEGATED_VIDEO" | grep -q 'saw bounded Molmo video evidence' || {
  echo "FAIL: first video turn did not reach primary-model synthesis: $DELEGATED_VIDEO"; exit 1;
}
if printf '%s' "$DELEGATED_VIDEO" | grep -q '"provider":"molmo2_4b"'; then
  echo "FAIL: first video turn bypassed primary synthesis: $DELEGATED_VIDEO"; exit 1;
fi

# The specialist lease must be released before synthesis. The only remaining
# child is the restarted fake primary backend.
sleep 0.1
CHILDREN=$(pgrep -P "$GW_PID" 2>/dev/null | wc -l | tr -d ' ')
[ "$CHILDREN" = "1" ] || { echo "FAIL: expected only the primary backend after Molmo teardown, found $CHILDREN children"; exit 1; }

# Molmo is also a directly selectable visual-chat backend. Selecting it must
# stop the resident primary without preloading Molmo, and a turn must return
# the specialist's own observation rather than sending it through Qwen.
SELECTED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/backends/select" \
  -d '{"backend":"molmo2-4b-mlx-q4-v1","model_version":"042abfa7a38879a376cec03d949eff0aefaa0600-q4-v1"}')
printf '%s' "$SELECTED" | grep -q '"state":"selected"' || {
  echo "FAIL: direct Molmo selection was not committed: $SELECTED"; exit 1;
}
HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
printf '%s' "$HEALTH" | grep -q '"backend":"molmo2-4b-mlx-q4-v1"' || {
  echo "FAIL: health did not expose direct Molmo as active: $HEALTH"; exit 1;
}
printf '%s' "$HEALTH" | grep -q '"ready":true' || {
  echo "FAIL: verified direct Molmo did not report ready: $HEALTH"; exit 1;
}
printf '%s' "$HEALTH" | grep -q '"pid":0' || {
  echo "FAIL: selecting direct Molmo preloaded a resident process: $HEALTH"; exit 1;
}
printf '%s' "$HEALTH" | grep -q '"supports_documents":false' || {
  echo "FAIL: direct Molmo falsely advertised document chat: $HEALTH"; exit 1;
}

NO_VISUAL=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d '{"model":"molmo2-4b-mlx-q4-v1","messages":[{"role":"user","content":"hello"}],"stream":false}')
printf '%s' "$NO_VISUAL" | grep -q '"code":"molmo2_visual_required"' || {
  echo "FAIL: direct Molmo text-only turn was not rejected honestly: $NO_VISUAL"; exit 1;
}

DIRECT=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"molmo2-4b-mlx-q4-v1\",\"messages\":[{\"role\":\"user\",\"content\":\"Describe this directly.\"}],\"attachment_ids\":[\"$IMAGE_A\"],\"conversation_id\":\"direct_fixture\",\"model_id\":\"molmo2-4b-mlx-q4-v1\",\"model_version\":\"042abfa7a38879a376cec03d949eff0aefaa0600-q4-v1\",\"stream\":true}")
printf '%s' "$DIRECT" | grep -q 'fixture image evidence: a 3 by 4 grid containing 12 charts' || {
  echo "FAIL: direct Molmo did not return its own generated text: $DIRECT"; exit 1;
}
printf '%s' "$DIRECT" | grep -q '"provider":"molmo2_4b"' || {
  echo "FAIL: direct response omitted Molmo provenance: $DIRECT"; exit 1;
}
printf '%s' "$DIRECT" | grep -q 'data: \[DONE\]' || {
  echo "FAIL: direct Molmo stream did not terminate cleanly: $DIRECT"; exit 1;
}
sleep 0.1
CHILDREN=$(pgrep -P "$GW_PID" 2>/dev/null | wc -l | tr -d ' ')
[ "$CHILDREN" = "0" ] || { echo "FAIL: direct Molmo left $CHILDREN resident child processes"; exit 1; }

echo "molmo2 gateway: PASS"
