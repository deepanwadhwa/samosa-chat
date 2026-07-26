#!/bin/sh
set -e

echo "Running scale tests..."

# Patch the quota limit to 100 for testing to avoid 4 minutes of popen overhead
sed -i '' 's/50000/100/g' src/samosa_chutni.c
make samosa-chutni > /dev/null

rm -f /tmp/chutni_scale.db
./build/samosa-chutni init /tmp/chutni_scale.db > /dev/null

rm -rf /tmp/test_chutni_scale
mkdir -p /tmp/test_chutni_scale

# Create 105 files
for i in $(seq 1 105); do
    touch "/tmp/test_chutni_scale/file_${i}.txt"
done

# Scan the directory
OUT=$(./build/samosa-chutni scan /tmp/chutni_scale.db scale_root /tmp/test_chutni_scale)
echo "$OUT"

# Restore original limits and rebuild
git checkout src/samosa_chutni.c
make samosa-chutni > /dev/null

# Verify that it hits the quota_exceeded limit of 100 files
if echo "$OUT" | grep -q 'quota_exceeded'; then
    echo "Quota exceeded test: PASS"
else
    echo "Quota exceeded test: FAIL (should hit quota_exceeded status)"
    exit 1
fi

if echo "$OUT" | grep -q '"files_seen": 100'; then
    echo "Quota exact boundary test: PASS"
else
    echo "Quota exact boundary test: FAIL (should stop at exactly 100)"
    exit 1
fi

rm -rf /tmp/test_chutni_scale /tmp/chutni_scale.db
echo "test_chutni_scale.sh: PASS"
