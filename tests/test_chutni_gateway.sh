#!/bin/sh
set -eu

fail() {
  echo "test_chutni_gateway.sh: FAIL: $1" >&2
  exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-build}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-gateway.XXXXXX")
PORT=19277
PID=
trap 'test -z "$PID" || kill "$PID" 2>/dev/null || true; test -z "$PID" || wait "$PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT HUP INT TERM

mkdir -p "$TMP/source"
printf 'renewal date June; chutni memory probe evidence\n' >"$TMP/source/report.txt"
i=0
while [ "$i" -lt 80 ]; do
  printf 'ordinary text padding before the bounded summary tail\n' >>"$TMP/source/report.txt"
  i=$((i + 1))
done
printf 'TAIL_CONTENT_LEAK\n' >>"$TMP/source/report.txt"
printf 'portable memory handoff\n' >"$TMP/source/notes.md"
cp "$ROOT/tests/fixtures/documents/multipage_7pages.pdf" "$TMP/source/guide.pdf"
cp "$ROOT/tools/testdata/ocr/tiny.png" "$TMP/source/scan.png"
mkdir -p "$TMP/home/qwen-model"
printf 'fixture\n' >"$TMP/home/qwen-model/experts.bin"
printf '{}\n' >"$TMP/tokenizer.json"

SAMOSA_EXTRACT="$ROOT/$BUILD_DIR/samosa-extract"
if [ ! -f "$SAMOSA_EXTRACT" ] || [ ! -x "$SAMOSA_EXTRACT" ]; then
  echo "test_chutni_gateway.sh: SKIPPED (no samosa-extract build on this machine)"
  exit 0
fi

HOME="$TMP/home" \
SAMOSA_HOME="$TMP/home" \
CHUTNI_HOME="$TMP/chutni-home" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$ROOT/assets/app.html" \
SAMOSA_QWEN_ENGINE="$ROOT/$BUILD_DIR/test_fake_openai_backend" \
SAMOSA_QWEN_MODEL="$TMP/home/qwen-model" \
SAMOSA_TOKENIZER="$TMP/tokenizer.json" \
SAMOSA_CHUTNI_SERVICE="$ROOT/$BUILD_DIR/chutni-mcp" \
SAMOSA_EXTRACT="$SAMOSA_EXTRACT" \
SAMOSA_OCR="$ROOT/tests/fake_ocr_sidecar.sh" \
SAMOSA_APP_VERSION="test-enrichment-1" \
"$ROOT/$BUILD_DIR/samosa-gateway" >"$TMP/gateway.log" 2>&1 &
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
[ -n "$PREFLIGHT" ] || fail "missing preflight_id"
printf '%s' "$PF" | grep -q '"action":"create_store"'
printf '%s' "$PF" | grep -q '"store_path":'
printf '%s' "$PF" | grep -q '\.chutni'
STORE=$(printf '%s' "$PF" | sed -n 's/.*"store_path":"\([^"]*\)".*/\1/p')
[ -n "$STORE" ] || fail "missing store_path"

CREATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT\",\"display_name\":\"Research\",\"summary_token_budget\":128}")
SCOPE=$(printf '%s' "$CREATED" | sed -n 's/.*"scope_id":"\([^"]*\)".*/\1/p')
JOB=$(printf '%s' "$CREATED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$SCOPE" ] && [ -n "$JOB" ] || fail "scope ID missing from create response"

i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "$STATUS" >&2; cat "$TMP/gateway.log" >&2; fail "scope never reached ready state"; }

