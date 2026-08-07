#!/bin/sh
set -eu

# Shared "existing legacy model install" fixture (docs/TASKS_UI_CHUTNI.md T0.1,
# reused by T2.1's legacy-detection tests). Places tiny placeholder files at
# the exact paths backend_available() / load_config() already look for
# (src/samosa_gateway.c ENV_PATH defaults), so tests can prove a pre-existing
# Bonsai/Ornith install is discovered and registered without ever fetching or
# storing a real multi-gigabyte model.
#
# Usage: gen_legacy_model_fixture.sh HOME_DIR [ornith|bonsai|maple|all]

home="${1:?usage: gen_legacy_model_fixture.sh HOME_DIR [ornith|bonsai|maple|all]}"
which="${2:-all}"

gen_ornith() {
  mkdir -p "$home/models/ornith-9b"
  printf 'ornith-9b fixture weights (not a real model)\n' \
    >"$home/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
}

gen_bonsai() {
  mkdir -p "$home/models/bonsai-27b-1bit"
  printf 'bonsai-27b fixture weights (not a real model)\n' \
    >"$home/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
  printf 'bonsai-27b fixture mmproj (not a real model)\n' \
    >"$home/bonsai-mmproj.gguf"
}

gen_maple() {
  mkdir -p "$home/models/maple"
  printf 'maple fixture weights (not a real model)\n' \
    >"$home/models/maple/model.safetensors"
  printf '{}' \
    >"$home/models/maple/tokenizer.json"
}

case "$which" in
  ornith) gen_ornith ;;
  bonsai) gen_bonsai ;;
  maple) gen_maple ;;
  all|both) gen_ornith; gen_bonsai; gen_maple ;;
  *) echo "unknown model selector: $which (expected ornith|bonsai|maple|all)" >&2; exit 64 ;;
esac

echo "gen_legacy_model_fixture.sh: placed $which fixture(s) under $home"
