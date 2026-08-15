#!/bin/sh
# Install the current source build as a local development release without
# copying the large group-32 model. Model payloads are hard-linked on APFS.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODEL_ROOT=${SAMOSA_MODELS_DIR:-"$(dirname "$ROOT")/samosa-models"}
SNAPSHOT=${SAMOSA_SNAPSHOT:-"$MODEL_ROOT/qwen36_group32_i8"}
TOKENIZER=${SAMOSA_TOKENIZER:-"$MODEL_ROOT/tokenizer_qwen36.json"}
MAPLE_MODEL=${SAMOSA_MAPLE_MODEL_SOURCE:-"$MODEL_ROOT/maple"}
HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
BUILD_DIR=${SAMOSA_BUILD_DIR:-"$ROOT/${BUILD_DIR:-build}"}
ENGINE="$BUILD_DIR/qwen36b"
MAPLE_ENGINE="$BUILD_DIR/samosa-maple"
MAPLE_METALLIB="$BUILD_DIR/mlx-build/mlx/backend/metal/kernels/mlx.metallib"
FS_SIDECAR="$BUILD_DIR/samosa-fs"
GATEWAY="$BUILD_DIR/samosa-gateway"
JOBSD="$BUILD_DIR/samosa-jobsd"
CHUTNI_SERVICE="$BUILD_DIR/chutni-mcp"
OCR="$BUILD_DIR/samosa-ocr"
SUMMARIZER_RUNTIME="$BUILD_DIR/native-summarizer-runtime"
SUMMARIZER="$SUMMARIZER_RUNTIME/bin/samosa-summarizer"
SUMMARIZER_MODEL="$BUILD_DIR/samosa-text-summarization-Q8_0.gguf"
SUMMARIZER_LIBS="libllama.0.dylib libggml.0.dylib libggml-cpu.0.dylib libggml-blas.0.dylib libggml-metal.0.dylib libggml-base.0.dylib"

# The application itself is what this installer must always be able to produce.
# A model is *content*: the app is expected to start with none installed, show
# the setup flow, and offer the catalogue for download. Requiring a 24 GB
# snapshot here made a model-less install impossible, which is backwards.
for path in "$ENGINE" "$MAPLE_ENGINE" "$MAPLE_METALLIB" "$FS_SIDECAR" "$GATEWAY" "$JOBSD" "$CHUTNI_SERVICE" "$OCR" "$ROOT/assets/app.html" "$ROOT/assets/samosa-chat.png" \
  "$ROOT/assets/models.json" "$ROOT/tools/samosa_voice_runtime.sh" "$ROOT/tools/samosa_kokoro_runtime.sh" \
  "$ROOT/dist/samosa"; do
  [ -f "$path" ] || { echo "missing local development input: $path" >&2; exit 1; }
done

# The native T5 summarizer is part of the Apple-Silicon application runtime,
# not an optional developer tool. Keep the dev install shaped like a published
# release and fail before activation when the staged runtime is incomplete.
SUMMARIZER_OK=0
if [ "$(uname -s):$(uname -m)" = "Darwin:arm64" ]; then
  SUMMARIZER_OK=1
  for path in "$SUMMARIZER" "$SUMMARIZER_MODEL"; do
    [ -f "$path" ] || SUMMARIZER_OK=0
  done
  for name in $SUMMARIZER_LIBS; do
    [ -f "$SUMMARIZER_RUNTIME/lib/$name" ] || SUMMARIZER_OK=0
  done
  if [ "$SUMMARIZER_OK" != "1" ]; then
    echo "missing native summarizer inputs; run tools/stage_native_summarizer_runtime.sh and place the verified GGUF at $SUMMARIZER_MODEL" >&2
    exit 1
  fi
fi

MAPLE_MODEL_OK=1
for path in "$MAPLE_MODEL/config.json" "$MAPLE_MODEL/tokenizer.json" \
  "$MAPLE_MODEL/maple-experts.bin" "$MAPLE_MODEL/maple-resident.safetensors" \
  "$MAPLE_MODEL/maple-manifest.json"; do
  [ -f "$path" ] || MAPLE_MODEL_OK=0
done

# The Qwen snapshot is staged when it happens to be present, and skipped
# cleanly when it is not. Partial snapshots are treated as absent rather than
# hard-linked in pieces.
SNAPSHOT_OK=1
for path in "$SNAPSHOT/experts.bin" "$SNAPSHOT/resident.safetensors" \
  "$SNAPSHOT/manifest.json" "$SNAPSHOT/config.json" \
  "$SNAPSHOT/generation_config.json"; do
  [ -f "$path" ] || SNAPSHOT_OK=0
