#!/bin/sh
set -eu
DB_BIN=${SAMOSA_CHUTNI_DB:-./build/samosa-chutni-db}
case "$DB_BIN" in
  /*) ;;
  *) DB_BIN="$PWD/$DB_BIN" ;;
esac

TOKENIZER=${SAMOSA_TOKENIZER:-"$PWD/tokenizer_qwen36.json"}

fail() {
  echo "test_chutni_scope.sh: FAIL: $1" >&2
  exit 1
}

[ "${SAMOSA_TEST_TRACE:-0}" = 1 ] && set -x
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-scope.XXXXXX")
ROOT="$TMP/root"; STATE="$TMP/state"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$ROOT/docs" "$ROOT/duplicate/a" "$ROOT/duplicate/b" "$ROOT/.samosa"
printf 'The renewal date is June.\n' >"$ROOT/docs/report.md"
printf 'same bytes\n' >"$ROOT/duplicate/a/copy.txt"; cp "$ROOT/duplicate/a/copy.txt" "$ROOT/duplicate/b/copy.txt"
printf 'hidden\n' >"$ROOT/.hidden.txt"; printf 'private\n' >"$ROOT/.samosa/private.txt"
printf 'PK\003\004unsupported\n' >"$ROOT/archive.zip"; ln -s /etc "$ROOT/escape"
"$DB_BIN" scope-create "$STATE" research folder Research "$ROOT" >/dev/null
if "$DB_BIN" scope-create "$STATE" research folder Duplicate "$ROOT" >/dev/null 2>&1; then echo 'FAIL: duplicate scope accepted' >&2; exit 1; fi
BUILD1_LOG="$TMP/build1.log"
"$DB_BIN" scope-build "$STATE" research "$TOKENIZER" \
  >"$BUILD1_LOG" 2>&1 || {
    cat "$BUILD1_LOG" >&2
    fail "initial scope-build failed"
  }
python3 - "$STATE" <<'PY'
import json, sqlite3, sys
from pathlib import Path
p=Path(sys.argv[1])/'scopes'/'research'; s=json.loads((p/'scope.json').read_text())
assert s['state']=='ready' and s['freshness_state']=='complete',s
assert s['files_indexed']+s['files_skipped']==s['regular_files_seen'],s
assert len(s['policy_fingerprint'])==64 and s['effective_policy']['mandatory_exclusions']==['.samosa']
a=json.loads((p/'active.json').read_text()); assert a['evidence_generation']==1
d=sqlite3.connect(p/a['database']); names={r[0] for r in d.execute("select name from sqlite_master where type in ('table','view')")}
assert {'manifest','scope_meta','publications'} <= names and not any('token_id' in n.lower() for n in names)
paths=[r[0] for r in d.execute("select relative_path from files where status='discovered' order by relative_path")]
assert paths==['docs/report.md','duplicate/a/copy.txt','duplicate/b/copy.txt'],paths
assert d.execute("select path_bytes from manifest where file_id=(select file_id from files where relative_path='docs/report.md')").fetchone()[0]==b'docs/report.md'
assert d.execute('select count(*) from chunks').fetchone()[0]==3
assert d.execute("select count(*) from publications where state='validated'").fetchone()[0]==1; d.close()
PY
QUERY1=$("$DB_BIN" scope-query "$STATE" research renewal 2>"$TMP/query1.err") || {
  cat "$TMP/query1.err" >&2
  fail "initial scope-query command failed"
}

printf '%s\n' "$QUERY1" | grep -q 'docs/report.md' || {
  echo "$QUERY1" >&2
  fail "initial query did not return docs/report.md"
}
printf 'The renewal date is July.\n' >"$ROOT/docs/report.md"
BUILD2_LOG="$TMP/build2.log"
"$DB_BIN" scope-build "$STATE" research "$TOKENIZER" \
  >"$BUILD2_LOG" 2>&1 || {
    cat "$BUILD2_LOG" >&2
    fail "second scope-build failed"
  }
python3 - "$STATE" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1])/'scopes'/'research'; s=json.loads((p/'scope.json').read_text()); a=json.loads((p/'active.json').read_text())
assert s['evidence_generation']==2 and a['evidence_generation']==2 and (p/'index.g1.sqlite3').exists() and (p/'index.g2.sqlite3').exists()
PY
QUERY2=$("$DB_BIN" scope-query "$STATE" research July 2>"$TMP/query2.err") || {
  cat "$TMP/query2.err" >&2
  fail "second scope-query command failed"
}

printf '%s\n' "$QUERY2" | grep -q 'docs/report.md' || {
  echo "$QUERY2" >&2
  fail "July query did not return docs/report.md"
}
echo 'test_chutni_scope.sh: PASS'
