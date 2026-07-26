#!/bin/sh
set -eu

# Authors the frozen "corrupt Chutni index" fixtures for T0.1 (reused by
# T4.1's quick_check/FTS-integrity tests and T4.5's crash-injection tests).
# This script is a one-time authoring tool — it requires the system `sqlite3`
# CLI to build a realistic baseline database, but its OUTPUT (committed under
# tests/fixtures/ui_chutni/corrupt_index/) is what tests actually read. Per
# CLAUDE.md, the shipped product never depends on a system SQLite at runtime;
# this script only runs on a developer machine to regenerate fixtures.
#
# Usage: gen_corrupt_index_fixture.sh OUTPUT_DIR

out="${1:?usage: gen_corrupt_index_fixture.sh OUTPUT_DIR}"
mkdir -p "$out"

VALID="$out/valid.sqlite3"
rm -f "$VALID"
sqlite3 "$VALID" <<'SQL'
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO metadata (key, value) VALUES ('schema_version', '1');
CREATE TABLE files (
  id TEXT PRIMARY KEY, relative_path BLOB NOT NULL, size INTEGER NOT NULL,
  mtime_ns INTEGER NOT NULL, content_sha256 TEXT NOT NULL, generation INTEGER NOT NULL
);
INSERT INTO files VALUES ('f1', 'report.md', 42, 1700000000000000000, 'abc123', 1);
CREATE TABLE chunks (
  id TEXT PRIMARY KEY, file_id TEXT NOT NULL REFERENCES files(id),
  ordinal INTEGER NOT NULL, text TEXT NOT NULL, generation INTEGER NOT NULL
);
INSERT INTO chunks VALUES ('c1', 'f1', 0, 'Quarterly report draft.', 1);
CREATE VIRTUAL TABLE chunks_fts USING fts5(text, content='chunks', content_rowid='rowid');
INSERT INTO chunks_fts(rowid, text) SELECT rowid, text FROM chunks;
SQL
sqlite3 "$VALID" "PRAGMA integrity_check;" | grep -q '^ok$' || {
  echo "gen_corrupt_index_fixture.sh: baseline database failed its own integrity check" >&2
  exit 1
}

# truncated.sqlite3: crash mid-write — cut off after 60% of the valid bytes.
FULL_SIZE=$(wc -c <"$VALID" | tr -d ' ')
KEEP=$((FULL_SIZE * 60 / 100))
dd if="$VALID" of="$out/truncated.sqlite3" bs=1 count="$KEEP" 2>/dev/null

# bad_header.sqlite3: full length, but the 16-byte "SQLite format 3\0" magic
# is overwritten — a wrong-file/wrong-format corruption, distinct from
# mid-file damage.
cp "$VALID" "$out/bad_header.sqlite3"
python3 -c "
path = '$out/bad_header.sqlite3'
data = bytearray(open(path, 'rb').read())
data[0:16] = b'NOT A SQLITEDB\\0\\0'
open(path, 'wb').write(data)
"

# garbage.sqlite3: same size as the valid database, but not a SQLite file at
# all (no magic, no page structure).
python3 -c "
import os
size = os.path.getsize('$VALID')
data = bytes((i * 91 + 7) % 256 for i in range(size))
open('$out/garbage.sqlite3', 'wb').write(data)
"

# zero_length.sqlite3: the empty-file edge case some corruption detectors
# mishandle (a brand-new file whose staging write never got past creat()).
: >"$out/zero_length.sqlite3"

echo "gen_corrupt_index_fixture.sh: wrote valid/truncated/bad_header/garbage/zero_length.sqlite3 under $out"