[ -f "$STORE/manifest.json" ] || fail "expected file $STORE/manifest.json"
[ -f "$STORE/catalog.sqlite" ] || fail "expected file $STORE/catalog.sqlite"
[ -f "$STORE/indexes/lexical.sqlite" ] || fail "expected file $STORE/indexes/lexical.sqlite"
printf '%s' "$STATUS" | grep -q '"files_indexed":4'
printf '%s' "$STATUS" | grep -q '"summary_token_budget":128'
printf '%s' "$STATUS" | grep -q '"content_readable_files":4'
printf '%s' "$STATUS" | grep -q '"metadata_only_files":0'
printf '%s' "$STATUS" | grep -q '"content_artifacts":15'
printf '%s' "$STATUS" | grep -q '"phase":"complete"'
printf '%s' "$STATUS" | grep -q '"scan_files_seen":4'
printf '%s' "$STATUS" | grep -q '"enrichment_files_total":4'
printf '%s' "$STATUS" | grep -q '"enrichment_files_done":4'
printf '%s' "$STATUS" | grep -q '"pdf_pages_read":7'
printf '%s' "$STATUS" | grep -q '"ocr_outputs":1'
printf '%s' "$STATUS" | grep -q '"image_captions":1'
printf '%s' "$STATUS" | grep -q '"summaries_created":4'
printf '%s' "$STATUS" | grep -q '"elapsed_seconds":'
printf '%s' "$STATUS" | grep -q '"files_per_second":'
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
[ -n "$REFRESH_JOB" ] || fail "missing refresh job ID"
i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' &&
    printf '%s' "$STATUS" | grep -q '"evidence_generation":2' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "$STATUS" >&2; cat "$TMP/gateway.log" >&2; fail "unchanged Chutni refresh did not publish"; }
printf '%s' "$STATUS" | grep -q '"content_readable_files":4'
printf '%s' "$STATUS" | grep -q '"metadata_only_files":0'

# Samosa enrichment is committed into the portable protocol store, not a
# private cache: PDF pages, image OCR, captions, and summaries are searchable
# with their protocol artifact kinds and provenance.
PDF_RESULT=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"synthetic fixture document\",\"limit\":20}")
printf '%s' "$PDF_RESULT" | grep -q '"artifact_kind":"page_text"'
OCR_RESULT=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"Poličar 2019\",\"limit\":20}")
printf '%s' "$OCR_RESULT" | grep -q '"artifact_kind":"ocr_text"'
CAPTION_RESULT=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"small repository OCR fixture\",\"limit\":20}")
printf '%s' "$CAPTION_RESULT" | grep -q '"artifact_kind":"image_caption"'
SUMMARY_RESULT=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"portable Chutni memory\",\"limit\":20}")
printf '%s' "$SUMMARY_RESULT" | grep -q '"artifact_kind":"summary_short"'

DB_URI="file:$STORE/catalog.sqlite?immutable=1"
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN derivations d USING(derivation_id) JOIN producers p USING(producer_id) WHERE a.artifact_kind='page_text' AND a.status='active' AND p.name='Samosa document reader' AND p.app_name='Samosa' AND p.app_version='test-enrichment-1';" \
  | grep -q '^7$'
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN derivations d USING(derivation_id) JOIN producers p USING(producer_id) WHERE a.artifact_kind IN ('image_caption','summary_short') AND a.status='active' AND p.producer_kind='model' AND p.model_id='qwen3.6-35b-a3b' AND p.model_revision<>'' AND p.app_name='Samosa';" \
  | grep -q '^5$'
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN sources s USING(source_id) WHERE a.artifact_kind='summary_short' AND a.status='active' AND json_extract(s.locator_json,'$.display_path') LIKE '%/guide.pdf' AND a.selector_json='{\"type\":\"pages\",\"start\":1,\"end\":3}' AND a.inline_text NOT LIKE 'ERROR:%';" \
  | grep -q '^1$'
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN derivations d USING(derivation_id) WHERE a.artifact_kind='summary_short' AND a.status='active' AND d.recipe_hash='samosa-summary-leading-content-v1' AND json_extract(d.parameters_json,'$.summary_input')='leading_content_window' AND json_extract(d.parameters_json,'$.token_budget')=128 AND json_extract(d.parameters_json,'$.token_estimator')='utf8_bytes_div_4_v1' AND json_extract(d.parameters_json,'$.max_input_bytes')=512 AND a.inline_text NOT LIKE 'ERROR:%';" \
  | grep -q '^4$'

RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"renewal\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$RESULT" | grep -q '"used":true'
printf '%s' "$RESULT" | grep -q 'report.txt'
printf '%s' "$RESULT" | grep -q '"freshness":"current"'

