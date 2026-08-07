#!/bin/sh
set -eu

echo "Checking Maple Readiness..."

echo "1. Checking build..."
make samosa-maple test-maple-native || { echo "Build failed"; exit 1; }

echo "2. Checking Python dependency (otool -L)..."
if otool -L build/samosa-maple | grep -i python; then
    echo "FAIL: samosa-maple depends on Python!"
    exit 1
fi
echo "Python check passed (no python dependencies)."

echo "3. Running native unit tests..."
build/test-maple-native || { echo "Native tests failed"; exit 1; }

if [ -f ".maple_real_validation_passed" ]; then
    echo "READY FOR CATALOGUE"
else
    echo "NATIVE IMPLEMENTATION PASSES"
    echo "REAL-MODEL VALIDATION NOT RUN"
fi
