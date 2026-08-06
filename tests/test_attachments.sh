#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T3.2 (docs/TASKS_UI_CHUTNI.md sec5.8): POST/GET/DELETE /v1/attachments is
# new content-addressed storage replacing base64-in-localStorage for
# composer image/document attachments, plus the attachment_ids resolution
# chat_completions_request() now performs before proxying to the backend
# (base64 image data URI injection for images, doc.read extraction for
# PDFs). This test drives all of it against a real compiled gateway and a
# fake backend standing in for qwen36b.c -- no 24 GB model needed. The
# document half additionally exercises the real samosa-extract/samosa-ocr
# binaries against a real PDF fixture when they're available, skipping
# gracefully (matching tests/test_samosa_extract.sh's own precedent) when
# this machine has no PDFium build.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
EXTRACT="${SAMOSA_EXTRACT:-./$BUILD_DIR/samosa-extract}"
OCR="${SAMOSA_OCR:-./$BUILD_DIR/samosa-ocr}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/attachments_test.XXXXXX")
HOME_DIR="$TMP/home"
# Distinct from every other tests/*.sh port (they cluster in 18642-18643,
# 18977-18998, and 19010). PORT+1 is the backend port, so a test whose
# public port equals another test's backend port would let a leftover fake
# backend answer the other test's requests -- exactly how an orphan from a
# crashed run once made test_settings_compact_proxy.sh see 200 instead of
# 401 on an unauthenticated route.
PORT=19020
GW_PID=""

cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  # The gateway forks the backend as a child; if the gateway died abnormally
  # (the T3.2 use-after-free did exactly this) that child survives and keeps
  # its port bound. Shut it down explicitly so a failing run can never leave
  # a listener behind to poison a later test.
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR/qwen-model"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_QWEN_ENGINE="$BACKEND" \
SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model" \
SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
SAMOSA_EXTRACT="$EXTRACT" \
SAMOSA_OCR="$OCR" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
GW_PID=$!
i=0
while [ "$i" -lt 100 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true' && break
  sleep 0.05; i=$((i + 1))
done
TOKEN=$(cat "$HOME_DIR/run/ui-token")
HEALTH=$(curl -fsS "http://127.0.0.1:$PORT/healthz")
field() { printf '%s' "$1" | python3 -c "import json,sys; print(json.load(sys.stdin).get('$2'))"; }

# --- fixtures ---
printf '\211PNG\r\n\032\n' >"$TMP/probe.png"
printf 'not-a-real-png-body-but-sniffing-only-checks-the-magic-header' >>"$TMP/probe.png"
printf 'plain text, not an image or a pdf' >"$TMP/probe.txt"

# --- 1. Auth: no token on any new route ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/attachments" \
  -H "Content-Type: image/png" --data-binary "@$TMP/probe.png")
[ "$STATUS" = "401" ] || { echo "FAIL: upload with no token should be 401, got $STATUS"; exit 1; }

# --- 2. Upload a PNG: sniffed as image, capabilities.image true ---
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H "X-Samosa-Media-Type: image/png" \
  -H "X-Samosa-Filename-B64: $(printf 'my photo.png' | base64)" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@$TMP/probe.png")
