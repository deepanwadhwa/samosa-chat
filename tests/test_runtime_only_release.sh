#!/bin/sh
set -eux

# T1.0 (docs/TASKS_UI_CHUTNI.md): a runtime-only release must install and
# boot the full browser control plane -- gateway, engine, app shell -- while
# making zero requests for any optional chat-model artifact, and an upgrade
# must leave an already-registered legacy model completely untouched. This runs the real
# smoke path in dist/install.sh (SAMOSA_INSTALL_TEST is deliberately NOT set),
# against a real HTTP server so every request the installer makes is logged.

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-runtime-only-test.$$
trap 'rm -rf "$TMP"; [ -z "${SERVER_PID:-}" ] || kill "$SERVER_PID" 2>/dev/null || true' EXIT HUP INT TERM
REMOTE="$TMP/remote"
HOME_DIR="$TMP/home"
mkdir -p "$REMOTE" "$HOME_DIR"

fail() { echo "runtime-only release: FAIL — $1" >&2; exit 1; }

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    echo "FAIL: neither shasum nor sha256sum is installed" >&2
    return 127
  fi
}

MODEL_ARTIFACT_NAMES='experts.bin resident.safetensors manifest.json config.json generation_config.json tokenizer_qwen36.json'

serve_remote() {
  SERVER_LOG="$TMP/http-server.log"
  : >"$SERVER_LOG"
  ( cd "$REMOTE" && python3 -m http.server "$SERVER_PORT" >"$SERVER_LOG" 2>&1 ) &
  SERVER_PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS --max-time 1 "http://127.0.0.1:$SERVER_PORT/release-manifest.tsv" >/dev/null 2>&1 && return 0
    sleep 0.05
    i=$((i + 1))
  done
  fail "fixture HTTP server never came up"
}

stop_remote() {
  [ -z "${SERVER_PID:-}" ] || kill "$SERVER_PID" 2>/dev/null || true
  [ -z "${SERVER_PID:-}" ] || wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
}

assert_no_model_requests() {
  for name in $MODEL_ARTIFACT_NAMES; do
    grep -q "$name" "$SERVER_LOG" &&
      fail "fixture server logged a request for model artifact '$name': $(grep "$name" "$SERVER_LOG")"
  done
  return 0
}

# --- Package a runtime-only release: no --snapshot, no --tokenizer -----------
SAMOSA_PACKAGE_TEST=1 python3 "$ROOT/tools/package_hf.py" --out "$REMOTE" --runtime-only \
  --repo-id test/samosa \
  --summarizer-model "$ROOT/tests/fixtures/native-summarizer/model.gguf" \
  --summarizer-runtime-dir "$ROOT/tests/fixtures/native-summarizer" >/dev/null
grep -q 'engine/samosa_gateway.c' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the mandatory gateway source"
grep -q 'engine/chutni/src/mcp.c' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the bundled Chutni service source"
grep -q 'engine/samosa_ocr.c' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the OCR reader source"
grep -q 'engine/samosa_voice_runtime.sh' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the local voice runtime builder"
grep -q 'engine/samosa_kokoro_runtime.sh' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the native Kokoro installer"
if [ "$(uname -s):$(uname -m)" = "Darwin:arm64" ]; then
  grep -q 'runtime/macos-arm64/samosa-maple$' "$REMOTE/release-manifest.tsv" ||
    fail "Apple-Silicon release manifest is missing samosa-maple"
  grep -q 'runtime/macos-arm64/mlx.metallib$' "$REMOTE/release-manifest.tsv" ||
    fail "Apple-Silicon release manifest is missing mlx.metallib"
  grep -q 'runtime/macos-arm64/samosa-summarizer$' "$REMOTE/release-manifest.tsv" ||
    fail "Apple-Silicon release manifest is missing the native summarizer"
  grep -q 'runtime/common/samosa-text-summarization-Q8_0.gguf$' "$REMOTE/release-manifest.tsv" ||
    fail "Apple-Silicon release manifest is missing the native summarizer model"
fi
for name in $MODEL_ARTIFACT_NAMES; do
  grep -q "	$name$" "$REMOTE/release-manifest.tsv" &&
    fail "runtime-only manifest unexpectedly lists model artifact '$name'"
done

