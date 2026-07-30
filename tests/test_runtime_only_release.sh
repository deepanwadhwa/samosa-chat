#!/bin/sh
set -eu

# T1.0 (docs/TASKS_UI_CHUTNI.md): a runtime-only release must install and
# boot the full browser control plane -- gateway, engine, app shell -- while
# making zero requests for any model artifact, and an upgrade must leave an
# already-registered legacy model completely untouched. This runs the real
# smoke path in dist/install.sh (SAMOSA_INSTALL_TEST is deliberately NOT set),
# against a real HTTP server so every request the installer makes is logged.

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-runtime-only-test.$$
trap 'rm -rf "$TMP"; [ -z "${SERVER_PID:-}" ] || kill "$SERVER_PID" 2>/dev/null || true' EXIT HUP INT TERM
REMOTE="$TMP/remote"
HOME_DIR="$TMP/home"
mkdir -p "$REMOTE" "$HOME_DIR"

fail() { echo "runtime-only release: FAIL — $1" >&2; exit 1; }

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
python3 "$ROOT/tools/package_hf.py" --out "$REMOTE" --runtime-only --repo-id test/samosa >/dev/null
grep -q 'engine/samosa_gateway.c' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the mandatory gateway source"
grep -q 'engine/chutni/src/mcp.c' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the bundled Chutni service source"
grep -q 'engine/samosa_ocr.c' "$REMOTE/release-manifest.tsv" ||
  fail "runtime-only manifest is missing the OCR reader source"
for name in $MODEL_ARTIFACT_NAMES; do
  grep -q "	$name$" "$REMOTE/release-manifest.tsv" &&
    fail "runtime-only manifest unexpectedly lists model artifact '$name'"
done

# --- Plant a pre-existing legacy model the versioned installer must never touch
LEGACY_MODEL_DIR="$HOME_DIR/model"
mkdir -p "$LEGACY_MODEL_DIR"
printf 'legacy-experts-content\n' >"$LEGACY_MODEL_DIR/experts.bin"
LEGACY_INODE_BEFORE=$(stat -f '%i' "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null || stat -c '%i' "$LEGACY_MODEL_DIR/experts.bin")
LEGACY_MTIME_BEFORE=$(stat -f '%m' "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null || stat -c '%Y' "$LEGACY_MODEL_DIR/experts.bin")
LEGACY_SHA_BEFORE=$(shasum -a 256 "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null | awk '{print $1}')
[ -n "$LEGACY_SHA_BEFORE" ] || LEGACY_SHA_BEFORE=$(sha256sum "$LEGACY_MODEL_DIR/experts.bin" | awk '{print $1}')

SERVER_PORT=$((19000 + $$ % 4000))
SERVER_PID=""
serve_remote

# --- Real install: the platform preflight and control-plane smoke both run --
# for real (SAMOSA_INSTALL_TEST is not set) since there is no model on disk
# anywhere the smoke path could touch, so nothing here needs a real 24 GB
# model to be safe.
SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
  SAMOSA_BASE_URL="http://127.0.0.1:$SERVER_PORT" SAMOSA_HOME="$HOME_DIR" \
  sh "$ROOT/dist/install.sh" >"$TMP/install-1.log" 2>&1 ||
  { sed -n '1,200p' "$TMP/install-1.log" >&2; fail "clean runtime-only install failed"; }

[ -x "$HOME_DIR/current/bin/samosa-gateway" ] || fail "gateway binary missing after install"
[ -x "$HOME_DIR/current/bin/samosa-fs" ] || fail "filesystem sidecar missing after install"
[ -x "$HOME_DIR/current/bin/chutni-mcp" ] || fail "Chutni service missing after install"
[ -x "$HOME_DIR/current/bin/qwen36b" ] || fail "engine binary missing after install"
[ -f "$HOME_DIR/current/app.html" ] || fail "app shell missing after install"
[ ! -e "$HOME_DIR/current/model" ] || fail "a model directory was created by a runtime-only install"

assert_no_model_requests
stop_remote

# --- Legacy model must be completely untouched by the clean install ---------
[ -f "$LEGACY_MODEL_DIR/experts.bin" ] || fail "legacy model file vanished after install"
[ "$(stat -f '%i' "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null || stat -c '%i' "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_INODE_BEFORE" ] ||
  fail "legacy model inode changed after install"
[ "$(stat -f '%m' "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null || stat -c '%Y' "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_MTIME_BEFORE" ] ||
  fail "legacy model mtime changed after install"

# --- doctor must not report failure merely because no model is installed ----
SAMOSA_HOME="$HOME_DIR" "$HOME_DIR/bin/samosa" doctor | grep -q 'none installed yet' ||
  fail "doctor did not report the model-free state as informational"
SAMOSA_HOME="$HOME_DIR" "$HOME_DIR/bin/samosa" doctor >/dev/null ||
  fail "doctor exited non-zero on a valid model-free runtime install"

# --- Upgrade: repackage with different runtime bytes, reinstall -------------
# A trivial content change (not just a re-run of the same bytes) forces a new
# RELEASE_ID so this actually exercises the upgrade path, not a same-release
# no-op.
cp -R "$ROOT" "$TMP/repo-v2"
printf '\n// t1.0 runtime-only upgrade test marker\n' >>"$TMP/repo-v2/src/samosa_http.h"
( cd "$TMP/repo-v2" && python3 tools/package_hf.py --out "$REMOTE" --runtime-only --repo-id test/samosa >/dev/null )

SERVER_PID=""
serve_remote
SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
  SAMOSA_BASE_URL="http://127.0.0.1:$SERVER_PORT" SAMOSA_HOME="$HOME_DIR" \
  sh "$ROOT/dist/install.sh" >"$TMP/install-2.log" 2>&1 ||
  { sed -n '1,200p' "$TMP/install-2.log" >&2; fail "runtime-only upgrade install failed"; }

assert_no_model_requests
stop_remote

[ "$(find "$HOME_DIR/releases" -mindepth 1 -maxdepth 1 -type d ! -name '.*.partial' | wc -l | tr -d ' ')" = 2 ] ||
  fail "upgrade did not produce a second retained release"

[ -f "$LEGACY_MODEL_DIR/experts.bin" ] || fail "legacy model file vanished after upgrade"
[ "$(stat -f '%i' "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null || stat -c '%i' "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_INODE_BEFORE" ] ||
  fail "legacy model inode changed after upgrade"
[ "$(stat -f '%m' "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null || stat -c '%Y' "$LEGACY_MODEL_DIR/experts.bin")" = "$LEGACY_MTIME_BEFORE" ] ||
  fail "legacy model mtime changed after upgrade"
LEGACY_SHA_AFTER=$(shasum -a 256 "$LEGACY_MODEL_DIR/experts.bin" 2>/dev/null | awk '{print $1}')
[ -n "$LEGACY_SHA_AFTER" ] || LEGACY_SHA_AFTER=$(sha256sum "$LEGACY_MODEL_DIR/experts.bin" | awk '{print $1}')
[ "$LEGACY_SHA_AFTER" = "$LEGACY_SHA_BEFORE" ] || fail "legacy model bytes changed after upgrade"

echo "runtime-only release: PASS"
