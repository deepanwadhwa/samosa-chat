#!/bin/sh
# Existing Samosa release assembly, ported from package_hf.py without Python.
# Model/runtime files are hard-linked where possible; source files are copied.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODEL_ROOT="$ROOT/../samosa-models"
OUT=""
SNAPSHOT="$MODEL_ROOT/qwen36_group32_i8"
TOKENIZER="$MODEL_ROOT/tokenizer_qwen36.json"
REPO_ID=REPO_ID_PLACEHOLDER
PDFIUM_DIR=""
MAPLE_RUNTIME="$ROOT/build/samosa-maple"
MAPLE_METALLIB="$ROOT/build/mlx-build/mlx/backend/metal/kernels/mlx.metallib"
VISIONPSY_RUNTIME="$ROOT/build/samosa-visionpsy"
MOLMO2_RUNTIME="$ROOT/build/samosa-molmo2"
MOLMO2_PACK="$ROOT/build/molmo2-pack"
AUDIO_DECODE_RUNTIME="$ROOT/build/samosa-audio-decode"
SUMMARIZER_MODEL="$ROOT/build/samosa-text-summarization-Q8_0.gguf"
SUMMARIZER_RUNTIME="$ROOT/build/native-summarizer-runtime"
OCR_RUNTIME=""
RUNTIME_ONLY=0

usage() {
  cat <<'EOF'
Usage: sh tools/package_hf.sh --out DIRECTORY [options]
  --snapshot DIRECTORY           existing Qwen model snapshot
  --tokenizer FILE               existing Qwen tokenizer
  --repo-id OWNER/REPOSITORY      substitute the distribution repository
  --pdfium-dir DIRECTORY         complete reviewed PDFium archive set
  --maple-runtime FILE           native Maple executable
  --maple-metallib FILE          paired Metal library
  --visionpsy-runtime FILE       native VisionPsy executable
  --molmo2-runtime FILE          native Molmo2 executable
  --molmo2-pack FILE             native Molmo2 package builder
  --audio-decode-runtime FILE    native audio decoder
  --summarizer-model FILE        pinned T5 GGUF
  --summarizer-runtime-dir DIR   staged T5 runtime
  --ocr-runtime-dir DIRECTORY    staged native Tesseract runtime
  --runtime-only                omit optional chat-model weights
EOF
}
fail() { printf '%s\n' "$*" >&2; exit 1; }
while [ "$#" -gt 0 ]; do
  option=$1; shift
  case "$option" in
    --help|-h) usage; exit 0 ;;
    --runtime-only) RUNTIME_ONLY=1; continue ;;
    --*=*) value=${option#*=}; option=${option%%=*} ;;
    --*) [ "$#" -gt 0 ] || fail "missing value for $option"; value=$1; shift ;;
    *) fail "unexpected argument: $option" ;;
  esac
  case "$option" in
    --out) OUT=$value ;;
    --snapshot) SNAPSHOT=$value ;;
    --tokenizer) TOKENIZER=$value ;;
    --repo-id) REPO_ID=$value ;;
    --pdfium-dir) PDFIUM_DIR=$value ;;
    --maple-runtime) MAPLE_RUNTIME=$value ;;
    --maple-metallib) MAPLE_METALLIB=$value ;;
    --visionpsy-runtime) VISIONPSY_RUNTIME=$value ;;
    --molmo2-runtime) MOLMO2_RUNTIME=$value ;;
    --molmo2-pack) MOLMO2_PACK=$value ;;
    --audio-decode-runtime) AUDIO_DECODE_RUNTIME=$value ;;
    --summarizer-model) SUMMARIZER_MODEL=$value ;;
    --summarizer-runtime-dir) SUMMARIZER_RUNTIME=$value ;;
    --ocr-runtime-dir) OCR_RUNTIME=$value ;;
    *) fail "unknown option: $option" ;;
  esac
