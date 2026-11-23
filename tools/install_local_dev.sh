#!/bin/sh
# Install the current source build as a local development release without
# copying the large group-32 model. Model payloads are hard-linked on APFS.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODEL_ROOT=${SAMOSA_MODELS_DIR:-"$(dirname "$ROOT")/samosa-models"}
SNAPSHOT=${SAMOSA_SNAPSHOT:-"$MODEL_ROOT/qwen36_group32_i8"}
TOKENIZER=${SAMOSA_TOKENIZER:-"$MODEL_ROOT/tokenizer_qwen36.json"}
HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
BUILD_DIR=${SAMOSA_BUILD_DIR:-"$ROOT/${BUILD_DIR:-build}"}
ENGINE="$BUILD_DIR/qwen36b"
VISIONPSY_ENGINE="$BUILD_DIR/samosa-visionpsy"
VISIONPSY_METALLIB="$BUILD_DIR/mlx-build/mlx/backend/metal/kernels/mlx.metallib"
FS_SIDECAR="$BUILD_DIR/samosa-fs"
GATEWAY="$BUILD_DIR/samosa-gateway"
AUDIO_DECODE="$BUILD_DIR/samosa-audio-decode"
JOBSD="$BUILD_DIR/samosa-jobsd"
CHUTNI_SERVICE="$BUILD_DIR/chutni-mcp"
OCR="$BUILD_DIR/samosa-ocr"
SUMMARIZER_RUNTIME="$BUILD_DIR/native-summarizer-runtime"
SUMMARIZER="$SUMMARIZER_RUNTIME/bin/samosa-summarizer"
SUMMARIZER_MODEL="$BUILD_DIR/samosa-text-summarization-Q8_0.gguf"
SUMMARIZER_LIBS="libllama.0.dylib libggml.0.dylib libggml-cpu.0.dylib libggml-blas.0.dylib libggml-metal.0.dylib libggml-base.0.dylib"
BROWSER_VOICE_ASSETS="$ROOT/assets/voice/browser"
WHISPER_RUNTIME="$ROOT/components/samosa-inference/tools/samosa_voice_runtime.sh"
WHISPER_PROBE="$ROOT/components/samosa-inference/assets/whisper/qualification.wav"

# The installer stages binaries; it must never silently copy a stale jobsd or
# gateway after source changes. Rebuild both from this checkout before taking
# any release hash or validating inputs. Set SAMOSA_INSTALL_SKIP_BUILD=1 only
# for an external packaging workflow that has already built the exact source.
if [ "${SAMOSA_INSTALL_SKIP_BUILD:-0}" != "1" ]; then
  make -C "$ROOT" BUILD_DIR="$BUILD_DIR" SAMOSA_GATEWAY_FLAGS=-DSAMOSA_QWEN_ONLY \
    samosa-gateway samosa-jobsd samosa-fs samosa-ocr
fi

# The application itself is what this installer must always be able to produce.
# A model is *content*: the app is expected to start with none installed, show
# the setup flow, and offer the catalogue for download. Requiring a 24 GB
# snapshot here made a model-less install impossible, which is backwards.
for path in "$ENGINE" "$FS_SIDECAR" "$GATEWAY" "$JOBSD" "$CHUTNI_SERVICE" "$OCR" "$ROOT/assets/app.html" "$ROOT/assets/samosa-chat.png" \
  "$ROOT/assets/models.json" "$WHISPER_RUNTIME" "$WHISPER_PROBE" "$ROOT/tools/samosa_kokoro_runtime.sh" \
  "$ROOT/dist/samosa" "$BROWSER_VOICE_ASSETS/THIRD_PARTY.md"; do
  [ -f "$path" ] || { echo "missing local development input: $path" >&2; exit 1; }
done
if [ "$(uname -s)" = "Darwin" ] && [ ! -x "$AUDIO_DECODE" ]; then
  echo "missing local development input: $AUDIO_DECODE" >&2
  exit 1
fi
if [ "$(uname -s):$(uname -m)" = "Darwin:arm64" ]; then
  for path in "$VISIONPSY_ENGINE" "$VISIONPSY_METALLIB"; do
    [ -f "$path" ] || { echo "missing local development input: $path" >&2; exit 1; }
  done
fi
command -v node >/dev/null 2>&1 || { echo 'Node.js is required to assemble the release catalog' >&2; exit 1; }

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

# Discover the document extractor before calculating the release ID.
# Otherwise rebuilding only this sidecar can silently reuse a stale release.
EXTRACT_BIN=""
EXTRACT_LIB=""
for candidate in "$BUILD_DIR/samosa-extract" "$ROOT/dist/samosa-extract"; do
  [ -x "$candidate" ] && EXTRACT_BIN="$candidate" && break
done
case "$(uname -s)" in
  Darwin)
    for candidate in "$ROOT/dist/libpdfium.dylib" "$ROOT/libpdfium.dylib"; do
      [ -f "$candidate" ] && EXTRACT_LIB="$candidate" && break
    done
    ;;
  *)
    for candidate in "$ROOT/dist/libpdfium.so" "$ROOT/libpdfium.so"; do
      [ -f "$candidate" ] && EXTRACT_LIB="$candidate" && break
    done
    ;;
esac
"$ROOT/tools/validate_local_document_runtime.sh" "$EXTRACT_BIN" "$EXTRACT_LIB"

