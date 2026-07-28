#!/bin/sh
set -eu
# T4.3: the extraction/OCR cache must actually be reused across rebuilds.
#
# Every build writes a brand-new index.g{N}.sqlite3 and unlinks anything at
# that path, so the `contents` table starts empty each time. Before this test
# existed the table was written and never read, which meant every rebuild
# re-ran samosa-extract and samosa-ocr over files that had not changed -- the
# expensive half of a build, repeated for nothing.
#
# The stub extractor counts its own invocations, so "was the sidecar skipped"
# is measured rather than inferred from a timing or a log line.

DB_BIN=${SAMOSA_CHUTNI_DB:-./build/samosa-chutni-db}
TOKENIZER=${SAMOSA_TOKENIZER:-tokenizer_qwen36.json}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-cache.XXXXXX")
ROOT="$TMP/root"; STATE="$TMP/state"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
fail() { echo "test_chutni_extraction_cache.sh: $1" >&2; exit 1; }

mkdir -p "$ROOT"
# Two PDFs by magic bytes; the stub decides what "extraction" returns, so no
# real PDF machinery is involved and the test stays hermetic.
printf '%%PDF-1.4 first document\n' >"$ROOT/one.pdf"
printf '%%PDF-1.4 second document\n' >"$ROOT/two.pdf"

COUNT="$TMP/extract-calls"
: >"$COUNT"
cat >"$TMP/stub-extract" <<'SH'
#!/bin/sh
# argv is: --json PATH
printf '%s\n' "$2" >>"$EXTRACT_COUNT"
printf '{"ok":true,"text":"renewal clause for %s"}' "$(basename "$2")"
SH
chmod +x "$TMP/stub-extract"
export EXTRACT_COUNT="$COUNT"

calls() { wc -l <"$COUNT" | tr -d ' '; }

"$DB_BIN" scope-create "$STATE" cache folder Cache "$ROOT" >/dev/null

# --- Build 1: nothing cached, so both PDFs must be extracted ---------------
"$DB_BIN" scope-build "$STATE" cache "$TOKENIZER" "$TMP/stub-extract" >/dev/null
[ "$(calls)" = "2" ] || fail "first build should extract both PDFs, got $(calls)"
"$DB_BIN" scope-query "$STATE" cache renewal | grep -q 'one.pdf' \
  || fail "first build did not index the extracted text"

# --- Build 2: nothing changed, so the sidecar must not run at all ----------
: >"$COUNT"
"$DB_BIN" scope-build "$STATE" cache "$TOKENIZER" "$TMP/stub-extract" >"$TMP/build2.log" 2>&1
[ "$(calls)" = "0" ] || fail "rebuild re-extracted $(calls) unchanged file(s); cache not reused"
grep -q 'reused 2 cached extractions' "$TMP/build2.log" \
  || fail "the build did not report reusing both extractions"
# Reuse must produce a real index, not an empty one that merely looks fast.
"$DB_BIN" scope-query "$STATE" cache renewal | grep -q 'one.pdf' \
  || fail "a cache-reusing build lost the indexed text"

# --- A changed file misses, and only that file ------------------------------
printf '%%PDF-1.4 second document, revised\n' >"$ROOT/two.pdf"
: >"$COUNT"
"$DB_BIN" scope-build "$STATE" cache "$TOKENIZER" "$TMP/stub-extract" >/dev/null 2>&1
[ "$(calls)" = "1" ] || fail "a single changed file should cost one extraction, got $(calls)"
grep -q 'two.pdf' "$COUNT" || fail "the wrong file was re-extracted"

# --- A changed extractor invalidates everything it produced -----------------
# This is why the fingerprint is the binary's own SHA-256: a fixed version
# string would keep serving text from an extractor that no longer exists.
cat >"$TMP/stub-extract" <<'SH'
#!/bin/sh
printf '%s\n' "$2" >>"$EXTRACT_COUNT"
printf '{"ok":true,"text":"rewritten clause for %s"}' "$(basename "$2")"
SH
chmod +x "$TMP/stub-extract"
: >"$COUNT"
"$DB_BIN" scope-build "$STATE" cache "$TOKENIZER" "$TMP/stub-extract" >/dev/null 2>&1
[ "$(calls)" = "2" ] || fail "a changed extractor must invalidate its cache, got $(calls) call(s)"
"$DB_BIN" scope-query "$STATE" cache rewritten | grep -q 'one.pdf' \
  || fail "the new extractor's text was not indexed"

# --- A missing previous generation is a miss, never a failure ---------------
rm -f "$STATE"/scopes/cache/index.g*.sqlite3
: >"$COUNT"
"$DB_BIN" scope-build "$STATE" cache "$TOKENIZER" "$TMP/stub-extract" >/dev/null 2>&1 \
  || fail "a build must survive the previous generation being gone"
[ "$(calls)" = "2" ] || fail "a build with no prior index must extract everything, got $(calls)"

echo 'test_chutni_extraction_cache.sh: PASS'
