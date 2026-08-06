#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T0.1: proves the shared Chutni folder-tree fixture generator itself is
# correct — build produces the exact expected shape, every mutate- subcommand
# does what it claims, and fix-perms leaves nothing behind that `rm -rf` can't
# remove. Future T4.2/T5.1 tests depend on this generator; this is its gate.

GEN="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)/tests/fixtures/ui_chutni/gen_folder_fixture.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/chutni_folder_fixture.XXXXXX")
TARGET="$TMP/root"

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
  sh "$GEN" fix-perms "$TARGET" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

sh "$GEN" build "$TARGET"

# --- expected shape ---
[ -f "$TARGET/plain/report.md" ] || { echo "FAIL: plain/report.md missing"; exit 1; }
[ -f "$TARGET/plain/notes.txt" ] || { echo "FAIL: plain/notes.txt missing"; exit 1; }
[ -f "$TARGET/pdf/multipage_7pages.pdf" ] || { echo "FAIL: multipage PDF missing"; exit 1; }
[ -f "$TARGET/pdf/hello.pdf" ] || { echo "FAIL: hello.pdf missing"; exit 1; }
[ -f "$TARGET/image/sample.png" ] || { echo "FAIL: sample.png missing"; exit 1; }
[ -f "$TARGET/unsupported/archive.zip" ] || { echo "FAIL: archive.zip missing"; exit 1; }
[ -f "$TARGET/duplicate/a/receipt.txt" ] || { echo "FAIL: duplicate/a missing"; exit 1; }
[ -f "$TARGET/duplicate/b/receipt-copy.txt" ] || { echo "FAIL: duplicate/b missing"; exit 1; }
[ -f "$TARGET/hidden/.secret-notes.txt" ] || { echo "FAIL: hidden file missing"; exit 1; }
[ -f "$TARGET/oversized/big.bin" ] || { echo "FAIL: oversized file missing"; exit 1; }
[ -d "$TARGET/unreadable/blocked" ] || { echo "FAIL: unreadable/blocked directory missing"; exit 1; }
[ -L "$TARGET/symlink-escape/escape-abs" ] || { echo "FAIL: absolute escape symlink missing"; exit 1; }
[ -L "$TARGET/symlink-escape/escape-rel" ] || { echo "FAIL: relative escape symlink missing"; exit 1; }

# --- duplicate content really is identical bytes at two distinct paths ---
SUM_A=$(sha256_file "$TARGET/duplicate/a/receipt.txt")
SUM_B=$(sha256_file "$TARGET/duplicate/b/receipt-copy.txt")
[ "$SUM_A" = "$SUM_B" ] || { echo "FAIL: duplicate fixture files are not byte-identical"; exit 1; }

# --- the PNG is real and decodable (magic bytes) ---
head -c 8 "$TARGET/image/sample.png" | od -An -tx1 | tr -d ' \n' | grep -qi '^89504e470d0a1a0a$' \
  || { echo "FAIL: sample.png is not a real PNG"; exit 1; }

# --- the blocked directory is genuinely unreadable to this process ---
if [ "$(id -u)" != "0" ]; then
  if ls "$TARGET/unreadable/blocked" >/dev/null 2>&1; then
    echo "FAIL: unreadable/blocked was listable despite chmod 000"; exit 1
  fi
fi

# --- both escape symlinks resolve outside the fixture root ---
RESOLVED_ABS=$(cd -P "$TARGET/symlink-escape" && python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" escape-abs)
case "$RESOLVED_ABS" in
  "$TARGET"/*) echo "FAIL: escape-abs did not resolve outside the root"; exit 1 ;;
esac
RESOLVED_REL=$(cd -P "$TARGET/symlink-escape" && python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" escape-rel)
case "$RESOLVED_REL" in
  "$TARGET"/*) echo "FAIL: escape-rel did not resolve outside the root"; exit 1 ;;
esac

# --- mutate- subcommands do what they say ---
sh "$GEN" mutate-add "$TARGET"
[ -f "$TARGET/plain/added-later.txt" ] || { echo "FAIL: mutate-add did not add a file"; exit 1; }

sh "$GEN" mutate-rename "$TARGET"
[ -f "$TARGET/plain/notes-renamed.txt" ] || { echo "FAIL: mutate-rename did not create the new name"; exit 1; }
[ ! -e "$TARGET/plain/notes.txt" ] || { echo "FAIL: mutate-rename left the old name behind"; exit 1; }

sh "$GEN" mutate-delete "$TARGET"
[ ! -e "$TARGET/plain/report.md" ] || { echo "FAIL: mutate-delete did not remove the file"; exit 1; }

BEFORE_SUM=$(sha256_file "$TARGET/duplicate/a/receipt.txt")
sh "$GEN" mutate-change "$TARGET"
AFTER_SUM=$(sha256_file "$TARGET/duplicate/a/receipt.txt")
[ "$BEFORE_SUM" != "$AFTER_SUM" ] || { echo "FAIL: mutate-change did not change content"; exit 1; }

# --- fix-perms leaves a tree rm -rf can remove cleanly ---
sh "$GEN" fix-perms "$TARGET"
rm -rf "$TARGET"
[ ! -d "$TARGET" ] || { echo "FAIL: fix-perms did not allow full removal"; exit 1; }

echo "test_chutni_folder_fixture.sh: PASS"