# --- Plant a pre-existing legacy model the versioned installer must never touch
LEGACY_MODEL_DIR="$HOME_DIR/model"
mkdir -p "$LEGACY_MODEL_DIR"
printf 'legacy-experts-content\n' >"$LEGACY_MODEL_DIR/experts.bin"
if [ "$(uname -s)" = "Darwin" ]; then
  get_inode() { stat -f '%i' "$1"; }
  get_mtime() { stat -f '%m' "$1"; }
else
  get_inode() { stat -c '%i' "$1"; }
  get_mtime() { stat -c '%Y' "$1"; }
fi

LEGACY_INODE_BEFORE=$(get_inode "$LEGACY_MODEL_DIR/experts.bin")
LEGACY_MTIME_BEFORE=$(get_mtime "$LEGACY_MODEL_DIR/experts.bin")
LEGACY_SHA_BEFORE=$(sha256_file "$LEGACY_MODEL_DIR/experts.bin")

SERVER_PORT=$((19000 + $$ % 4000))
SERVER_PID=""
serve_remote

# --- Real install: the platform preflight and control-plane smoke both run --
# for real (SAMOSA_INSTALL_TEST is not set) since there is no model on disk
# anywhere the smoke path could touch, so nothing here needs a real 24 GB
# model to be safe.
SAMOSA_IGNORE_RAM_CHECK=1 SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
  SAMOSA_BASE_URL="http://127.0.0.1:$SERVER_PORT" SAMOSA_HOME="$HOME_DIR" \
  sh "$ROOT/dist/install.sh" >"$TMP/install-1.log" 2>&1 ||
  { sed -n '1,200p' "$TMP/install-1.log" >&2; fail "clean runtime-only install failed"; }

[ -x "$HOME_DIR/current/bin/samosa-gateway" ] || fail "gateway binary missing after install"
[ -x "$HOME_DIR/current/bin/samosa-fs" ] || fail "filesystem sidecar missing after install"
[ -x "$HOME_DIR/current/bin/chutni-mcp" ] || fail "Chutni service missing after install"

# dist/install.sh compiles Chutni without its Makefile, so it must pass
# -DCHUTNI_VERSION itself. The in-source fallback is "0.0.0-unversioned", and
# the scanner writes its version into the producer record of every artifact
# (SPEC §16.1) -- a shipped fallback would permanently attribute a real user's
# store to a build that never existed. Assert against the store, not against
# the binary's own help output, because the store is where it does damage.
CHUTNI_PROBE="$TMP/chutni-version-probe"
mkdir -p "$CHUTNI_PROBE/d" "$CHUTNI_PROBE/home"
printf 'version probe\n' >"$CHUTNI_PROBE/d/a.txt"
HOME="$CHUTNI_PROBE/home" CHUTNI_HOME="$CHUTNI_PROBE/home/chutni" \
  "$HOME_DIR/current/bin/chutni-mcp" --call chutni_folder_activate \
  "{\"path\":\"$CHUTNI_PROBE/d\",\"confirmed\":true,\"register\":true,\"label\":\"probe\",\"app_name\":\"Samosa\",\"app_version\":\"runtime-only-test\"}" \
  >"$CHUTNI_PROBE/activate.json" 2>&1 ||
  fail "installed Chutni service could not activate a folder"
EXPECTED_CHUTNI_VERSION=$(tr -d ' \n' <"$ROOT/vendor/chutni/VERSION")
RECORDED_CHUTNI_VERSION=$(sqlite3 \
  "file:$CHUTNI_PROBE/d.chutni/catalog.sqlite?immutable=1" \
  "SELECT DISTINCT version FROM producers WHERE name='chutni-reference-scanner';")
[ "$RECORDED_CHUTNI_VERSION" = "$EXPECTED_CHUTNI_VERSION" ] ||
  fail "installed Chutni recorded producer version '$RECORDED_CHUTNI_VERSION', expected '$EXPECTED_CHUTNI_VERSION'"
[ -x "$HOME_DIR/current/bin/qwen36b" ] || fail "engine binary missing after install"
[ -f "$HOME_DIR/current/app.html" ] || fail "app shell missing after install"
[ ! -e "$HOME_DIR/current/model" ] || fail "a model directory was created by a runtime-only install"
[ -x "$HOME_DIR/current/bin/samosa-voice-runtime" ] ||
  fail "runtime-only install did not stage the local voice runtime builder"
