#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-gateway-install-test.$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
SNAP="$TMP/snapshot"
REMOTE="$TMP/remote"
HOME_DIR="$TMP/home"
mkdir -p "$SNAP"

printf 'experts-v1\n' >"$SNAP/experts.bin"
printf 'resident-v1\n' >"$SNAP/resident.safetensors"
printf '{"experts":{}}\n' >"$SNAP/manifest.json"
printf '{"text_config":{}}\n' >"$SNAP/config.json"
printf '{}\n' >"$SNAP/generation_config.json"
printf '{}\n' >"$TMP/tokenizer.json"

python3 "$ROOT/tools/package_hf.py" --out "$REMOTE" --snapshot "$SNAP" \
  --tokenizer "$TMP/tokenizer.json" --repo-id test/samosa >/dev/null

grep -q 'engine/samosa_fs.c' "$REMOTE/release-manifest.tsv"
grep -q 'engine/samosa_gateway.c' "$REMOTE/release-manifest.tsv"
grep -q 'engine/samosa_ocr.c' "$REMOTE/release-manifest.tsv"
grep -q 'engine/read_cache.h' "$REMOTE/release-manifest.tsv"
grep -q 'engine/samosa_voice_runtime.sh' "$REMOTE/release-manifest.tsv"
grep -q 'engine/samosa_kokoro_runtime.sh' "$REMOTE/release-manifest.tsv"
grep -q 'engine/samosa_kokoro.h' "$REMOTE/release-manifest.tsv"
grep -q 'engine/chutni/src/mcp.c' "$REMOTE/release-manifest.tsv"

SAMOSA_INSTALL_TEST=1 SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
  SAMOSA_BASE_URL="file://$REMOTE" SAMOSA_HOME="$HOME_DIR" \
  sh "$ROOT/dist/install.sh" >/dev/null
[ -x "$HOME_DIR/current/bin/samosa-voice-runtime" ] || { echo "missing staged local voice runtime builder" >&2; exit 1; }
[ -x "$HOME_DIR/current/bin/samosa-kokoro-runtime" ] || { echo "missing staged native Kokoro installer" >&2; exit 1; }
[ ! -e "$HOME_DIR/current/bin/samosa-pocket-tts-runtime" ] || { echo "obsolete Python voice runtime was staged" >&2; exit 1; }

[ -x "$HOME_DIR/current/bin/samosa-fs" ]
[ -x "$HOME_DIR/current/bin/samosa-gateway" ]
[ -x "$HOME_DIR/current/bin/samosa-ocr" ]
[ -x "$HOME_DIR/current/bin/chutni-mcp" ]
# The launchd scheduler's plist runs current/bin/samosa-jobsd, so the installer
# must build it or the scheduler is broken on a clean install.
[ -x "$HOME_DIR/current/bin/samosa-jobsd" ]
[ ! -e "$HOME_DIR/current/bin/jobs_fs.py" ]
[ ! -e "$HOME_DIR/current/bin/samosa_jobs.py" ]
file "$HOME_DIR/current/bin/samosa-gateway" | grep -q 'executable'
file "$HOME_DIR/current/bin/samosa-ocr" | grep -q 'executable'
file "$HOME_DIR/current/bin/samosa-jobsd" | grep -q 'executable'
file "$HOME_DIR/current/bin/chutni-mcp" | grep -q 'executable'

echo "gateway installer: PASS"