done
[ -f "$TOKENIZER" ] || SNAPSHOT_OK=0

# Discover the optional document extractor before calculating the release ID.
# Otherwise rebuilding only this sidecar can silently reuse a stale release.
EXTRACT_BIN=""
EXTRACT_LIB=""
for candidate in "$BUILD_DIR/samosa-extract" "$ROOT/dist/samosa-extract"; do
  [ -x "$candidate" ] && EXTRACT_BIN="$candidate" && break
done
for candidate in "$ROOT/dist/libpdfium.dylib" "$ROOT/libpdfium.dylib" "$ROOT/dist/libpdfium.so" "$ROOT/libpdfium.so"; do
  [ -f "$candidate" ] && EXTRACT_LIB="$candidate" && break
done

set -- "$ENGINE" "$MAPLE_ENGINE" "$MAPLE_METALLIB" "$FS_SIDECAR" "$GATEWAY" "$JOBSD" "$CHUTNI_SERVICE" "$OCR" \
  "$ROOT/assets/app.html" "$ROOT/assets/models.json" "$ROOT/tools/install_local_dev.sh" \
  "$ROOT/tools/samosa_voice_runtime.sh" "$ROOT/tools/samosa_kokoro_runtime.sh" "$ROOT/dist/samosa"
if [ "$SNAPSHOT_OK" = "1" ]; then set -- "$@" "$SNAPSHOT/manifest.json"; fi
if [ "$MAPLE_MODEL_OK" = "1" ]; then set -- "$@" "$MAPLE_MODEL/maple-manifest.json"; fi
if [ -n "$EXTRACT_BIN" ] && [ -n "$EXTRACT_LIB" ]; then set -- "$@" "$EXTRACT_BIN" "$EXTRACT_LIB"; fi
if [ "$SUMMARIZER_OK" = "1" ]; then
  set -- "$@" "$SUMMARIZER" "$SUMMARIZER_MODEL"
  for name in $SUMMARIZER_LIBS; do
    set -- "$@" "$SUMMARIZER_RUNTIME/lib/$name"
  done
fi
release_hash=$(shasum -a 256 "$@" |
  shasum -a 256 | awk '{print substr($1,1,12)}')
release_id="dev-$release_hash"
stage="$HOME_DIR/releases/.${release_id}.partial.$$"
final="$HOME_DIR/releases/$release_id"
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/bin" "$HOME_DIR/models/qwen" "$HOME_DIR/releases" "$HOME_DIR/bin"
if [ "$MAPLE_MODEL_OK" = "1" ]; then mkdir -p "$stage/models/maple"; fi
if [ "$SUMMARIZER_OK" = "1" ]; then
  mkdir -p "$stage/lib" "$stage/models/native-summarizer"
fi

if [ "$SNAPSHOT_OK" = "1" ]; then
  for name in experts.bin resident.safetensors manifest.json config.json generation_config.json; do
    ln -f "$SNAPSHOT/$name" "$HOME_DIR/models/qwen/$name" || {
      echo "hard-link failed for $name; refusing to duplicate the model" >&2
      exit 1
    }
  done
  ln -f "$TOKENIZER" "$HOME_DIR/models/qwen/tokenizer_qwen36.json" || {
    echo "hard-link failed for tokenizer; refusing an implicit copy" >&2
    exit 1
  }
fi
cp "$ENGINE" "$stage/bin/qwen36b"
cp "$MAPLE_ENGINE" "$stage/bin/samosa-maple"
ln "$MAPLE_METALLIB" "$stage/bin/mlx.metallib" || {
  echo "hard-link failed for mlx.metallib; refusing to duplicate the Metal runtime" >&2
  exit 1
}
cp "$FS_SIDECAR" "$stage/bin/samosa-fs"
cp "$ROOT/dist/samosa" "$stage/bin/samosa"
cp "$GATEWAY" "$stage/bin/samosa-gateway"
cp "$JOBSD" "$stage/bin/samosa-jobsd"
cp "$CHUTNI_SERVICE" "$stage/bin/chutni-mcp"
cp "$OCR" "$stage/bin/samosa-ocr"
cp "$ROOT/tools/samosa_voice_runtime.sh" "$stage/bin/samosa-voice-runtime"
cp "$ROOT/tools/samosa_kokoro_runtime.sh" "$stage/bin/samosa-kokoro-runtime"
cp "$ROOT/assets/app.html" "$stage/app.html"
cp "$ROOT/assets/samosa-chat.png" "$stage/samosa-chat.png"
cp "$ROOT/assets/models.json" "$stage/models.json"
chmod +x "$stage/bin/qwen36b" "$stage/bin/samosa-fs" "$stage/bin/samosa" "$stage/bin/samosa-gateway" "$stage/bin/samosa-jobsd" "$stage/bin/chutni-mcp" "$stage/bin/samosa-ocr" "$stage/bin/samosa-voice-runtime" "$stage/bin/samosa-kokoro-runtime"
chmod +x "$stage/bin/samosa-maple"