[ -x "$HOME_DIR/current/bin/samosa-kokoro-runtime" ] ||
  fail "runtime-only install did not stage the native Kokoro installer"
[ ! -e "$HOME_DIR/current/bin/samosa-pocket-tts-runtime" ] ||
  fail "runtime-only install staged the obsolete Python voice runtime"
if [ "$(uname -s):$(uname -m)" = "Darwin:arm64" ]; then
  [ -x "$HOME_DIR/current/bin/samosa-maple" ] ||
    fail "Apple-Silicon install did not stage samosa-maple"
  [ -f "$HOME_DIR/current/bin/mlx.metallib" ] ||
    fail "Apple-Silicon install did not stage mlx.metallib"
fi

assert_no_model_requests
stop_remote

# --- Legacy model must be completely untouched by the clean install ---------
[ -f "$LEGACY_MODEL_DIR/experts.bin" ] || fail "legacy model file vanished after install"
[ "$(get_inode "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_INODE_BEFORE" ] ||
  fail "legacy model inode changed after install"
[ "$(get_mtime "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_MTIME_BEFORE" ] ||
  fail "legacy model mtime changed after install"

# --- doctor must not report failure merely because no model is installed ----
SAMOSA_HOME="$HOME_DIR" "$HOME_DIR/bin/samosa" doctor > "$TMP/doctor.log"
grep -q 'none installed yet' "$TMP/doctor.log" ||
  fail "doctor did not report the model-free state as informational"
SAMOSA_HOME="$HOME_DIR" "$HOME_DIR/bin/samosa" doctor >/dev/null ||
  fail "doctor exited non-zero on a valid model-free runtime install"

# --- Upgrade: change one packaged runtime file, reinstall -------------------
# A content change plus its matching manifest entry forces a new RELEASE_ID,
# so this exercises the upgrade path rather than a same-release no-op. Do not
# clone the whole working tree here: developer clones can contain many GB of
# Git objects and ignored build artifacts that are irrelevant to this test.
UPGRADE_REL=engine/samosa_http.h
UPGRADE_FILE="$REMOTE/$UPGRADE_REL"
printf '\n// t1.0 runtime-only upgrade test marker\n' >>"$UPGRADE_FILE"
UPGRADE_SHA=$(sha256_file "$UPGRADE_FILE")
UPGRADE_BYTES=$(wc -c <"$UPGRADE_FILE" | tr -d ' ')
awk -F '\t' -v OFS='\t' -v path="$UPGRADE_REL" -v sha="$UPGRADE_SHA" -v bytes="$UPGRADE_BYTES" '
  $3 == path { $1 = sha; $2 = bytes; found = 1 }
  { print }
  END { if (!found) exit 2 }
' "$REMOTE/release-manifest.tsv" >"$TMP/release-manifest.next.tsv" ||
  fail "could not update the upgrade fixture manifest"
mv "$TMP/release-manifest.next.tsv" "$REMOTE/release-manifest.tsv"

SERVER_PID=""
serve_remote
SAMOSA_IGNORE_RAM_CHECK=1 SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
  SAMOSA_BASE_URL="http://127.0.0.1:$SERVER_PORT" SAMOSA_HOME="$HOME_DIR" \
  sh "$ROOT/dist/install.sh" >"$TMP/install-2.log" 2>&1 ||
  { sed -n '1,200p' "$TMP/install-2.log" >&2; fail "runtime-only upgrade install failed"; }

assert_no_model_requests
stop_remote

[ "$(find "$HOME_DIR/releases" -mindepth 1 -maxdepth 1 -type d ! -name '.*.partial' | wc -l | tr -d ' ')" = 2 ] ||
  fail "upgrade did not produce a second retained release"

[ -f "$LEGACY_MODEL_DIR/experts.bin" ] || fail "legacy model file vanished after upgrade"
[ "$(get_inode "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_INODE_BEFORE" ] ||
  fail "legacy model inode changed after upgrade"
[ "$(get_mtime "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_MTIME_BEFORE" ] ||
  fail "legacy model mtime changed after upgrade"
LEGACY_SHA_AFTER=$(sha256_file "$LEGACY_MODEL_DIR/experts.bin")
[ "$LEGACY_SHA_AFTER" = "$LEGACY_SHA_BEFORE" ] || fail "legacy model bytes changed after upgrade"

echo "runtime-only release: PASS"
