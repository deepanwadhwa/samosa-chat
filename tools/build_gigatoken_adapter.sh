#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MANIFEST="$ROOT/third_party/gigatoken-adapter/Cargo.toml"
TARGET="$ROOT/third_party/gigatoken-adapter/target/release/samosa-gigatoken-adapter"
OUT="$ROOT/${BUILD_DIR:-build}/samosa-gigatoken-adapter"

command -v cargo >/dev/null 2>&1 || {
    echo "cargo is required only to build the bundled adapter" >&2
    exit 2
}

cargo +nightly build --release --offline --manifest-path "$MANIFEST"
mkdir -p "$ROOT/${BUILD_DIR:-build}"
cp "$TARGET" "$OUT"
chmod 0755 "$OUT"
echo "$OUT"
