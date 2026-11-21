#!/bin/sh
# The three original release-selection/version scenarios, using no Python.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-package-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
ARCHIVES='pdfium-mac-arm64.tgz pdfium-linux-x64.tgz pdfium-linux-arm64.tgz'
fail() { printf 'package selection: FAIL: %s\n' "$*" >&2; exit 1; }
sha256_file() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  else sha256sum "$1" | awk '{print $1}'; fi
}
prepare() {
  mkdir -p "$1/snapshot" "$1/pdfium"
  for name in experts.bin resident.safetensors manifest.json config.json generation_config.json; do
    printf 'fixture\n' >"$1/snapshot/$name"
  done
  printf '{}\n' >"$1/tokenizer.json"
}
run_package() {
  SAMOSA_PACKAGE_TEST=1 sh "$ROOT/tools/package_hf.sh" \
    --out "$1/out" --snapshot "$1/snapshot" --tokenizer "$1/tokenizer.json" \
    --pdfium-dir "$1/pdfium" \
    --maple-runtime "$ROOT/tests/fixtures/maple-runtime/samosa-maple" \
    --maple-metallib "$ROOT/tests/fixtures/maple-runtime/mlx.metallib" \
    --visionpsy-runtime "$ROOT/tests/fixtures/maple-runtime/samosa-maple" \
    --molmo2-runtime "$ROOT/tests/fixtures/maple-runtime/samosa-maple" \
    --molmo2-pack "$ROOT/tests/fixtures/maple-runtime/samosa-maple" \
    --audio-decode-runtime "$ROOT/tests/fixtures/maple-runtime/samosa-maple" \
    --summarizer-model "$ROOT/tests/fixtures/native-summarizer/model.gguf" \
    --summarizer-runtime-dir "$ROOT/tests/fixtures/native-summarizer" \
    --ocr-runtime-dir "$ROOT/tests/fixtures/native-ocr"
}

# Original case 1: a partial PDFium platform set must fail closed.
prepare "$TMP/incomplete"
printf 'one archive' >"$TMP/incomplete/pdfium/pdfium-mac-arm64.tgz"
if run_package "$TMP/incomplete" >"$TMP/incomplete.log" 2>&1; then
  fail 'accepted an incomplete PDFium archive set'
fi
grep -q 'missing PDFium archive' "$TMP/incomplete.log" || {
  cat "$TMP/incomplete.log" >&2; fail 'wrong incomplete-archive failure';
}

# Original case 2: exact bytes/hashes for every archive and extracted owner source.
prepare "$TMP/complete"
for name in $ARCHIVES; do printf 'fixture %s\n' "$name" >"$TMP/complete/pdfium/$name"; done
run_package "$TMP/complete" >"$TMP/complete.log" 2>&1 || { cat "$TMP/complete.log" >&2; fail 'complete package failed'; }
manifest="$TMP/complete/out/release-manifest.tsv"
for name in $ARCHIVES; do
  expected=$(sha256_file "$TMP/complete/pdfium/$name")
  actual=$(awk -F '\t' -v name="pdfium/$name" '$3==name {print $1}' "$manifest")
  [ "$actual" = "$expected" ] || fail "wrong archive hash: $name"
done
for name in src/specialist_supervisor.c src/specialist_protocol.c engines/json.h src/summary_process.c src/controlled_pipe.h src/json.h src/llama_launch.c src/engine_process.c src/engine_process.h src/backend_process.c src/whisper_process.c src/vision_budget.c \
  include/samosa_inference/specialist_protocol.h \
  include/samosa_inference/specialist_supervisor.h include/samosa_inference/summary_process.h \
  include/samosa_inference/llama_launch.h include/samosa_inference/backend_process.h include/samosa_inference/whisper_process.h include/samosa_inference/vision_budget.h LICENSE; do
  expected=$(sha256_file "$ROOT/components/samosa-inference/$name")
  actual=$(awk -F '\t' -v name="engine/inference/$name" '$3==name {print $1}' "$manifest")
  [ "$actual" = "$expected" ] || fail "wrong owner-source hash: $name"
  cmp "$ROOT/components/samosa-inference/$name" "$TMP/complete/out/engine/inference/$name" || fail "changed owner source: $name"
done
if awk -F '\t' '$3=="engine/samosa_multimodal.c" {found=1} END {exit !found}' "$manifest"; then
  fail 'retired monolith supervisor was staged'
fi
cmp "$ROOT/components/samosa-inference/engines/t5_summary.cpp" "$TMP/complete/out/engine/samosa_summarizer.cpp" || fail 'wrong T5 source owner'
for name in samosa_visionpsy.cpp visionpsy_model.cpp visionpsy_model.h; do
  cmp "$ROOT/components/samosa-inference/engines/visionpsy/$name" "$TMP/complete/out/engine/visionpsy/$name" || fail "wrong VisionPsy source owner: $name"
done
for name in qwen36b.c expert_cache.c expert_cache.h vision.c vision.h kernels.h st.h compat.h repetition_guard.h thinking_budget.h; do
  cmp "$ROOT/components/samosa-inference/engines/$name" "$TMP/complete/out/engine/$name" || fail "wrong Qwen source owner: $name"
done
while IFS="$(printf '\t')" read -r digest bytes relative; do
  [ "$(wc -c <"$TMP/complete/out/$relative" | tr -d '[:space:]')" = "$bytes" ] || fail "wrong manifest size: $relative"
  [ "$(sha256_file "$TMP/complete/out/$relative")" = "$digest" ] || fail "wrong manifest digest: $relative"
done <"$manifest"

# Original case 3: bake the actual project version into the launcher.
prepare "$TMP/version"
for name in $ARCHIVES; do printf 'fixture %s\n' "$name" >"$TMP/version/pdfium/$name"; done
run_package "$TMP/version" >"$TMP/version.log" 2>&1 || { cat "$TMP/version.log" >&2; fail 'version package failed'; }
version=$(tr -d '\r\n' <"$ROOT/VERSION")
grep -F "SAMOSA_VERSION=\"$version\"" "$TMP/version/out/samosa" >/dev/null || fail 'version was not baked in'
if grep -F 'SAMOSA_VERSION=""' "$TMP/version/out/samosa" >/dev/null; then fail 'empty version marker retained'; fi
printf '%s\n' 'package selection: PASS (complete PDFium set, exact owner source/hashes/sizes, baked version)'
