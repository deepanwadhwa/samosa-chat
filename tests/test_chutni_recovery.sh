#!/bin/sh
set -eu
# T4.5 durable build lifecycle: injected-failure recovery.
#
# The acceptance list requires that an injected failure prove, every time:
# source bytes unchanged, no partial generation queryable, the old generation
# still queryable when one existed, restart reaches one deterministic state,
# and retry duplicates no rows or work. Those five are asserted for each
# injection below rather than spot-checked once.
#
# Injections are real, not simulated: a genuinely full filesystem (a small
# RAM disk), a genuinely unwritable directory, a genuinely absent volume
# identity, and a genuine SIGKILL landed mid-build.

DB_BIN=${SAMOSA_CHUTNI_DB:-./build/samosa-chutni-db}
case "$DB_BIN" in /*) ;; *) DB_BIN="$PWD/$DB_BIN" ;; esac
TOKENIZER=${SAMOSA_TOKENIZER:-$PWD/tokenizer_qwen36.json}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-chutni-recovery.XXXXXX")
RAM_DEV=""; RAM_MP=""
cleanup() {
  [ -z "$RAM_MP" ] || umount "$RAM_MP" 2>/dev/null || true
  [ -z "$RAM_DEV" ] || hdiutil detach "$RAM_DEV" >/dev/null 2>&1 || true
  chmod -R u+rwX "$TMP" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
fail() { echo "test_chutni_recovery.sh: $1" >&2; exit 1; }

ROOT="$TMP/root"; STATE="$TMP/state"
mkdir -p "$ROOT"
printf 'The renewal date is June and the clause is binding.\n' >"$ROOT/a.txt"
printf 'A second document about renewal terms.\n' >"$ROOT/b.txt"

# A digest of the source tree. The single most important property of a failed
# build is that it did not touch the user's files.
source_digest() { find "$ROOT" -type f -exec shasum {} \; | sort | shasum | cut -d' ' -f1; }
active_gen() {
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["evidence_generation"])' \
    "$STATE/scopes/$1/active.json" 2>/dev/null || echo "none"
}
queryable() { "$DB_BIN" scope-query "$STATE" "$1" renewal 2>/dev/null | grep -q . && echo YES || echo NO; }
chunk_count() {
  python3 - "$STATE" "$1" <<'PY'
import json,sqlite3,sys
from pathlib import Path
p=Path(sys.argv[1])/'scopes'/sys.argv[2]
try:
    a=json.loads((p/'active.json').read_text())
    d=sqlite3.connect(p/a['database']); print(d.execute('select count(*) from chunks').fetchone()[0]); d.close()
except Exception: print('none')
PY
}

BEFORE=$(source_digest)

# ===========================================================================
# Baseline: one good generation to protect.
# ===========================================================================
"$DB_BIN" scope-create "$STATE" s folder S "$ROOT" >/dev/null
"$DB_BIN" scope-build "$STATE" s "$TOKENIZER" >/dev/null 2>&1 || fail "baseline build failed"
[ "$(active_gen s)" = "1" ] || fail "baseline should publish generation 1"
[ "$(queryable s)" = "YES" ] || fail "baseline generation is not queryable"
BASE_CHUNKS=$(chunk_count s)

# Asserts the five invariants after an injected failure.
# $1 case name, $2 generation that must still be active
assert_survived() {
  [ "$(source_digest)" = "$BEFORE" ] || fail "$1: source bytes changed"
  [ "$(active_gen s)" = "$2" ] || fail "$1: active generation moved to $(active_gen s), expected $2"
  [ "$(queryable s)" = "YES" ] || fail "$1: the last complete generation stopped being queryable"
  [ "$(chunk_count s)" = "$BASE_CHUNKS" ] || fail "$1: chunk count changed to $(chunk_count s), expected $BASE_CHUNKS"
}

# ===========================================================================
# 1. Volume detach -- the scope's volume identity no longer matches the root.
# ===========================================================================
python3 - "$STATE" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1])/'scopes'/'s'/'scope.json'
d=json.loads(p.read_text()); d['volume_identity']='999999999'; p.write_text(json.dumps(d))
PY
if "$DB_BIN" scope-build "$STATE" s "$TOKENIZER" >/dev/null 2>&1; then
  fail "a detached volume must not build"
fi
STATE_NOW=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["state"])' "$STATE/scopes/s/scope.json")
[ "$STATE_NOW" = "disconnected" ] || fail "detach should report disconnected, got $STATE_NOW"
assert_survived "volume detach" 1

# Reconnecting is deterministic: the same command now succeeds.
python3 - "$STATE" "$ROOT" <<'PY'
import json,os,sys
from pathlib import Path
p=Path(sys.argv[1])/'scopes'/'s'/'scope.json'
d=json.loads(p.read_text()); d['volume_identity']=str(os.stat(sys.argv[2]).st_dev); p.write_text(json.dumps(d))
PY
"$DB_BIN" scope-build "$STATE" s "$TOKENIZER" >/dev/null 2>&1 || fail "reconnect build failed"
[ "$(active_gen s)" = "2" ] || fail "reconnect should publish generation 2"
[ "$(chunk_count s)" = "$BASE_CHUNKS" ] || fail "a rebuild of unchanged sources duplicated rows"

# ===========================================================================
# 2. Permission loss -- the scope directory becomes unwritable mid-lifecycle.
# ===========================================================================
chmod 500 "$STATE/scopes/s"
if "$DB_BIN" scope-build "$STATE" s "$TOKENIZER" >/dev/null 2>&1; then
  chmod 700 "$STATE/scopes/s"; fail "a build must not report success when it cannot write"
fi
chmod 700 "$STATE/scopes/s"
assert_survived "permission loss" 2
"$DB_BIN" scope-build "$STATE" s "$TOKENIZER" >/dev/null 2>&1 || fail "retry after permission loss failed"
[ "$(active_gen s)" = "3" ] || fail "retry should publish generation 3"
[ "$(chunk_count s)" = "$BASE_CHUNKS" ] || fail "retry after permission loss duplicated rows"

# ===========================================================================
# 3. SIGKILL landed mid-build.
#
# A slow stub extractor makes the timing deterministic: without it the build
# finishes before the kill and the test proves nothing.
# ===========================================================================
for i in 1 2 3 4 5 6 7 8; do printf '%%PDF-1.4 renewal document %d\n' "$i" >"$ROOT/doc$i.pdf"; done
BEFORE=$(source_digest)
cat >"$TMP/slow-extract" <<'SH'
#!/bin/sh
sleep 0.4
printf '{"ok":true,"text":"renewal clause from %s"}' "$(basename "$2")"
SH
chmod +x "$TMP/slow-extract"

# The baseline must be built with the same extractor the retry will use, or the
# chunk counts differ for a reason that has nothing to do with the SIGKILL.
"$DB_BIN" scope-build "$STATE" s "$TOKENIZER" "$TMP/slow-extract" >/dev/null 2>&1 \
  || fail "build with pdfs failed"
GEN_BEFORE_KILL=$(active_gen s)
BASE_CHUNKS=$(chunk_count s)

# The extraction cache would otherwise make this second build finish before
# the kill lands -- it reuses all eight extractions and takes milliseconds.
# Rewriting the stub with different bytes but identical output changes its
# SHA-256 fingerprint, so every extraction runs again at full cost while the
# resulting chunk count stays comparable.
cat >"$TMP/slow-extract" <<'SH'
#!/bin/sh
# fingerprint-breaking comment; output below is byte-identical to the baseline
sleep 0.4
printf '{"ok":true,"text":"renewal clause from %s"}' "$(basename "$2")"
SH
chmod +x "$TMP/slow-extract"

"$DB_BIN" scope-build "$STATE" s "$TOKENIZER" "$TMP/slow-extract" >/dev/null 2>&1 &
BUILD_PID=$!
sleep 1
kill -9 "$BUILD_PID" 2>/dev/null || true
wait "$BUILD_PID" 2>/dev/null || true
# The child extractor may outlive the killed parent; let it drain.
sleep 0.6
assert_survived "SIGKILL mid-build" "$GEN_BEFORE_KILL"
# No half-written staging database may be left addressable.
ACTIVE_DB=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["database"])' "$STATE/scopes/s/active.json")
[ -f "$STATE/scopes/s/$ACTIVE_DB" ] || fail "active.json points at a database that does not exist"
"$DB_BIN" scope-build "$STATE" s "$TOKENIZER" "$TMP/slow-extract" >/dev/null 2>&1 || fail "retry after SIGKILL failed"
[ "$(chunk_count s)" = "$BASE_CHUNKS" ] || fail "retry after SIGKILL duplicated rows"

# ===========================================================================
# 4. ENOSPC -- a genuinely full filesystem under the scope's state directory.
#
# Skipped rather than faked where a RAM disk cannot be created without
# privileges; a silent skip would be worse than none, so it says so.
# ===========================================================================
RAM_OK=0
if [ "$(uname -s)" = "Darwin" ]; then
  # 32 MiB: large enough that a complete index fits comfortably, small enough
  # that ballast can leave too little for one.
  RAM_DEV=$(hdiutil attach -nomount ram://65536 2>/dev/null | awk 'NR==1{print $1}') || RAM_DEV=""
  if [ -n "$RAM_DEV" ] && newfs_hfs -v T45 "$RAM_DEV" >/dev/null 2>&1; then
    RAM_MP="$TMP/full"; mkdir -p "$RAM_MP"
    mount -t hfs "$RAM_DEV" "$RAM_MP" >/dev/null 2>&1 && RAM_OK=1
  fi
fi

if [ "$RAM_OK" = "1" ]; then
  FULL_STATE="$RAM_MP/state"; FULL_ROOT="$TMP/fullroot"
  mkdir -p "$FULL_ROOT"
  # Measured on the reference Mac: this corpus is ~400 KB of text and produces
  # a ~1.3 MB index, so leaving ~600 KB free cannot complete a build.
  i=0; while [ "$i" -lt 25 ]; do
    awk 'BEGIN{for(n=0;n<300;n++)printf "renewal clause paragraph %d with filler text\n", n}' \
      >"$FULL_ROOT/big$i.txt"
    i=$((i + 1))
  done
  "$DB_BIN" scope-create "$FULL_STATE" f folder F "$FULL_ROOT" >/dev/null 2>&1 \
    || fail "could not create a scope on the small volume"

  FREE_KB=$(df -k "$RAM_MP" | awk 'NR==2{print $4}')
  BALLAST_KB=$((FREE_KB - 600))
  [ "$BALLAST_KB" -gt 0 ] || fail "the small volume has no room to work with"
  dd if=/dev/zero of="$RAM_MP/ballast" bs=1024 count="$BALLAST_KB" >/dev/null 2>&1 || true

  FULL_SRC_BEFORE=$(find "$FULL_ROOT" -type f -exec shasum {} \; | sort | shasum | cut -d' ' -f1)
  if "$DB_BIN" scope-build "$FULL_STATE" f "$TOKENIZER" >/dev/null 2>&1; then
    fail "a build must not report success on a full filesystem"
  fi
  # This is an *initial* generation failure: there is no older generation to
  # fall back to, so the scope must not pretend one exists.
  [ "$(find "$FULL_ROOT" -type f -exec shasum {} \; | sort | shasum | cut -d' ' -f1)" = "$FULL_SRC_BEFORE" ] \
    || fail "ENOSPC: source bytes changed"
  if [ -f "$FULL_STATE/scopes/f/active.json" ]; then
    fail "ENOSPC on the initial build must not publish an active generation"
  fi
  "$DB_BIN" scope-query "$FULL_STATE" f renewal >/dev/null 2>&1 \
    && fail "a scope with no complete generation must not answer queries"

  # Freeing space makes the retry succeed: one deterministic state, no repair.
  rm -f "$RAM_MP/ballast"
  "$DB_BIN" scope-build "$FULL_STATE" f "$TOKENIZER" >/dev/null 2>&1 \
    || fail "the build did not recover once space was available"
  "$DB_BIN" scope-query "$FULL_STATE" f renewal >/dev/null 2>&1 \
    || fail "the recovered generation is not queryable"
else
  echo "test_chutni_recovery.sh: SKIP ENOSPC (no RAM disk available on $(uname -s))" >&2
fi

echo 'test_chutni_recovery.sh: PASS'
