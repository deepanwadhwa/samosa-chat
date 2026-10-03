#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build}"

fail() {
  echo "test_chutni_gateway.sh: FAIL: $1" >&2
  exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
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
mkdir -p "$TMP/source/Documents.chutni/objects"
printf 'GENERATED_STORE_SECRET_SENTINEL\n' >"$TMP/source/Documents.chutni/objects/ignored.txt"
printf '<!doctype html><title>Fixture</title><p>HTML_SENTINEL_CEDAR_HARBOR</p>\n' >"$TMP/source/fixture.html"
mkdir -p "$TMP/source/.venv" "$TMP/source/node_modules/package"
mkdir -p "$TMP/source/Private"
printf 'DEPENDENCY_TREE_SECRET_SENTINEL\n' >"$TMP/source/.venv/ignored.txt"
printf 'DEPENDENCY_TREE_SECRET_SENTINEL\n' >"$TMP/source/node_modules/package/ignored.txt"
printf 'USER_EXCLUSION_SECRET_SENTINEL\n' >"$TMP/source/Private/ignored.txt"
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

# The reference scanner enforces the same aggregate cap the metadata preview
# uses, and persists exact-name exclusions with the root policy.
mkdir -p "$TMP/scanner-budget/Private" "$TMP/scanner-budget-home"
printf 'aa' >"$TMP/scanner-budget/a.txt"
printf 'bb' >"$TMP/scanner-budget/b.txt"
printf 'excluded' >"$TMP/scanner-budget/Private/secret.txt"
SCANNER_BUDGET=$(HOME="$TMP/scanner-budget-home" CHUTNI_HOME="$TMP/scanner-budget-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$TMP/scanner-budget\",\"confirmed\":true,\"register\":true,\"label\":\"Budget fixture\",\"max_depth\":32,\"max_files\":10000,\"max_directories\":5000,\"max_seconds\":20,\"max_file_size_bytes\":64,\"max_eligible_bytes\":2,\"exclude_globs\":[\"private\"]}")
printf '%s' "$SCANNER_BUDGET" | grep -q '"partial":true'
printf '%s' "$SCANNER_BUDGET" | grep -q '"limiting_reason":"maximum_eligible_bytes"'
printf '%s' "$SCANNER_BUDGET" | grep -q '"eligible_bytes":2'
SCANNER_POLICY=$(HOME="$TMP/scanner-budget-home" CHUTNI_HOME="$TMP/scanner-budget-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_store_info \
  "{\"store_path\":\"$TMP/scanner-budget.chutni\"}")
printf '%s' "$SCANNER_POLICY" | grep -q '"scan_max_file_size_bytes":64'
printf '%s' "$SCANNER_POLICY" | grep -q '"max_eligible_bytes":2'
SCANNER_SEARCH=$(HOME="$TMP/scanner-budget-home" CHUTNI_HOME="$TMP/scanner-budget-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$TMP/scanner-budget.chutni\",\"query\":\"excluded\",\"limit\":10}")
! printf '%s' "$SCANNER_SEARCH" | grep -q 'Private/secret.txt'

# At an aggregate-byte boundary under the shared per-file ceiling, preflight
# and Chutni must admit the same paths. The scanner keeps native readdir order for budget decisions,
# while each directory's persisted listing hash remains canonically sorted.
mkdir -p "$TMP/parity" "$TMP/parity-home"
PARITY_ROOT=$(CDPATH= cd "$TMP/parity" && pwd -P)
printf 'a' >"$TMP/parity/z-last.txt"
printf 'bb' >"$TMP/parity/a-first.txt"
printf 'ccc' >"$TMP/parity/m-middle.txt"
printf 'd' >"$TMP/parity/b-next.txt"
printf 'eee' >"$TMP/parity/y-last.txt"
mkdir -p "$TMP/parity/Other.CHUTNI/objects"
printf 'generated store bytes' >"$TMP/parity/Other.CHUTNI/objects/ignored.txt"
"$ROOT/$BUILD_DIR/samosa-fs" chutni-inventory --root "$PARITY_ROOT" \
  --max-files 100 --max-directories 5000 --max-seconds 20 --max-depth 32 \
  --max-file-bytes 3 --max-eligible-bytes 6 >"$TMP/parity-preview.ndjson"
HOME="$TMP/parity-home" CHUTNI_HOME="$TMP/parity-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$PARITY_ROOT\",\"confirmed\":true,\"register\":true,\"label\":\"Parity fixture\",\"max_depth\":32,\"max_files\":100,\"max_directories\":5000,\"max_seconds\":20,\"max_file_size_bytes\":3,\"max_eligible_bytes\":6}" \
  >"$TMP/parity-scan.json"
HOME="$TMP/parity-home" CHUTNI_HOME="$TMP/parity-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_list_sources \
  "{\"store_path\":\"$PARITY_ROOT.chutni\",\"source_path\":\"$PARITY_ROOT\",\"limit\":200}" \
  >"$TMP/parity-sources.json"
python3 "$ROOT/tests/assert_chutni_inventory_parity.py" \
  "$TMP/parity-preview.ndjson" "$TMP/parity-scan.json" \
  "$TMP/parity-sources.json" "$PARITY_ROOT"

# File-count boundary uses the same comparison, independent of byte limits.
mkdir -p "$TMP/parity-files" "$TMP/parity-files-home"
PARITY_FILES_ROOT=$(CDPATH= cd "$TMP/parity-files" && pwd -P)
printf 'first' >"$TMP/parity-files/z.txt"
printf 'second' >"$TMP/parity-files/a.txt"
printf 'third' >"$TMP/parity-files/m.txt"
"$ROOT/$BUILD_DIR/samosa-fs" chutni-inventory --root "$PARITY_FILES_ROOT" \
  --max-files 2 --max-directories 5000 --max-seconds 20 --max-depth 32 \
  --max-file-bytes 64 --max-eligible-bytes 1024 >"$TMP/parity-files-preview.ndjson"
HOME="$TMP/parity-files-home" CHUTNI_HOME="$TMP/parity-files-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$PARITY_FILES_ROOT\",\"confirmed\":true,\"register\":true,\"label\":\"File-count parity\",\"max_depth\":32,\"max_files\":2,\"max_directories\":5000,\"max_seconds\":20,\"max_file_size_bytes\":64,\"max_eligible_bytes\":1024}" \
  >"$TMP/parity-files-scan.json"
HOME="$TMP/parity-files-home" CHUTNI_HOME="$TMP/parity-files-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_list_sources \
  "{\"store_path\":\"$PARITY_FILES_ROOT.chutni\",\"source_path\":\"$PARITY_FILES_ROOT\",\"limit\":200}" \
  >"$TMP/parity-files-sources.json"
python3 "$ROOT/tests/assert_chutni_inventory_parity.py" \
  "$TMP/parity-files-preview.ndjson" "$TMP/parity-files-scan.json" \
  "$TMP/parity-files-sources.json" "$PARITY_FILES_ROOT"

# Per-file ceiling also skips the same oversized path without stopping later
# eligible files.
mkdir -p "$TMP/parity-file-size" "$TMP/parity-file-size-home"
PARITY_FILE_SIZE_ROOT=$(CDPATH= cd "$TMP/parity-file-size" && pwd -P)
printf 'aa' >"$TMP/parity-file-size/a.txt"
printf '12345' >"$TMP/parity-file-size/b-too-large.txt"
printf 'ccc' >"$TMP/parity-file-size/c.txt"
"$ROOT/$BUILD_DIR/samosa-fs" chutni-inventory --root "$PARITY_FILE_SIZE_ROOT" \
  --max-files 100 --max-directories 5000 --max-seconds 20 --max-depth 32 \
  --max-file-bytes 4 --max-eligible-bytes 1024 >"$TMP/parity-file-size-preview.ndjson"
HOME="$TMP/parity-file-size-home" CHUTNI_HOME="$TMP/parity-file-size-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$PARITY_FILE_SIZE_ROOT\",\"confirmed\":true,\"register\":true,\"label\":\"Per-file parity\",\"max_depth\":32,\"max_files\":100,\"max_directories\":5000,\"max_seconds\":20,\"max_file_size_bytes\":4,\"max_eligible_bytes\":1024}" \
  >"$TMP/parity-file-size-scan.json"
HOME="$TMP/parity-file-size-home" CHUTNI_HOME="$TMP/parity-file-size-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_list_sources \
  "{\"store_path\":\"$PARITY_FILE_SIZE_ROOT.chutni\",\"source_path\":\"$PARITY_FILE_SIZE_ROOT\",\"limit\":200}" \
  >"$TMP/parity-file-size-sources.json"
python3 "$ROOT/tests/assert_chutni_inventory_parity.py" \
  "$TMP/parity-file-size-preview.ndjson" "$TMP/parity-file-size-scan.json" \
  "$TMP/parity-file-size-sources.json" "$PARITY_FILE_SIZE_ROOT"

# Directory budget excludes the selected root and admits exactly one child
# directory in both walkers.
mkdir -p "$TMP/parity-directories/a-first" "$TMP/parity-directories/b-second" \
  "$TMP/parity-directories-home"
PARITY_DIRS_ROOT=$(CDPATH= cd "$TMP/parity-directories" && pwd -P)
printf 'first child file' >"$TMP/parity-directories/a-first/first.txt"
printf 'second child file' >"$TMP/parity-directories/b-second/second.txt"
printf 'top-level file' >"$TMP/parity-directories/z-top.txt"
"$ROOT/$BUILD_DIR/samosa-fs" chutni-inventory --root "$PARITY_DIRS_ROOT" \
  --max-files 100 --max-directories 1 --max-seconds 20 --max-depth 32 \
  --max-file-bytes 64 --max-eligible-bytes 1024 >"$TMP/parity-directories-preview.ndjson"
HOME="$TMP/parity-directories-home" CHUTNI_HOME="$TMP/parity-directories-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$PARITY_DIRS_ROOT\",\"confirmed\":true,\"register\":true,\"label\":\"Directory-count parity\",\"max_depth\":32,\"max_files\":100,\"max_directories\":1,\"max_seconds\":20,\"max_file_size_bytes\":64,\"max_eligible_bytes\":1024}" \
  >"$TMP/parity-directories-scan.json"
HOME="$TMP/parity-directories-home" CHUTNI_HOME="$TMP/parity-directories-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_list_sources \
  "{\"store_path\":\"$PARITY_DIRS_ROOT.chutni\",\"source_path\":\"$PARITY_DIRS_ROOT\",\"limit\":200}" \
  >"$TMP/parity-directories-sources.json"
python3 "$ROOT/tests/assert_chutni_inventory_parity.py" \
  "$TMP/parity-directories-preview.ndjson" "$TMP/parity-directories-scan.json" \
  "$TMP/parity-directories-sources.json" "$PARITY_DIRS_ROOT"

# Depth boundary marks child directories opaque, reports partial coverage, and
# leaves root-level files available to both traversals.
mkdir -p "$TMP/parity-depth/child" "$TMP/parity-depth-home"
PARITY_DEPTH_ROOT=$(CDPATH= cd "$TMP/parity-depth" && pwd -P)
printf 'in scope' >"$TMP/parity-depth/root.txt"
printf 'outside depth' >"$TMP/parity-depth/child/hidden.txt"
"$ROOT/$BUILD_DIR/samosa-fs" chutni-inventory --root "$PARITY_DEPTH_ROOT" \
  --max-files 100 --max-directories 5000 --max-seconds 20 --max-depth 0 \
  --max-file-bytes 64 --max-eligible-bytes 1024 >"$TMP/parity-depth-preview.ndjson"
HOME="$TMP/parity-depth-home" CHUTNI_HOME="$TMP/parity-depth-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$PARITY_DEPTH_ROOT\",\"confirmed\":true,\"register\":true,\"label\":\"Depth parity\",\"max_depth\":0,\"max_files\":100,\"max_directories\":5000,\"max_seconds\":20,\"max_file_size_bytes\":64,\"max_eligible_bytes\":1024}" \
  >"$TMP/parity-depth-scan.json"
HOME="$TMP/parity-depth-home" CHUTNI_HOME="$TMP/parity-depth-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_list_sources \
  "{\"store_path\":\"$PARITY_DEPTH_ROOT.chutni\",\"source_path\":\"$PARITY_DEPTH_ROOT\",\"limit\":200}" \
  >"$TMP/parity-depth-sources.json"
python3 "$ROOT/tests/assert_chutni_inventory_parity.py" \
  "$TMP/parity-depth-preview.ndjson" "$TMP/parity-depth-scan.json" \
  "$TMP/parity-depth-sources.json" "$PARITY_DEPTH_ROOT"

# A deterministic delay crosses the one-second deadline before any file is
# admitted, proving both walkers stop with the same reason and selected set.
mkdir -p "$TMP/parity-deadline" "$TMP/parity-deadline-home"
PARITY_DEADLINE_ROOT=$(CDPATH= cd "$TMP/parity-deadline" && pwd -P)
printf 'one' >"$TMP/parity-deadline/one.txt"
printf 'two' >"$TMP/parity-deadline/two.txt"
SAMOSA_CHUTNI_TEST_DELAY_US=1100000 \
  "$ROOT/$BUILD_DIR/samosa-fs" chutni-inventory --root "$PARITY_DEADLINE_ROOT" \
  --max-files 100 --max-directories 5000 --max-seconds 1 --max-depth 32 \
  --max-file-bytes 64 --max-eligible-bytes 1024 >"$TMP/parity-deadline-preview.ndjson"
HOME="$TMP/parity-deadline-home" CHUTNI_HOME="$TMP/parity-deadline-home/chutni" \
CHUTNI_TEST_SCAN_DELAY_US=1100000 \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$PARITY_DEADLINE_ROOT\",\"confirmed\":true,\"register\":true,\"label\":\"Deadline parity\",\"max_depth\":32,\"max_files\":100,\"max_directories\":5000,\"max_seconds\":1,\"max_file_size_bytes\":64,\"max_eligible_bytes\":1024}" \
  >"$TMP/parity-deadline-scan.json"
HOME="$TMP/parity-deadline-home" CHUTNI_HOME="$TMP/parity-deadline-home/chutni" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_list_sources \
  "{\"store_path\":\"$PARITY_DEADLINE_ROOT.chutni\",\"source_path\":\"$PARITY_DEADLINE_ROOT\",\"limit\":200}" \
  >"$TMP/parity-deadline-sources.json"
python3 "$ROOT/tests/assert_chutni_inventory_parity.py" \
  "$TMP/parity-deadline-preview.ndjson" "$TMP/parity-deadline-scan.json" \
  "$TMP/parity-deadline-sources.json" "$PARITY_DEADLINE_ROOT"

if "$SAMOSA_EXTRACT" --version 2>/dev/null | grep -q ';pdfium)'; then
  PDFIUM_ENABLED=1
  EXPECT_READABLE=5
  EXPECT_METADATA_ONLY=0
  EXPECT_CONTENT_ARTIFACTS=17
  EXPECT_PDF_PAGES=7
  EXPECT_SUMMARIES=5
  EXPECT_MODEL_ARTIFACTS=6
  EXPECT_ENRICHMENT_FAILURES=0
else
  # Runtime-only releases deliberately retain text/HTML/DOCX extraction but
  # do not advertise PDF. Chutni must finish honestly with the PDF as metadata
  # instead of making this portable release gate depend on a host PDFium SDK.
  PDFIUM_ENABLED=0
  EXPECT_READABLE=4
  EXPECT_METADATA_ONLY=1
  EXPECT_CONTENT_ARTIFACTS=9
  EXPECT_PDF_PAGES=0
  EXPECT_SUMMARIES=4
  EXPECT_MODEL_ARTIFACTS=5
  EXPECT_ENRICHMENT_FAILURES=1
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
SAMOSA_FS="$ROOT/$BUILD_DIR/samosa-fs" \
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
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}],\"user_exclusions\":[\"PRIVATE\"]}")
[ "$CODE" = 401 ] || { echo "FAIL: Chutni route did not fail closed" >&2; exit 1; }

INVALID_EXCLUSIONS=$(curl -sS -o "$TMP/invalid-exclusions.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}],\"user_exclusions\":[\"../outside\"]}")
[ "$INVALID_EXCLUSIONS" = 400 ] || fail "accepted a path-like user exclusion"

PF=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}],\"user_exclusions\":[\"PRIVATE\"]}")
PREFLIGHT=$(printf '%s' "$PF" | sed -n 's/.*"preflight_id":"\([^"]*\)".*/\1/p')
[ -n "$PREFLIGHT" ] || fail "missing preflight_id"
POLICY=$(printf '%s' "$PF" | tr '\n' ' ' | sed -n 's/.*"policy_fingerprint":"\([^"]*\)".*/\1/p')
[ -n "$POLICY" ] || fail "missing policy fingerprint"
PF_DEFAULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}]}")
POLICY_DEFAULT=$(printf '%s' "$PF_DEFAULT" | tr '\n' ' ' | sed -n 's/.*"policy_fingerprint":"\([^"]*\)".*/\1/p')
[ -n "$POLICY_DEFAULT" ] && [ "$POLICY" != "$POLICY_DEFAULT" ] || fail "user exclusions were not bound into the policy fingerprint"
printf '%s' "$PF" | grep -q '"inventory":{"regular_files":'
printf '%s' "$PF" | grep -Fq '"regular_files":5'
printf '%s' "$PF" | grep -q '"directories_entered":'
printf '%s' "$PF" | grep -q '"representative_path":"node_modules"'
printf '%s' "$PF" | grep -q '"representative_path":".venv"'
printf '%s' "$PF" | grep -q '"reason":"user_exclusion"'
printf '%s' "$PF" | grep -Fq '"user_exclusions":["private"]'
printf '%s' "$PF" | grep -q '"action":"create_store"'
printf '%s' "$PF" | grep -q '"store_path":'
printf '%s' "$PF" | grep -q '\.chutni'
STORE=$(printf '%s' "$PF" | sed -n 's/.*"store_path":"\([^"]*\)".*/\1/p')
[ -n "$STORE" ] || fail "missing store_path"

