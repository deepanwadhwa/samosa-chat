#!/bin/sh
set -e

echo "Testing samosa-chutni init..."

DB_PATH="/tmp/test_chutni_$$"

# 1. Initialize the database
./build/samosa-chutni init "$DB_PATH" > /tmp/init_out_$$

# Verify output is valid JSON and status is OK
grep '"status": "ok"' /tmp/init_out_$$ > /dev/null || {
    echo "FAIL: Expected status ok, got:"
    cat /tmp/init_out_$$
    exit 1
}

# 2. Check schema with sqlite3
sqlite3 "$DB_PATH" "SELECT value FROM meta WHERE key = 'schema_version';" | grep "1" > /dev/null || {
    echo "FAIL: schema_version != 1"
    exit 1
}

# 3. Verify FTS5 is available
sqlite3 "$DB_PATH" "INSERT INTO chunks_fts(content, file_id, chunk_index) VALUES('hello world', 1, 0);" || {
    echo "FAIL: Could not insert into FTS5 table"
    exit 1
}
sqlite3 "$DB_PATH" "SELECT file_id FROM chunks_fts WHERE chunks_fts MATCH 'hello';" | grep "1" > /dev/null || {
    echo "FAIL: FTS5 MATCH query failed"
    exit 1
}

# Cleanup
rm -f "$DB_PATH" /tmp/init_out_$$

echo "samosa-chutni init: PASS"
