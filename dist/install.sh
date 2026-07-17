#!/bin/sh
# Samosa Chat installer — versioned, checksum-verified, atomic activation.

set -eu

BASE_URL="${SAMOSA_BASE_URL:-https://huggingface.co/REPO_ID_PLACEHOLDER/resolve/main}"
HOME_DIR="${SAMOSA_HOME:-$HOME/.samosa}"
RELEASES_DIR="$HOME_DIR/releases"
LAUNCHER_DIR="$HOME_DIR/bin"
MIN_FREE_AFTER_GB="${SAMOSA_MIN_FREE_AFTER_GB:-2}"

say()  { printf '\033[1;36m[samosa]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[samosa] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    fail "neither sha256sum nor shasum is available"
  fi
}

if [ "${SAMOSA_INSTALL_TEST:-0}" != 1 ]; then
  OS=$(uname -s)
  ARCH=$(uname -m)
  [ "$OS" = "Darwin" ] || [ "$OS" = "Linux" ] || fail "this installer supports macOS and Linux only"
  if [ "$OS" = "Darwin" ]; then
    [ "$ARCH" = "arm64" ] || fail "an Apple Silicon Mac (M1 or newer) is required"
    RAM_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
    [ "$RAM_GB" -ge 16 ] || fail "16 GB of RAM required (this Mac has ${RAM_GB} GB)"
    if ! command -v clang >/dev/null 2>&1 || ! xcode-select -p >/dev/null 2>&1; then
      say "The Apple command-line tools are needed (one-time, free)."
      say "A dialog will pop up - click Install, then RE-RUN this installer."
      xcode-select --install 2>/dev/null || true
      exit 1
    fi
  else
    [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "aarch64" ] || fail "only x86_64 and aarch64 architectures are supported on Linux"
    if [ "$ARCH" = "x86_64" ]; then
      if ! grep -qw avx2 /proc/cpuinfo; then
        say "WARNING: This CPU does not support the AVX2 instruction set."
        say "Without AVX2, Samosa Chat will run on the scalar math path,"
        say "which is approximately 7.6x slower than vectorized execution."
        if [ "${SAMOSA_ALLOW_SLOW_CPU:-0}" = 1 ]; then
          say "Proceeding anyway because SAMOSA_ALLOW_SLOW_CPU=1 is set."
        else
          fail "Installation aborted. Set SAMOSA_ALLOW_SLOW_CPU=1 and re-run to proceed."
        fi
      fi
    fi
    RAM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
    RAM_GB=$(( RAM_KB / 1048576 ))
    [ "$RAM_GB" -ge 16 ] || fail "16 GB of RAM required (this system has ${RAM_GB} GB)"
    if ! command -v clang >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
      fail "a C compiler (clang or gcc) is required. Install build-essential or clang."
    fi
  fi
fi

mkdir -p "$HOME_DIR" "$RELEASES_DIR" "$LAUNCHER_DIR"

if [ "${SAMOSA_INSTALL_TEST:-0}" != 1 ] && [ "$(uname -s)" = "Linux" ]; then
  dev=$(df -P "$HOME_DIR" 2>/dev/null | awk 'NR==2 {print $1}')
  if [ -b "$dev" ] || [ -c "$dev" ] || echo "$dev" | grep -q '^/dev/'; then
    real_dev=$(readlink -f "$dev")
    dev_name=$(basename "$real_dev")
    base_dev=$(echo "$dev_name" | sed 's/[0-9]*$//')
    if echo "$dev_name" | grep -q '^nvme'; then
      base_dev=$(echo "$dev_name" | sed -E 's/p[0-9]+$//')
    fi
    if [ -f "/sys/block/$base_dev/queue/rotational" ]; then
      rotational=$(cat "/sys/block/$base_dev/queue/rotational")
      if [ "$rotational" = 1 ]; then
        fail "Samosa Chat cannot run on an HDD (rotational drive) because the random 16 KB expert streaming reads will take minutes per token. An SSD (preferably NVMe) is required."
      fi
    fi
  fi
fi

MANIFEST_NEXT="$HOME_DIR/.release-manifest.next.tsv"

say "Fetching release manifest..."
curl -fL --retry 5 --retry-delay 3 --progress-bar \
  "$BASE_URL/release-manifest.tsv" -o "$MANIFEST_NEXT" \
  || fail "release manifest download failed"

# Format: SHA-256<TAB>byte-size<TAB>relative-path. Reject unsafe paths before
# using any field as a destination.
awk -F '\t' '
  NF != 3 || length($1) != 64 || $1 !~ /^[0-9a-f]+$/ || $2 !~ /^[0-9]+$/ ||
  $3 == "" || $3 ~ /^\// || $3 ~ /(^|\/)\.\.(\/|$)/ { exit 1 }
