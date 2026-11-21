#!/bin/sh
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
checks() { node "$REPO_ROOT/tests/visionpsy_e2e_checks.mjs" "$@"; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/samosa-visionpsy-e2e-XXXXXX")"

mkdir -p "$TMP/bin" "$TMP/models/visionpsy" "$TMP/home" "$TMP/jobs" "$TMP/current/bin" "$TMP/current/models"

# Deterministic local PDF fixture reader. It exposes three pages and renders
# each to a turn-scoped file; the fake vision helper does not need image bytes.
cat << 'EOF' > "$TMP/bin/fake-extract"
#!/bin/sh
case "$1" in
  --version) echo 'samosa-extract-test-v1' ;;
  --json) echo '{"ok":true,"text_layer":true,"page_count":3,"text":"PAGE ONE\nPAGE TWO\nPAGE THREE","tokens":6,"tokens_estimate":6}' ;;
  --json-pages)
    start="$3"
    echo "{\"ok\":true,\"text_layer\":true,\"page_count\":3,\"page_start\":$start,\"page_end\":$start,\"pages\":[{\"index\":$start,\"text_chars\":8,\"tokens\":2,\"has_raster_figure\":true,\"text\":\"PAGE $start\"}],\"text\":\"PAGE $start\",\"tokens_estimate\":2}"
    ;;
  --render-ppm) : > "$4"; echo "{\"ok\":true,\"page\":$3,\"format\":\"image/x-portable-pixmap\"}" ;;
  *) exit 64 ;;
esac
EOF
chmod +x "$TMP/bin/fake-extract"

cat << 'EOF' > "$TMP/bin/fake-ocr"
#!/bin/sh
if [ "${1:-}" = "--version" ]; then echo 'samosa-ocr-test-v1'; exit 0; fi
if [ "${1:-}" = "read" ]; then
  echo '{"ok":true,"text":"LINE ONE\nLINE TWO","lines":[{"text":"LINE ONE","conf":0.99},{"text":"LINE TWO","conf":0.99}]}'
  exit 0
fi
exit 64
EOF
chmod +x "$TMP/bin/fake-ocr"

echo "Building test binaries..."
make samosa-gateway samosa-visionpsy test-visionpsy-components
cc -O2 -Wall -Wextra -std=c11 -pthread -Isrc tests/fake_openai_backend.c -o "$BUILD_DIR/test_fake_openai_backend"

PORT=9876
BACKEND_PORT=9877

# Create mock ornith model file so backend is available
mkdir -p "$TMP/home/models/ornith-9b"
touch "$TMP/home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'ornith\n' > "$TMP/home/model-backend"

