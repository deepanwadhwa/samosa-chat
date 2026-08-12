#!/bin/sh
# Stage the pinned Prism/llama.cpp pieces needed by samosa-summarizer.
# The release packager consumes this directory; end-user installs do not run
# this script and do not need a C++ or Python runtime.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
APP_STATE="${SAMOSA_HOME:-${HOME}/.samosa}"
PRISM_ROOT="${PRISM_LLAMA_DIR:-$APP_STATE/backends/prism-llama.cpp}"
PRISM_BIN="${PRISM_LLAMA_BUILD:-$PRISM_ROOT/build}/bin"
OUT="${1:-$ROOT/build/native-summarizer-runtime}"

revision=$(git -C "$PRISM_ROOT" rev-parse HEAD 2>/dev/null || true)
[ "$revision" = "9fcaed763ccda38ea81068ad9d7f991aaddca451" ] || {
  echo "stage_native_summarizer_runtime: expected pinned Prism revision 9fcaed763ccda38ea81068ad9d7f991aaddca451, found ${revision:-none}" >&2
  exit 2
}

make -C "$ROOT" samosa-summarizer \
  PRISM_LLAMA_DIR="$PRISM_ROOT" PRISM_LLAMA_BUILD="${PRISM_LLAMA_BUILD:-$PRISM_ROOT/build}"
mkdir -p "$OUT/bin" "$OUT/lib"
cp "$ROOT/build/samosa-summarizer" "$OUT/bin/samosa-summarizer"

copy_library() {
  source=$1
  destination=$2
  [ -f "$PRISM_BIN/$source" ] || {
    echo "stage_native_summarizer_runtime: missing $PRISM_BIN/$source" >&2
    exit 2
  }
  cp -L "$PRISM_BIN/$source" "$OUT/lib/$destination"
}

copy_library libllama.0.dylib libllama.0.dylib
copy_library libggml.0.dylib libggml.0.dylib
copy_library libggml-cpu.0.dylib libggml-cpu.0.dylib
copy_library libggml-blas.0.dylib libggml-blas.0.dylib
copy_library libggml-metal.0.dylib libggml-metal.0.dylib
copy_library libggml-base.0.dylib libggml-base.0.dylib

chmod 0755 "$OUT/bin/samosa-summarizer"
echo "staged native summarizer runtime at $OUT"