done
[ -n "$OUT" ] || { usage >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/samosa-package.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
STAGED="$WORK/staged"
: >"$STAGED"
[ -f "$ROOT/VERSION" ] || fail "missing VERSION file: $ROOT/VERSION"
VERSION=$(awk '{gsub(/^[[:space:]]+|[[:space:]]+$/, ""); printf "%s", $0}' "$ROOT/VERSION")

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    digest=$(shasum -a 256 "$1") || return 1
  else
    digest=$(sha256sum "$1") || return 1
  fi
  printf '%s\n' "$digest" | awk '{print $1}'
}
file_size() {
  bytes=$(wc -c <"$1") || return 1
  printf '%s' "$bytes" | tr -d '[:space:]'
}
place() {
  # Positional arguments keep paths containing spaces out of word splitting.
  mkdir -p "$(dirname -- "$OUT/$2")"
  rm -f "$OUT/$2"
  if [ "$3" != link ] || ! ln "$1" "$OUT/$2" 2>/dev/null; then
    cp -p "$1" "$OUT/$2"
  fi
  printf '%s\n' "$2" >>"$STAGED"
}
replace_literal() {
  # Environment values avoid shell/sed replacement-string interpretation.
  SAMOSA_PACKAGE_FROM=$1 SAMOSA_PACKAGE_TO=$2 awk '
    BEGIN {from=ENVIRON["SAMOSA_PACKAGE_FROM"]; to=ENVIRON["SAMOSA_PACKAGE_TO"]}
    {rest=$0; output=""; while ((at=index(rest,from))>0) {
      output=output substr(rest,1,at-1) to; rest=substr(rest,at+length(from))
    } print output rest}
  ' "$3" >"$WORK/replaced"
  cat "$WORK/replaced" >"$3"
}
native_runtime() {
  [ -f "$1" ] || fail "missing Apple-Silicon native runtime: $1"
  if [ "$2" != mlx.metallib ] && [ ! -x "$1" ]; then
    fail "Apple-Silicon native runtime is not executable: $1"
  fi
  place "$1" "runtime/macos-arm64/$2" link
}
stage_summarizer() {
  [ -f "$SUMMARIZER_RUNTIME/bin/samosa-summarizer" ] &&
    [ -x "$SUMMARIZER_RUNTIME/bin/samosa-summarizer" ] ||
    fail 'missing native summarizer runtime; run tools/stage_native_summarizer_runtime.sh'
  place "$SUMMARIZER_RUNTIME/bin/samosa-summarizer" runtime/macos-arm64/samosa-summarizer link
  for name in libllama.0.dylib libggml.0.dylib libggml-cpu.0.dylib libggml-blas.0.dylib libggml-metal.0.dylib libggml-base.0.dylib; do
    [ -f "$SUMMARIZER_RUNTIME/lib/$name" ] || fail "missing native summarizer library: $SUMMARIZER_RUNTIME/lib/$name"
    place "$SUMMARIZER_RUNTIME/lib/$name" "runtime/macos-arm64/lib/$name" link
  done
  [ -f "$SUMMARIZER_MODEL" ] || fail "missing native summarizer model: $SUMMARIZER_MODEL"
  if [ "${SAMOSA_PACKAGE_TEST:-}" != 1 ]; then
    [ "$(file_size "$SUMMARIZER_MODEL")" = 65364992 ] &&
      [ "$(sha256_file "$SUMMARIZER_MODEL")" = 30c73e0b2ebd4f65538d87f5157aa7193261e8ff362e441df86c6941f3e9265c ] ||
      fail 'native summarizer model failed pinned byte/SHA-256 verification'
  fi
  place "$SUMMARIZER_MODEL" runtime/common/samosa-text-summarization-Q8_0.gguf link
}
stage_ocr() {
  if [ -z "$OCR_RUNTIME" ]; then
    OCR_RUNTIME="$WORK/ocr"
    [ -f "$ROOT/build/samosa-ocr" ] || fail 'missing native OCR executable; run make samosa-ocr'
    mkdir -p "$OCR_RUNTIME/bin"
    cp -p "$ROOT/build/samosa-ocr" "$OCR_RUNTIME/bin/samosa-ocr"
    sh "$ROOT/tools/stage_tesseract_runtime.sh" "$OCR_RUNTIME"
  fi
  for relative in bin/samosa-ocr share/tessdata/eng.traineddata lib/ocr/libtesseract.5.dylib; do
    [ -f "$OCR_RUNTIME/$relative" ] || fail "incomplete native OCR runtime: $relative"
  done
  if [ "${SAMOSA_PACKAGE_TEST:-}" != 1 ]; then "$OCR_RUNTIME/bin/samosa-ocr" --check >/dev/null; fi
  for directory in bin lib/ocr share/tessdata share/licenses/ocr; do
    [ -d "$OCR_RUNTIME/$directory" ] || continue
    find "$OCR_RUNTIME/$directory" \( -type f -o -type l \) -print | LC_ALL=C sort >"$WORK/ocr-files"
    while IFS= read -r source; do
      [ -f "$source" ] || continue
      place "$source" "runtime/macos-arm64/ocr/${source#"$OCR_RUNTIME/"}" copy
    done <"$WORK/ocr-files"
  done
}

