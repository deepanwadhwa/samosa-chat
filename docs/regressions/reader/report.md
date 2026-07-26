# E-R1 — PP-OCRv6 small OCR pack: export + numeric validation

**Status: RUN (2026-07-23), on the reference 16 GB M3 MacBook Air.** This is the
run-first gate of [docs/TASKS_READER.md](../../TASKS_READER.md) (build step R1).
It decides whether the C port of the printed-OCR cascade is worth building — and
it passes: the dependency-free NumPy reimplementation reproduces PaddleOCR's own
forward pass to float32 rounding, and reads the exact printed strings that 1-bit
Bonsai got wrong.

## What ran

- **Reference implementation:** PaddleOCR **3.7.0** / PaddlePaddle **3.3.1**,
  native arm64, loading the **pinned** `PP-OCRv6_small_det` / `PP-OCRv6_small_rec`
  safetensors (the exact revisions in the TASKS_READER pins table) from a local
  dir — so the reference and the port run *identical weights*.
- **Port under test:** [tools/ocr_ref.py](../../../tools/ocr_ref.py) — NumPy only
  (no paddle/torch/onnx), reading the Samosa flat pack built by
  [tools/export_ocr_pack.py](../../../tools/export_ocr_pack.py).
- **Fixtures:** 27 synthetic **printed** pages, 127 ground-truth lines, spanning
  clean → degraded (small, blur, noise, JPEG, rotation, low-contrast, and
  aggressive combinations). Includes the concrete Bonsai failure cases —
  diacritic names (`Poličar`, `Håkon Ærøskøbing`) and printed years (`2019`).
  Regenerable deterministically (seed 1234) by
  [tools/run_e_r1.py](../../../tools/run_e_r1.py); ground truth in
  `fixtures/ground_truth.json`; full results in `e_r1_results.json`.

**Scope honesty:** fixtures are *printed* text + Latin diacritics — that is what
E-R1 measures and all it licenses a claim about. Photographed receipts and
handwriting are E-R2 / R5 territory and are **not** claimed here.

## Deliverables

### (a) Pack format frozen

`reader-v0` flat pack ([tools/ocr_pack.py](../../../tools/ocr_pack.py)):
`magic("SMSAOCR\0") | version | header_len | JSON header | float32 data`,
32-byte aligned. One dtype (f32) keeps the C reader trivial. The export tool
enforces **decision 8** at download time — re-reads the HF license tag at the
pinned revision, checks payload **byte sizes** against the pins table, and
records **SHA-256** of every file. Committed manifest:
`ocr-pack-v1.manifest.json` (both pins verified **apache-2.0**; det
`model.safetensors` 9,938,124 B, rec 21,204,736 B + `inference.yml` 150,579 B —
all exact).

### (b) NumPy port matches PaddleOCR

| Check | Result |
|---|---|
| Detector forward, max abs diff vs paddle (prob map ∈ [0,1]) | **9.72e-05** |
| Recognizer forward, max abs diff vs paddle (softmax) | **2.05e-05** |
| Recognizer argmax agreement per timestep | **100.0 %** |
| End-to-end line-for-line vs PaddleOCR, **clean fixtures** | **1.000** |
| End-to-end line agreement, all fixtures incl. heavy degradation | 0.916 |
| Mean page-text agreement vs PaddleOCR | 0.972 |

The tensor-level diffs are float32 accumulation noise: the port is the same
computation. On clean printed pages it reproduces PaddleOCR's lines exactly; the
0.916 all-fixtures figure is dominated by the aggressively-degraded pages where
detection segments the same garbage differently (both readers are "wrong" in
different ways there). Concretely, both the Bonsai misses read correctly:
`Invoice 2019` (conf 1.000) and `Poličar total 42.50` (conf 0.99).

### (c) T_ACCEPT / T_DECIDE calibrated

131 recognized lines, **118 correct / 13 incorrect** vs ground truth
(whitespace-insensitive substring: robust to column-splitting and
missing-space-after-colon, still flags real substitutions). The confidence
histogram separates cleanly:

```
conf bin      n   correct  incorrect
[0.1,0.2)     4      0        4     <- pure-blur garbage: hallucinated CJK, conf 0.13–0.15
[0.6,0.7)     1      0        1
[0.7,0.8)     2      0        2
[0.8,0.9)     5      0        5     <- real misreads, e.g. "Fimo petint clause T dalied 2019"
[0.9,1.0]   119    118        1
```

- **Genuine OCR errors are all ≤ 0.834; correct reads are all ≥ 0.960.** The lone
  "incorrect" at conf 0.987 is `Håkon AErøskøbing` for GT `Håkon Ærøskøbing` —
  an Æ→AE ligature normalization, not a misrecognition.
- **T_ACCEPT = 0.84** (escalate below): maximizes Youden's J = **0.923**; J is
  flat across the whole empty gap [0.84, 0.96), so any value there escalates
  every genuine misread while accepting every correct read.
- **T_DECIDE = 0.99** (gate Jobs file actions): lowest conf giving ≥99 %
  precision-that-a-line-is-correct — excludes even the benign Æ case. This is the
  safety property row of the TASKS_READER `review_required` table: a
  `conf < T_DECIDE` line under a JO move predicate parks for review, never acts.

These supersede the spec's initial guesses (0.80 / 0.90) with measured values and
are written into the pack manifest.

### (d) Render resolution — 768 vs 1536 px long edge

Mean char accuracy on the dense/small/codes fixtures: **768 px = 0.9957**,
**1536 px = 0.9918**. No gain from 1536 — slightly worse. Reason: the det model
caps its input long edge at 960 internally, so a 1536 render is downscaled for
detection anyway, and cubic-upscaling small text adds no real information for the
recognizer. **Decision: keep `samosa-extract --render-ppm` at 768; do not raise
the cap for OCR.** The open question in TASKS_READER is answered with a number.

### (e) Small vs medium tier

Mean page accuracy: **small 0.9457 / medium 0.9677**. Medium beats small on **3**
pages, small beats medium on **0** — and all 3 medium wins are
heavily-degraded pages (`microblur`, `jpegtiny`, `blurnoisehard`). On every clean
printed fixture small == medium. Per the promotion rule, medium only helps where
tier-2 escalation (the strong reader) is the intended remedy anyway. **Decision:
ship the small tier (det+rec ≈ 31 MB) as v1.** The C port is shape-generic
(architecture read from the pack), so medium is a pack swap if real-document
evidence later justifies it.

## Verdict

E-R1 **passes**. The pack format is frozen, the NumPy port is numerically
faithful to PaddleOCR on identical weights, thresholds are calibrated from real
histograms, and the resolution/tier questions are decided with measurements. The
C port (R2/R3) is cleared to proceed against this NumPy reference as its golden
oracle.

## Reproduce

```sh
# build the pack (downloads pinned revisions, verifies license/size/SHA)
python tools/export_ocr_pack.py --tier small --out ~/.samosa/models/ocr-pack-v1 \
    --t-accept 0.84 --t-decide 0.99
# run E-R1 (needs paddleocr>=3.7.0 + paddlepaddle in a venv; ~47 s of fixtures)
python tools/run_e_r1.py --pack ~/.samosa/models/ocr-pack-v1 \
    --src <pinned_small_src> --medium-src <pinned_medium_src> \
    --out docs/regressions/reader
```