if [ "$SUMMARIZER_OK" = "1" ]; then
  cp "$SUMMARIZER" "$stage/bin/samosa-summarizer"
  for name in $SUMMARIZER_LIBS; do
    cp "$SUMMARIZER_RUNTIME/lib/$name" "$stage/lib/$name"
  done
  ln "$SUMMARIZER_MODEL" \
    "$stage/models/native-summarizer/samosa-text-summarization-Q8_0.gguf" || {
      echo "hard-link failed for the native summarizer model; refusing an implicit copy" >&2
      exit 1
    }
  chmod +x "$stage/bin/samosa-summarizer"
fi

if [ "$MAPLE_MODEL_OK" = "1" ]; then
  for file in "$MAPLE_MODEL"/*; do
    [ -f "$file" ] || continue
    ln "$file" "$stage/models/maple/$(basename "$file")" || {
      echo "hard-link failed for Maple asset $(basename "$file")" >&2
      exit 1
    }
  done
fi

# Document extraction (PDF text via libpdfium, docs/TASKS_DOCUMENTS.md) is an
# optional capability, not a hard dependency of this installer: most dev
# checkouts have not run `make samosa-extract` (it needs PDFIUM_DIR set to an
# unpacked PDFium artifact). When both the sidecar and its dylib exist —
# checking repo root (the Makefile's freshly built output) before dist/ (the
# fallback prebuilt convention) — stage them together in
# bin/, where a loader-relative rpath finds the dylib.
if [ -n "$EXTRACT_BIN" ] && [ -n "$EXTRACT_LIB" ]; then
  cp "$EXTRACT_BIN" "$stage/bin/samosa-extract"
  cp "$EXTRACT_LIB" "$stage/bin/$(basename "$EXTRACT_LIB")"
  chmod +x "$stage/bin/samosa-extract"
  if [ "$(uname -s)" = "Darwin" ] &&
     ! otool -l "$stage/bin/samosa-extract" | grep -F 'path @loader_path (offset' >/dev/null; then
    install_name_tool -add_rpath @loader_path "$stage/bin/samosa-extract"
  fi
  EXTRACT_SMOKE_INPUT="$stage/.samosa-extract-interface-smoke.txt"
  EXTRACT_SMOKE_LOG="$stage/.samosa-extract-interface-smoke.log"
  printf 'not a pdf\n' >"$EXTRACT_SMOKE_INPUT"
  if "$stage/bin/samosa-extract" --json-pages "$EXTRACT_SMOKE_INPUT" 1 1 >"$EXTRACT_SMOKE_LOG" 2>&1; then
    echo "staged document extractor accepted a non-PDF interface smoke input" >&2
    exit 1
  fi
  if grep -F 'not_pdf' "$EXTRACT_SMOKE_LOG" >/dev/null; then
    rm -f "$EXTRACT_SMOKE_INPUT" "$EXTRACT_SMOKE_LOG"
    DOCUMENTS_ENABLED=1
  else
    sed -n '1,40p' "$EXTRACT_SMOKE_LOG" >&2 || true
    echo "warning: skipping incompatible document extractor; Maple/app installation will continue" >&2
    rm -f "$stage/bin/samosa-extract" "$stage/bin/$(basename "$EXTRACT_LIB")"
    rm -f "$EXTRACT_SMOKE_INPUT" "$EXTRACT_SMOKE_LOG"
    DOCUMENTS_ENABLED=0
  fi
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
if [ "$SNAPSHOT_OK" = "1" ]; then
  echo "Model files were hard-linked, not copied."
else
  echo "No local Qwen snapshot found, so none was staged."
  echo "  The app starts without one: open it and pick a model to download."
fi
if [ "$DOCUMENTS_ENABLED" = "1" ]; then
  echo "Document reading: on (PDF text via $final/bin/samosa-extract; OCR via $final/bin/samosa-ocr)."
else
  echo "OCR reading: on ($final/bin/samosa-ocr)."
  echo "PDF text reading: off — samosa-extract/libpdfium.dylib not found."
  echo "  Build with: PDFIUM_DIR=<unpacked pdfium> make samosa-extract, then re-run this installer."
fi
if [ "$SUMMARIZER_OK" = "1" ]; then
  echo "Native summaries: on ($final/bin/samosa-summarizer)."
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