STALE_POLICY_CODE=$(curl -sS -o "$TMP/stale-policy.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT\",\"policy_fingerprint\":\"wrong\",\"display_name\":\"Research\"}")
[ "$STALE_POLICY_CODE" = 409 ] || fail "scope confirmation accepted a stale policy fingerprint"

CREATED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$PREFLIGHT\",\"policy_fingerprint\":\"$POLICY\",\"display_name\":\"Research\",\"summary_token_budget\":128}")
SCOPE=$(printf '%s' "$CREATED" | sed -n 's/.*"scope_id":"\([^"]*\)".*/\1/p')
JOB=$(printf '%s' "$CREATED" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[ -n "$SCOPE" ] && [ -n "$JOB" ] || fail "scope ID missing from create response"

# A matching policy token is insufficient if the selected directory itself was
# replaced after preview.
mkdir -p "$TMP/stale-source"
printf 'stale root fixture\n' >"$TMP/stale-source/file.txt"
STALE_PF=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/stale-source\"}]}")
STALE_ID=$(printf '%s' "$STALE_PF" | sed -n 's/.*"preflight_id":"\([^"]*\)".*/\1/p')
STALE_POLICY=$(printf '%s' "$STALE_PF" | tr '\n' ' ' | sed -n 's/.*"policy_fingerprint":"\([^"]*\)".*/\1/p')
mv "$TMP/stale-source" "$TMP/stale-source-old"
mkdir "$TMP/stale-source"
STALE_ROOT_CODE=$(curl -sS -o "$TMP/stale-root.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$STALE_ID\",\"policy_fingerprint\":\"$STALE_POLICY\",\"display_name\":\"Stale fixture\"}")
[ "$STALE_ROOT_CODE" = 409 ] || fail "scope confirmation accepted a replaced folder root"

