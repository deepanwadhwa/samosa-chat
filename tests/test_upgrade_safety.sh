#!/bin/sh
set -e

echo "Running upgrade safety tests..."

rm -f /tmp/chutni_upgrade.db

# 1. Initialize and add data
./build/samosa-chutni init /tmp/chutni_upgrade.db > /dev/null
sqlite3 /tmp/chutni_upgrade.db "INSERT INTO roots (root_id, path, volume_identity, root_file_identity) VALUES ('r1', '/path', 'v1', 'f1');"
sqlite3 /tmp/chutni_upgrade.db "INSERT INTO files (id, root_id, rel_path, size, mtime, status) VALUES (1, 'r1', 'file.txt', 100, 1000, 'indexed');"

# 2. Re-initialize (simulate upgrade/migration idempotency)
./build/samosa-chutni init /tmp/chutni_upgrade.db > /dev/null

# 3. Verify data is intact
COUNT=$(sqlite3 /tmp/chutni_upgrade.db "SELECT COUNT(*) FROM files;")
if [ "$COUNT" -eq 1 ]; then
    echo "Idempotent schema init test: PASS"
else
    echo "Idempotent schema init test: FAIL (data was erased)"
    exit 1
fi

rm -f /tmp/chutni_upgrade.db
echo "test_upgrade_safety.sh: PASS"