IMG_ID=$(field "$RESP" id)
[ ${#IMG_ID} = 64 ] || { echo "FAIL: expected a 64-char hex attachment id, got: $RESP"; exit 1; }
[ "$(field "$RESP" filename)" = "my photo.png" ] || { echo "FAIL: filename not decoded, got: $RESP"; exit 1; }
printf '%s' "$RESP" | grep -q '"image":true' || { echo "FAIL: expected capabilities.image:true, got: $RESP"; exit 1; }
printf '%s' "$RESP" | grep -q '"document":false' || { echo "FAIL: expected capabilities.document:false, got: $RESP"; exit 1; }

# --- 3. Re-upload identical bytes: same content-addressed id ---
RESP2=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@$TMP/probe.png")
[ "$(field "$RESP2" id)" = "$IMG_ID" ] || { echo "FAIL: re-uploading identical bytes should return the same id"; exit 1; }

# --- 4. Reject an unsupported type ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" \
  -X POST "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@$TMP/probe.txt")
[ "$STATUS" = "415" ] || { echo "FAIL: plain text upload should be 415, got $STATUS"; exit 1; }

# --- 5. GET without token -> 401; with token -> exact bytes back ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/attachments/$IMG_ID")
[ "$STATUS" = "401" ] || { echo "FAIL: GET with no token should be 401, got $STATUS"; exit 1; }
curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/attachments/$IMG_ID" -o "$TMP/roundtrip.png"
cmp -s "$TMP/probe.png" "$TMP/roundtrip.png" || { echo "FAIL: GET did not return the exact uploaded bytes"; exit 1; }

# --- 6. Invalid attachment_id shape -> 400 ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/attachments/not-a-hash")
[ "$STATUS" = "400" ] || { echo "FAIL: malformed attachment id should be 400, got $STATUS"; exit 1; }

# --- 7. Unknown but well-formed id -> 404 ---
FAKE_ID=$(printf '0%.0s' $(seq 1 64))
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/attachments/$FAKE_ID")
[ "$STATUS" = "404" ] || { echo "FAIL: unknown attachment id should be 404, got $STATUS"; exit 1; }

# --- 8. Chat completions with attachment_ids resolves into a real image_url ---
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment image probe\"}],\"attachment_ids\":[\"$IMG_ID\"],\"stream\":false}")
printf '%s' "$RESP" | grep -q "saw the image attachment" || { echo "FAIL: gateway did not inject the image attachment into the outgoing request: $RESP"; exit 1; }

# --- 9. That attachment is now referenced: DELETE is refused ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X DELETE "http://127.0.0.1:$PORT/v1/attachments/$IMG_ID")
[ "$STATUS" = "409" ] || { echo "FAIL: deleting a referenced attachment should be 409, got $STATUS"; exit 1; }

# --- 10. An attachment never sent in a chat turn can be deleted freely ---
printf '\211PNG\r\n\032\n' >"$TMP/probe2.png"
printf 'second-unused-attachment' >>"$TMP/probe2.png"
RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@$TMP/probe2.png")
UNUSED_ID=$(field "$RESP" id)
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X DELETE "http://127.0.0.1:$PORT/v1/attachments/$UNUSED_ID")
[ "$STATUS" = "200" ] || { echo "FAIL: deleting an unreferenced attachment should be 200, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/attachments/$UNUSED_ID")
[ "$STATUS" = "404" ] || { echo "FAIL: a deleted attachment should 404 on GET, got $STATUS"; exit 1; }

# --- 11. Chat completions naming an attachment that doesn't exist -> 404 ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"attachment_ids\":[\"$FAKE_ID\"],\"stream\":false}")
[ "$STATUS" = "404" ] || { echo "FAIL: attachment_ids naming a missing attachment should be 404, got $STATUS"; exit 1; }

# --- 12. Document attachment: real doc.read extraction, only when this
#     machine actually has a working samosa-extract/samosa-ocr build. ---
SUPPORTS_DOCS=$(field "$HEALTH" supports_documents)
if [ "$SUPPORTS_DOCS" != "True" ]; then
  echo "test_attachments.sh: document half SKIPPED (no samosa-extract/samosa-ocr build on this machine)"
else
  RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -H "X-Samosa-Media-Type: application/pdf" \
    -X POST "http://127.0.0.1:$PORT/v1/attachments" --data-binary "@tests/fixtures/documents/hello.pdf")
  DOC_ID=$(field "$RESP" id)
  [ ${#DOC_ID} = 64 ] || { echo "FAIL: expected a 64-char hex attachment id for the PDF, got: $RESP"; exit 1; }
  printf '%s' "$RESP" | grep -q '"document":true' || { echo "FAIL: expected capabilities.document:true for a PDF, got: $RESP"; exit 1; }
  printf '%s' "$RESP" | grep -q '"image":false' || { echo "FAIL: expected capabilities.image:false for a PDF, got: $RESP"; exit 1; }

  RESP=$(curl -sS -H "X-Samosa-Token: $TOKEN" -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"attachment document probe\"}],\"attachment_ids\":[\"$DOC_ID\"],\"stream\":false}")
  printf '%s' "$RESP" | grep -q "saw the document attachment" || { echo "FAIL: gateway did not inject real extracted document text into the outgoing request: $RESP"; exit 1; }
fi

echo "test_attachments.sh: PASS"