i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "$STATUS" >&2; cat "$TMP/gateway.log" >&2; fail "scope never reached ready state"; }

[ -f "$STORE/manifest.json" ]
[ -f "$STORE/catalog.sqlite" ]
[ -f "$STORE/indexes/lexical.sqlite" ]
printf '%s' "$STATUS" | grep -q '"files_indexed":5'
printf '%s' "$STATUS" | grep -q '"summary_token_budget":128'
printf '%s' "$STATUS" | grep -Fq '"user_exclusions":["private"]'
printf '%s' "$STATUS" | grep -q "\"content_readable_files\":$EXPECT_READABLE"
printf '%s' "$STATUS" | grep -q "\"metadata_only_files\":$EXPECT_METADATA_ONLY"
APP_POLICY=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_status \
  "{\"path\":\"$TMP/source\"}")
printf '%s' "$APP_POLICY" | grep -q '"inventory_policy_version":1'

POLICY_CHANGE_CODE=$(curl -sS -o "$TMP/existing-policy.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}],\"user_exclusions\":[\"new-private\"]}")
[ "$POLICY_CHANGE_CODE" = 200 ] || fail "existing index policy mismatch did not produce a rebuild preview"
grep -q '"rebuild_required":true' "$TMP/existing-policy.json"
mkdir -p "$TMP/legacy-policy-source"
printf 'legacy index evidence\n' >"$TMP/legacy-policy-source/legacy.txt"
HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$TMP/legacy-policy-source\",\"confirmed\":true,\"register\":true,\"label\":\"Legacy fixture\"}" \
  >"$TMP/legacy-policy-create.json"
