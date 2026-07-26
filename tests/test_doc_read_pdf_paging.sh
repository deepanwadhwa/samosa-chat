#!/bin/sh
set -eu

# T0.3 (docs/TASKS_UI_CHUTNI.md): "A PDF longer than five pages is processed
# without violating the extractor contract." Runs the REAL samosa-extract
# (samosa_extract.c enforces --json-pages COUNT<=5) against a real 7-page
# PDF through the actual Jobs find/verify loop's doc.read tool. Before the
# T0.3 fix, the gateway's initial cache-fill call requested COUNT=100 and
# samosa-extract exited 64 (usage error) on every real PDF; doc.read would
# have surfaced that as "image_invalid" and never reached page 7 at all.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
EXTRACTOR="${SAMOSA_EXTRACT:-./$BUILD_DIR/samosa-extract}"
FS_SIDECAR="${SAMOSA_FS:-./$BUILD_DIR/samosa-fs}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/doc_read_pdf_paging.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18982
BACKEND_PORT=18983
PID=""
BPID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  [ -z "$BPID" ] || kill "$BPID" 2>/dev/null || true
  [ -z "$BPID" ] || wait "$BPID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

if [ ! -x "$EXTRACTOR" ]; then
  echo "test_doc_read_pdf_paging.sh: SKIP (no real samosa-extract at $EXTRACTOR -- build with" >&2
  echo "  make samosa-extract PDFIUM_DIR=<unpacked PDFium SDK> first)" >&2
  exit 0
fi

mkdir -p "$HOME_DIR/models/ornith-9b" "$TMP/files"
printf 'fixture\n' >"$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'ornith\n' >"$HOME_DIR/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
cp "$ROOT/tests/fixtures/documents/multipage_7pages.pdf" "$TMP/files/multipage_7pages.pdf"

"$BACKEND" --port "$BACKEND_PORT" &
BPID=$!
i=0
while [ "$i" -lt 50 ]; do
  curl -fsS "http://127.0.0.1:$BACKEND_PORT/health" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_BONSAI_SERVER="$BACKEND" \
SAMOSA_ORNITH_MODEL="$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_EXTRACT="$EXTRACTOR" \
SAMOSA_FS="$FS_SIDECAR" \
  "$GATEWAY" >"$TMP/gateway.log" 2>&1 &
PID=$!

i=0
while [ "$i" -lt 50 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done

RESPONSE=$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find pdf paging probe\",\"folder\":\"$TMP/files\"}")

printf '%s' "$RESPONSE" | grep -q '"type":"result"' || {
  echo "FAIL: no result event"; echo "$RESPONSE"; exit 1;
}
printf '%s' "$RESPONSE" | grep -q '"type":"done"' || {
  echo "FAIL: job did not complete"; echo "$RESPONSE"; exit 1;
}
printf '%s' "$RESPONSE" | grep -q 'Page 7 of 7' || {
  echo "FAIL: page 7 text never reached the model -- pagination did not cover the full document"
  echo "$RESPONSE"; exit 1;
}
if printf '%s' "$RESPONSE" | grep -q 'pagination is broken'; then
  echo "FAIL: fake backend explicitly reported missing page 7"; echo "$RESPONSE"; exit 1
fi
if printf '%s' "$RESPONSE" | grep -qi 'image_invalid'; then
  echo "FAIL: doc.read failed (the exact pre-T0.3 symptom: extractor rejected an oversized batch)" >&2
  echo "$RESPONSE"; exit 1
fi

echo "test_doc_read_pdf_paging.sh: PASS (7-page PDF read in full through the real extractor)"
