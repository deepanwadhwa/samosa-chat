#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-gateway.XXXXXX")
PORT=19277
PID=
trap 'test -z "$PID" || kill "$PID" 2>/dev/null || true; test -z "$PID" || wait "$PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT HUP INT TERM

mkdir -p "$TMP/source"
printf 'renewal date June; chutni memory probe evidence\n' >"$TMP/source/report.txt"
printf 'portable memory handoff\n' >"$TMP/source/notes.md"
mkdir -p "$TMP/home/qwen-model"
printf 'fixture\n' >"$TMP/home/qwen-model/experts.bin"
printf '{}\n' >"$TMP/tokenizer.json"

SAMOSA_HOME="$TMP/home" \
CHUTNI_HOME="$TMP/chutni-home" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$ROOT/assets/app.html" \
SAMOSA_QWEN_ENGINE="$ROOT/build/test_fake_openai_backend" \
SAMOSA_QWEN_MODEL="$TMP/home/qwen-model" \
SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
SAMOSA_CHUTNI_SERVICE="$ROOT/build/chutni-mcp" \
"$ROOT/build/samosa-gateway" >"$TMP/gateway.log" 2>&1 &
PID=$!

i=0
while [ "$i" -lt 200 ]; do
  if curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true'; then break; fi
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
printf '%s' "$PF" | grep -q '"action":"create_store"'
printf '%s' "$PF" | grep -q '"store_path":'
printf '%s' "$PF" | grep -q '\.chutni'
STORE=$(printf '%s' "$PF" | sed -n 's/.*"store_path":"\([^"]*\)".*/\1/p')
[ -n "$STORE" ]

CREATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT\",\"display_name\":\"Research\"}")
SCOPE=$(printf '%s' "$CREATED" | sed -n 's/.*"scope_id":"\([^"]*\)".*/\1/p')
JOB=$(printf '%s' "$CREATED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$SCOPE" ] && [ -n "$JOB" ]

i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "FAIL: Chutni build did not publish" >&2; sed -n '1,160p' "$TMP/gateway.log" >&2; exit 1; }

[ -f "$STORE/manifest.json" ]
[ -f "$STORE/catalog.sqlite" ]
[ -f "$STORE/indexes/lexical.sqlite" ]
printf '%s' "$STATUS" | grep -q '"files_indexed":2'
printf '%s' "$STATUS" | grep -q '"chunks_indexed":2'
printf '%s' "$STATUS" | grep -q "\"active_database\":\"$STORE\""

EVENTS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/events?job_id=$JOB&after=0")
printf '%s' "$EVENTS" | grep -q '"kind":"chutni_build"'
printf '%s' "$EVENTS" | grep -q '"state":"completed"'

# An unchanged refresh reports the store's total active artifacts rather than
# the scan's zero-change delta.
REFRESHED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/refresh" \
  --data-binary '{}')
REFRESH_JOB=$(printf '%s' "$REFRESHED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$REFRESH_JOB" ]
i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' &&
    printf '%s' "$STATUS" | grep -q '"evidence_generation":2' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "FAIL: unchanged Chutni refresh did not publish" >&2; exit 1; }
printf '%s' "$STATUS" | grep -q '"chunks_indexed":2'

RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"renewal\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$RESULT" | grep -q '"used":true'
printf '%s' "$RESULT" | grep -q 'report.txt'
printf '%s' "$RESULT" | grep -q '"freshness":"current"'

# Binding the ready scope to a chat turn makes the gateway retrieve, bound,
# label, and inject the evidence before the local model receives the request.
CHAT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  --data-binary "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"please find the chutni memory probe now\"}],\"directory_context\":{\"scope_id\":\"$SCOPE\"},\"stream\":false}")
printf '%s' "$CHAT" | grep -q 'saw Chutni memory'

# A second host reads the exact store Samosa created; there is no migration or
# Samosa-private catalog in the retrieval path.
DIRECT=$(CHUTNI_HOME="$TMP/chutni-home" "$ROOT/build/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"handoff\"}")
printf '%s' "$DIRECT" | grep -q '"count":1'
printf '%s' "$DIRECT" | grep -q 'notes.md'

# The generic service updates the store, and Samosa immediately reads the
# other host's update.
printf 'retention date September\n' >"$TMP/source/report.txt"
CHUTNI_HOME="$TMP/chutni-home" "$ROOT/build/chutni-mcp" --call chutni_scan \
  "{\"store_path\":\"$STORE\",\"confirmed\":true,\"app_name\":\"handoff-test\",\"app_version\":\"1\"}" \
  >"$TMP/direct-scan.json"
UPDATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"September\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$UPDATED" | grep -q '"used":true'
printf '%s' "$UPDATED" | grep -q 'report.txt'
OLD=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"June\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$OLD" | grep -q '"used":false'

# Forgetting only detaches Samosa metadata. The portable store belongs to the
# user and remains available to the other host.
FORGOTTEN=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/forget" \
  --data-binary '{"confirm":true}')
printf '%s' "$FORGOTTEN" | grep -q '"portable_store_preserved":true'
[ -f "$STORE/manifest.json" ]
SCOPES=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes")
printf '%s' "$SCOPES" | grep -q '"scopes":\[\]'

echo "test_chutni_gateway.sh: PASS"
