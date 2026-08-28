#!/bin/bash

echo "Maple Memory Safety Regression Test"
echo "-----------------------------------"

FAILED=0

# 1. Dangerous pattern check
echo -n "Dangerous pattern check: "
BAD_PATTERNS=$(grep -rn --exclude="test_maple_memory_safety.sh" "weights.update(mx.load" tests/fixtures/maple/ tools/ 2>/dev/null)
if [ -n "$BAD_PATTERNS" ]; then
    echo "FAIL"
    echo "Found dangerous weights.update() pattern:"
    echo "$BAD_PATTERNS"
    FAILED=1
else
    echo "PASS"
fi

# 2. Check full model construction in fixtures
echo -n "Full model construction in fixtures check: "
if ! python3 tools/audit_maple_fixture_models.py tests/fixtures/maple; then
    echo "FAIL"
    FAILED=1
else
    echo "PASS"
fi

# 3. Run components under guard
echo "Running synthetic fixtures..."
make test-maple-components
if [ $? -ne 0 ]; then
    echo "Synthetic fixtures FAIL"
    FAILED=1
else
    echo "Synthetic fixtures PASS"
fi

# 4. Run real checkpoint selective sanitization
echo "Running selective checkpoint read..."
make test-maple-sanitize
if [ $? -ne 0 ]; then
    echo "Selective checkpoint read FAIL"
    FAILED=1
else
    echo "Selective checkpoint read PASS"
fi

echo ""
if [ $FAILED -eq 0 ]; then
    echo "RESULT: SAFE"
    exit 0
else
    echo "RESULT: UNSAFE"
    exit 1
fi
