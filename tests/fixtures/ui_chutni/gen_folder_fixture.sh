#!/bin/sh
set -eu

# Shared Chutni folder-tree fixture generator (docs/TASKS_UI_CHUTNI.md T0.1,
# reused by T4.2/T5.1). Builds one small, deterministic tree covering every
# scan/skip scenario named in the spec's acceptance criteria: plain text, a
# real multi-page PDF, a real PNG, an unsupported binary format, duplicate
# content at two paths, a hidden file, an oversized file, a permission-denied
# directory, and a symlink that tries to escape the root.
#
# Usage:
#   gen_folder_fixture.sh build TARGET_DIR
#   gen_folder_fixture.sh mutate-add TARGET_DIR      # add a new file
#   gen_folder_fixture.sh mutate-rename TARGET_DIR   # rename an existing file
#   gen_folder_fixture.sh mutate-delete TARGET_DIR   # delete an existing file
#   gen_folder_fixture.sh mutate-change TARGET_DIR   # change file content+mtime
#   gen_folder_fixture.sh fix-perms TARGET_DIR       # restore perms before rm -rf
#
# All content is generated from small inline data or copied from other
# checked-in fixtures under tests/fixtures/documents/ — nothing here is a
# multi-gigabyte or externally-fetched artifact.

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
DOCS_FIXTURES="$ROOT_DIR/fixtures/documents"

cmd="${1:?usage: gen_folder_fixture.sh <build|mutate-add|mutate-rename|mutate-delete|mutate-change|fix-perms> TARGET_DIR}"
target="${2:?usage: gen_folder_fixture.sh <build|mutate-add|mutate-rename|mutate-delete|mutate-change|fix-perms> TARGET_DIR}"

gen_png() {
  # A real, valid 1x1 white PNG, generated from inline bytes (no network,
  # no dependency on any other repo asset).
  python3 -c "
import zlib, struct

def chunk(tag, data):
    out = struct.pack('>I', len(data)) + tag + data
    out += struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    return out

sig = b'\211PNG\r\n\032\n'
ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)
raw = b'\000' + b'\377\377\377'
idat = zlib.compress(raw)
data = sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')
open('$1', 'wb').write(data)
"
}

build() {
  mkdir -p "$target"
  rm -rf "${target:?}"/* 2>/dev/null || true

  mkdir -p "$target/plain" "$target/pdf" "$target/image" "$target/unsupported" \
           "$target/duplicate/a" "$target/duplicate/b" "$target/hidden" \
           "$target/oversized" "$target/unreadable/blocked" "$target/symlink-escape"

  printf 'Quarterly report draft.\nRevenue is up 12%% year over year.\n' >"$target/plain/report.md"
  printf 'Just a scratch note.\n' >"$target/plain/notes.txt"

  cp "$DOCS_FIXTURES/multipage_7pages.pdf" "$target/pdf/multipage_7pages.pdf"
  cp "$DOCS_FIXTURES/hello.pdf" "$target/pdf/hello.pdf"

  gen_png "$target/image/sample.png"

  # Unsupported by v1 (§6.1): a real, minimal ZIP container (PK magic), not a
  # v1-supported text/PDF/image type.
  python3 -c "
import zipfile
with zipfile.ZipFile('$target/unsupported/archive.zip', 'w') as z:
    z.writestr('inner.txt', 'unsupported archive contents')
"

  # Duplicate content at two distinct paths: both must survive as separate
  # 'files' rows sharing one extraction/content record (CLAUDE.md G-defect).
  printf 'Cafe total 4.50\n' >"$target/duplicate/a/receipt.txt"
  printf 'Cafe total 4.50\n' >"$target/duplicate/b/receipt-copy.txt"

  printf 'Hidden scratch notes, excluded unless the user opts in.\n' >"$target/hidden/.secret-notes.txt"

  # "Oversized" is policy-relative; callers pass a small maximum_file_bytes in
  # tests (e.g. 2048) so this modest file trips too_large without a huge fixture.
  python3 -c "
open('$target/oversized/big.bin', 'wb').write(bytes((i % 251) for i in range(4096)))
"

  printf 'Never reached: parent directory is permission-denied.\n' \
    >"$target/unreadable/blocked/inner.txt"
  chmod 000 "$target/unreadable/blocked"

  # Escape attempts: both an absolute and a relative symlink pointing outside
  # the fixture root, at a universally-present, read-only, harmless target.
  ln -s /etc "$target/symlink-escape/escape-abs"
  ln -s ../../../../../../../../etc "$target/symlink-escape/escape-rel"

  echo "gen_folder_fixture.sh: built fixture tree at $target"
}

mutate_add() {
  printf 'A file added after the initial build.\n' >"$target/plain/added-later.txt"
}

mutate_rename() {
  [ -f "$target/plain/notes.txt" ] || { echo "mutate-rename: plain/notes.txt missing" >&2; exit 1; }
  mv "$target/plain/notes.txt" "$target/plain/notes-renamed.txt"
}

mutate_delete() {
  [ -f "$target/plain/report.md" ] || { echo "mutate-delete: plain/report.md missing" >&2; exit 1; }
  rm -f "$target/plain/report.md"
}

mutate_change() {
  [ -f "$target/duplicate/a/receipt.txt" ] || { echo "mutate-change: duplicate/a/receipt.txt missing" >&2; exit 1; }
  printf 'Cafe total 5.75 (price change)\n' >"$target/duplicate/a/receipt.txt"
}

fix_perms() {
  # Best-effort: restores read/write/execute so `rm -rf` never fails on the
  # deliberately-locked-down subtree. Callers should run this in their cleanup
  # trap before removing the fixture directory.
  [ -d "$target" ] && chmod -R u+rwX "$target" 2>/dev/null || true
}

case "$cmd" in
  build) build ;;
  mutate-add) mutate_add ;;
  mutate-rename) mutate_rename ;;
  mutate-delete) mutate_delete ;;
  mutate-change) mutate_change ;;
  fix-perms) fix_perms ;;
  *) echo "unknown command: $cmd" >&2; exit 64 ;;
esac