LEGACY_CODE=$(curl -sS -o "$TMP/legacy-policy.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/legacy-policy-source\"}]}" )
[ "$LEGACY_CODE" = 200 ] || fail "legacy index did not produce a rebuild preview"
grep -q '"rebuild_required":true' "$TMP/legacy-policy.json"
mkdir -p "$TMP/future-policy-source"
printf 'future index evidence\n' >"$TMP/future-policy-source/future.txt"
HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" \
  "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$TMP/future-policy-source\",\"confirmed\":true,\"register\":true,\"label\":\"Future fixture\",\"inventory_policy_version\":2}" \
  >"$TMP/future-policy-create.json"
FUTURE_CODE=$(curl -sS -o "$TMP/future-policy.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/future-policy-source\"}]}" )
[ "$FUTURE_CODE" = 200 ] || fail "unsupported future policy did not produce a rebuild preview"
grep -q '"rebuild_required":true' "$TMP/future-policy.json"
printf '%s' "$STATUS" | grep -q "\"content_artifacts\":$EXPECT_CONTENT_ARTIFACTS"
printf '%s' "$STATUS" | grep -q '"phase":"complete"'
printf '%s' "$STATUS" | grep -q '"scan_files_seen":5'
printf '%s' "$STATUS" | grep -q '"enrichment_files_total":5'
printf '%s' "$STATUS" | grep -q '"enrichment_files_done":5'
printf '%s' "$STATUS" | grep -q "\"pdf_pages_read\":$EXPECT_PDF_PAGES"
printf '%s' "$STATUS" | grep -q '"ocr_outputs":1'
printf '%s' "$STATUS" | grep -q '"image_captions":1'
printf '%s' "$STATUS" | grep -q "\"summaries_created\":$EXPECT_SUMMARIES"
printf '%s' "$STATUS" | grep -q "\"enrichment_failures\":$EXPECT_ENRICHMENT_FAILURES"
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
printf '%s' "$STATUS" | grep -q "\"content_readable_files\":$EXPECT_READABLE"
printf '%s' "$STATUS" | grep -q "\"metadata_only_files\":$EXPECT_METADATA_ONLY"