if [ "$(uname -s):$(uname -m)" = Darwin:arm64 ]; then
  native_runtime "$MAPLE_RUNTIME" samosa-maple
  native_runtime "$VISIONPSY_RUNTIME" samosa-visionpsy
  native_runtime "$MOLMO2_RUNTIME" samosa-molmo2
  native_runtime "$MOLMO2_PACK" molmo2-pack
  native_runtime "$AUDIO_DECODE_RUNTIME" samosa-audio-decode
  native_runtime "$MAPLE_METALLIB" mlx.metallib
  [ -f "$ROOT/assets/molmo2/processor.json" ] || fail "missing Molmo2 processor contract: $ROOT/assets/molmo2/processor.json"
  place "$ROOT/assets/molmo2/processor.json" runtime/common/molmo2-processor.json copy
  stage_summarizer
  stage_ocr
fi
if [ "$RUNTIME_ONLY" != 1 ]; then
  for name in experts.bin resident.safetensors manifest.json config.json generation_config.json; do
    [ -f "$SNAPSHOT/$name" ] || fail "missing model file: $SNAPSHOT/$name"
    place "$SNAPSHOT/$name" "$name" link
  done
  [ -f "$TOKENIZER" ] || fail "missing tokenizer: $TOKENIZER"
  place "$TOKENIZER" tokenizer_qwen36.json link
fi