' "$MANIFEST_NEXT" || fail "release manifest is malformed or unsafe"

RELEASE_ID=$(sha256_file "$MANIFEST_NEXT" | awk '{print substr($1,1,16)}')
STAGE="$RELEASES_DIR/.${RELEASE_ID}.partial"
FINAL="$RELEASES_DIR/$RELEASE_ID"
mkdir -p "$STAGE/model" "$STAGE/engine" "$STAGE/bin"

manifest_field() { # manifest_field <path> <column>
  awk -F '\t' -v p="$1" -v c="$2" '$3==p {print $c; found=1} END {if(!found) exit 1}' "$MANIFEST_NEXT"
}

destination() { # destination <remote-path>
  case "$1" in
    experts.bin|resident.safetensors|manifest.json|config.json|generation_config.json)
      printf '%s/model/%s\n' "$STAGE" "$1" ;;
    tokenizer_qwen36.json) printf '%s/tokenizer_qwen36.json\n' "$STAGE" ;;
    app.html|samosa-chat.png) printf '%s/%s\n' "$STAGE" "$1" ;;
    engine/*) printf '%s/%s\n' "$STAGE" "$1" ;;
    pdfium/*.tgz) printf '%s/%s\n' "$STAGE" "$1" ;;
    samosa) printf '%s/bin/samosa\n' "$STAGE" ;;
    *) return 1 ;;
  esac
}

INSTALL_FILES="experts.bin resident.safetensors manifest.json config.json generation_config.json tokenizer_qwen36.json app.html samosa-chat.png engine/qwen36b.c engine/expert_cache.c engine/expert_cache.h engine/vision.c engine/vision.h engine/stb_image.h engine/kernels.h engine/st.h engine/json.h engine/tok.h engine/tok_unicode.h engine/compat.h engine/repetition_guard.h engine/thinking_budget.h engine/samosa_http.h samosa"

# Document extraction is an optional release capability, not a host-package
# dependency. A PDFium archive is fetched only when the verified release
# manifest includes both the platform artifact and its sidecar source. Keeping
# old/source-only release fixtures valid makes capability absence explicit rather
# than silently falling back to a system PDF tool.
PDFIUM_ARCHIVE=""
PDFIUM_LIBRARY=""
case "$(uname -s):$(uname -m)" in
  Darwin:arm64) PDFIUM_ARCHIVE="pdfium/pdfium-mac-arm64.tgz"; PDFIUM_LIBRARY="libpdfium.dylib" ;;
  Linux:x86_64) PDFIUM_ARCHIVE="pdfium/pdfium-linux-x64.tgz"; PDFIUM_LIBRARY="libpdfium.so" ;;
  Linux:aarch64) PDFIUM_ARCHIVE="pdfium/pdfium-linux-arm64.tgz"; PDFIUM_LIBRARY="libpdfium.so" ;;
esac
DOCUMENTS_ENABLED=0
if [ -n "$PDFIUM_ARCHIVE" ] && manifest_field "$PDFIUM_ARCHIVE" 1 >/dev/null 2>&1 && \
   manifest_field "engine/samosa_extract.c" 1 >/dev/null 2>&1; then
  INSTALL_FILES="$INSTALL_FILES engine/samosa_extract.c $PDFIUM_ARCHIVE"
  DOCUMENTS_ENABLED=1
fi

required_remaining=0
for relative in $INSTALL_FILES; do
  size=$(manifest_field "$relative" 2) || fail "release manifest missing $relative"
  target=$(destination "$relative") || fail "unsupported release path $relative"
  present=0
  [ -f "$target" ] && present=$(wc -c <"$target" | tr -d ' ')
  [ "$present" -le "$size" ] || { rm -f "$target"; present=0; }
  required_remaining=$((required_remaining + size - present))
done
free_bytes=$(df -Pk "$HOME_DIR" | awk 'NR==2 {printf "%.0f\n", $4 * 1024}')
reserve_bytes=$((MIN_FREE_AFTER_GB * 1000000000))
[ "$free_bytes" -ge $((required_remaining + reserve_bytes)) ] ||
  fail "atomic install needs $(( (required_remaining + reserve_bytes + 999999999) / 1000000000 )) GB free; found $((free_bytes / 1000000000)) GB. The live release was not changed."

fetch() { # fetch <relative-path> <destination>
  relative=$1; target=$2
  mkdir -p "$(dirname "$target")"
  say "downloading $relative ..."
  curl -fL --retry 5 --retry-delay 3 -C - --progress-bar \
    "$BASE_URL/$relative" -o "$target" \
    || fail "download failed for $relative - re-run to resume the inactive staging release"
}

verified() { # verified <relative-path> <local-file>
  want=$(manifest_field "$1" 1) || return 1
  size=$(manifest_field "$1" 2) || return 1
  [ -f "$2" ] || return 1
  [ "$(wc -c <"$2" | tr -d ' ')" = "$size" ] || return 1
  have=$(sha256_file "$2")
  [ "$want" = "$have" ]
}

for relative in $INSTALL_FILES; do
  target=$(destination "$relative")
  if verified "$relative" "$target"; then
    say "$relative already staged and verified - skipping"
  else
    fetch "$relative" "$target"
    verified "$relative" "$target" ||
      fail "checksum mismatch for $relative in inactive staging; live release was not changed"
  fi
done
cp "$MANIFEST_NEXT" "$STAGE/release-manifest.tsv"
chmod +x "$STAGE/bin/samosa"

say "Compiling the staged engine..."
COMPILER=""
if command -v clang >/dev/null 2>&1; then
  COMPILER="clang"
elif command -v gcc >/dev/null 2>&1; then
  COMPILER="gcc"
else
  COMPILER="cc"
fi

OMP_FLAGS=""
if [ "$(uname -s)" = "Darwin" ]; then
  for prefix in /opt/homebrew/opt/libomp /usr/local/opt/libomp; do
    if [ -f "$prefix/lib/libomp.dylib" ]; then
      OMP_FLAGS="-Xclang -fopenmp -I$prefix/include -L$prefix/lib -lomp"
      break
    fi
  done
else
  # Linux OpenMP support check
  if echo "int main() {}" | $COMPILER -fopenmp -x c - -o /dev/null >/dev/null 2>&1; then
    OMP_FLAGS="-fopenmp"
  fi
fi

$COMPILER -O3 -pthread $OMP_FLAGS -Wno-unused-function \
  "$STAGE/engine/qwen36b.c" "$STAGE/engine/expert_cache.c" "$STAGE/engine/vision.c" \
  -o "$STAGE/bin/qwen36b" -lm ||
  fail "staged engine compilation failed; live release was not changed"

if [ "$DOCUMENTS_ENABLED" = 1 ]; then
  command -v tar >/dev/null 2>&1 || fail "PDF support needs tar to unpack its verified release artifact"
  PDFIUM_ROOT="$STAGE/pdfium/unpacked"
  mkdir -p "$PDFIUM_ROOT" "$STAGE/lib"
  tar -xzf "$STAGE/$PDFIUM_ARCHIVE" -C "$PDFIUM_ROOT" ||
    fail "could not unpack the verified PDFium artifact"
  [ -f "$PDFIUM_ROOT/include/fpdfview.h" ] && [ -f "$PDFIUM_ROOT/lib/$PDFIUM_LIBRARY" ] ||
    fail "verified PDFium artifact has an unexpected layout"
  cp "$PDFIUM_ROOT/lib/$PDFIUM_LIBRARY" "$STAGE/lib/$PDFIUM_LIBRARY"
  if [ "$(uname -s)" = "Darwin" ]; then
    EXTRACT_RPATH='@loader_path/../lib'
  else
    EXTRACT_RPATH='$ORIGIN/../lib'
  fi
  $COMPILER -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -I"$PDFIUM_ROOT/include" \
    "$STAGE/engine/samosa_extract.c" "$PDFIUM_ROOT/lib/$PDFIUM_LIBRARY" \
    -Wl,-rpath,"$EXTRACT_RPATH" -o "$STAGE/bin/samosa-extract" ||
    fail "staged document extractor compilation failed; live release was not changed"
  if [ "$(uname -s)" = "Darwin" ]; then
    install_name_tool -change ./libpdfium.dylib @rpath/libpdfium.dylib "$STAGE/bin/samosa-extract" ||
      fail "could not set the staged PDFium runtime path"
  fi
  chmod +x "$STAGE/bin/samosa-extract"
fi

if [ "${SAMOSA_INSTALL_TEST:-0}" != 1 ]; then
  say "Smoke-testing the inactive local app..."
  SMOKE_PORT=$((18000 + $$ % 10000))
  SMOKE_LOG="$STAGE/app-smoke.log"
  smoke_pid=""
  stop_smoke() {
    [ -n "$smoke_pid" ] || return 0
    curl -fsS --max-time 5 -X POST \
      "http://127.0.0.1:$SMOKE_PORT/v1/shutdown" >/dev/null 2>&1 || true
    kill -TERM "$smoke_pid" >/dev/null 2>&1 || true
    wait "$smoke_pid" >/dev/null 2>&1 || true
  }
  trap 'stop_smoke' EXIT HUP INT TERM
  SAMOSA_RELEASE_DIR="$STAGE" SAMOSA_PORT="$SMOKE_PORT" \
    "$STAGE/bin/samosa" serve >"$SMOKE_LOG" 2>&1 &
  smoke_pid=$!
  ready=0
  i=0
  while [ "$i" -lt 240 ]; do
    if curl -fsS --max-time 2 "http://127.0.0.1:$SMOKE_PORT/healthz" >/dev/null 2>&1; then
      ready=1
      break
    fi
    kill -0 "$smoke_pid" >/dev/null 2>&1 || break
    sleep 0.5
    i=$((i + 1))
  done
  [ "$ready" = 1 ] || {
    sed -n '1,120p' "$SMOKE_LOG" >&2 || true
    fail "staged app server did not become healthy; live release was not changed"
  }
  curl -fsS --max-time 5 "http://127.0.0.1:$SMOKE_PORT/" |
    grep -q 'Your model.' ||
    fail "staged app UI smoke failed; live release was not changed"
  curl -fsS --max-time 120 "http://127.0.0.1:$SMOKE_PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    --data-binary '{"messages":[{"role":"user","content":"Reply with hello."}],"thinking":"off","max_tokens":16,"seed":11}' |
    grep -q '"choices"' ||
    fail "staged app generation smoke failed; live release was not changed"
  stop_smoke
  smoke_pid=""
  trap - EXIT HUP INT TERM
  rm -f "$SMOKE_LOG"
fi

# Publish the immutable release directory, then atomically switch one symlink.
if [ ! -d "$FINAL" ]; then mv "$STAGE" "$FINAL"; else rm -rf "$STAGE"; fi
rm -f "$HOME_DIR/.current.next"
ln -s "releases/$RELEASE_ID" "$HOME_DIR/.current.next"
if [ "$(uname -s)" = "Darwin" ]; then
  mv -fh "$HOME_DIR/.current.next" "$HOME_DIR/current"
else
  mv -T "$HOME_DIR/.current.next" "$HOME_DIR/current"
fi

LAUNCHER_NEXT="$LAUNCHER_DIR/.samosa.next"
cat >"$LAUNCHER_NEXT" <<'EOF'
#!/bin/sh
set -eu
HOME_DIR="${SAMOSA_HOME:-$HOME/.samosa}"
exec "$HOME_DIR/current/bin/samosa" "$@"
EOF
chmod +x "$LAUNCHER_NEXT"
mv -f "$LAUNCHER_NEXT" "$LAUNCHER_DIR/samosa"
mv -f "$MANIFEST_NEXT" "$HOME_DIR/release-manifest.tsv"

NEEDS_NEW_SHELL=0
# Guarded separately from SAMOSA_INSTALL_TEST on purpose. That flag also skips
# the platform preflight and the app smoke test, both of which need a real
# model — so anything hiding behind it could never be covered by a test. This
# block writes to $HOME, so a test overrides HOME rather than skipping it.
if [ "${SAMOSA_SKIP_PATH_SETUP:-0}" != 1 ]; then
  case ":$PATH:" in *":$LAUNCHER_DIR:"*) ;; *)
    # The launcher is not on PATH in this shell. Adding it to the rc file only
    # affects shells started afterwards, so the caller must be told.
    NEEDS_NEW_SHELL=1
    case "${SHELL:-}" in
      */zsh) RC="$HOME/.zshrc" ;;
      */bash) RC="$HOME/.bashrc" ;;
      *) RC="$HOME/.profile" ;;
    esac
    if grep -qs "\.samosa/bin" "$RC" 2>/dev/null; then
      say "~/.samosa/bin is already configured in $RC"
    else
      printf '\nexport PATH="$HOME/.samosa/bin:$PATH"\n' >>"$RC"
      say "added ~/.samosa/bin to PATH in $RC"
    fi
  esac
fi

say "Activated verified release $RELEASE_ID."
say "Previous releases and any legacy ~/.samosa/model directory were left untouched for rollback."
say "Samosa is installed at $LAUNCHER_DIR/samosa"
if [ "$NEEDS_NEW_SHELL" = 1 ]; then
  # Do not tell people to run `samosa` in this shell: the PATH change above
  # only applies to shells started after it, so it would fail here.
  say ""
  say "One more step: this terminal does not know about samosa yet."
  say "Open a new terminal, or run:"
  say "    export PATH=\"\$HOME/.samosa/bin:\$PATH\""
  say ""
  say "Then try:  samosa \"explain how DNS works\""
else
  say "Try:  samosa \"explain how DNS works\""
fi