# Samosa enrichment is committed into the portable protocol store, not a
# private cache: PDF pages, image OCR, captions, and summaries are searchable
# with their protocol artifact kinds and provenance.
PDF_RESULT=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"synthetic fixture document\",\"limit\":20}")
if [ "$PDFIUM_ENABLED" = 1 ]; then
  printf '%s' "$PDF_RESULT" | grep -q '"artifact_kind":"page_text"'
  printf '%s' "$PDF_RESULT" | grep -q 'guide.pdf'
else
  ! printf '%s' "$PDF_RESULT" | grep -q '"artifact_kind":"page_text"'
fi
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
  | grep -q "^$EXPECT_PDF_PAGES\$"
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN derivations d USING(derivation_id) JOIN producers p USING(producer_id) WHERE a.artifact_kind IN ('image_caption','summary_short') AND a.status='active' AND p.producer_kind='model' AND p.model_id='qwen3.6-35b-a3b' AND p.model_revision<>'' AND p.app_name='Samosa';" \
  | grep -q "^$EXPECT_MODEL_ARTIFACTS\$"
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN sources s USING(source_id) WHERE a.artifact_kind='summary_short' AND a.status='active' AND json_extract(s.locator_json,'$.display_path') LIKE '%/guide.pdf' AND a.selector_json='{\"type\":\"pages\",\"start\":1,\"end\":3}' AND a.inline_text NOT LIKE 'ERROR:%';" \
  | grep -q "^$PDFIUM_ENABLED\$"
