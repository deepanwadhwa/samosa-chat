#!/bin/sh
set -e

echo "Running security tests..."

# Ensure we have a clean db
rm -f /tmp/chutni_sec.db
./build/samosa-chutni init /tmp/chutni_sec.db > /dev/null

# 1. System Exclusion Test
mkdir -p /tmp/test_chutni_sec/.ssh
touch /tmp/test_chutni_sec/.ssh/id_rsa

# Scan the test directory
OUT=$(./build/samosa-chutni scan /tmp/chutni_sec.db sec_root /tmp/test_chutni_sec)
echo "$OUT"

# Verify that 0 files were seen/indexed because .ssh is excluded
if echo "$OUT" | grep -q '"files_seen": 0'; then
    echo "System exclusion test: PASS"
else
    echo "System exclusion test: FAIL (should have 0 files seen)"
    exit 1
fi

rm -rf /tmp/test_chutni_sec /tmp/chutni_sec.db
echo "test_chutni_security.sh: PASS"