for name in \
  qwen36b.c expert_cache.c expert_cache.h vision.c vision.h stb_image.h kernels.h st.h json.h tok.h tok_unicode.h compat.h \
  repetition_guard.h thinking_budget.h samosa_http.h samosa_kokoro.h samosa_extract.c samosa_html.c samosa_html.h \
  samosa_docx.c samosa_docx.h samosa_ocr.c visionpsy/samosa_visionpsy.cpp visionpsy/visionpsy_model.cpp visionpsy/visionpsy_model.h \
  samosa_gateway.c samosa_memory.h samosa_prompt_budget.h samosa_audio_decode.mm samosa_multimodal.h samosa_evidence.c \
  samosa_evidence.h samosa_summarizer.cpp samosa_fs.c read_cache.h durable_job.h; do
  source="$ROOT/src/$name"
  if [ "$name" = samosa_summarizer.cpp ]; then source="$ROOT/components/samosa-inference/engines/t5_summary.cpp"; fi
  case "$name" in visionpsy/*) source="$ROOT/components/samosa-inference/engines/$name" ;; esac
  case "$name" in qwen36b.c|expert_cache.c|expert_cache.h|vision.c|vision.h|kernels.h|st.h|compat.h|repetition_guard.h|thinking_budget.h)
    source="$ROOT/components/samosa-inference/engines/$name" ;;
  esac
  [ -f "$source" ] || fail "missing source file: $source"
  place "$source" "engine/$name" copy
done
for name in src/specialist_supervisor.c src/specialist_protocol.c engines/json.h src/summary_process.c src/controlled_pipe.h src/json.h src/llama_launch.c src/engine_process.c src/engine_process.h src/backend_process.c src/whisper_process.c src/vision_budget.c src/turn_control.c \
  include/samosa_inference/specialist_protocol.h \
  include/samosa_inference/specialist_supervisor.h include/samosa_inference/summary_process.h \
  include/samosa_inference/llama_launch.h include/samosa_inference/backend_process.h include/samosa_inference/whisper_process.h include/samosa_inference/vision_budget.h include/samosa_inference/turn_control.h LICENSE; do
  source="$ROOT/components/samosa-inference/$name"
  [ -f "$source" ] || fail "missing Inference source: $source"
  place "$source" "engine/inference/$name" copy
done
for name in LICENSE miniz.c miniz.h miniz_common.h miniz_export.h miniz_tdef.h miniz_tinfl.c miniz_tinfl.h miniz_zip.c miniz_zip.h; do
  [ -f "$ROOT/vendor/miniz/$name" ] || fail "missing miniz source: $ROOT/vendor/miniz/$name"
  place "$ROOT/vendor/miniz/$name" "engine/miniz/$name" copy
done
for name in LICENSE NOTICE VERSION include/chutni.h src/chutni.c src/scan.c src/cj.c src/cj.h src/mcp.c \
  third_party/blake3/LICENSE_A2 third_party/blake3/LICENSE_CC0 third_party/blake3/blake3.c third_party/blake3/blake3.h \
  third_party/blake3/blake3_dispatch.c third_party/blake3/blake3_impl.h third_party/blake3/blake3_portable.c \
  third_party/sqlite/sqlite3.c third_party/sqlite/sqlite3.h; do
  [ -f "$ROOT/vendor/chutni/$name" ] || fail "missing Chutni source: $ROOT/vendor/chutni/$name; run git submodule update --init"
  place "$ROOT/vendor/chutni/$name" "engine/chutni/$name" copy
done
if [ -n "$PDFIUM_DIR" ]; then
  for name in pdfium-mac-arm64.tgz pdfium-linux-x64.tgz pdfium-linux-arm64.tgz; do
    [ -f "$PDFIUM_DIR/$name" ] || fail "missing PDFium archive: $PDFIUM_DIR/$name"
    place "$PDFIUM_DIR/$name" "pdfium/$name" copy
  done
fi

stage_dist() {
  [ -f "$ROOT/$1" ] || fail "missing dist file: $ROOT/$1"
  place "$ROOT/$1" "$2" copy
  if [ "$2" = samosa ]; then
    grep -F 'SAMOSA_VERSION=""' "$OUT/$2" >/dev/null || fail 'launcher is missing the SAMOSA_VERSION="" marker'
    replace_literal 'SAMOSA_VERSION=""' "SAMOSA_VERSION=\"$VERSION\"" "$OUT/$2"
  fi
  case "$2" in
    install.sh|samosa|README.md)
      if [ "$REPO_ID" != REPO_ID_PLACEHOLDER ]; then
        replace_literal REPO_ID_PLACEHOLDER "$REPO_ID" "$OUT/$2"
      fi ;;
  esac
}
stage_dist dist/install.sh install.sh
stage_dist dist/samosa samosa
stage_dist tools/samosa_voice_runtime.sh engine/samosa_voice_runtime.sh
stage_dist tools/stage_tesseract_runtime.sh engine/stage_tesseract_runtime.sh
stage_dist tools/samosa_kokoro_runtime.sh engine/samosa_kokoro_runtime.sh
stage_dist dist/MODEL_CARD.md README.md
stage_dist assets/app.html app.html
stage_dist assets/samosa-chat.png samosa-chat.png
stage_dist assets/models.json models.json

BROWSER_ROOT="$ROOT/assets/voice/browser"
[ -d "$BROWSER_ROOT" ] || fail "missing browser voice assets: $BROWSER_ROOT"
find "$BROWSER_ROOT" \( -type f -o -type l \) -print | LC_ALL=C sort >"$WORK/browser-files"
while IFS= read -r source; do
  [ -f "$source" ] || continue
  place "$source" "voice/browser/${source#"$BROWSER_ROOT/"}" copy
done <"$WORK/browser-files"

LC_ALL=C sort "$STAGED" >"$WORK/sorted"
: >"$OUT/checksums.txt"
: >"$OUT/release-manifest.tsv"
total=0; count=0
while IFS= read -r relative; do
  digest=$(sha256_file "$OUT/$relative")
  bytes=$(file_size "$OUT/$relative")
  printf '%s  %s\n' "$digest" "$relative" >>"$OUT/checksums.txt"
  printf '%s\t%s\t%s\n' "$digest" "$bytes" "$relative" >>"$OUT/release-manifest.tsv"
  printf '%.16s  %s\n' "$digest" "$relative"
  total=$((total + bytes)); count=$((count + 1))
done <"$WORK/sorted"
awk -v count="$count" -v bytes="$total" -v out="$OUT" 'BEGIN {printf "\nstaged %d files, %.2f GB -> %s\n", count, bytes/1e9, out}'
