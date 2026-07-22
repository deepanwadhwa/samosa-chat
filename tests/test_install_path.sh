#!/bin/sh
# Covers install.sh's PATH setup — the code that decides whether `samosa` will
# actually be runnable after an install, and what the installer tells you.
#
# This went untested and shipped a real bug: the installer appended PATH to the
# shell rc and then said "Run: samosa doctor", which fails in that same shell
# because the rc only affects terminals opened later. A new user's first
# command produced "command not found".
#
# Every case runs the real dist/install.sh with HOME redirected at a temp dir,
# so the logic is exercised for real and no developer profile is ever touched.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-install-path-test.$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
SNAP="$TMP/snapshot"
REMOTE="$TMP/remote"
mkdir -p "$SNAP"

printf 'experts-v1\n'        >"$SNAP/experts.bin"
printf 'resident-v1\n'       >"$SNAP/resident.safetensors"
printf '{"experts":{}}\n'    >"$SNAP/manifest.json"
printf '{"text_config":{}}\n'>"$SNAP/config.json"
printf '{}\n'                >"$SNAP/generation_config.json"
printf '{}\n'                >"$TMP/tokenizer.json"

python3 "$ROOT/tools/package_hf.py" --out "$REMOTE" --snapshot "$SNAP" \
  --tokenizer "$TMP/tokenizer.json" --repo-id test/samosa >/dev/null

# Runs the installer against the file:// fixture remote.
#   $1 = fake HOME, $2 = SHELL, $3 = PATH, $4 = extra env assignment ("" for none)
# SAMOSA_INSTALL_TEST=1 skips only the platform preflight and the smoke test
# (both need a real model); it must NOT skip PATH setup, which is the subject.
run_install() {
  fake_home=$1; fake_shell=$2; fake_path=$3; extra=${4:-}
  mkdir -p "$fake_home"
  env HOME="$fake_home" SHELL="$fake_shell" PATH="$fake_path" \
      SAMOSA_INSTALL_TEST=1 SAMOSA_MIN_FREE_AFTER_GB=0 \
      SAMOSA_BASE_URL="file://$REMOTE" SAMOSA_HOME="$fake_home/.samosa" \
      ${extra:+$extra} \
      sh "$ROOT/dist/install.sh" 2>&1
}

fail() { echo "install-path: FAIL — $1" >&2; exit 1; }
# A real PATH is needed for the installer's own tools (clang, awk, tar, ...).
REAL_PATH=$PATH

# --- Case 1: launcher not on PATH -> configure rc, and say so honestly -------
H="$TMP/case1"
out=$(run_install "$H" /bin/zsh "$REAL_PATH")
[ -f "$H/.zshrc" ] || fail "case1: zsh rc was not created"
[ "$(grep -c 'samosa/bin' "$H/.zshrc")" = 1 ] || fail "case1: expected exactly one PATH line"
# The critical assertion: it must not tell you to run a command this shell
# cannot resolve. It must send you to a new terminal instead.
echo "$out" | grep -q 'this terminal does not know about samosa yet' \
  || fail "case1: installer did not warn that the current shell cannot find samosa"
echo "$out" | grep -q 'Open a new terminal' || fail "case1: missing new-terminal guidance"
echo "$out" | grep -q 'samosa is installed at\|Samosa is installed at' \
  || fail "case1: installer did not report the install location"

# --- Case 2: re-running must be idempotent ----------------------------------
out=$(run_install "$H" /bin/zsh "$REAL_PATH")
[ "$(grep -c 'samosa/bin' "$H/.zshrc")" = 1 ] || fail "case2: PATH line was duplicated on re-install"
echo "$out" | grep -q 'already configured' || fail "case2: did not report the rc as already configured"

# --- Case 3: launcher already on PATH -> touch nothing, no nag --------------
H="$TMP/case3"; mkdir -p "$H/.samosa/bin"
out=$(run_install "$H" /bin/zsh "$H/.samosa/bin:$REAL_PATH")
[ ! -f "$H/.zshrc" ] || fail "case3: rc was written even though PATH already resolved"
echo "$out" | grep -q 'this terminal does not know about samosa yet' \
  && fail "case3: warned about a new terminal when samosa was already runnable"
echo "$out" | grep -q 'Try:  samosa' || fail "case3: expected the direct try-it hint"

# --- Case 4: rc file is chosen from the shell -------------------------------
H="$TMP/case4"
run_install "$H" /bin/bash "$REAL_PATH" >/dev/null
[ -f "$H/.bashrc" ] || fail "case4: bash did not get .bashrc"
[ ! -f "$H/.zshrc" ] || fail "case4: bash wrote a zsh rc"

H="$TMP/case5"
run_install "$H" /usr/bin/fish "$REAL_PATH" >/dev/null
[ -f "$H/.profile" ] || fail "case5: unknown shell did not fall back to .profile"

# --- Case 6: opt-out must never touch a profile -----------------------------
H="$TMP/case6"
run_install "$H" /bin/zsh "$REAL_PATH" "SAMOSA_SKIP_PATH_SETUP=1" >/dev/null
[ ! -f "$H/.zshrc" ] || fail "case6: SAMOSA_SKIP_PATH_SETUP=1 still wrote to the rc"

echo "install PATH setup: PASS"
