#!/bin/sh
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
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
python3 -c "
import json
h=json.loads('''$HEALTH''')
assert h['supports_images'] is False, h
assert h['supports_image_attachments'] is True, h
assert h['vision']['auxiliary_runtime_available'] is True, h
assert h['ocr'] == {'runtime_available': True, 'pack_ready': False, 'ready': False}, h
print('Health capability separation: PASS')
"

# Fetch catalog with token
curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/catalog" > "$TMP/catalog.json"

python3 -c "
import json
d = json.load(open('$TMP/catalog.json'))
by_id = {m['id']: m for m in d['models']}
assert 'visionpsy-nano-460m-mlx-bf16' in by_id, f'VisionPsy missing from catalog: {by_id.keys()}'
vp = by_id['visionpsy-nano-460m-mlx-bf16']
assert vp['category'] == 'vision'
assert vp['role'] == 'auxiliary'
assert vp['backend_kind'] == 'mlx_vision_native'
assert vp['active'] is False
assert vp['routing'] == 'automatic'
assert vp['load_policy'] == 'on_demand_per_turn'
assert len(vp['artifacts']) == 7
print('Catalog verification: PASS')
"

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

ATTACH_ID=$(echo "$ATTACH_RES" | python3 -c "import sys, json; print(json.load(sys.stdin).get('id', ''))")
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

python3 -c "
import json
d = json.loads('''$CHAT_RES''')
err = d.get('error', {})
assert err.get('code') == 'vision_model_required', f'Expected vision_model_required, got: {d}'
print('Missing VisionPsy error flow: PASS')
"

# 4. Test IPC helper communication with all 7 pinned files in place
mkdir -p "$TMP/models/visionpsy"
python3 -c "
with open('$TMP/models/visionpsy/model.safetensors', 'wb') as f:
    f.truncate(1014772920)
for fn in ['config.json', 'preprocessor_config.json', 'processor_config.json', 'tokenizer.json', 'tokenizer_config.json', 'chat_template.jinja']:
    with open(f'$TMP/models/visionpsy/{fn}', 'w') as f:
        f.write('{}')
"

# Create fake samosa-visionpsy helper to test framed IPC response and session lifecycle
cat << 'EOF' > "$TMP/bin/fake-visionpsy"
#!/bin/sh
while read -r line; do
    if [ -n "${SAMOSA_VISIONPSY_TEST_LOG:-}" ]; then printf '%s\n' "$line" >> "$SAMOSA_VISIONPSY_TEST_LOG"; fi
    cmd=$(echo "$line" | python3 -c "import sys, json; print(json.load(sys.stdin).get('command', ''))")
    if [ "$cmd" = "ping" ]; then
        echo '{"status":"ok","pong":true}'
    elif [ "$cmd" = "inspect" ]; then
        echo '{"status":"ok","observation":"A detailed high-resolution diagram with labels and charts.","n_w":4,"n_h":3,"has_global":true,"prompt_tokens":896,"generated_tokens":45,"prefill_ms":12,"decode_ms":30}'
    elif [ "$cmd" = "quit" ]; then
        echo '{"status":"ok","message":"bye"}'
        break
    else
        echo '{"status":"ok"}'
    fi
done
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

ATTACH_ID=$(echo "$ATTACH_RES" | python3 -c "import sys, json; print(json.load(sys.stdin).get('id', ''))")

# This wording contains none of the conservative fallback's OCR keywords. The
# fake LLM explicitly routes it to OCR only, proving the validated plan is
# honored instead of the fallback heuristic.
: > "$TMP/visionpsy-commands.jsonl"
OCR_ONLY_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$ATTACH_ID\"],\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"Copy the lettering exactly and preserve line breaks.\"}]}")
python3 -c "import json; assert 'choices' in json.loads('''$OCR_ONLY_RES''')"
[ ! -s "$TMP/visionpsy-commands.jsonl" ] || {
    echo "FAIL: OCR-only model plan started VisionPsy" >&2
    cat "$TMP/visionpsy-commands.jsonl" >&2
    exit 1
}
echo "Validated OCR-only planner route: PASS"

# Developer mode is authenticated, off by default, and persistent. Restart
# after enabling so this test proves the saved setting is actually honored by
# load_config(), rather than merely reflected in one process's memory.
UNAUTH_DEBUG=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/v1/developer/trace")
[ "$UNAUTH_DEBUG" = "401" ] || { echo "FAIL: unauthenticated Developer status was $UNAUTH_DEBUG" >&2; exit 1; }
DEBUG_STATUS=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace")
python3 -c "import json; d=json.loads('''$DEBUG_STATUS'''); assert d['enabled'] is False and d['captures_sensitive_data'] is True and d['authentication_tokens_logged'] is False, d"
DEBUG_STATUS=$(curl -sS -X PUT -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d '{"enabled":true}' "http://127.0.0.1:$PORT/v1/developer/trace")
FIRST_TRACE_PATH=$(printf '%s' "$DEBUG_STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); assert d['enabled'] is True and d['path']; print(d['path'])")
[ "$(stat -f '%Lp' "$FIRST_TRACE_PATH")" = "600" ] || { echo "FAIL: Developer trace permissions are not 0600" >&2; exit 1; }
[ "$(cat "$TMP/home/developer-mode")" = "enabled" ] || { echo "FAIL: Developer mode was not persisted" >&2; exit 1; }
ACTIVE_CLEAR=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Samosa-Token: $TOKEN" \
    "http://127.0.0.1:$PORT/v1/developer/trace/clear")
