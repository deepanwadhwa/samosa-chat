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
    [ "$RAM_GB" -ge 16 ] || [ "${SAMOSA_IGNORE_RAM_CHECK:-0}" = 1 ] || fail "16 GB of RAM required (this Mac has ${RAM_GB} GB)"
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
    [ "$RAM_GB" -ge 16 ] || [ "${SAMOSA_IGNORE_RAM_CHECK:-0}" = 1 ] || fail "16 GB of RAM required (this system has ${RAM_GB} GB)"
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
# $STAGE/model is deliberately not pre-created here: a runtime-only release
# (docs/TASKS_UI_CHUTNI.md T1.0) stages nothing there at all, and fetch()
# below creates it on demand only when an actual model artifact is staged.
mkdir -p "$STAGE/engine" "$STAGE/bin"

manifest_field() { # manifest_field <path> <column>
  awk -F '\t' -v p="$1" -v c="$2" '$3==p {print $c; found=1} END {if(!found) exit 1}' "$MANIFEST_NEXT"
}

destination() { # destination <remote-path>
  case "$1" in
    experts.bin|resident.safetensors|manifest.json|config.json|generation_config.json)
      printf '%s/model/%s\n' "$STAGE" "$1" ;;
    tokenizer_qwen36.json) printf '%s/tokenizer_qwen36.json\n' "$STAGE" ;;
    app.html|samosa-chat.png|models.json) printf '%s/%s\n' "$STAGE" "$1" ;;
    engine/samosa_voice_runtime.sh) printf '%s/bin/samosa-voice-runtime\n' "$STAGE" ;;
    engine/samosa_kokoro_runtime.sh) printf '%s/bin/samosa-kokoro-runtime\n' "$STAGE" ;;
    engine/*) printf '%s/%s\n' "$STAGE" "$1" ;;
    pdfium/*.tgz) printf '%s/%s\n' "$STAGE" "$1" ;;
    samosa) printf '%s/bin/%s\n' "$STAGE" "$1" ;;
    *) return 1 ;;
  esac
}

# Runtime files are mandatory: they are the browser control plane, not an
# optional package with a raw-Qwen fallback (docs/TASKS_UI_CHUTNI.md T1.0).
# The compiled gateway and filesystem sidecar are part of that runtime, so
# they are staged unconditionally rather than gated behind a manifest probe.
INSTALL_FILES="app.html samosa-chat.png models.json engine/qwen36b.c engine/expert_cache.c engine/expert_cache.h engine/vision.c engine/vision.h engine/stb_image.h engine/kernels.h engine/st.h engine/json.h engine/tok.h engine/tok_unicode.h engine/compat.h engine/repetition_guard.h engine/thinking_budget.h engine/samosa_http.h engine/samosa_kokoro.h samosa engine/samosa_gateway.c engine/samosa_fs.c engine/samosa_ocr.c engine/read_cache.h engine/durable_job.h engine/samosa_voice_runtime.sh engine/samosa_kokoro_runtime.sh"

# Chutni is an application runtime component, not a user-installed prerequisite.
# These pinned sources build the same generic service Samosa uses locally and
# MCP-capable applications can launch directly.
CHUTNI_FILES="engine/sqlite/sqlite3.c engine/sqlite/sqlite3.h engine/chutni/LICENSE engine/chutni/NOTICE engine/chutni/VERSION engine/chutni/include/chutni.h engine/chutni/src/chutni.c engine/chutni/src/scan.c engine/chutni/src/cj.c engine/chutni/src/cj.h engine/chutni/src/mcp.c engine/chutni/third_party/blake3/LICENSE_A2 engine/chutni/third_party/blake3/LICENSE_CC0 engine/chutni/third_party/blake3/blake3.c engine/chutni/third_party/blake3/blake3.h engine/chutni/third_party/blake3/blake3_dispatch.c engine/chutni/third_party/blake3/blake3_impl.h engine/chutni/third_party/blake3/blake3_portable.c"
INSTALL_FILES="$INSTALL_FILES $CHUTNI_FILES"

# Model weights are a separate, optional concern from the runtime: a
# runtime-only release (tools/package_hf.py --runtime-only) omits them
# entirely, and a clean install must make no request for any of them. Stage
# each only if the release manifest actually lists it -- same pattern as the
# PDFium/optional-capability blocks below.
MODEL_FILES="experts.bin resident.safetensors manifest.json config.json generation_config.json tokenizer_qwen36.json"

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

# Stage a model weight file only if this release's manifest actually lists
# it. A runtime-only manifest lists none of them, so this loop adds nothing
# and the install below makes no request for any model artifact.
for relative in $MODEL_FILES; do
  if manifest_field "$relative" 1 >/dev/null 2>&1; then
    INSTALL_FILES="$INSTALL_FILES $relative"
  fi
done

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
# Downloads do not retain source file modes.
chmod 755 "$STAGE/bin/samosa" "$STAGE/bin/samosa-voice-runtime" "$STAGE/bin/samosa-kokoro-runtime"

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
DL_FLAGS=""
if [ "$(uname -s)" = "Darwin" ]; then
  for prefix in /opt/homebrew/opt/libomp /usr/local/opt/libomp; do
    if [ -f "$prefix/lib/libomp.dylib" ]; then
      OMP_FLAGS="-Xclang -fopenmp -I$prefix/include -L$prefix/lib -lomp"
      break
    fi
  done
else
  DL_FLAGS="-ldl"
  # Linux OpenMP support check
  if echo "int main() {}" | $COMPILER -fopenmp -x c - -o /dev/null >/dev/null 2>&1; then
    OMP_FLAGS="-fopenmp"
  fi
fi

$COMPILER -O3 -pthread $OMP_FLAGS -Wno-unused-function \
  "$STAGE/engine/qwen36b.c" "$STAGE/engine/expert_cache.c" "$STAGE/engine/vision.c" \
  -o "$STAGE/bin/qwen36b" -lm ||
  fail "staged engine compilation failed; live release was not changed"

# Chutni's own Makefile compiles its release version in from VERSION, and that
# string lands in the producer record of every artifact the service writes
# (SPEC §16.1). This build bypasses that Makefile, so it must pass the same
# define: the in-source fallback is "0.0.0-unversioned", which would attribute
# every artifact in a user's store to a build that never existed. Fail loudly
# rather than silently shipping the fallback.
CHUTNI_VERSION=$(cat "$STAGE/engine/chutni/VERSION" 2>/dev/null | tr -d ' \n')
[ -n "$CHUTNI_VERSION" ] ||
  fail "staged Chutni release is missing VERSION; live release was not changed"

$COMPILER -std=gnu99 -O2 -pthread \
  -I"$STAGE/engine/chutni/include" -I"$STAGE/engine/chutni/src" \
  -I"$STAGE/engine/chutni/third_party/blake3" -I"$STAGE/engine/sqlite" \
  -DCHUTNI_VERSION="\"$CHUTNI_VERSION\"" \
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 \
  -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 \
  -DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 \
  -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_JSON1 \
  "$STAGE/engine/chutni/src/chutni.c" \
  "$STAGE/engine/chutni/src/scan.c" \
  "$STAGE/engine/chutni/src/cj.c" \
  "$STAGE/engine/chutni/src/mcp.c" \
  "$STAGE/engine/chutni/third_party/blake3/blake3.c" \
  "$STAGE/engine/chutni/third_party/blake3/blake3_dispatch.c" \
  "$STAGE/engine/chutni/third_party/blake3/blake3_portable.c" \
  "$STAGE/engine/sqlite/sqlite3.c" \
  -o "$STAGE/bin/chutni-mcp" -lm ||
  fail "staged Chutni service compilation failed; live release was not changed"
chmod +x "$STAGE/bin/chutni-mcp"

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
  EXTRACT_SMOKE_INPUT="$STAGE/.samosa-extract-interface-smoke.txt"
  EXTRACT_SMOKE_LOG="$STAGE/.samosa-extract-interface-smoke.log"
  printf 'not a pdf\n' >"$EXTRACT_SMOKE_INPUT"
  if "$STAGE/bin/samosa-extract" --json-pages "$EXTRACT_SMOKE_INPUT" 1 1 >"$EXTRACT_SMOKE_LOG" 2>&1; then
    fail "staged document extractor accepted a non-PDF interface smoke input"
  fi
  grep -F 'not_pdf' "$EXTRACT_SMOKE_LOG" >/dev/null || {
    sed -n '1,40p' "$EXTRACT_SMOKE_LOG" >&2 || true
    fail "staged document extractor does not support the required --json-pages interface"
  }
  rm -f "$EXTRACT_SMOKE_INPUT" "$EXTRACT_SMOKE_LOG"
fi

# The gateway is the mandatory browser control plane (docs/TASKS_UI_CHUTNI.md
# T1.0), so it is always compiled -- there is no raw-Qwen-only release path.
$COMPILER -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -I"$STAGE/engine" \
  "$STAGE/engine/samosa_gateway.c" -o "$STAGE/bin/samosa-gateway" $DL_FLAGS ||
  fail "staged gateway compilation failed; live release was not changed"
# samosa-jobsd is the same source under a launchd-friendly name (invoked as
# `samosa-jobsd jobsd-once`, it polls armed schedules and exits). The launchd
# plist the gateway installs points at current/bin/samosa-jobsd, so the
# scheduler is broken on a clean install unless this binary exists.
$COMPILER -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -I"$STAGE/engine" \
  "$STAGE/engine/samosa_gateway.c" -o "$STAGE/bin/samosa-jobsd" $DL_FLAGS ||
  fail "staged jobs daemon compilation failed; live release was not changed"
$COMPILER -O2 -Wall -Wextra -Werror -std=c11 \
  "$STAGE/engine/samosa_fs.c" -o "$STAGE/bin/samosa-fs" ||
  fail "staged filesystem sidecar compilation failed; live release was not changed"
$COMPILER -O3 -Wno-unused-function -std=c11 -I"$STAGE/engine" \
  "$STAGE/engine/samosa_ocr.c" -o "$STAGE/bin/samosa-ocr" -lm ||
  fail "staged OCR sidecar compilation failed; live release was not changed"
chmod +x "$STAGE/bin/samosa-gateway" "$STAGE/bin/samosa-jobsd" "$STAGE/bin/samosa-fs" "$STAGE/bin/samosa-ocr" "$STAGE/bin/chutni-mcp"

if [ "${SAMOSA_INSTALL_TEST:-0}" != 1 ]; then
  # This is a control-plane smoke, not a model smoke (docs/TASKS_UI_CHUTNI.md
  # T1.0): the installer's job is the runtime, and a runtime install must
  # succeed identically whether or not a model happens to be present. It
  # never requires or forces a real generation.
  say "Smoke-testing the inactive local control plane..."
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
  # Isolate the smoke run's home from the real ~/.samosa: SAMOSA_RELEASE_DIR
  # only redirects the engine/app/gateway paths dist/samosa itself computes,
  # not the gateway's own internal defaults (profile, jobs, models catalog),
  # which fall back to SAMOSA_HOME. Without an explicit, separate SAMOSA_HOME
  # here, those routes would silently read and write the real user's
  # existing ~/.samosa state instead of this inactive staged release.
  SMOKE_HOME="$STAGE/.smoke-home"
  SAMOSA_RELEASE_DIR="$STAGE" SAMOSA_HOME="$SMOKE_HOME" \
    SAMOSA_MODELS_CATALOG="$STAGE/models.json" SAMOSA_PORT="$SMOKE_PORT" \
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
    fail "staged control plane did not become healthy; live release was not changed"
  }
  curl -fsS --max-time 5 "http://127.0.0.1:$SMOKE_PORT/" |
    grep -q 'Your model.' ||
    fail "staged app UI smoke failed; live release was not changed"

  # /v1/profile, /v1/setup/status, and /v1/models/catalog all require the
  # per-launch UI token (docs/TASKS_UI_CHUTNI.md §5.0); read it from the same
  # place the served page and the gateway itself do.
  TOKEN_FILE="$SMOKE_HOME/run/ui-token"
  i=0
  while [ ! -s "$TOKEN_FILE" ] && [ "$i" -lt 40 ]; do sleep 0.1; i=$((i + 1)); done
  [ -s "$TOKEN_FILE" ] || fail "staged control plane never wrote a UI session token; live release was not changed"
  UI_TOKEN=$(cat "$TOKEN_FILE")

  # A fresh install has no profile yet, so 404 profile_not_found is the
  # honest, correct answer here, not a failure -- only reject a status this
  # route isn't allowed to return at all.
  PROFILE_STATUS=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 5 \
    -H "X-Samosa-Token: $UI_TOKEN" "http://127.0.0.1:$SMOKE_PORT/v1/profile")
  case "$PROFILE_STATUS" in
    200|404) ;;
    *) fail "staged profile endpoint smoke returned unexpected status $PROFILE_STATUS; live release was not changed" ;;
  esac
  curl -fsS --max-time 5 -H "X-Samosa-Token: $UI_TOKEN" \
    "http://127.0.0.1:$SMOKE_PORT/v1/setup/status" >/dev/null ||
    fail "staged setup status endpoint smoke failed; live release was not changed"
  curl -fsS --max-time 5 -H "X-Samosa-Token: $UI_TOKEN" \
    "http://127.0.0.1:$SMOKE_PORT/v1/models/catalog" >/dev/null || {
    curl -sS --max-time 5 -H "X-Samosa-Token: $UI_TOKEN" "http://127.0.0.1:$SMOKE_PORT/v1/models/catalog" >&2
    fail "staged model catalog endpoint smoke failed; live release was not changed"
  }

  # Chat never requires a model to respond honestly: with none installed it
  # must return the structured 409 model_required error; with one already
  # installed (e.g. an upgrade around an existing model) a real reply is
  # fine too, but neither case is forced or assumed here.
  CHAT_LOG="$STAGE/.chat-smoke.json"
  CHAT_STATUS=$(curl -sS -o "$CHAT_LOG" -w '%{http_code}' --max-time 120 \
    "http://127.0.0.1:$SMOKE_PORT/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    --data-binary '{"messages":[{"role":"user","content":"Reply with hello."}],"thinking":"off","max_tokens":16,"seed":11}')
  case "$CHAT_STATUS" in
    200)
      grep -q '"choices"' "$CHAT_LOG" ||
        fail "staged app generation smoke returned 200 without a completion; live release was not changed"
      ;;
    409)
      grep -q 'model_required' "$CHAT_LOG" ||
        fail "staged app chat smoke returned 409 without model_required; live release was not changed"
      ;;
    *)
      sed -n '1,40p' "$CHAT_LOG" >&2 || true
      fail "staged app chat smoke returned unexpected status $CHAT_STATUS; live release was not changed"
      ;;
  esac
  rm -f "$CHAT_LOG"

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
