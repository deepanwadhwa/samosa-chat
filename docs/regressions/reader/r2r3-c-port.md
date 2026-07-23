# R2/R3 — `samosa-ocr` C sidecar: forward pass validated vs the golden reference

**Status: RUN (2026-07-23), reference M3 Air.** The C sidecar
[src/samosa_ocr.c](../../../src/samosa_ocr.c) implements the PP-OCRv6 small det +
rec forward passes, DB box extraction, rotate-crop, and CTC decode — a faithful
translation of the E-R1 NumPy reference ([tools/ocr_ref.py](../../../tools/ocr_ref.py)).
It is dependency-free (only `stb_image.h`, `kernels.h`, `json.h`, libc); no
paddle/torch/onnx.

## The numerical gate — `make ocr-test`

`samosa-ocr _selftest` runs the C forward pass on the **same** preprocessed
input tensors the NumPy reference used (dumped by
[tools/dump_ocr_golden.py](../../../tools/dump_ocr_golden.py) into
`tools/testdata/ocr/*.gold`) and compares outputs. This isolates the neural math
from image-resampling and is the R2/R3 definition-of-done.

| Check | Result |
|---|---|
| Detector prob map, max abs diff vs NumPy golden | **1.085e-05** |
| Recognizer argmax per timestep vs golden | **100 %** (T=40) |
| Recognizer max-prob per timestep, max diff | **2.03e-06** |
| CTC decode text | `Poličar 2019` — **exact** |

The NumPy reference itself matched PaddleOCR 3.7.0 to 9.7e-05 / argmax 100 %
(E-R1), so the chain **C ≈ NumPy ≈ PaddleOCR** holds to float32 rounding. The C
reads the diacritic name 1-bit Bonsai got wrong.

## End-to-end

`samosa-ocr read` on the receipt fixture returns all 14 lines with correct text
(`GROCERY MART #418`, `Date 2023-04-17 14:22`, `4.29`, `SUBTOTAL`, `26.53`, …),
splitting columns exactly as PaddleOCR does. `detect`, `recognize --box`, and
`read --emit-crops --below CONF` all implement the reader-v0 JSON contract with
stable error codes (`ocr_unavailable`, `image_invalid`), `--version`, CPU/address
rlimits, and `lstat`/`O_NOFOLLOW`/`fstat` file discipline per SIDECAR_CONTRACT.

## Box geometry — connected-components (documented approximation)

The detector *forward* (prob map) is exact. DB box extraction uses
connected-components + bbox-unclip instead of cv2 `minAreaRect` + pyclipper
`JT_ROUND` offset. Measured against the NumPy reference boxes:

| Fixture | C boxes | NumPy boxes | mean IoU | min IoU |
|---|---|---|---|---|
| f02_receipt | 14 | 14 | 0.928 | 0.863 |
| f03_form | 7 | 7 | 0.937 | 0.901 |
| f04_dense | 8 | 8 | 0.940 | 0.873 |
| f01_diacritics | 6 | 6 | 0.916 | 0.886 |

Same box **count** on every fixture and correct downstream text; the ~0.9 IoU
(vs 1.0) is corner rounding of the uniform-bbox unclip vs clipper's rounded
offset. This is exact for axis-aligned printed lines — the v1 scope. **Rotated
text and exact minAreaRect/clipper parity are a documented refinement**, not
claimed here.

## Performance

First read of the 820×474 receipt: **14.3 s** portable, **5.6 s** with the OpenMP
build (`make samosa-ocr-omp`, `OMP_NUM_THREADS=8`) — the f32 `matmul` in
`kernels.h` is scalar. Reads are content-addressed and cached forever after
(R4), so this is a one-time cost per file. Follow-up optimizations (a NEON f32
GEMM path, BatchNorm folding at export) are open; correctness came first.

## Scope / not done

- Box geometry parity (minAreaRect + clipper), rotated-text crops.
- Architecture is specialized to the small tier's block tables (weights,
  thresholds, charset are pack-driven); a medium pack would add medium tables.
- R4 (gateway `doc.read`, cache, Jobs `review_required`) and R5 (tier-2
  escalation) are not built. E-R2 (strong-reader-on-crop, real backends) is the
  gate for R5/R6 and needs the machine free.
