# Samosa Reader — `doc.read` tools contract + build card

**Status: design only. Nothing in this card is built, and no experiment has
run.** Every model size, speed, accuracy figure, and license named below is an
upstream report — marked *unverified* — until it is pinned and measured on the
reference machine. Program bar per [ISSUE_TASKS.md](ISSUE_TASKS.md): acceptance
is measured, a negative result is a result, "should work" is not a status.

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
an expensive strong reader. Cost concentrates exactly where the hard pixels
are. VLM-class generality at 16 GB is manufactured, not bought.

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
- **Tier 2 — strong reader (expensive, bounded).** Lines with
  `conf < T_ACCEPT` are re-read from their crops by the strong reader —
  **v1: a crop-sized request to the active vision backend** (Bonsai mmproj or
  the Qwen tower; per-crop cost unmeasured until E-R2). The tier-2 result
  replaces the text, keeps `reader:"vlm_crop"`, and the line is labeled
  `script:"uncertain"` — low printed-OCR confidence means handwriting *or*
  blur *or* a stamp; v1 does not pretend to know which.

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

## Model packs and licensing

All figures upstream-reported, **unverified** until pinned:

- **Text detector:** PP-OCR mobile det (DBNet-family, ~5 MB). **Recognizer:**
  PP-OCR mobile rec (~10–16 MB + charset). PaddleOCR publishes under
  Apache-2.0 — **verify the license of the exact model files at pin time**;
  house precedent is PyMuPDF's rejection for AGPL (ISSUE_TASKS.md conflict 1).
  Upstream det input convention is ~960 px long edge; actual working
  resolution is decided by measurement in E-R1, not by convention.
- **Handwriting head (R6, conditional):** TrOCR-small-handwritten class,
  ~62 M params (~65 MB int8), reported CER ≈4–6 % on IAM English cursive —
  reported, unverified. MIT-licensed per upstream — verify. Its encoder is the
  [vision.c](../src/vision.c) block pattern; its decoder is a miniature of the
  engine's decode loop. Built **only if E-R2 fails** (see below).
- **Export:** `tools/export_ocr_pack.py`, offline, one-time, producing the
  flat pack + SHA-manifest. Packs ship opt-in and manifest-pinned like PDFium
  and the Bonsai mmproj; outward publishing waits for owner confirmation.
- **Credit:** when the pack ships, the PaddleOCR (and if used, TrOCR) teams
  are credited at the **top** of README/model card, per the standing rule.

Not in this card: `image.look` (thin wrapper over the vision backends for
"describe/what is this" questions) and `image.detect` (object
counting). Each gets its own small card when needed. One warning recorded now
so it is not rediscovered late: **Ultralytics YOLO models are AGPL-3.0** — the
same conflict that disqualified PyMuPDF. If `image.detect` is ever built,
start from an Apache-licensed detector family (e.g. RT-DETR / D-FINE) and
verify at pin time.

## Experiments — run these before the C

**E-R1 — export + numeric validation (RUN THIS FIRST; ~1 day; pure Python,
no C, no 24 GB model).** The E-V1 pattern. Export det+rec to the flat pack;
re-implement both forward passes in NumPy; run PaddleOCR upstream as the
reference on ≥20 fixture images — printed forms, receipts, a dense page, and
the JSS page whose diacritics broke 1-bit Bonsai. Deliverables: (a) pack
format frozen; (b) NumPy output matches the reference line-for-line within
stated tolerance — if it cannot, the C port is resized or killed *here*;
(c) `T_ACCEPT`/`T_DECIDE` calibrated from the confidence histograms of
correct vs. incorrect lines; (d) accuracy at 768 px vs. 1536 px long edge,
which **decides the render-cap question** (below) with a measurement instead
of an opinion.

**E-R2 — strong-reader-on-crop (~0.5 day; real backends; machine-safety
rules apply).** ~10 photographed handwritten field crops (names, DOBs; block
capitals and cursive). Measure per-crop seconds and correctness through
(a) Bonsai + mmproj and (b) the Qwen tower. Decides which backend is tier-2
v1 — and whether R6 is needed at all: if neither reads handwritten fields
acceptably, R6 is promoted from conditional to required; if the tower reads
them in acceptable per-crop time, R6 stays a throughput optimization.
Currently **unmeasured** — the 8-min figure is full-page, and cost should
scale roughly with crop area (unverified until run).

**E-R3 — the motto scenario (after R4).** A 20-file fixture folder; job:
"which files mention NAME". Verify: first run reads every file once; second
run is 100 % cache hits; a planted low-confidence file lands
`review_required`, not moved. This is the end-to-end acceptance for the
whole card.

## Build order (each step lands against the frozen contract above)

| Step | What | Gate / definition of done |
|---|---|---|
| **R1** | `tools/export_ocr_pack.py` + **E-R1** | E-R1 deliverables committed under `docs/regressions/reader/`; thresholds recorded |
| **R2** | `samosa-ocr detect` — DBNet forward pass in C (`kernels.h` GEMM, `stb_image.h` input) | Boxes match the E-R1 NumPy reference on all fixtures within stated tolerance; SIDECAR_CONTRACT limits in place; `make ocr-test` (own target, like `jobs-test`) green offline |
| **R3** | `recognize` + `read` + confidences + `--emit-crops` | Line texts match the E-R1 reference; deterministic across runs; `make ocr-test` green |
| **R4** | Gateway `doc.read`: tiers 0+1, cache, `detail` views, Jobs reasons | Offline tests against fixtures; the no-jobs/no-tool path stays byte-identical to today (the standing #3/#4 gate); E-R3 passes |
| **R5** | Tier-2 escalation via vision backend + interlock/priority wiring | Gated on E-R2's backend choice; guarded live run on the reference machine; evidence committed |
| **R6** | Handwriting recognizer head (`reader:"rec_hand"` inside `samosa-ocr`) | **Only if E-R2 demands it.** Same pack/export/validation pattern as R1–R3 |
| **R7** | Printed/handwritten classifier head (~1 MB) enabling `script:"handwritten"` | Optional polish; only if "find handwriting" jobs prove common |

## Open questions

- **Render resolution.** `samosa-extract --render-ppm` caps at 768 px long
  edge; TASKS_JOBS already records that small text needs full resolution.
  E-R1(d) measures the OCR accuracy delta at 1536 px; raising the sidecar's
  bounded cap for the OCR path is a `samosa-extract` contract change with
  memory implications — owner-visible, decided on the E-R1 number.
- **Annotated printed pages.** Tier 0 takes a page's text layer and never
  looks at the pixels, so handwritten margin notes on an otherwise clean
  printed PDF are missed in v1. Documented gap; a v2 could OCR only regions
  outside text-layer boxes. Do not silently promise this works.
- **Language scope.** v1 is Latin-script printed text + (via tier 2/R6)
  English handwriting. Say so wherever user-facing. Multilingual packs exist
  upstream but each adds a charset and a pack; out of scope until asked for.
- **`script` fidelity.** Until R7, "uncertain" is honest and "handwritten" is
  banned from output. "Move files with handwriting" is served in the interim
  by `lines_uncertain > 0` — document that it also catches blur and stamps.