start_gateway() {
    SAMOSA_PORT="$PORT" \
    SAMOSA_BACKEND_PORT="$BACKEND_PORT" \
    SAMOSA_HOME="$TMP/home" \
    SAMOSA_MODELS_CATALOG="$REPO_ROOT/assets/models.json" \
    SAMOSA_BONSAI_SERVER="$BUILD_DIR/test_fake_openai_backend" \
    SAMOSA_EXTRACT="$TMP/bin/fake-extract" \
    SAMOSA_OCR="$TMP/bin/fake-ocr" \
    SAMOSA_VISIONPSY_ENGINE="${1:-$BUILD_DIR/samosa-visionpsy}" \
    SAMOSA_VISIONPSY_MODEL="$TMP/models/visionpsy" \
    SAMOSA_VISIONPSY_TEST_LOG="$TMP/visionpsy-commands.jsonl" \
    "$BUILD_DIR/samosa-gateway" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
    GATEWAY_PID=$!

    i=0
    health=""
    while [ "$i" -lt 100 ]; do
        health=$(curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
        printf '%s' "$health" | grep -q '"ready":true' && break
        sleep 0.05
        i=$((i + 1))
    done
    TOKEN=$(cat "$TMP/home/run/ui-token")
}

cleanup() {
    if [ -n "${GATEWAY_PID:-}" ]; then
        kill -TERM "$GATEWAY_PID" 2>/dev/null || true
        wait "$GATEWAY_PID" 2>/dev/null || true
        GATEWAY_PID=""
    fi
}
trap 'cleanup; rm -rf "$TMP"' EXIT

# 1. Start gateway with non-existent visionpsy model dir and test catalog validation and model properties
start_gateway "$BUILD_DIR/samosa-visionpsy"

HEALTH=$(curl -sS "http://127.0.0.1:$PORT/healthz")
printf '%s' "$HEALTH" | checks health

# Fetch catalog with token
curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/catalog" > "$TMP/catalog.json"

checks catalog "$TMP/catalog.json"

# 2. Verify VisionPsy cannot be selected as active chat model
SELECT_RES=$(curl -s -w "\n%{http_code}" -X POST "http://127.0.0.1:$PORT/v1/backends/select" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"backend":"visionpsy-nano-460m-mlx-bf16"}')

HTTP_CODE=$(echo "$SELECT_RES" | tail -n 1)
if [ "$HTTP_CODE" != "400" ]; then
    echo "Expected 400 when selecting auxiliary model as chat backend, got $HTTP_CODE" >&2
    exit 1
fi
echo "Auxiliary model selection rejection: PASS"

# 3. Test missing VisionPsy error code when attaching an image
# Create an image attachment
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15c4\x00\x00\x00\nIDATx\x9cc\x00\x01\x00\x00\x05\x00\x01\r\n-\xb4\x00\x00\x00\x00IEND\xaeB`\x82' > "$TMP/test.png"

ATTACH_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: image/png" \
    -H "X-Filename: test.png" \
    --data-binary @"$TMP/test.png")

ATTACH_ID=$(echo "$ATTACH_RES" | checks id)
if [ -z "$ATTACH_ID" ]; then
    echo "Failed to upload attachment: $ATTACH_RES" >&2
    exit 1
fi
echo "Attachment uploaded: $ATTACH_ID"

# Send a chat request referencing this attachment with backend 'ornith' (which doesn't natively take images)
CHAT_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{
        \"attachment_ids\": [\"$ATTACH_ID\"],
        \"messages\": [
            {
                \"role\": \"user\",
                \"content\": \"What is in this image?\"
            }
        ]
    }")

printf '%s' "$CHAT_RES" | checks missing-model

# 4. Test IPC helper communication with all 7 pinned files in place
mkdir -p "$TMP/models/visionpsy"
checks model-fixture "$TMP/models/visionpsy"

# Create fake samosa-visionpsy helper to test framed IPC response and session lifecycle
cat << 'EOF' > "$TMP/bin/fake-visionpsy"
#!/usr/bin/env node
const fs = require('node:fs');
const readline = require('node:readline');
(async () => {
  const lines = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
  for await (const line of lines) {
    if (process.env.SAMOSA_VISIONPSY_TEST_LOG) fs.appendFileSync(process.env.SAMOSA_VISIONPSY_TEST_LOG, line + '\n');
    let command = ''; try { command = JSON.parse(line).command; } catch {}
    if (command === 'ping') console.log('{"status":"ok","pong":true}');
    else if (command === 'inspect') console.log('{"status":"ok","observation":"A detailed high-resolution diagram with labels and charts.","n_w":4,"n_h":3,"has_global":true,"prompt_tokens":896,"generated_tokens":45,"prefill_ms":12,"decode_ms":30}');
    else if (command === 'quit') { console.log('{"status":"ok","message":"bye"}'); lines.close(); process.stdin.destroy(); break; }
    else console.log('{"status":"ok"}');
  }
})();
EOF
chmod +x "$TMP/bin/fake-visionpsy"

cleanup
start_gateway "$TMP/bin/fake-visionpsy"

# Re-upload attachment for fresh gateway instance
ATTACH_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: image/png" \
    -H "X-Filename: test.png" \
    --data-binary @"$TMP/test.png")

ATTACH_ID=$(echo "$ATTACH_RES" | checks id)

# Images retain the current OCR-plus-visual evidence floor even if the fixture
# planner asks for text only. Pure document text can still avoid the specialist.
# The older image-only assertion predates that policy; both behaviors are checked.
: > "$TMP/visionpsy-commands.jsonl"
IMAGE_TEXT_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$ATTACH_ID\"],\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"Copy the lettering exactly and preserve line breaks.\"}]}")
printf '%s' "$IMAGE_TEXT_RES" | checks image-floor "$TMP/visionpsy-commands.jsonl"
printf '%%PDF-1.4\nlettering-only fixture\n' > "$TMP/lettering.pdf"
TEXT_UPLOAD=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/pdf" -H "X-Filename: lettering.pdf" \
    --data-binary @"$TMP/lettering.pdf")
TEXT_DOCUMENT_ID=$(printf '%s' "$TEXT_UPLOAD" | checks id)
[ -n "$TEXT_DOCUMENT_ID" ] || { echo "Text document upload failed: $TEXT_UPLOAD" >&2; exit 1; }
: > "$TMP/visionpsy-commands.jsonl"
OCR_ONLY_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$TEXT_DOCUMENT_ID\"],\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"Copy the lettering exactly and preserve line breaks.\"}]}")
printf '%s' "$OCR_ONLY_RES" | checks choices
[ ! -s "$TMP/visionpsy-commands.jsonl" ] || {
    echo "FAIL: OCR-only model plan started VisionPsy" >&2
    cat "$TMP/visionpsy-commands.jsonl" >&2
    exit 1
}
echo "Validated document text-only route: PASS"

# Developer mode is authenticated, off by default, and persistent. Restart
# after enabling so this test proves the saved setting is actually honored by
# load_config(), rather than merely reflected in one process's memory.
UNAUTH_DEBUG=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/v1/developer/trace")
[ "$UNAUTH_DEBUG" = "401" ] || { echo "FAIL: unauthenticated Developer status was $UNAUTH_DEBUG" >&2; exit 1; }
DEBUG_STATUS=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace")
printf '%s' "$DEBUG_STATUS" | checks developer-default
DEBUG_STATUS=$(curl -sS -X PUT -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d '{"enabled":true}' "http://127.0.0.1:$PORT/v1/developer/trace")
FIRST_TRACE_PATH=$(printf '%s' "$DEBUG_STATUS" | checks developer-path)
[ "$(stat -f '%Lp' "$FIRST_TRACE_PATH")" = "600" ] || { echo "FAIL: Developer trace permissions are not 0600" >&2; exit 1; }
[ "$(cat "$TMP/home/developer-mode")" = "enabled" ] || { echo "FAIL: Developer mode was not persisted" >&2; exit 1; }
ACTIVE_CLEAR=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/developer/trace/clear")
[ "$ACTIVE_CLEAR" = "409" ] || { echo "FAIL: clearing an active Developer trace returned $ACTIVE_CLEAR" >&2; exit 1; }

cleanup
start_gateway "$TMP/bin/fake-visionpsy"
DEBUG_STATUS=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace")
TRACE_PATH=$(printf '%s' "$DEBUG_STATUS" | checks developer-path)
[ -f "$TRACE_PATH" ] || { echo "FAIL: persisted Developer trace was not created after restart" >&2; exit 1; }

# Rebind the same content-addressed attachment under the restarted process.
ATTACH_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: image/png" -H "X-Filename: test.png" \
    --data-binary @"$TMP/test.png")
ATTACH_ID=$(echo "$ATTACH_RES" | checks id)

# Send chat request with attachment
: > "$TMP/visionpsy-commands.jsonl"
AUGMENTED_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{
        \"attachment_ids\": [\"$ATTACH_ID\"],
        \"messages\": [
            {
                \"role\": \"user\",
                \"content\": \"Describe the diagram in this image.\"
            }
        ]
    }")

printf '%s' "$AUGMENTED_RES" | checks choices
echo "VisionPsy end-to-end augmentation: PASS"

# A single correlated trace must contain every decision boundary needed to
# determine whether the visual specialist ran and whether its observation or
# the final LLM introduced a bad claim. It must not contain the UI auth token.
checks developer-trace "$TRACE_PATH" "$TOKEN"

DEBUG_STATUS=$(curl -sS -X PUT -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d '{"enabled":false}' "http://127.0.0.1:$PORT/v1/developer/trace")
printf '%s' "$DEBUG_STATUS" | checks developer-disabled
[ "$(cat "$TMP/home/developer-mode")" = "disabled" ] || { echo "FAIL: Developer mode disable was not persisted" >&2; exit 1; }
curl -sS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d '{"stream":false,"messages":[{"role":"user","content":"TRACE_DISABLED_SECRET_7482"}]}' >/dev/null
! grep -q 'TRACE_DISABLED_SECRET_7482' "$TRACE_PATH" || { echo "FAIL: disabled Developer mode kept logging" >&2; exit 1; }
CLEAR_RESULT=$(curl -sS -X POST -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace/clear")
printf '%s' "$CLEAR_RESULT" | checks developer-cleared
[ ! -e "$TRACE_PATH" ] || { echo "FAIL: Developer trace was not cleared" >&2; exit 1; }
echo "Developer mode disable and clear: PASS"

# 5. A whole-document visual task processes every selected PDF page
# sequentially through one helper session. There is no fixed five-page product
# cap; this three-page fixture verifies page iteration and turn-level reuse.
: > "$TMP/visionpsy-commands.jsonl"
printf '%%PDF-1.4\nfixture\n' > "$TMP/visual.pdf"
PDF_ATTACH_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/pdf" \
    -H "X-Filename: visual.pdf" \
    --data-binary @"$TMP/visual.pdf")
PDF_ATTACH_ID=$(echo "$PDF_ATTACH_RES" | checks id)
[ -n "$PDF_ATTACH_ID" ] || { echo "PDF upload failed: $PDF_ATTACH_RES" >&2; exit 1; }

PDF_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$PDF_ATTACH_ID\"],\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"Describe every chart across the entire document.\"}]}")

printf '%s' "$PDF_RES" | checks multipage "$TMP/visionpsy-commands.jsonl"

# 6. OCR success plus a VisionPsy inference failure is visibly labelled as a
# partial answer. The helper retries once at the next lower hardware tier,
# then both helper sessions are released.
cat << 'EOF' > "$TMP/bin/failing-visionpsy"
#!/bin/sh
while read -r line; do
  if [ -n "${SAMOSA_VISIONPSY_TEST_LOG:-}" ]; then
    printf '%s\n' "$line" >> "$SAMOSA_VISIONPSY_TEST_LOG"
  fi
  case "$line" in
    *'"command":"ping"'*) echo '{"status":"ok","pong":true}' ;;
    *'"command":"inspect"'*) echo '{"status":"error","code":"vision_inference_failed","message":"injected failure"}' ;;
    *'"command":"quit"'*) echo '{"status":"ok","message":"bye"}'; break ;;
    *) echo '{"status":"error","code":"unknown_command"}' ;;
  esac
done
EOF
chmod +x "$TMP/bin/failing-visionpsy"
cleanup
: > "$TMP/visionpsy-commands.jsonl"
start_gateway "$TMP/bin/failing-visionpsy"
ATTACH_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: image/png" \
    -H "X-Filename: test.png" --data-binary @"$TMP/test.png")
ATTACH_ID=$(echo "$ATTACH_RES" | checks id)
PARTIAL_RES=$(curl -sN -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$ATTACH_ID\"],\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"Read the text and describe the layout.\"}]}")
printf '%s' "$PARTIAL_RES" | grep -q 'Partial answer.*text/OCR only' || {
    echo "FAIL: OCR-only recovery was not visibly labelled: $PARTIAL_RES" >&2
    exit 1
}
checks partial "$TMP/visionpsy-commands.jsonl"

echo "test_visionpsy_e2e.sh: PASS"
