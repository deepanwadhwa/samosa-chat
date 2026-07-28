#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-gateway.XXXXXX")
PORT=19277
PID=
trap 'test -z "$PID" || kill "$PID" 2>/dev/null || true; test -z "$PID" || wait "$PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT HUP INT TERM

mkdir -p "$TMP/source"
i=1
while [ "$i" -le 160 ]; do
  printf 'renewal date June; fixture %s\n' "$i" >"$TMP/source/report-$i.txt"
  i=$((i + 1))
done
printf '%%PDF-1.4 fixture bytes\n' >"$TMP/source/contract.pdf"
printf '\211PNG\r\n\032\nfixture bytes\n' >"$TMP/source/scan.png"
printf '%s\n' '#!/bin/sh' \
  'if [ "$1" = "--json" ]; then printf "%s\n" '\''{"ok":true,"text_layer":true,"page_count":1,"text":"PDF renewal date July"}'\''; exit 0; fi' \
  'printf "%s\n" '\''{"ok":false,"error":"unsupported"}'\''' >"$TMP/extract-wrapper"
printf '%s\n' '#!/bin/sh' \
  'printf "%s\n" '\''{"ok":true,"lines":[{"text":"OCR renewal date August","conf":0.99}]}'\''' >"$TMP/ocr-wrapper"
chmod +x "$TMP/extract-wrapper" "$TMP/ocr-wrapper"

SAMOSA_HOME="$TMP/home" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$ROOT/assets/app.html" \
SAMOSA_CHUTNI_DB="$ROOT/build/samosa-chutni-db" \
SAMOSA_TOKENIZER="$ROOT/tokenizer_qwen36.json" \
SAMOSA_EXTRACT="$TMP/extract-wrapper" \
SAMOSA_OCR="$TMP/ocr-wrapper" \
SAMOSA_CHUTNI_TEST_DELAY_US=10000 \
"$ROOT/build/samosa-gateway" >"$TMP/gateway.log" 2>&1 &
PID=$!

i=0
while [ "$i" -lt 200 ]; do
  if curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then break; fi
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 200 ] || { sed -n '1,120p' "$TMP/gateway.log" >&2; exit 1; }
TOKEN=$(tr -d '\n' <"$TMP/home/run/ui-token")

CODE=$(curl -sS -o "$TMP/unauth.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}]}")
[ "$CODE" = 401 ] || { echo "FAIL: Chutni route did not fail closed" >&2; exit 1; }

PF=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}]}")
PREFLIGHT=$(printf '%s' "$PF" | sed -n 's/.*"preflight_id":"\([^"]*\)".*/\1/p')
[ -n "$PREFLIGHT" ]

CREATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT\",\"display_name\":\"Research\"}")
SCOPE=$(printf '%s' "$CREATED" | sed -n 's/.*"scope_id":"\([^"]*\)".*/\1/p')
JOB=$(printf '%s' "$CREATED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$SCOPE" ] && [ -n "$JOB" ]

PAUSED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/pause" \
  --data-binary "{\"job_id\":\"$JOB\"}")
printf '%s' "$PAUSED" | grep -q '"state":"paused_user"'

i=0
while [ "$i" -lt 100 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"paused_user"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ]

RESUMED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/resume" \
  --data-binary "{\"job_id\":\"$JOB\"}")
printf '%s' "$RESUMED" | grep -q '"state":"queued"'

i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "FAIL: resumed Chutni build did not publish" >&2; exit 1; }

EVENTS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/events?job_id=$JOB&after=0")
printf '%s' "$EVENTS" | grep -q '"kind":"chutni_build"'
printf '%s' "$EVENTS" | grep -q '"state":"paused_user"'
printf '%s' "$EVENTS" | grep -q '"state":"completed"'

RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"renewal\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$RESULT" | grep -q '"used":true'
printf '%s' "$RESULT" | grep -q 'report-1.txt'
PDF_RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"PDF renewal\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$PDF_RESULT" | grep -q 'contract.pdf'

echo "test_chutni_gateway.sh: PASS"
