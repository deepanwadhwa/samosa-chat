#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-atomic-install-test.$$
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

package() {
  python3 "$ROOT/tools/package_hf.py" --out "$REMOTE" --snapshot "$SNAP" \
    --tokenizer "$TMP/tokenizer.json" --repo-id test/samosa >/dev/null
}

install_release() {
  # SAMOSA_SKIP_PATH_SETUP keeps this test focused on atomicity and stops it
  # writing to the developer's shell rc. PATH setup has its own test.
  SAMOSA_INSTALL_TEST=1 SAMOSA_SKIP_PATH_SETUP=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
    SAMOSA_BASE_URL="file://$REMOTE" SAMOSA_HOME="$HOME_DIR" \
    sh "$ROOT/dist/install.sh" >/dev/null
}

package
install_release
first=$(readlink "$HOME_DIR/current")
[ -n "$first" ]
[ -x "$HOME_DIR/bin/samosa" ]
SAMOSA_HOME="$HOME_DIR" "$HOME_DIR/bin/samosa" doctor | grep -q 'LEGACY whole-row q4'

printf 'experts-v2\n' >"$SNAP/experts.bin"
printf '%s\n' '{"expert_quantization":{"format":"groupwise-symmetric-q4-v1","group_size":32,"down_bits":4},"experts":{}}' >"$SNAP/manifest.json"
package
# A killed earlier installer may leave this inactive pointer. It must never
# prevent a later verified release from activating.
ln -s releases/interrupted "$HOME_DIR/.current.next"
install_release
second=$(readlink "$HOME_DIR/current")
[ "$first" != "$second" ]
SAMOSA_HOME="$HOME_DIR" "$HOME_DIR/bin/samosa" doctor | grep -q 'groupwise q4 (group 32)'
[ "$(find "$HOME_DIR/releases" -mindepth 1 -maxdepth 1 -type d ! -name '.*.partial' | wc -l | tr -d ' ')" = 2 ]

# Corrupt a remote payload without updating its signed-by-TLS release
# manifest. Verification must fail before the current pointer changes.
printf 'corruption\n' >>"$REMOTE/experts.bin"
if install_release; then
  echo "corrupt release unexpectedly installed" >&2
  exit 1
fi
[ "$(readlink "$HOME_DIR/current")" = "$second" ]

echo "atomic installer: PASS"
