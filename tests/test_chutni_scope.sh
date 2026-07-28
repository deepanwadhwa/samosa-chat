#!/bin/sh
set -eu
DB_BIN=${SAMOSA_CHUTNI_DB:-./build/samosa-chutni-db}
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
"$DB_BIN" scope-build "$STATE" research tokenizer_qwen36.json >/dev/null
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
$DB_BIN scope-query "$STATE" research renewal | grep -q 'docs/report.md'
printf 'The renewal date is July.\n' >"$ROOT/docs/report.md"
"$DB_BIN" scope-build "$STATE" research tokenizer_qwen36.json >/dev/null
python3 - "$STATE" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1])/'scopes'/'research'; s=json.loads((p/'scope.json').read_text()); a=json.loads((p/'active.json').read_text())
assert s['evidence_generation']==2 and a['evidence_generation']==2 and (p/'index.g1.sqlite3').exists() and (p/'index.g2.sqlite3').exists()
PY
$DB_BIN scope-query "$STATE" research July | grep -q 'docs/report.md'
echo 'test_chutni_scope.sh: PASS'
