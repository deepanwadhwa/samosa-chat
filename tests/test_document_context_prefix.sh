#!/bin/sh
set -eu

# Backend-neutral document reuse for llama.cpp text models.  The second turn
# must reconstruct the same first-user prompt prefix, request llama.cpp prompt
# caching, and keep recovery-only pinned context out of the hot request.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
EXTRACT="${SAMOSA_EXTRACT:-./$BUILD_DIR/samosa-extract}"
OCR="${SAMOSA_OCR:-./$BUILD_DIR/samosa-ocr}"

if [ ! -x "$EXTRACT" ]; then
  echo "test_document_context_prefix.sh: SKIP (no samosa-extract build)"
  exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/document_prefix_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=19022
GW_PID=""

cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/models/ornith-9b" "$HOME_DIR/qwen-model"
printf 'fixture\n' >"$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'ornith\n' >"$HOME_DIR/model-backend"
printf '<!doctype html><title>Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_QWEN_ENGINE="$BACKEND" \
SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
SAMOSA_BONSAI_SERVER="$BACKEND" \
SAMOSA_ORNITH_MODEL="$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_EXTRACT="$EXTRACT" \
SAMOSA_OCR="$OCR" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
GW_PID=$!

i=0
while [ "$i" -lt 100 ]; do
  HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$HEALTH" | grep -q '"ready":true' &&
    printf '%s' "$HEALTH" | grep -q '"backend":"ornith"' && break
  sleep 0.05
  i=$((i + 1))
done
TOKEN=$(cat "$HOME_DIR/run/ui-token")
HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
MODEL_VERSION=$(printf '%s' "$HEALTH" | python3 -c 'import json,sys; print(json.load(sys.stdin)["model_version"])')

python3 - "$TMP/notes.txt" <<'PY'
import sys
with open(sys.argv[1], "w", encoding="utf-8") as f:
    f.write("BEGIN_FILE\n")
    f.write("A" * 7200)
    f.write("\nLATE_FILE_SENTINEL\n")
PY
RESP=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  -H "X-Samosa-Filename-B64: $(printf 'notes.txt' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@$TMP/notes.txt")
ATTACHMENT_ID=$(printf '%s' "$RESP" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
CONVERSATION_ID=ornith-stable-prefix

RESP=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"ornith-1.0-9b\",\"model_id\":\"ornith\",\"model_version\":\"$MODEL_VERSION\",\"conversation_id\":\"$CONVERSATION_ID\",\"messages\":[{\"role\":\"user\",\"content\":\"stable prefix initial probe\"}],\"attachment_ids\":[\"$ATTACHMENT_ID\"],\"stream\":false}")
printf '%s' "$RESP" | grep -q 'stable prefix initial ok' || {
  echo "FAIL: first Ornith document turn did not establish a cacheable prefix: $RESP"; exit 1;
}
[ -s "$HOME_DIR/chats/$CONVERSATION_ID/document-context.txt" ] || {
  echo "FAIL: stable document context sidecar was not saved"; exit 1;
}

RESP=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -d "{\"model\":\"ornith-1.0-9b\",\"model_id\":\"ornith\",\"model_version\":\"$MODEL_VERSION\",\"conversation_id\":\"$CONVERSATION_ID\",\"messages\":[{\"role\":\"user\",\"content\":\"stable prefix initial probe\"},{\"role\":\"assistant\",\"content\":\"stable prefix initial ok\"},{\"role\":\"user\",\"content\":\"stable prefix followup probe\"}],\"stream\":false}")
printf '%s' "$RESP" | grep -q 'stable prefix followup ok' || {
  echo "FAIL: Ornith follow-up did not reuse a stable cached prefix: $RESP"; exit 1;
}

echo "test_document_context_prefix.sh: PASS"
