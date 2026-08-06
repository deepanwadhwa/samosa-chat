#!/bin/sh
set -eu

# T0.2 (docs/TASKS_UI_CHUTNI.md): samosa-fs chutni-inventory/chutni-hash must
# stream instead of materializing+sorting the whole tree, preserve duplicate
# content as separate path records, keep scanning past an unreadable
# descendant instead of aborting, never follow a symlink, and only ever
# produce a complete (never truncated/prefix) hash on explicit request.

BUILD_DIR="${BUILD_DIR:-build}"
FS="${SAMOSA_FS:-./$BUILD_DIR/samosa-fs}"
GEN="$(CDPATH= cd -- "$(dirname "$0")" && pwd)/fixtures/ui_chutni/gen_folder_fixture.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/chutni_inventory_test.XXXXXX")
ROOT="$TMP/root"

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

cleanup() {
  sh "$GEN" fix-perms "$ROOT" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make samosa-fs
sh "$GEN" build "$ROOT"

OUT="$TMP/inventory.ndjson"
"$FS" chutni-inventory --root "$ROOT" >"$OUT"

# --- duplicate content preserved as two distinct path records ---
DUP_COUNT=$(grep -c '"rel_path":"duplicate/' "$OUT")
[ "$DUP_COUNT" = 2 ] || { echo "FAIL: expected 2 duplicate-content file records, got $DUP_COUNT"; cat "$OUT"; exit 1; }

# --- the unreadable nested directory is counted, not fatal ---
if [ "$(id -u)" != 0 ]; then
  grep -q '"rel_path":"unreadable/blocked","reason":"permission_denied"' "$OUT" \
    || {
      echo "FAIL: missing permission_denied skip for unreadable/blocked"
      cat "$OUT"
      exit 1
    }
else
  echo "test_chutni_inventory.sh: SKIP permission-denied assertion as root" >&2
fi
# ...and the scan continued past it: files alongside it are still present.
grep -q '"rel_path":"plain/report.md"' "$OUT" || { echo "FAIL: scan did not continue past the unreadable dir"; exit 1; }

# --- symlinks are never followed, both absolute and relative escapes ---
grep -q '"rel_path":"symlink-escape/escape-abs","reason":"symlink"' "$OUT" \
  || { echo "FAIL: absolute symlink escape was not skipped as symlink"; exit 1; }
grep -q '"rel_path":"symlink-escape/escape-rel","reason":"symlink"' "$OUT" \
  || { echo "FAIL: relative symlink escape was not skipped as symlink"; exit 1; }
# No line may reference /etc -- proof nothing was ever resolved/followed into it.
if grep -q '/etc' "$OUT"; then
  echo "FAIL: inventory output references the escape target -- a symlink was followed"; exit 1
fi

# --- hidden file excluded by default, included with --include-hidden ---
if grep -q '"type":"file","rel_path":"hidden/.secret-notes.txt"' "$OUT"; then
  echo "FAIL: hidden file appeared as a file record without --include-hidden"; exit 1
fi
grep -q '"type":"skip","rel_path":"hidden/.secret-notes.txt","reason":"hidden_excluded"' "$OUT" \
  || { echo "FAIL: missing hidden_excluded skip record"; exit 1; }
"$FS" chutni-inventory --root "$ROOT" --include-hidden \
  | grep -q '"type":"file","rel_path":"hidden/.secret-notes.txt"' \
  || { echo "FAIL: --include-hidden did not surface the hidden file"; exit 1; }

# --- inventory never reads content: no hash field anywhere in the output ---
if grep -q 'sha256\|"hash"' "$OUT"; then
  echo "FAIL: inventory-only scan emitted a hash -- it must be metadata-only"; exit 1
fi

# --- --exclude drops a named component everywhere it appears ---
"$FS" chutni-inventory --root "$ROOT" --exclude oversized >"$TMP/excluded.ndjson"
grep -q '"rel_path":"oversized","reason":"user_exclusion"' "$TMP/excluded.ndjson" \
  || { echo "FAIL: --exclude oversized did not produce a user_exclusion skip"; cat "$TMP/excluded.ndjson"; exit 1; }
if grep -q '"rel_path":"oversized/big.bin"' "$TMP/excluded.ndjson"; then
  echo "FAIL: --exclude oversized did not stop descent into the excluded directory"; exit 1
fi

# --- the done summary accounts for what was seen ---
grep -q '"type":"done","canceled":false' "$OUT" || { echo "FAIL: missing clean done summary"; cat "$OUT"; exit 1; }

sh "$GEN" fix-perms "$ROOT"
rm -rf "$ROOT"

# --- chutni-hash: a complete SHA-256, never a prefix, with identity checks ---
sh "$GEN" build "$ROOT"
EXPECTED=$(sha256_file "$ROOT/plain/report.md") || exit 1
GOT=$("$FS" chutni-hash --root "$ROOT" plain/report.md)
printf '%s' "$GOT" | grep -q "\"sha256\":\"$EXPECTED\"" \
  || { echo "FAIL: chutni-hash mismatch"; echo "$GOT"; exit 1; }
printf '%s' "$GOT" | grep -q '"size":58' || { echo "FAIL: chutni-hash size mismatch"; echo "$GOT"; exit 1; }

# --- chutni-hash refuses a symlink outright ---
STATUS=0
"$FS" chutni-hash --root "$ROOT" symlink-escape/escape-abs >"$TMP/hash-symlink.out" 2>&1 || STATUS=$?
[ "$STATUS" != 0 ] || { echo "FAIL: chutni-hash accepted a symlink"; exit 1; }

# --- chutni-hash refuses a path outside the scope root ---
STATUS=0
"$FS" chutni-hash --root "$ROOT/plain" "../pdf/hello.pdf" >"$TMP/hash-outside.out" 2>&1 || STATUS=$?
[ "$STATUS" != 0 ] || { echo "FAIL: chutni-hash accepted a path outside its scope root"; exit 1; }
grep -q 'path_outside_scope\|bad_args' "$TMP/hash-outside.out" \
  || { echo "FAIL: expected path_outside_scope/bad_args error"; cat "$TMP/hash-outside.out"; exit 1; }

sh "$GEN" fix-perms "$ROOT"
rm -rf "$ROOT"

# --- cancellation: SIGTERM shortly after start must stop within ~2 seconds
#     and still emit a clean "done" summary. A small fixture plus the
#     SAMOSA_CHUTNI_TEST_DELAY_US determinism seam (2ms/file -> ~1s total for
#     500 files) makes this reliable without a huge, slow-to-create fixture. ---
mkdir -p "$TMP/big"
i=0
while [ "$i" -lt 500 ]; do
  printf 'fixture file %s\n' "$i" >"$TMP/big/file-$i.txt"
  i=$((i + 1))
done
SAMOSA_CHUTNI_TEST_DELAY_US=2000 "$FS" chutni-inventory --root "$TMP/big" >"$TMP/big.ndjson" &
FSPID=$!
sleep 0.1
kill -TERM "$FSPID" 2>/dev/null || true
START=$(date +%s)
while kill -0 "$FSPID" 2>/dev/null; do
  NOW=$(date +%s)
  if [ $((NOW - START)) -gt 2 ]; then
    echo "FAIL: chutni-inventory did not stop within 2 seconds of SIGTERM" >&2
    kill -KILL "$FSPID" 2>/dev/null || true
    exit 1
  fi
  sleep 0.05
done
wait "$FSPID" 2>/dev/null || true
grep -q '"canceled":true' "$TMP/big.ndjson" || { echo "FAIL: canceled run did not report canceled:true"; tail -5 "$TMP/big.ndjson"; exit 1; }
TOTAL_LINES=$(wc -l <"$TMP/big.ndjson" | tr -d ' ')
[ "$TOTAL_LINES" -lt 501 ] || { echo "FAIL: canceled run appears to have completed the full 500-file scan"; exit 1; }

echo "test_chutni_inventory.sh: PASS"