sqlite3 "$DB_URI" \
  "SELECT count(*) FROM artifacts a JOIN derivations d USING(derivation_id) WHERE a.artifact_kind='summary_short' AND a.status='active' AND d.recipe_hash='samosa-summary-leading-content-v1' AND json_extract(d.parameters_json,'$.summary_input')='leading_content_window' AND json_extract(d.parameters_json,'$.token_budget')=128 AND json_extract(d.parameters_json,'$.token_estimator')='utf8_bytes_div_4_v1' AND json_extract(d.parameters_json,'$.max_input_bytes')=512 AND a.inline_text NOT LIKE 'ERROR:%';" \
  | grep -q "^$EXPECT_SUMMARIES\$"

RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"renewal\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$RESULT" | grep -q '"used":true'
printf '%s' "$RESULT" | grep -q 'report.txt'
printf '%s' "$RESULT" | grep -q '"freshness":"current"'
HTML_RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"HTML_SENTINEL_CEDAR_HARBOR\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$HTML_RESULT" | grep -q 'fixture.html'
printf '%s' "$HTML_RESULT" | grep -q 'HTML_SENTINEL_CEDAR_HARBOR'
EXCLUDED_RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"DEPENDENCY_TREE_SECRET_SENTINEL\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$EXCLUDED_RESULT" | grep -q '"used":false'
STORE_EXCLUDED_RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"GENERATED_STORE_SECRET_SENTINEL\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$STORE_EXCLUDED_RESULT" | grep -q '"used":false'
USER_EXCLUDED_RESULT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"USER_EXCLUSION_SECRET_SENTINEL\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$USER_EXCLUDED_RESULT" | grep -q '"used":false'
! printf '%s' "$EXCLUDED_RESULT" | grep -q 'ignored.txt'

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
  --data-binary "{\"model\":\"qwen3.6-35b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"find the chutni memory probe about renewal date\"}],\"directory_context\":{\"scope_id\":\"$SCOPE\"},\"stream\":false}")
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

