#!/bin/sh
# Install the current source build as a local development release without
# copying the large group-32 model. Model payloads are hard-linked on APFS.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODEL_ROOT=${SAMOSA_MODELS_DIR:-"$(dirname "$ROOT")/samosa-models"}
SNAPSHOT=${SAMOSA_SNAPSHOT:-"$MODEL_ROOT/qwen36_group32_i8"}
TOKENIZER=${SAMOSA_TOKENIZER:-"$MODEL_ROOT/tokenizer_qwen36.json"}
HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
BUILD_DIR=${SAMOSA_BUILD_DIR:-"$ROOT/build"}
ENGINE="$BUILD_DIR/qwen36b"
FS_SIDECAR="$BUILD_DIR/samosa-fs"
GATEWAY="$BUILD_DIR/samosa-gateway"
JOBSD="$BUILD_DIR/samosa-jobsd"

for path in "$ENGINE" "$FS_SIDECAR" "$GATEWAY" "$JOBSD" "$ROOT/assets/app.html" "$ROOT/assets/samosa-chat.png" \
  "$ROOT/dist/samosa" \
  "$SNAPSHOT/experts.bin" "$SNAPSHOT/resident.safetensors" \
  "$SNAPSHOT/manifest.json" "$SNAPSHOT/config.json" \
  "$SNAPSHOT/generation_config.json" "$TOKENIZER"; do
  [ -f "$path" ] || { echo "missing local development input: $path" >&2; exit 1; }
done

release_hash=$(shasum -a 256 "$ENGINE" "$FS_SIDECAR" "$GATEWAY" "$JOBSD" "$ROOT/assets/app.html" "$ROOT/dist/samosa" |
  shasum -a 256 | awk '{print substr($1,1,12)}')
release_id="dev-$release_hash"
stage="$HOME_DIR/releases/.${release_id}.partial.$$"
final="$HOME_DIR/releases/$release_id"
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/bin" "$stage/model" "$HOME_DIR/releases" "$HOME_DIR/bin"

for name in experts.bin resident.safetensors manifest.json config.json generation_config.json; do
  ln "$SNAPSHOT/$name" "$stage/model/$name" || {
    echo "hard-link failed for $name; refusing to duplicate the model" >&2
    exit 1
  }
done
ln "$TOKENIZER" "$stage/tokenizer_qwen36.json" || {
  echo "hard-link failed for tokenizer; refusing an implicit copy" >&2
  exit 1
}
cp "$ENGINE" "$stage/bin/qwen36b"
cp "$FS_SIDECAR" "$stage/bin/samosa-fs"
cp "$ROOT/dist/samosa" "$stage/bin/samosa"
cp "$GATEWAY" "$stage/bin/samosa-gateway"
cp "$JOBSD" "$stage/bin/samosa-jobsd"
cp "$ROOT/assets/app.html" "$stage/app.html"
cp "$ROOT/assets/samosa-chat.png" "$stage/samosa-chat.png"
chmod +x "$stage/bin/qwen36b" "$stage/bin/samosa-fs" "$stage/bin/samosa" "$stage/bin/samosa-gateway" "$stage/bin/samosa-jobsd"

# Document extraction (PDF text via libpdfium, docs/TASKS_DOCUMENTS.md) is an
# optional capability, not a hard dependency of this installer: most dev
# checkouts have not run `make samosa-extract` (it needs PDFIUM_DIR set to an
# unpacked PDFium artifact). When both the sidecar and its dylib exist —
# checking repo root (the Makefile's freshly built output) before dist/ (the
# fallback prebuilt convention) — stage them together in
# bin/, where the binary's baked-in @loader_path rpath finds the dylib.
EXTRACT_BIN=""
EXTRACT_LIB=""
for candidate in "$BUILD_DIR/samosa-extract" "$ROOT/dist/samosa-extract"; do
  [ -x "$candidate" ] && EXTRACT_BIN="$candidate" && break
done
for candidate in "$ROOT/dist/libpdfium.dylib" "$ROOT/libpdfium.dylib" "$ROOT/dist/libpdfium.so" "$ROOT/libpdfium.so"; do
  [ -f "$candidate" ] && EXTRACT_LIB="$candidate" && break
done
if [ -n "$EXTRACT_BIN" ] && [ -n "$EXTRACT_LIB" ]; then
  cp "$EXTRACT_BIN" "$stage/bin/samosa-extract"
  cp "$EXTRACT_LIB" "$stage/bin/$(basename "$EXTRACT_LIB")"
  chmod +x "$stage/bin/samosa-extract"
  EXTRACT_SMOKE_INPUT="$stage/.samosa-extract-interface-smoke.txt"
  EXTRACT_SMOKE_LOG="$stage/.samosa-extract-interface-smoke.log"
  printf 'not a pdf\n' >"$EXTRACT_SMOKE_INPUT"
  if "$stage/bin/samosa-extract" --json-pages "$EXTRACT_SMOKE_INPUT" 1 1 >"$EXTRACT_SMOKE_LOG" 2>&1; then
    echo "staged document extractor accepted a non-PDF interface smoke input" >&2
    exit 1
  fi
  grep -F 'not_pdf' "$EXTRACT_SMOKE_LOG" >/dev/null || {
    sed -n '1,40p' "$EXTRACT_SMOKE_LOG" >&2 || true
    echo "staged document extractor does not support the required --json-pages interface" >&2
    exit 1
  }
  rm -f "$EXTRACT_SMOKE_INPUT" "$EXTRACT_SMOKE_LOG"
  DOCUMENTS_ENABLED=1
else
  DOCUMENTS_ENABLED=0
fi

if [ ! -d "$final" ]; then mv "$stage" "$final"; else rm -rf "$stage"; fi
rm -f "$HOME_DIR/.current.next"
ln -s "releases/$release_id" "$HOME_DIR/.current.next"
mv -fh "$HOME_DIR/.current.next" "$HOME_DIR/current"

cat >"$HOME_DIR/bin/samosa" <<'EOF'
#!/bin/sh
set -eu
HOME_DIR="${SAMOSA_HOME:-$HOME/.samosa}"
exec "$HOME_DIR/current/bin/samosa" "$@"
EOF
chmod +x "$HOME_DIR/bin/samosa"
trap - EXIT HUP INT TERM

echo "Installed local development release $release_id"
echo "Launcher: $HOME_DIR/bin/samosa"
echo "Model files were hard-linked, not copied."
if [ "$DOCUMENTS_ENABLED" = "1" ]; then
  echo "Document reading: on (PDF text via $final/bin/samosa-extract)."
else
  echo "Document reading: off — samosa-extract/libpdfium.dylib not found."
  echo "  Build with: PDFIUM_DIR=<unpacked pdfium> make samosa-extract, then re-run this installer."
fi

# Unlike dist/install.sh, this script never edits your shell rc — a dev install
# should not mutate your profile behind your back. So say plainly whether the
# launcher is reachable, instead of leaving you to find out via
# "command not found".
case ":$PATH:" in
  *":$HOME_DIR/bin:"*)
    echo "PATH: ok — 'samosa' is runnable in this shell."
    ;;
  *)
    echo
    echo "NOTE: $HOME_DIR/bin is not on your PATH, so 'samosa' will not be found."
    echo "      For this shell:   export PATH=\"\$HOME/.samosa/bin:\$PATH\""
    echo "      To make it stick: echo 'export PATH=\"\$HOME/.samosa/bin:\$PATH\"' >> ~/.zshrc"
    echo "      Or run it directly: $HOME_DIR/bin/samosa \"how are you\""
    ;;
esac
