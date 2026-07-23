# Samosa Reader — `doc.read` tools contract + build card

**Status: R1/E-R1 + R2 + R3 RUN and passed (2026-07-23); R4–R7 not built.** The
run-first gate and the C forward-pass port are measured on the reference
machine — the pack format is frozen, the pinned det+rec are exported to a Samosa
flat pack (licenses/sizes/SHA verified), the NumPy port reproduces PaddleOCR
3.7.0 to float32 rounding (det 9.7e-05, rec argmax 100 %, line-for-line exact on
clean fixtures), and the dependency-free C sidecar `samosa-ocr` reproduces that
NumPy port (det prob map 1.1e-05, rec argmax 100 %, CTC text exact) with
`make ocr-test` green. Thresholds calibrated (T_ACCEPT 0.84, T_DECIDE 0.99);
render cap stays 768; ship the small tier. Evidence:
[E-R1](regressions/reader/report.md), [R2/R3](regressions/reader/r2r3-c-port.md).
R4 (gateway `doc.read`, cache, Jobs `review_required`) and R5–R7 remain **design
until built and measured**; E-R2 (strong-reader-on-crop) is the R5/R6 gate and
runs on **Bonsai + mmproj only — no 24 GB Qwen tower** (decision 9). Every
remaining model size, speed, and accuracy figure is an upstream report,
*unverified*, until measured here. Program bar per
[ISSUE_TASKS.md](ISSUE_TASKS.md): acceptance is measured, a negative result is a
result, "should work" is not a status.