# Removal is reconciled on refresh as well as content changes.
rm "$TMP/source/notes.md"
REMOVED_REFRESH=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE/refresh" --data-binary '{}')
[ -n "$(printf '%s' "$REMOVED_REFRESH" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')" ] || fail "removal refresh did not create a job"
i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' &&
    printf '%s' "$STATUS" | grep -q '"evidence_generation":3' && break
  sleep 0.05; i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "$STATUS" >&2; fail "removed file was not refreshed"; }
REMOVED=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"handoff\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$REMOVED" | grep -q '"used":false'

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

# Rebuilding an existing app scope requires a matching preflight and an
# explicit second confirmation. The reset withdraws old indexed passages
# before it applies the new exclusion policy.
mkdir -p "$TMP/source/new-private"
printf 'REBUILD_STALE_SENTINEL\n' >"$TMP/source/new-private/stale.txt"
HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_scan \
  "{\"store_path\":\"$STORE\",\"confirmed\":true,\"app_name\":\"rebuild-fixture\",\"app_version\":\"1\"}" \
  >"$TMP/rebuild-seed.json"
SEEDED=$(HOME="$TMP/home" CHUTNI_HOME="$TMP/chutni-home" "$ROOT/$BUILD_DIR/chutni-mcp" --call chutni_search \
  "{\"store_path\":\"$STORE\",\"query\":\"REBUILD_STALE_SENTINEL\"}")
