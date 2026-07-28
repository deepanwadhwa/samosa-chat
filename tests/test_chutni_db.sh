#!/bin/sh
set -eu

DB=$(mktemp "${TMPDIR:-/tmp}/samosa-chutni-db.XXXXXX")
cleanup() { rm -f "$DB" "$DB-wal" "$DB-shm"; }
trap cleanup EXIT HUP INT TERM

DB_BIN=${SAMOSA_CHUTNI_DB:-./build/samosa-chutni-db}
"$DB_BIN" init "$DB"
"$DB_BIN" add-file "$DB" file-a docs/a.md stable-a 10 100 sha-a text indexed
"$DB_BIN" add-file "$DB" file-b docs/b.md stable-b 10 101 sha-b text indexed
"$DB_BIN" add-chunk "$DB" chunk-b file-b 0 docs/b.md renewal renewal 1 0 0 20 "renewal date is June" 5
"$DB_BIN" add-chunk "$DB" chunk-a file-a 0 docs/a.md renewal renewal 1 0 0 20 "renewal date is May" 5

OUT=$($DB_BIN query "$DB" renewal)
FIRST=$(printf '%s\n' "$OUT" | sed -n '1p')
printf '%s\n' "$FIRST" | grep -q '^chunk-a\tdocs/a.md'

"$DB_BIN" integrity "$DB" | grep -q '"ok":true'
if sqlite3 "$DB" '.schema' 2>/dev/null | grep -q 'token_ids'; then
  echo "FAIL: token ID blob storage appeared in the schema" >&2
  exit 1
fi
echo "test_chutni_db.sh: PASS"