This card fixes the **Tools-layer contract for general document reading** —
printed or handwritten, image or PDF — so the build order underneath it
(text detector → printed recognizer → escalation → handwriting head) can land
piece by piece without the interface moving. It follows the sidecar rules in
[SIDECAR_CONTRACT.md](SIDECAR_CONTRACT.md), composes with
[`samosa-extract`](samosa-extract.md) (#5), and plugs into Jobs
([TASKS_JOBS.md](TASKS_JOBS.md)) through the existing `review_required`
machinery. It was split out of TASKS_JOBS.md deliberately: that card is the
*Jobs* layer; this is a *Tools*-layer contract that chat and Jobs both call.

Contract version: **reader-v0 (draft).** Field renames/removals after v1 ships
require a documented revision, per SIDECAR_CONTRACT.md §Versioning.

## Why this exists (the design argument, recorded once)

The orchestrating model (Ornith, Qwen) must be able to plan folder-scale grunt
work — "move every file that mentions John Doe" — **without branching on how
the text got onto the page**. If the model has to reason printed-vs-handwritten
per file, every plan grows if/else arms and every arm is a hallucination
surface. The generality therefore lives in the *interface*: one tool that
returns text for anything, with the printed/handwritten/blurry routing done by
deterministic code inside it.

The alternative — one VLM that "just reads anything" — fails on this host's
measured numbers:

- Qwen's built-in tower is the accuracy ceiling here but costs **8+ min per
  full page** ([qwen-image-thinking-fix-2026-07-22.md](regressions/jobs/e-j1/qwen-image-thinking-fix-2026-07-22.md)).
- 1-bit Bonsai + mmproj reads a page in **~14 s** but misread *printed*
  diacritic names and a printed year on a real fixture
  ([bonsai-vision-2026-07-22.md](regressions/jobs/e-j1/bonsai-vision-2026-07-22.md)).
- Folder economics: 200 files × 14 s ≈ 47 min *per question*, with no
  per-line confidence signal to gate file actions on.

The cascade inverts this: a cheap printed-OCR pass handles the bulk of most
real documents at CNN cost, and only the crops that resist it are escalated to
the strong reader — **Bonsai + mmproj**, a bounded per-crop request, not a
full-page pass and **not** the 24 GB Qwen tower (decision 9). Cost concentrates
exactly where the hard pixels are. VLM-class generality at 16 GB is
manufactured, not bought.

## Decisions locked (do not reopen without the owner)

1. **One general tool; the cascade is invisible to the model.** Escalation is
   a deterministic threshold rule in C, never a model decision and never a
   prompt instruction.
2. **Model-facing default output is serialized text, not geometry.** Prefill
   is the binding constraint on this machine; a full per-line bbox JSON of a
   dense page multiplies token cost for nothing. Geometry is a `detail` view.
3. **The OCR runtime is a dependency-free C sidecar** (`samosa-ocr`), pure
   `argv`/JSON per SIDECAR_CONTRACT.md, reusing `kernels.h` GEMM and
   `stb_image.h`. Model weights are exported **offline** by a one-time Python
   tool into a Samosa-owned flat pack; no Paddle/PyTorch/ONNX at runtime.
4. **Sidecars stay network-free**, so tier-2 escalation to a vision backend is
   orchestrated by the **gateway**, not by `samosa-ocr`.
5. **Read results are cached centrally, content-addressed.** Never as
   companion files in user folders.
6. **A low-confidence read never silently drives a file action.** It lands
   the unit in `review_required`, reusing the Jobs states. Medical records get
   flagged, not mis-filed.
7. **No deletes anywhere in this path** (standing owner rule, 2026-07-19).
8. **License policy: Apache-2.0 or MIT only (owner, 2026-07-23).** Every
   model pack, vendored library, and exported weight file in this card must
   be Apache-2.0 or MIT, verified against the exact files at pin time —
   not the repo's headline license. AGPL and other copyleft are
   disqualifying, full stop (house precedent: PyMuPDF, rejected in
   [TASKS_DOCUMENTS.md](TASKS_DOCUMENTS.md)). If a named candidate fails
   this check, the candidate is replaced; the policy is not.
9. **Tier-2 strong reader is Bonsai + mmproj — never the 24 GB Qwen tower
   (owner, 2026-07-23).** The read path must not load the 24 GB model. Ornith
   is the text orchestrator and is text-only (`backend_supports_images` returns
   0 for it), so it cannot serve tier-2 crops; when Ornith is the active
   backend, a needed escalation surfaces `vision_backend_required` and the page
   parks for review. E-R2 therefore measures Bonsai only.

## The tool — `doc.read`

Registry name `doc.read` (namespacing per SIDECAR_CONTRACT.md §Registry
Names). Implemented in the gateway; callable by chat models through the
existing tools path and by the Jobs runner directly.

**Arguments**

```json
{ "path": "/granted/root/scan.pdf",
  "detail": "text" | "lines",          // default "text"
  "pages": [start, count],             // optional; 1-based, count 1..5 —
                                       //   same bound as samosa-extract
                                       //   --json-pages; interactive calls
                                       //   must page, batch may go whole-file
                                       //   only under a Jobs token budget
  "refresh": false }                   // true bypasses + rewrites the cache
```

**Result, `detail:"text"` (the model-facing default)**

```json
{ "ok": true,
  "page_count": 3,
  "text": "…full reading-order text of the selected pages…",
  "pages": [
    { "index": 1, "source": "text_layer",
      "lines_total": 41, "lines_uncertain": 0, "min_conf": 1.0,
      "needs_review": false },
    { "index": 2, "source": "ocr",
      "lines_total": 38, "lines_uncertain": 3, "min_conf": 0.44,
      "needs_review": false } ],
  "any_uncertain": true,
  "needs_review": false }
```

**Result, `detail:"lines"`** adds per-page `lines` arrays (this is also the
shape stored in the cache — the cache always stores full detail):

```json
{ "bbox": [x0, y0, x1, y1],            // pixels, origin top-left, in the
                                       //   rendered page space; page pixel
                                       //   size is reported alongside
  "text": "03/14/1987",
  "conf": 0.61,                        // final reader's confidence, [0,1]
  "script": "printed" | "uncertain",   // "handwritten" is RESERVED until the
                                       //   classifier head (R7) exists — v1
                                       //   must not claim it
  "reader": "text_layer" | "rec_print" | "vlm_crop" }
                                       // "rec_hand" reserved for R6
```

`text` is always the complete serialized page — uncertain lines are included
in place (their best available reading), never dropped. `conf` is the
recognizer's mean per-character probability; it is comparable only within one
reader and one pack version. Thresholds are calibrated per pack (E-R1), not
assumed portable.

**Failure envelope** follows SIDECAR_CONTRACT.md: `{"ok":false,"error":code}`
with stable codes — `ocr_unavailable` (pack or sidecar missing),
`vision_backend_required` (tier 2 needed, active backend text-only),
`image_invalid`, `image_too_large`, `path_denied`, `wall_timeout`.

## The cascade (inside the tool — normative)

Cheapest reader first; escalate only what resists.

- **Tier 0 — text layer (free).** For PDFs, `samosa-extract` per-page
  metadata decides: a page with `text_layer` and adequate text tokens is taken
  as-is (`source:"text_layer"`, `conf:1.0`). A page failing the existing Jobs
  `needs_image` test (`text_tokens < LOW_TEXT_TOKENS OR has_raster_figure`) is
  rendered and falls to tier 1. Standalone images (PNG/JPEG/PPM) start at
  tier 1.
- **Tier 1 — printed OCR (fast path).** `samosa-ocr read`: text detector
  finds line boxes, printed recognizer reads each crop, per-line `conf` comes
  back. Lines with `conf >= T_ACCEPT` are accepted as `script:"printed"`.
- **Tier 2 — strong reader (bounded).** Lines with `conf < T_ACCEPT` are
  re-read from their crops by the strong reader — **v1: a crop-sized request to
  Bonsai + mmproj**, the vision backend. The **24 GB Qwen tower is excluded
  from this path** (owner, 2026-07-23; decision 9) — no 24 GB model load for
  reading. Ornith is the text orchestrator and has **no vision** (see
  `backend_supports_images` in [src/samosa_gateway.c](../src/samosa_gateway.c)),
  so when Ornith is the active backend and tier 2 is needed the tool surfaces
  `vision_backend_required` and the page parks for review. Per-crop cost on
  Bonsai is unmeasured until E-R2. The tier-2 result replaces the text, keeps
  `reader:"vlm_crop"`, and the line is labeled `script:"uncertain"` — low
  printed-OCR confidence means handwriting *or* blur *or* a stamp; v1 does not
  pretend to know which.

**Escalation rule (deterministic, calibrated not assumed):**

```
T_ACCEPT = 0.80        # initial; calibrated from the E-R1 conf histogram
MAX_ESCALATIONS_PER_PAGE = 8
```

If a page has more than `MAX_ESCALATIONS_PER_PAGE` low-confidence lines, the
first 8 (by area, largest first) are escalated, the rest keep their tier-1
reading, and the page is marked `needs_review:true`. If tier 2 is needed but
no vision backend is active, no escalation runs and the page is marked
`needs_review:true` with the tool-level error surfaced to Jobs as
`vision_backend_required`. Pages are never silently truncated.

Tier-2 calls made under Jobs go through the background admission class and
the chat interlock exactly as any Jobs inference does (TASKS_JOBS.md §J1.13);
interactive `doc.read` calls count as interactive.

## The sidecar — `samosa-ocr`

One domain (turning pixels into positioned text), subcommand family per
SIDECAR_CONTRACT.md. Network-free, one JSON object on stdout, stable error
codes, own CPU/address limits + parent watchdog, `lstat`/`O_NOFOLLOW`/`fstat`
file discipline — all inherited requirements, not restated here.

```sh
samosa-ocr read IMAGE                          # detect + recognize; full lines JSON
samosa-ocr read IMAGE --emit-crops DIR --below CONF
                                               # additionally write PPM crops of
                                               #   lines under CONF into caller-owned
                                               #   DIR (mode 0600; caller deletes) —
                                               #   this is how the gateway gets
                                               #   tier-2 crops without re-decoding
samosa-ocr detect IMAGE                        # boxes only (test/debug granularity)
samosa-ocr recognize IMAGE --box x0,y0,x1,y1   # one crop (test/debug granularity)
samosa-ocr --version
```

Input: PPM/PNG/JPEG via the already-vendored `stb_image.h`. PDF pages arrive
as PPMs rendered by `samosa-extract --render-ppm`.

**Pack discovery:** `~/.samosa/models/ocr-pack-v1/` (`det.bin`, `rec.bin`,
`charset.txt`, `manifest.json`), override `SAMOSA_OCR_PACK`. Absent pack →
`{"ok":false,"error":"ocr_unavailable"}` — clean capability degradation,
mirroring the `samosa-extract`-absent behavior. Never a host-tool fallback.

## The read cache

- **Path:** `~/.samosa/cache/read/<sha256[0:2]>/<sha256>.json` — key is the
  SHA-256 of the file bytes. Content-addressed, so a moved or renamed file
  still hits, and an edited file misses. Dirs `0700`, files `0600`.
- **Entry:** `{contract_version, pack_fingerprint, created, result}` where
  `result` is the full `detail:"lines"` payload and `pack_fingerprint` hashes
  the det/rec/hand pack manifests + thresholds + `samosa-ocr --version`.
  A fingerprint mismatch is a miss (recompute, overwrite).
- **Bound:** `SAMOSA_READ_CACHE_MAX_MB`, default 512; on write over the cap,
  prune oldest-mtime entries first.
- **Never in user folders.** The cache holds extracted text of possibly
  sensitive documents; it lives inside `~/.samosa` under the same local trust
  boundary as the chats directory, and nothing here ever leaves the machine.

This is what makes the motto scenario cheap: a folder is read **once per file
content, ever**. "Now find the ones from 2023" re-reads nothing.

## Jobs wiring — `review_required`

Reuses the existing states and reason style (TASKS_JOBS.md; compare
`extractor_unavailable:application/pdf`):

| Condition | Unit status | Reason |
|---|---|---|
| OCR pack or sidecar absent | `review_required` | `ocr_unavailable` |
| Tier 2 needed, active backend text-only | `review_required` | `vision_backend_required` (existing reason, reused) |
| Page `needs_review` (escalation budget exceeded / strong reader unavailable) | `review_required` | `low_confidence_read` |
| A Jobs decision predicate (e.g. a JO move condition, a required schema field) depended on any line with `conf < T_DECIDE` (initial 0.90, calibrated in E-R1) | `review_required` | `low_confidence_read` |

The last row is the safety property this whole design exists for: "move all
files that mention John Doe" over medical records must park the shaky reads
for a human, not act on them. JO's no-delete / journaled-undo rules apply
unchanged to whatever moves *are* approved.

## Model pins (licenses verified 2026-07-23; decision 8 applied)

Every model below is **pinned to an exact Hugging Face repo revision** and
was license-verified on 2026-07-23 by the method stated in the table.
Accuracy and speed figures remain upstream reports — unverified on the
reference machine until E-R1/E-R2 run. The executing agent must, at download
time: (1) re-read the license tag at the pinned revision, (2) download at
that revision (`?revision=<sha>`), (3) check the payload byte sizes against
this table, and (4) record SHA-256 of every downloaded file in the pack
manifest. Any mismatch — tag, revision, or size — is **stop and report to
the owner**, never substitute.

| Role | Pin (HF repo `@` revision) | Payload (exact bytes) | License — verification method |
|---|---|---|---|
| Text detector (R2) | `PaddlePaddle/PP-OCRv6_small_det_safetensors` `@ eae2ee920a39fb3087637d3dbb58df1896ec1f24` | `model.safetensors` 9,938,124 | Apache-2.0 — HF `license` tag on the official PaddlePaddle org, read 2026-07-23 |
| Text recognizer (R3) | `PaddlePaddle/PP-OCRv6_small_rec_safetensors` `@ fe049fb103f57443fe8840c54ed06b702f3c1de5` | `model.safetensors` 21,204,736 + charset/decode config `inference.yml` 150,579 | Apache-2.0 — same method |
| Accuracy upgrade tier (promotion rule in E-R1) | `PaddlePaddle/PP-OCRv6_medium_det_safetensors` `@ 4236c2b61741a259c091fd879dcc4edc339e916c` and `PP-OCRv6_medium_rec_safetensors` `@ 024cad6a831de75c2c3c26e711ba8c4a82ccd24b` | det 88,020,412 · rec 76,741,720 | Apache-2.0 — same method |
| Handwriting head (R6, **conditional on E-R2**) | `microsoft/trocr-base-handwritten` `@ eaacaf452b06415df8f10bb6fad3a4c11e609406` | `model.safetensors` 1,333,384,464 (334 M params F32; ≈335 MB at int8 export — arithmetic, unmeasured) | MIT — HF `license` tag set by Microsoft, read 2026-07-23 |

Binding notes on these pins:

- **Safetensors payloads only.** Every pin above ships `model.safetensors`;
  the export tool reads it with a minimal header-plus-buffer parser (NumPy
  side) — **no Paddle, no PyTorch, no ONNX, and never a pickle
  (`pytorch_model.bin`, `.pdiparams`) at any stage.**
- **Tier rule.** The shipped pack is the **small** tier (det+rec ≈ 31 MB
  F32). E-R1 scores the medium tier on the same fixtures; promote to medium
  only if it corrects fixture lines small reads wrong. The C port is
  shape-generic — tier promotion is a pack swap, not a code change.
- **One recognizer, one scope.** The v6 recognizer is a single unified model
  whose upstream claim spans English + 46 Latin-script languages — no
  separate en/latin models, and the diacritics fixture ("Poličar") falls
  inside its charset. Claim only what the fixtures measure (Open questions).
- **The TrOCR variant is fixed at `base`.** It is the only handwritten TrOCR
  with upstream license metadata (checked 2026-07-23: `small` and `large`
  carry none, which places them outside decision 8). Do not substitute them,
  even for size. Its encoder is the [vision.c](../src/vision.c) block
  pattern; its decoder is a miniature of the engine's decode loop.
- **Preprocessing is data, not convention.** Resize/normalization/charset
  come from each pinned repo's `inference.yml` / `preprocessor_config.json`
  at the pinned revision; the export tool embeds them in the pack manifest.
  Do not hardcode input conventions from older PP-OCR generations.
- **Export:** `tools/export_ocr_pack.py`, offline, one-time, producing the
  flat pack + SHA-manifest. Packs ship opt-in and manifest-pinned like PDFium
  and the Bonsai mmproj; outward publishing waits for owner confirmation.
- **Credit:** when the pack ships, the PaddleOCR/PP-OCRv6 team (and, if R6
  ships, the Microsoft TrOCR team) are credited at the **top** of
  README/model card, per the standing rule.

Not in this card: `image.look` (thin wrapper over the vision backends for
"describe/what is this" questions) and `image.detect` (object counting).
Each gets its own small card when needed. For `image.detect`, the
license-cleared starting candidates are **RT-DETR**
(`lyuwenyu/RT-DETR`, Apache-2.0, GitHub LICENSE verified 2026-07-23) and
**D-FINE** (`Peterande/D-FINE`, Apache-2.0, GitHub LICENSE verified
2026-07-23); decision 8 applies to the exact weight files when that card is
written.

## Experiments — run these before the C

**E-R1 — export + numeric validation. ✅ RUN 2026-07-23; passed. Results:
[docs/regressions/reader/report.md](regressions/reader/report.md).** All five
deliverables measured: (a) pack frozen; (b) NumPy port matches PaddleOCR 3.7.0
line-for-line on clean fixtures (tensor max-abs-diff det 9.7e-05 / rec 2.1e-05,
argmax 100 %); (c) T_ACCEPT 0.84 (Youden J 0.923), T_DECIDE 0.99 — genuine
errors all ≤ 0.834, correct all ≥ 0.96; (d) keep 768 render cap; (e) ship small
tier (medium only helps heavily-degraded pages, where tier-2 is the remedy). The
original design text is preserved below for provenance.

The E-V1 pattern. Export the pinned det+rec to the
flat pack; re-implement both forward passes in NumPy; run **PaddleOCR
≥ 3.7.0** (the PP-OCRv6 release) as the reference implementation on ≥20
fixture images — printed forms, receipts, a dense page, and the JSS page
whose diacritics broke 1-bit Bonsai. Deliverables: (a) pack format frozen;
(b) NumPy output matches the reference line-for-line within stated
tolerance — if it cannot, the C port is resized or killed *here*;
(c) `T_ACCEPT`/`T_DECIDE` calibrated from the confidence histograms of
correct vs. incorrect lines; (d) accuracy at 768 px vs. 1536 px long edge,
which **decides the render-cap question** (below) with a measurement instead
of an opinion; (e) the small-vs-medium tier decision per the promotion rule
in the pins table, recorded with the fixture lines that decided it.

**E-R2 — strong-reader-on-crop (~0.5 day; Bonsai only; no 24 GB load).**
~10 photographed handwritten field crops (names, DOBs; block capitals and
cursive). Measure per-crop seconds and correctness through **Bonsai + mmproj**
(decision 9 excludes the 24 GB Qwen tower; Ornith is text-only and cannot read
crops). Decides **whether R6 is needed at all**: if Bonsai reads handwritten
fields acceptably in acceptable per-crop time, R6 stays a throughput
optimization; if it does not, R6 (the TrOCR handwriting head) is promoted from
conditional to required. Currently **unmeasured** — Bonsai's ~14 s figure is
full-page, and per-crop cost should scale roughly with crop area (unverified
until run). Because this drops the 24 GB model, the earlier machine-safety
blocker on E-R2 is lifted; it still runs real Bonsai inference via
llama-server, so watch memory/thermals per the standing rule.

**E-R3 — the motto scenario (after R4).** A 20-file fixture folder; job:
"which files mention NAME". Verify: first run reads every file once; second
run is 100 % cache hits; a planted low-confidence file lands
`review_required`, not moved. This is the end-to-end acceptance for the
whole card.

## Build order (each step lands against the frozen contract above)

| Step | What | Gate / definition of done |
|---|---|---|
| **R1** | `tools/export_ocr_pack.py` + **E-R1** | ✅ **DONE 2026-07-23.** Pack frozen ([tools/ocr_pack.py](../tools/ocr_pack.py)); NumPy port ([tools/ocr_ref.py](../tools/ocr_ref.py)) matches paddle; thresholds recorded ([report](regressions/reader/report.md), [results](regressions/reader/e_r1_results.json)) |
| **R2** | `samosa-ocr detect` — the pinned PP-OCRv6 det forward pass in C (`kernels.h` GEMM, `stb_image.h` input; architecture as found in the pinned weights, not assumed from older PP-OCR generations) | ✅ **DONE 2026-07-23.** Forward exact vs NumPy golden (prob map max-abs-diff **1.1e-05**); boxes same count + mean IoU **0.92–0.94** (connected-components; minAreaRect/clipper parity a documented refinement); rlimits + file discipline in place; `make ocr-test` green ([report](regressions/reader/r2r3-c-port.md)) |
| **R3** | `recognize` + `read` + confidences + `--emit-crops` | ✅ **DONE 2026-07-23.** Rec argmax **100 %** vs golden, CTC text exact (`Poličar 2019`); `read`/`recognize`/`--emit-crops` implement the reader-v0 JSON contract; `make ocr-test` green |
| **R4** | Gateway `doc.read`: tiers 0+1, cache, `detail` views, Jobs reasons | 🟡 **PARTIAL 2026-07-23.** Content-addressed read **cache** built + tested ([src/read_cache.h](../src/read_cache.h), `make read-cache-test`: SHA-256 keying, moved-file hit, fingerprint/contract-miss guard, 0600/0700 perms, no companion files). **Pending:** the gateway `doc_read` handler (tier-0 PDF text-layer / tier-1 image OCR orchestration + `detail` reshaping), Jobs `review_required` states, and the E-R3 end-to-end acceptance |
| **R5** | Tier-2 escalation via **Bonsai + mmproj** + interlock/priority wiring | Gated on E-R2 (Bonsai per-crop cost / whether R6 is needed); the 24 GB Qwen tower is not used (decision 9); guarded live run on the reference machine; evidence committed |
| **R6** | Handwriting recognizer head (`reader:"rec_hand"` inside `samosa-ocr`) | **Only if E-R2 demands it.** Same pack/export/validation pattern as R1–R3 |
| **R7** | Printed/handwritten classifier head (~1 MB) enabling `script:"handwritten"` | Optional polish; only if "find handwriting" jobs prove common |

## Open questions

- **Render resolution.** ✅ **Answered by E-R1(d) 2026-07-23: keep 768.** Mean
  char accuracy 768 px = 0.9957 vs 1536 px = 0.9918 (no gain — slightly worse).
  The det model caps its input long edge at 960 internally, so a 1536 render is
  downscaled for detection anyway and cubic-upscaling small text adds no real
  information. No `samosa-extract` contract change for the OCR path.
- **Annotated printed pages.** Tier 0 takes a page's text layer and never
  looks at the pixels, so handwritten margin notes on an otherwise clean
  printed PDF are missed in v1. Documented gap; a v2 could OCR only regions
  outside text-layer boxes. Do not silently promise this works.
- **Language scope.** The pinned v6 recognizer spans English + 46
  Latin-script languages *by upstream claim*; v1 **claims** only what the
  fixture set measures — English printed text plus Latin diacritics — and
  (via tier 2/R6) English handwriting. Say so wherever user-facing; do not
  advertise the wider coverage without fixtures for it.
- **`script` fidelity.** Until R7, "uncertain" is honest and "handwritten" is
  banned from output. "Move files with handwriting" is served in the interim
  by `lines_uncertain > 0` — document that it also catches blur and stamps.