printf '%s' "$SEEDED" | grep -q 'new-private/stale.txt'
REBUILD_PREFLIGHT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}],\"user_exclusions\":[\"new-private\"]}")
printf '%s' "$REBUILD_PREFLIGHT" | grep -q '"rebuild_required":true'
REBUILD_PREFLIGHT_ID=$(printf '%s' "$REBUILD_PREFLIGHT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["preflight_id"])')
REBUILD_FINGERPRINT=$(printf '%s' "$REBUILD_PREFLIGHT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["policy_fingerprint"])')
NO_CONFIRM_CODE=$(curl -sS -o "$TMP/rebuild-unconfirmed.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$REBUILD_PREFLIGHT_ID\",\"policy_fingerprint\":\"$REBUILD_FINGERPRINT\",\"display_name\":\"source\",\"summary_token_budget\":512}")
[ "$NO_CONFIRM_CODE" = 409 ] || fail "rebuild started without explicit confirmation"
grep -q 'rebuild_confirmation_required' "$TMP/rebuild-unconfirmed.json"
REBUILD_CREATE_CODE=$(curl -sS -o "$TMP/rebuild-create.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/scopes" \
  --data-binary "{\"preflight_id\":\"$REBUILD_PREFLIGHT_ID\",\"policy_fingerprint\":\"$REBUILD_FINGERPRINT\",\"display_name\":\"source\",\"summary_token_budget\":512,\"confirm_rebuild\":true}")
[ "$REBUILD_CREATE_CODE" = 201 ] || { cat "$TMP/rebuild-create.json" >&2; fail "confirmed rebuild did not start"; }
i=0
while [ "$i" -lt 300 ]; do
  STATUS=$(curl -fsS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/chutni/scopes/$SCOPE")
  printf '%s' "$STATUS" | grep -q '"state":"ready"' &&
    printf '%s' "$STATUS" | grep -q '"evidence_generation":4' && break
  sleep 0.05; i=$((i + 1))
done
[ "$i" -lt 300 ] || { echo "$STATUS" >&2; cat "$TMP/gateway.log" >&2; fail "confirmed rebuild did not complete"; }
REBUILT_POLICY=$(printf '%s' "$STATUS")
printf '%s' "$REBUILT_POLICY" | grep -q '"user_exclusions":\["new-private"\]'
REBUILT_QUERY=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/query" \
  --data-binary "{\"query\":\"REBUILD_STALE_SENTINEL\",\"directory_context\":{\"scope_id\":\"$SCOPE\"}}")
printf '%s' "$REBUILT_QUERY" | grep -q '"used":false'
REMOVED_RULE_PREFLIGHT=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H 'Content-Type: application/json' -X POST \
  "http://127.0.0.1:$PORT/v1/chutni/preflight" \
  --data-binary "{\"kind\":\"folder\",\"roots\":[{\"path\":\"$TMP/source\"}]}")
printf '%s' "$REMOVED_RULE_PREFLIGHT" | grep -q '"rebuild_required":true'

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
