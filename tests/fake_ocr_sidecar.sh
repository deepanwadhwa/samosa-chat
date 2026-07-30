#!/bin/sh
set -eu

case "${1:-}" in
  --version)
    printf '%s\n' 'samosa-ocr fixture-1'
    ;;
  read)
    printf '%s\n' '{"ok":true,"script":"printed","reader":"fixture","lines":[{"bbox":[0,0,10,10],"text":"OCR fixture text Poličar 2019","conf":0.99,"script":"printed","reader":"fixture"}]}'
    ;;
  *)
    printf '%s\n' '{"ok":false,"error":"unsupported_fixture_call"}'
    exit 64
    ;;
esac
