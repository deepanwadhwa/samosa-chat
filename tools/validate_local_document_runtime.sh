#!/bin/sh
# Validate the extractor/library pair before a local release can activate it.
set -eu

extract=${1:-}
library=${2:-}
[ -n "$extract" ] || exit 0
[ -x "$extract" ] || {
  echo "local document install refused: extractor is not executable: $extract" >&2
  exit 1
}
version=$("$extract" --version 2>/dev/null) || {
  echo "local document install refused: extractor version probe failed" >&2
  exit 1
}
case "$version" in
  *';pdfium)')
    [ -n "$library" ] && [ -f "$library" ] || {
      echo "local document install refused: PDFium-enabled extractor has no runtime library" >&2
      exit 1
    }
    ;;
  *';no-pdfium)')
    [ -z "$library" ] || {
      echo "local document install refused: $(basename "$extract") was built without PDFium while $(basename "$library") is present" >&2
      echo "rebuild samosa-extract with the matching PDFium SDK before installing" >&2
      exit 1
    }
    ;;
  *)
    echo "local document install refused: extractor has an unknown capability version" >&2
    exit 1
    ;;
esac
