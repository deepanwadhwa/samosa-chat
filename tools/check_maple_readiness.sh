#!/bin/sh
set -eu

VALIDATION_RECORD=".maple_validation_record.json"
BUILD_DIR="${BUILD_DIR:-build}"
MAPLE_EXE="$BUILD_DIR/samosa-maple"

echo "================================================================"
echo "MAPLE READINESS CHECK"
echo "================================================================"

echo ""
echo "1. Building samosa-maple and test-maple-native..."
make samosa-maple test-maple-native || { echo "FAIL: Build failed"; exit 1; }
echo "   Build: PASS"

echo ""
echo "2. Zero-Python audit (otool -L)..."
if otool -L "$MAPLE_EXE" | grep -i python; then
    echo "   FAIL: samosa-maple depends on Python!"
    exit 1
fi
echo "   Zero-Python: PASS"

echo ""
echo "3. Running native unit tests..."
METAL_PATH="$BUILD_DIR/mlx-build/mlx/backend/metal/kernels" "$BUILD_DIR/test-maple-native" || { echo "FAIL: Native tests failed"; exit 1; }
echo "   Native tests: PASS"

echo ""
echo "4. Checking validation record..."
if [ ! -f "$VALIDATION_RECORD" ]; then
    echo "   No validation record found."
    echo ""
    echo "================================================================"
    echo "NATIVE IMPLEMENTATION PASSES"
    echo "REAL-MODEL VALIDATION NOT RUN"
    echo "================================================================"
    exit 0
fi

# Compute current binary hash and git revision
CURRENT_BINARY_SHA=$(shasum -a 256 "$MAPLE_EXE" | awk '{print $1}')
CURRENT_GIT_REV=$(git rev-parse HEAD 2>/dev/null || echo "unknown")

# Validate the record is not stale
python3 -c "
import json, sys

with open('$VALIDATION_RECORD') as f:
    record = json.load(f)

errors = []

# Check binary hash matches
recorded_sha = record.get('samosa_maple_sha256', '')
if recorded_sha != '$CURRENT_BINARY_SHA':
    errors.append(f'Binary SHA mismatch: record={recorded_sha[:16]}... current=$CURRENT_BINARY_SHA')

# Check git revision matches
recorded_git = record.get('samosa_git_commit', '')
if recorded_git != '$CURRENT_GIT_REV':
    errors.append(f'Git commit mismatch: record={recorded_git[:12]}... current=$CURRENT_GIT_REV')

# Check checkpoint revision
if record.get('checkpoint_revision') != '361db5da5e74ff6fcdd852d478e1f266ce11013a':
    errors.append('Checkpoint revision mismatch')

# Check both gates passed
parity = record.get('parity_result', 'NOT_RUN')
lifecycle = record.get('lifecycle_result', 'NOT_RUN')

if errors:
    print('   STALE VALIDATION RECORD:')
    for e in errors:
        print(f'     - {e}')
    print('')
    print('   The implementation has changed since the last validation.')
    print('   Re-run: make test-maple-parity && sh tests/test_maple_real_lifecycle.sh')
    print('')
    print('================================================================')
    print('NATIVE IMPLEMENTATION PASSES')
    print('REAL-MODEL VALIDATION STALE')
    print('================================================================')
    sys.exit(0)

if parity != 'PASS':
    print(f'   Parity result: {parity}')
    print(f'   Lifecycle result: {lifecycle}')
    print('')
    print('================================================================')
    print('NATIVE IMPLEMENTATION PASSES')
    if parity == 'NOT_RUN':
        print('PARITY NOT RUN')
    else:
        print(f'PARITY: {parity}')
    if lifecycle == 'NOT_RUN':
        print('LIFECYCLE NOT RUN')
    else:
        print(f'LIFECYCLE: {lifecycle}')
    print('================================================================')
    sys.exit(0)

if lifecycle != 'PASS':
    print(f'   Parity result: {parity}')
    print(f'   Lifecycle result: {lifecycle}')
    print('')
    print('================================================================')
    print('NATIVE IMPLEMENTATION PASSES')
    print('PARITY: PASS')
    if lifecycle == 'NOT_RUN':
        print('LIFECYCLE NOT RUN')
    else:
        print(f'LIFECYCLE: {lifecycle}')
    print('================================================================')
    sys.exit(0)

# Both passed and record is current
print(f'   Checkpoint:    {record[\"checkpoint_revision\"]}')
print(f'   Git commit:    {record[\"samosa_git_commit\"]}')
print(f'   Binary SHA:    {record[\"samosa_maple_sha256\"][:16]}...')
print(f'   MLX revision:  {record.get(\"mlx_revision\", \"unknown\")}')
print(f'   Parity:        {parity}')
print(f'   Lifecycle:     {lifecycle}')
print(f'   Validated at:  {record.get(\"timestamp\", \"unknown\")}')
print('')
print('================================================================')
print('READY FOR CATALOGUE')
print('================================================================')
"