set -- "$ENGINE" "$FS_SIDECAR" "$GATEWAY" "$JOBSD" "$CHUTNI_SERVICE" "$OCR" \
  "$ROOT/assets/app.html" "$ROOT/assets/models.json" "$ROOT/tools/install_local_dev.sh" "$ROOT/tools/write_release_model_catalog.mjs" \
  "$WHISPER_RUNTIME" "$WHISPER_PROBE" "$ROOT/tools/samosa_kokoro_runtime.sh" \
  "$ROOT/tools/stage_tesseract_runtime.sh" \
  "$ROOT/dist/samosa"
if [ "$(uname -s)" = "Darwin" ]; then set -- "$@" "$AUDIO_DECODE"; fi
if [ "$(uname -s):$(uname -m)" = "Darwin:arm64" ]; then
  set -- "$@" "$VISIONPSY_ENGINE" "$VISIONPSY_METALLIB"
fi
for file in $(find "$BROWSER_VOICE_ASSETS" -type f -print | sort); do set -- "$@" "$file"; done
if [ "$SNAPSHOT_OK" = "1" ]; then set -- "$@" "$SNAPSHOT/manifest.json"; fi
if [ -n "$EXTRACT_BIN" ]; then set -- "$@" "$EXTRACT_BIN"; fi
if [ -n "$EXTRACT_LIB" ]; then set -- "$@" "$EXTRACT_LIB"; fi
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
mkdir -p "$stage/bin" "$stage/voice/browser" "$HOME_DIR/models/qwen" "$HOME_DIR/releases" "$HOME_DIR/bin"
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
cp "$FS_SIDECAR" "$stage/bin/samosa-fs"
cp "$ROOT/dist/samosa" "$stage/bin/samosa"
cp "$GATEWAY" "$stage/bin/samosa-gateway"
if [ "$(uname -s)" = "Darwin" ]; then
  cp "$AUDIO_DECODE" "$stage/bin/samosa-audio-decode"
  chmod +x "$stage/bin/samosa-audio-decode"
fi
cp "$JOBSD" "$stage/bin/samosa-jobsd"
cp "$CHUTNI_SERVICE" "$stage/bin/chutni-mcp"
cp "$OCR" "$stage/bin/samosa-ocr"
sh "$ROOT/tools/stage_tesseract_runtime.sh" "$stage"
if [ "$(uname -s):$(uname -m)" = "Darwin:arm64" ]; then
  cp "$VISIONPSY_ENGINE" "$VISIONPSY_METALLIB" "$stage/bin/"
fi
cp "$WHISPER_RUNTIME" "$stage/bin/samosa-voice-runtime"
mkdir -p "$stage/voice/whisper"
cp "$WHISPER_PROBE" "$stage/voice/whisper/qualification.wav"
cp "$ROOT/tools/samosa_kokoro_runtime.sh" "$stage/bin/samosa-kokoro-runtime"
cp "$ROOT/assets/app.html" "$stage/app.html"
cp "$ROOT/assets/samosa-chat.png" "$stage/samosa-chat.png"
node "$ROOT/tools/write_release_model_catalog.mjs" "$ROOT/assets/models.json" >"$stage/models.json"
for file in $(find "$BROWSER_VOICE_ASSETS" -type f -print | sort); do
  relative=${file#"$BROWSER_VOICE_ASSETS"/}
  mkdir -p "$stage/voice/browser/$(dirname "$relative")"
  cp "$file" "$stage/voice/browser/$relative"
done
chmod +x "$stage/bin/qwen36b" "$stage/bin/samosa-fs" "$stage/bin/samosa" "$stage/bin/samosa-gateway" "$stage/bin/samosa-jobsd" "$stage/bin/chutni-mcp" "$stage/bin/samosa-ocr" "$stage/bin/samosa-voice-runtime" "$stage/bin/samosa-kokoro-runtime"

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

# Text, HTML, and DOCX extraction is portable; PDF support is optional and is
# enabled when the built sidecar also has its PDFium library. Stage the
# sidecar whenever it exists and colocate PDFium only when present.
if [ -n "$EXTRACT_BIN" ]; then
  cp "$EXTRACT_BIN" "$stage/bin/samosa-extract"
  if "$EXTRACT_BIN" --version 2>/dev/null | grep -F ';pdfium)' >/dev/null; then
    cp "$EXTRACT_LIB" "$stage/bin/$(basename "$EXTRACT_LIB")"
  fi
  chmod +x "$stage/bin/samosa-extract"
  if [ -n "$EXTRACT_LIB" ] && [ "$(uname -s)" = "Darwin" ] &&
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
    echo "warning: skipping incompatible document extractor; app installation will continue" >&2
    rm -f "$stage/bin/samosa-extract"
    if [ -n "$EXTRACT_LIB" ]; then rm -f "$stage/bin/$(basename "$EXTRACT_LIB")"; fi
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
SAMOSA_VOICE_TRACE_AUTO="${SAMOSA_VOICE_TRACE_AUTO:-1}"
export SAMOSA_VOICE_TRACE_AUTO
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
  echo "  Open the app to download Qwen."
fi
if [ "$DOCUMENTS_ENABLED" = "1" ]; then
  echo "Document reading: on (text/HTML/DOCX via $final/bin/samosa-extract; OCR via $final/bin/samosa-ocr)."
  if "$final/bin/samosa-extract" --version | grep -F ';pdfium)' >/dev/null; then
    echo "PDF text reading: on (PDFium)."
  else
    echo "PDF text reading: off — rebuild with PDFIUM_DIR=<unpacked pdfium> to enable it."
  fi
else
  echo "OCR reading: on ($final/bin/samosa-ocr)."
  echo "Document text reading: off — samosa-extract was not built."
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
