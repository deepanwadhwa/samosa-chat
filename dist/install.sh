#!/bin/sh
# Jugnu installer — one command, no admin rights, no app store.
#
#   curl -fsSL https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main/install.sh | sh
#
# What it does, in order:
#   1. Checks your Mac: Apple Silicon, 16 GB RAM, ~25 GB free disk, a C compiler.
#   2. Downloads the model (~18 GB) into ~/.jugnu — resumable, checksum-verified.
#   3. Compiles the inference engine locally (one clang command, ~15 seconds).
#   4. Installs the `jugnu` chat command and runs a hello-world test.
#
# Re-running is safe: finished downloads are verified and skipped.
# Uninstall: rm -rf ~/.jugnu (and the PATH line it added to your shell rc).

set -eu

BASE_URL="${JUGNU_BASE_URL:-https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main}"
HOME_DIR="${JUGNU_HOME:-$HOME/.jugnu}"
MODEL_DIR="$HOME_DIR/model"
ENGINE_DIR="$HOME_DIR/engine"
BIN_DIR="$HOME_DIR/bin"

say()  { printf '\033[1;36m[jugnu]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[jugnu] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------- 1. machine checks ----------
[ "$(uname -s)" = "Darwin" ] || fail "this installer currently supports macOS only"
[ "$(uname -m)" = "arm64" ]  || fail "an Apple Silicon Mac (M1 or newer) is required"

RAM_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
[ "$RAM_GB" -ge 16 ] || fail "16 GB of RAM required (this Mac has ${RAM_GB} GB)"

FREE_GB=$(df -g "$HOME" | awk 'NR==2 {print $4}')
NEED_GB=25
if [ -f "$MODEL_DIR/experts.bin" ]; then NEED_GB=5; fi
[ "$FREE_GB" -ge "$NEED_GB" ] || fail "need ~${NEED_GB} GB free (found ${FREE_GB} GB). Free some space and re-run."

if ! command -v clang >/dev/null 2>&1 || ! xcode-select -p >/dev/null 2>&1; then
  say "The Apple command-line tools are needed (one-time, free)."
  say "A dialog will pop up - click Install, then RE-RUN this installer."
  xcode-select --install 2>/dev/null || true
  exit 1
fi

say "Mac check passed: Apple Silicon, ${RAM_GB} GB RAM, ${FREE_GB} GB free."

# ---------- 2. download ----------
mkdir -p "$MODEL_DIR" "$ENGINE_DIR" "$BIN_DIR"

fetch() { # fetch <relative-path> <destination>
  say "downloading $1 ..."
  curl -fL --retry 5 --retry-delay 3 -C - --progress-bar "$BASE_URL/$1" -o "$2" \
    || fail "download failed for $1 - re-run the installer to resume"
}

fetch checksums.txt "$HOME_DIR/checksums.txt"

verified() { # verified <relative-path> <local-file>  -> 0 if checksum matches
  want=$(awk -v f="$1" '$2==f {print $1}' "$HOME_DIR/checksums.txt")
  [ -n "$want" ] || return 1
  [ -f "$2" ] || return 1
  have=$(shasum -a 256 "$2" | awk '{print $1}')
  [ "$want" = "$have" ]
}

get() { # get <relative-path> <destination>
  if verified "$1" "$2"; then say "$1 already present and verified - skipping"; return; fi
  fetch "$1" "$2"
  verified "$1" "$2" || fail "checksum mismatch for $1 - delete $2 and re-run"
}

say "Fetching the model (~18 GB total). Safe to interrupt; re-run resumes."
get experts.bin              "$MODEL_DIR/experts.bin"
get resident.safetensors     "$MODEL_DIR/resident.safetensors"
get manifest.json            "$MODEL_DIR/manifest.json"
get config.json              "$MODEL_DIR/config.json"
get generation_config.json   "$MODEL_DIR/generation_config.json"
get tokenizer_qwen36.json    "$HOME_DIR/tokenizer_qwen36.json"

for f in qwen36b.c expert_cache.c expert_cache.h kernels.h st.h json.h tok.h tok_unicode.h compat.h; do
  get "engine/$f" "$ENGINE_DIR/$f"
done
get jugnu "$BIN_DIR/jugnu"
chmod +x "$BIN_DIR/jugnu"

# ---------- 3. build ----------
say "Compiling the engine..."
OMP_FLAGS=""
for prefix in /opt/homebrew/opt/libomp /usr/local/opt/libomp; do
  if [ -f "$prefix/lib/libomp.dylib" ]; then
    OMP_FLAGS="-Xclang -fopenmp -I$prefix/include -L$prefix/lib -lomp"
    break
  fi
done
# shellcheck disable=SC2086
clang -O3 $OMP_FLAGS \
  -Wno-unused-function \
  "$ENGINE_DIR/qwen36b.c" "$ENGINE_DIR/expert_cache.c" \
  -o "$BIN_DIR/qwen36b" -lm \
  || fail "compilation failed - please report this"
if [ -z "$OMP_FLAGS" ]; then
  say "note: built single-threaded (2-4x slower). For full speed:"
  say "      brew install libomp   then re-run this installer."
fi

# ---------- 4. PATH + smoke test ----------
case ":$PATH:" in *":$BIN_DIR:"*) ;; *)
  # The user's LOGIN shell decides the rc file, not the shell running this
  # script (curl | sh runs under bash even for zsh users).
  case "${SHELL:-}" in
    */zsh)  RC="$HOME/.zshrc" ;;
    */bash) RC="$HOME/.bashrc" ;;
    *)      RC="$HOME/.profile" ;;
  esac
  if ! grep -qs "\.jugnu/bin" "$RC" 2>/dev/null; then
    printf '\nexport PATH="$HOME/.jugnu/bin:$PATH"\n' >> "$RC"
    say "added ~/.jugnu/bin to PATH in $RC (takes effect in new terminals)"
  fi
esac

say "Running a quick hello-world (first run reads the model from disk)..."
"$BIN_DIR/jugnu" "Say hello in exactly five words." || fail "smoke test failed"

say ""
say "Done! Try:   jugnu \"explain git rebase simply\""
say "             jugnu --continue \"give me an example\""
say "             jugnu --think \"a tricky logic puzzle\""
say "Open a NEW terminal (or: export PATH=\"\$HOME/.jugnu/bin:\$PATH\")"