# Chutni 0.2 indexes a {"size_bytes":N,"depth":N} file_metadata artifact per
# file and a directory_listing per enumerated directory. Both rank alongside
# real content, so retrieval must drop them: they are machine bookkeeping, and
# on this machine every token spliced into a prompt is paid for in prefill.
# The store genuinely contains them -- assert that directly, so this stays a
# test of Samosa's filter and not of whether the scanner wrote them.
METADATA_COUNT=$(sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts WHERE artifact_kind='file_metadata' AND status='active';")
test "$METADATA_COUNT" -gt 0 || fail "missing metadata"
METADATA_PROBE=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"size_bytes depth\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
! printf '%s' "$METADATA_PROBE" | grep -q 'size_bytes'
! printf '%s' "$METADATA_PROBE" | grep -q '"artifact_kind":"file_metadata"'
! printf '%s' "$METADATA_PROBE" | grep -q 'directory_listing'
# A query that matches only bookkeeping is not useful evidence, and saying
# otherwise would report a retrieval the model never sees.
printf '%s' "$METADATA_PROBE" | grep -q '"used":false'

# Binding the ready scope to a chat turn makes the gateway retrieve, bound,
# label, and inject the evidence before the local model receives the request.
CHAT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  --data-binary "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"please find the chutni memory probe now\"}],\"directory_context\":{\"scope_id\":\"$SCOPE\"},\"stream\":false}")
printf '%s' "$CHAT" | grep -q 'saw Chutni memory'

# Inventory-shaped questions do not depend on the literal word "folder"
# appearing inside a document. The selected scope's bounded catalog is
# injected instead, so the model cannot falsely claim no folder is attached.
OVERVIEW=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  --data-binary "{\"model\":\"qwen3.6-35b-a3b\",\"seed\":424242,\"messages\":[{\"role\":\"user\",\"content\":\"this folder - what can you tell me about it?\"}],\"directory_context\":{\"scope_id\":\"$SCOPE\"},\"stream\":false}")
printf '%s' "$OVERVIEW" | grep -q 'saw Research inventory'

# A selected memory with no lexical hit is still explicit context. The model
# receives an honest no-match status instead of silently falling back to
# "I cannot access your filesystem."
NO_MATCH=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" \
  --data-binary "{\"model\":\"qwen3.6-35b-a3b\",\"seed\":424243,\"messages\":[{\"role\":\"user\",\"content\":\"platypus\"}],\"directory_context\":{\"scope_id\":\"$SCOPE\"},\"stream\":false}")
printf '%s' "$NO_MATCH" | grep -q 'saw honest no-match status'

# A second host reads the exact store Samosa created; there is no migration or
# Samosa-private catalog in the retrieval path.
DIRECT=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"handoff\"}")
printf '%s' "$DIRECT" | grep -q '"count":1'
printf '%s' "$DIRECT" | grep -q 'notes.md'

# The generic service updates the store, and Samosa immediately reads the
# other host's update.
printf 'retention date September\n' >"$TMP/source/report.txt"
HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_scan \
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

# A user can change the per-memory budget without rebuilding immediately.
# The value is persisted in scope metadata and explicitly applies next time.
BUDGET=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/summary-budget" \
  --data-binary '{"token_budget":512}')
printf '%s' "$BUDGET" | grep -q '"summary_token_budget":512'
printf '%s' "$BUDGET" | grep -q '"applies":"next_refresh"'
STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
printf '%s' "$STATUS" | grep -q '"summary_token_budget":512'

# Forgetting only detaches Samosa metadata. The portable store belongs to the
# user and remains available to the other host.
FORGOTTEN=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/forget" \
  --data-binary '{"confirm":true}')
printf '%s' "$FORGOTTEN" | grep -q '"portable_store_preserved":true'
[ -f "$STORE/manifest.json" ] || fail "expected file $STORE/manifest.json"
SCOPES=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes")
printf '%s' "$SCOPES" | grep -q '"scopes":\[\]'

echo "test_chutni_gateway.sh: PASS"
