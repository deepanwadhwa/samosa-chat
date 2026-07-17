#!/bin/sh
# Optional integration test: a real macOS PDFium archive must survive the
# verified-release installer and be usable from its final, relative rpath.
set -eu

ARCHIVE=${PDFIUM_MAC_ARM64_ARCHIVE:-}
if [ -z "$ARCHIVE" ] || [ ! -f "$ARCHIVE" ]; then
  echo "document installer: SKIP (set PDFIUM_MAC_ARM64_ARCHIVE to the reviewed artifact)"
  exit 0
fi
if [ "$(uname -s):$(uname -m)" != "Darwin:arm64" ]; then
  echo "document installer: SKIP (macOS arm64 artifact test)"
  exit 0
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-document-installer.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
SNAP="$TMP/snapshot"
REMOTE="$TMP/remote"
PDFIUM="$TMP/pdfium"
HOME_DIR="$TMP/home"
mkdir -p "$SNAP" "$PDFIUM"

printf 'experts fixture\n' >"$SNAP/experts.bin"
printf 'resident fixture\n' >"$SNAP/resident.safetensors"
printf '{"experts":{}}\n' >"$SNAP/manifest.json"
printf '{"text_config":{}}\n' >"$SNAP/config.json"
printf '{}\n' >"$SNAP/generation_config.json"
printf '{}\n' >"$TMP/tokenizer.json"
cp "$ARCHIVE" "$PDFIUM/pdfium-mac-arm64.tgz"
# package_hf requires a complete release set; the installer under test selects
# only the real macOS archive, never these inert Linux placeholders.
printf 'not selected on this platform\n' >"$PDFIUM/pdfium-linux-x64.tgz"
printf 'not selected on this platform\n' >"$PDFIUM/pdfium-linux-arm64.tgz"

python3 "$ROOT/tools/package_hf.py" --out "$REMOTE" --snapshot "$SNAP" \
  --tokenizer "$TMP/tokenizer.json" --repo-id test/samosa --pdfium-dir "$PDFIUM" >/dev/null
SAMOSA_INSTALL_TEST=1 SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
  SAMOSA_BASE_URL="file://$REMOTE" SAMOSA_HOME="$HOME_DIR" \
  sh "$ROOT/dist/install.sh" >/dev/null

RELEASE=$(readlink "$HOME_DIR/current")
RELEASE_DIR="$HOME_DIR/$RELEASE"
[ -x "$RELEASE_DIR/bin/samosa-extract" ]
[ -f "$RELEASE_DIR/lib/libpdfium.dylib" ]
"$RELEASE_DIR/bin/samosa-extract" --json "$ROOT/tests/fixtures/documents/hello.pdf" |
  grep -F '"ok":true' >/dev/null
otool -L "$RELEASE_DIR/bin/qwen36b" | grep -F 'libpdfium' >/dev/null && {
  echo "document installer: engine gained a PDFium dependency" >&2
  exit 1
}

echo "document installer: PASS"