[ "$ACTIVE_CLEAR" = "409" ] || { echo "FAIL: clearing an active Developer trace returned $ACTIVE_CLEAR" >&2; exit 1; }

cleanup
start_gateway "$TMP/bin/fake-visionpsy"
DEBUG_STATUS=$(curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace")
TRACE_PATH=$(printf '%s' "$DEBUG_STATUS" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["enabled"] is True; print(d["path"])')
[ -f "$TRACE_PATH" ] || { echo "FAIL: persisted Developer trace was not created after restart" >&2; exit 1; }

# Rebind the same content-addressed attachment under the restarted process.
ATTACH_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/attachments" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: image/png" -H "X-Filename: test.png" \
    --data-binary @"$TMP/test.png")
ATTACH_ID=$(echo "$ATTACH_RES" | python3 -c "import sys, json; print(json.load(sys.stdin).get('id', ''))")

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

python3 -c "
import json
d = json.loads('''$AUGMENTED_RES''')
assert 'choices' in d, f'Expected choices in response, got: {d}'
print('VisionPsy end-to-end augmentation: PASS')
"

# A single correlated trace must contain every decision boundary needed to
# determine whether the visual specialist ran and whether its observation or
# the final LLM introduced a bad claim. It must not contain the UI auth token.
python3 - "$TRACE_PATH" "$TOKEN" <<'PY'
import json, sys
path, token = sys.argv[1:]
raw = open(path, encoding="utf-8").read()
assert token not in raw, "UI authentication token leaked into Developer trace"
events = [json.loads(line) for line in raw.splitlines() if line.strip()]
names = [event["event"] for event in events]
required = {
    "chat_request_received", "vision_router_request", "vision_router_response",
    "vision_router_validated_plan", "vision_resource_budget", "attachment_selected",
    "visionpsy_process_started", "visionpsy_request", "visionpsy_raw_response",
    "visionpsy_complete", "vision_evidence_observation", "backend_request",
    "backend_response", "backend_complete", "chat_turn_completed",
}
missing = sorted(required - set(names))
assert not missing, f"Developer trace missing events: {missing}\n{names}"
visual = [event for event in events if event["event"] == "visionpsy_raw_response"][-1]
assert "A detailed high-resolution diagram" in visual["fields"]["payload"], visual
final_request = [event for event in events if event["event"] == "backend_request"][-1]
assert "A detailed high-resolution diagram" in final_request["fields"]["payload"], final_request
turn_ids = {event.get("turn_id") for event in events if event["event"] in required and event.get("turn_id")}
assert len(turn_ids) == 1, turn_ids
print("Full correlated Developer trace: PASS")
PY

DEBUG_STATUS=$(curl -sS -X PUT -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d '{"enabled":false}' "http://127.0.0.1:$PORT/v1/developer/trace")
python3 -c "import json; assert json.loads('''$DEBUG_STATUS''')['enabled'] is False"
[ "$(cat "$TMP/home/developer-mode")" = "disabled" ] || { echo "FAIL: Developer mode disable was not persisted" >&2; exit 1; }
curl -sS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d '{"stream":false,"messages":[{"role":"user","content":"TRACE_DISABLED_SECRET_7482"}]}' >/dev/null
! grep -q 'TRACE_DISABLED_SECRET_7482' "$TRACE_PATH" || { echo "FAIL: disabled Developer mode kept logging" >&2; exit 1; }
CLEAR_RESULT=$(curl -sS -X POST -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/developer/trace/clear")
python3 -c "import json; assert json.loads('''$CLEAR_RESULT''')['removed'] >= 1"
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
PDF_ATTACH_ID=$(echo "$PDF_ATTACH_RES" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))")
[ -n "$PDF_ATTACH_ID" ] || { echo "PDF upload failed: $PDF_ATTACH_RES" >&2; exit 1; }

PDF_RES=$(curl -s -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$PDF_ATTACH_ID\"],\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"Describe every chart across the entire document.\"}]}")

python3 -c "
import json
response=json.loads('''$PDF_RES''')
assert 'choices' in response, response
commands=[json.loads(line) for line in open('$TMP/visionpsy-commands.jsonl') if line.strip()]
assert [c['command'] for c in commands].count('ping') == 1, commands
inspects=[c for c in commands if c.get('command') == 'inspect']
assert len(inspects) == 3, commands
assert [next(int(word) for word in c['prompt'].split() if word.isdigit()) for c in inspects] == [1,2,3], inspects
assert len({c['max_side_len'] for c in inspects}) == 1, inspects
assert commands[-1]['command'] == 'quit', commands
print('VisionPsy multi-page turn reuse: PASS')
"

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
ATTACH_ID=$(echo "$ATTACH_RES" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))")
PARTIAL_RES=$(curl -sN -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H "X-Samosa-Token: $TOKEN" -H "Content-Type: application/json" \
    -d "{\"attachment_ids\":[\"$ATTACH_ID\"],\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"Read the text and describe the layout.\"}]}")
printf '%s' "$PARTIAL_RES" | grep -q 'Partial answer.*text/OCR only' || {
    echo "FAIL: OCR-only recovery was not visibly labelled: $PARTIAL_RES" >&2
    exit 1
}
python3 -c "
import json
commands=[json.loads(line) for line in open('$TMP/visionpsy-commands.jsonl') if line.strip()]
assert [c.get('command') for c in commands].count('inspect') == 2, commands
assert [c.get('command') for c in commands].count('ping') == 2, commands
assert [c.get('command') for c in commands].count('quit') == 2, commands
print('Labelled OCR-only partial recovery: PASS')
"

echo "test_visionpsy_e2e.sh: PASS"
