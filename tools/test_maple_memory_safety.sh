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
# Ensure maple_ref.Model(args) isn't used in fixture generators unless carefully guarded,
# or args comes from a synthetic config.
# We'll just do a rough check.
BAD_MODEL_INIT=$(grep -rn "maple_ref.Model(" tests/fixtures/maple/ 2>/dev/null | grep -v "361db")
if [ -n "$BAD_MODEL_INIT" ]; then
    echo "FAIL (investigate if safe)"
    echo "$BAD_MODEL_INIT"
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
