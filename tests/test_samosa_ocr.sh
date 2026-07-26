#!/bin/sh
# Offline gate for samosa-ocr (R2/R3): validates the C forward pass numerically
# against the NumPy golden tensors (tools/testdata/ocr) that E-R1 verified
# line-for-line against PaddleOCR 3.7.0. Also checks a full read on the tiny
# fixture. Skips cleanly if the OCR pack is not installed (it is generated
# offline by tools/export_ocr_pack.py; the weights are not committed).
set -eu

OCR="${SAMOSA_OCR:-./build/samosa-ocr}"
PACK="${SAMOSA_OCR_PACK:-$HOME/.samosa/models/ocr-pack-v1}"
GOLD="tools/testdata/ocr"

if [ ! -f "$OCR" ]; then echo "FAIL: $OCR not built"; exit 1; fi

# --version always works (no pack needed)
"$OCR" --version | grep -q "samosa-ocr" || { echo "FAIL: --version"; exit 1; }

if [ ! -f "$PACK/det.bin" ] || [ ! -f "$PACK/rec.bin" ] || [ ! -f "$PACK/charset.txt" ]; then
  echo "SKIP: OCR pack not installed at $PACK"
  echo "      build it offline: python tools/export_ocr_pack.py --tier small --out $PACK"
  exit 0
fi

export SAMOSA_OCR_PACK="$PACK"

echo "== numerical selftest (C forward vs NumPy golden) =="
OUT=$("$OCR" _selftest "$GOLD")
echo "$OUT"
echo "$OUT" | grep -q "selftest: PASS" || { echo "FAIL: selftest did not PASS"; exit 1; }

echo "== end-to-end read on tiny fixture =="
READ=$("$OCR" read "$GOLD/tiny.png")
echo "$READ"
echo "$READ" | grep -q '"ok":true' || { echo "FAIL: read not ok"; exit 1; }
echo "$READ" | grep -q 'Poličar 2019' || { echo "FAIL: expected 'Poličar 2019' in read output"; exit 1; }

echo "ocr-test: PASS"
