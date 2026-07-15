# Issue #3 — Vision capabilities

Read [ISSUE_TASKS.md](ISSUE_TASKS.md) first for shared ground truth and the
accuracy rules this program runs under.

## The finding that reframes this issue

**The complete, quantized Qwen3.6 vision tower already ships inside
`resident.safetensors`. Every user who ran the one-line installer already
downloaded it. It is sitting inert on their disk right now.**

Verified 2026-07-15 against `~/.samosa/current/model/resident.safetensors`
(inode-identical, confirmed by `ls -li`, to the published source model at
`~/Documents/samosa-models/qwen36_group32_i8`, which is the artifact uploaded
to Hugging Face):

- **444 `model.visual.*` tensors, 0.454 GB** of the 3.015 GB resident file.
- All **27 blocks** (matching `vision_config.depth: 27`), plus `patch_embed`,
  `merger`, and `pos_embed`.
- The tensor count reconciles exactly, which is why this is stated as fact and
  not as a guess. By filename suffix the 444 split into **167 `.weight` + 111
  `.qs` + 166 `.bias`**, and each group accounts for itself:
  - **167 `.weight`** = **111 quantized** (27 blocks × 4 matrices — `attn.qkv`,
    `attn.proj`, `mlp.linear_fc1`, `mlp.linear_fc2` — plus `merger.linear_fc1`,
    `merger.linear_fc2`, `pos_embed`) + **56 unquantized** (27 × 2 LayerNorm
    weights + `merger.norm.weight` + `patch_embed.proj.weight`).
  - **111 `.qs`** = exactly one F32 scale tensor per quantized weight. Matches
    the 111 above, one for one.
  - **166 `.bias`** = 27 × 6 + merger's 3 + `patch_embed.proj.bias`.
  - 167 + 111 + 166 = **444.** Nothing is missing and nothing is duplicated.

### Why it shipped

[convert_qwen36.py:508-510](../tools/convert_qwen36.py#L508-L510) reads:

```python
# Skip vision tower and indexers
if any(k in name for k in ["vision", "indexer", "indexers_proj", "eh_proj", "shared_head"]):
    return "skip"
```

The tensors are named `model.visual.*`. **`"vision" in "model.visual.blocks.0.attn.qkv.weight"` is `False`** — "visual" is not "vision". The filter never
matched. The vision tower fell through to the generic quantization path and was
converted, quantized, packaged, and published alongside the language model.

The code comment states an intent that the code does not implement. Treat the
comment as wrong, not the artifact.

### Why this matters

The weights are stored in **exactly the format the engine's existing resident
quant loader already consumes.** [qwen36b.c:1134-1152](../src/qwen36b.c#L1134-L1152)
resolves `<name>` as U8 packed data and `<name>.qs` as F32 scales, and sets
`t.fmt = (nbytes == O*I) ? 1 /* int8 */ : 2 /* int4 */`, allocating `t.s = falloc(O)`
— one scale per output row.

Measured against that loader:

| Tensor | dtype | bytes | = O × I | fmt | scales |
|---|---|---|---|---|---|
| `blocks.0.attn.qkv.weight` | U8 | 3,981,312 | 3456 × 1152 | **int8** | 3456 |
| `blocks.0.mlp.linear_fc1.weight` | U8 | 4,958,208 | 4304 × 1152 | **int8** | 4304 |
| `merger.linear_fc1.weight` | U8 | 21,233,664 | 4608 × 4608 | **int8** | 4608 |
| `merger.linear_fc2.weight` | U8 | 9,437,184 | 2048 × 4608 | **int8** | 2048 |
| `pos_embed.weight` | U8 | 2,654,208 | 2304 × 1152 | **int8** | 2304 |
| `patch_embed.proj.weight` | F32 | 7,077,888 | [1152,3,2,16,16] | 0 (F32) | — |

`bytes == O × I` in every quantized case, so every one loads as `fmt = 1`,
whole-row int8, through `matmul_qt` **with zero new kernel work**.

**Note precisely what scheme this is, because it is not the one the release is
named for.** The vision tower sits in `resident.safetensors` and therefore got
the generic **whole-row int8** treatment — one F32 scale per output row. It did
**not** get the group-32 symmetric q4 treatment that the *experts* get
(`groupwise-symmetric-q4-v1`, `group_size: 32`). See
[ISSUE_TASKS.md](ISSUE_TASKS.md) for both schemes.

That contrast matters for E-V1 and its fallback: group-wise scales track local
dynamic range far better than a single scale per row, so **re-quantizing the
tower at group-32 q4 would likely be both smaller (~0.25 GB vs 0.454 GB) and
more accurate than what ships today.** If E-V1 finds the row-wise int8 tower
degraded, "re-quantize at group-32" is a more attractive fallback than "ship
BF16" — and the group-32 machinery already exists in the converter.

`merger.linear_fc2` outputs **2048**, which is exactly `text_config.hidden_size`.
The projector already lands in the language model's embedding space.

**So the accurate framing of issue #3 is not "build colibrì for vision".** No
re-download, no re-quantization, no new matmul kernel. The missing work is the
forward pass, the image decoder, and the embedding splice — and one experiment
that decides whether the shipped weights are numerically usable at all.

## Configuration, verified

From the shipped `config.json` and `tokenizer_qwen36.json`:

```
vision_config: depth 27, hidden_size 1152, num_heads 16 (head_dim 72),
               intermediate_size 4304, patch_size 16, in_channels 3,
               temporal_patch_size 2, spatial_merge_size 2,
               num_position_embeddings 2304 (= 48×48), out_hidden_size 2048,
               hidden_act "gelu_pytorch_tanh",
               deepstack_visual_indexes []        <-- EMPTY
media tokens:  image 248056, video 248057, vision_start 248053, vision_end 248054
architecture:  Qwen3_5MoeForConditionalGeneration
```

`deepstack_visual_indexes` being empty means **no DeepStack multi-level feature
injection is configured** — one simplification you get for free.

The tokenizer carries all 11 media tokens as atomic added-tokens
(`<|vision_start|>`, `<|image_pad|>`, …), so the language side already
tokenizes an image prompt correctly.

## What is actually missing

Each of these is a real gap, verified absent from the engine today.

1. **Image decoding.** JPEG/PNG → RGB. Nothing in the engine decodes images.
   **Do not use ImageIO/CoreGraphics** — it would make vision macOS-only and
   collide with issues #1 and #2 exactly the way `textutil` does in #5. Vendor a
   portable single-header decoder (`stb_image.h`, public domain), consistent with
   how the repo already vendors `json.h`, `tok.h`, `st.h`.
2. **LayerNorm.** `blocks.N.norm1` has **both `.weight` and `.bias`** (F32,
   [1152]) — that is LayerNorm, not RMSNorm. The language model uses RMSNorm and
   the engine implements only that. LayerNorm must be written.
3. **GELU-tanh.** `hidden_act: gelu_pytorch_tanh`. The engine has `silu`,
   `sigmoid`, `softplus` ([qwen36b.c:71-74](../src/qwen36b.c#L71-L74)) and no
   GELU. Must match PyTorch's tanh approximation exactly, not the erf form.
4. **Patch embedding.** A conv3d over [1152, 3, 2, 16, 16] — in_channels 3,
   temporal_patch_size 2, 16×16 spatial. For a single image the temporal
   dimension is replicated; confirm against the reference rather than assuming.
5. **Position embedding interpolation.** `pos_embed` is a fixed 48×48 = 2304
   grid; Qwen VL uses dynamic resolution, so it must be interpolated to the
   actual patch grid. Match the reference's interpolation mode exactly.
6. **The merger.** Note the ordering implied by the shapes: `merger.norm` is
   over **1152**, not 4608 — so normalize per patch *first*, then concatenate the
   2×2 spatial group into 4608, then `fc1` (4608→4608) → GELU → `fc2` (4608→2048).
7. **Embedding splice.** Prefill currently goes token ids → `embed_tokens`
   lookup. It needs a path accepting precomputed embeddings at `<|image_pad|>`
   positions. This is the most invasive change to existing code.
8. **`preprocessor_config.json` is not shipped.** `image_mean`, `image_std`,
   `min_pixels`, `max_pixels` are all unknown locally and must be recovered from
   upstream. Getting normalization wrong produces plausible-looking garbage
   rather than an error — this is a silent-failure risk, so pin it early.

## Experiments

### E-V1 — Are the shipped weights numerically usable?  ~1 day  **RUN THIS FIRST**

Everything else in this issue is contingent on this. Pure Python, no C, no new
downloads beyond the upstream reference.

The vision tower was quantized by a converter that **did not intend to quantize
it**, so it received the generic whole-row int8 treatment — one scale per output
row, no group-32, no vision-specific calibration. ViTs are more
quantization-sensitive than decoder LMs, and `pos_embed` is the classic
casualty: a 2304×1152 table given one scale per position is a plausible way to
lose fine positional structure. Nobody has ever run a single image through these
weights.

**Method.** Dequantize the shipped int8 rows (`w * scale[row]`) and compare to
the BF16 reference from `Qwen/Qwen3.6-35B-A3B`:

1. Per-tensor: cosine similarity and max absolute relative error, for all 111
   quantized tensors. Report the distribution, not just the mean — one destroyed
   tensor is enough to break the tower.
2. End-to-end in PyTorch: run the reference ViT twice on ≥20 images (natural
   photos, a screenshot, a document scan, a chart), once with original BF16
   weights and once with the dequantized-shipped weights. Compare **merger output
   embeddings**: report cosine similarity per image.
3. Then run the *full* model both ways on the same images and compare generated
   text on a fixed seed.

**Acceptance:** per-tensor cosine ≥ 0.99 for all 111; merger-output cosine
≥ 0.99 mean with no image below 0.97; generated text substantively equivalent on
≥ 18/20 images.

**Kill criterion and the fallback.** If the row-wise int8 tower is degraded, say
so plainly and stop — do not tune around it. Cost the fallback in the same
report, and note that it is cheap in every direction: the tower is 0.454 GB
today, so **group-32 q4 would be ~0.25 GB and BF16 ~0.9 GB**.

**Prefer group-32 q4 over BF16 as the fallback.** It is smaller *and* likely
more accurate than what ships now, the converter already implements
`groupwise-symmetric-q4-v1`, and the engine already runs group-32 for every
expert. Include a third arm in this experiment measuring group-32-requantized
weights alongside BF16 and shipped-int8 — it is nearly free once the harness
exists, and it may turn the fallback into the recommendation.

What this experiment decides is whether #3 is "write a forward pass against
weights users already have" or "republish the resident artifact and write a
forward pass" — a difference of days, not months. Either way the issue survives;
the plan changes.

### E-V2 — Reference activation fixtures  ~0.5 day

The C port needs an oracle or it cannot be verified. Dump reference
intermediates as `.npy` for 3 fixed images at 2 resolutions: post-`patch_embed`,
post-block-0, post-block-13, post-block-26, post-`merger`. These become the unit
test fixtures for every task below. Without this, "the ViT works" is unfalsifiable.

Fixtures go in `tests/fixtures/vision/`. Keep them small — 3 images at capped
resolution, not a corpus.

### E-V3 — 1-D RoPE or M-RoPE?  ~0.5 day  **Blocks V3**

Genuinely open, and it changes the engine's attention path if the answer is
M-RoPE.

The engine implements **plain 1-D RoPE**:
[`rope_head(x, pos, theta, head_dim, rotary_dim)`](../src/qwen36b.c#L2173).
Qwen2-VL and 2.5-VL use M-RoPE with (t, h, w) sections. But this model's
`text_config.rope_scaling` is `None` and there is **no `mrope_section` anywhere
in the shipped config** — which is evidence, not proof, that image tokens take
sequential 1-D positions here.

**Method.** Read the upstream `Qwen3_5MoeForConditionalGeneration` modeling code
and find how `position_ids` are constructed for image tokens. Confirm empirically:
run the reference on an image prompt and dump the actual `position_ids` tensor.

**Outcome:** if 1-D, the engine needs no RoPE change and V3 is much cheaper. If
M-RoPE, `rope_head` needs sectioned frequencies and the change touches the
language model's hot path — re-cost V3 before starting it.

### E-V4 — Recover the preprocessor config  ~0.25 day

Fetch `preprocessor_config.json` from upstream. Pin `image_mean`, `image_std`,
`min_pixels`, `max_pixels`, `patch_size`, `merge_size`. Add it to the release
manifest so future installs carry it. Cross-check the values against a reference
`Qwen3VLImageProcessor` run — do not transcribe them by hand.

### E-V5 — What does an image actually cost?  ~1 day  **May resize the whole issue**

This is the experiment most likely to constrain the product, and it can be run
in Python before any C exists.

**Estimated arithmetic** (mine, shown so it can be checked and then replaced by
measurement — these are *estimates*, not results):

For a 1024×1024 image: 64×64 = **4096 patches**, → 4096/4 = **1024 LM tokens**
after the 2×2 merge. Per block, with N=4096, d=1152, ffn=4304:

| Op | FLOPs |
|---|---|
| qkv (N·d·3d·2) | 3.26e10 |
| QKᵀ (N²·d·2) | 3.87e10 |
| A·V (N²·d·2) | 3.87e10 |
| proj (N·d·d·2) | 1.09e10 |
| fc1 + fc2 (2·N·d·ffn·2) | 8.12e10 |
| **per block** | **≈2.02e11** |

× 27 blocks ≈ **5.5 TFLOP for one 1024×1024 image.** At an optimistic 50–100
GFLOP/s for hand-written int8 C on 2 threads, that is **~55–110 seconds of ViT
compute**, *before* the ~43–73 s to prefill the resulting 1024 tokens at
14–24 tok/s.

At 512×512: 1024 patches → **256 LM tokens**, ≈0.98 TFLOP → roughly **10–20 s**
ViT + ~11–18 s prefill. Attention is the term that explodes: it scales as N² and
is 38% of the 1024×1024 cost versus 13% at 512×512.

**A memory finding that must shape the implementation.** At N=4096 a single
attention score matrix is 4096×4096×4B = **67 MB per head**; materializing all
16 heads at once is **1.07 GB** — against a ~4 GiB total footprint budget and a
standing zero-new-swap guardrail. `vision_config` has no `window_size` or
`fullatt_block_indexes`, so all 27 blocks appear to be **full attention**.
Compute attention **one head at a time** (67 MB peak) at minimum; prefer an
online-softmax streaming form (negligible peak). Do not write the naive version
"for now" — it will not fit.

**Method.** Measure real ViT wall-clock and peak RSS at 256×256, 512×512,
768×768, 1024×1024, on the reference machine, at 2 and 4 threads.

**Acceptance:** a published table of resolution → LM tokens → ViT seconds →
prefill seconds → peak RSS. That table *is* the deliverable, and it sets the
default resolution cap.

**The likely honest conclusion, stated in advance so it is not a surprise:**
Samosa should cap image resolution by default (512×512 is the current best
guess) and tell the user the real cost before they commit — the same honesty the
document path already applies to prefill ETA.

**The architectural upside, and it is real:** an image is prefilled once and
lands in the session snapshot, exactly like a document. Every follow-up question
about that image is free. Vision is expensive to *read* and free to *revisit* —
which is the same shape as this architecture's existing advantage, not a new
weakness.

## Tasks

Costed **assuming E-V1 passes.** If it fails, add ~2–3 days for re-quantization
and republication.

### V1 — Image decode and preprocessing  ~2 days

Vendor `stb_image.h` into `src/`. Implement Qwen VL preprocessing to the E-V4
config: resize to a multiple of `patch_size × spatial_merge_size` = **32**,
respect the pixel budget, normalize with the pinned mean/std, emit the patch
grid `(grid_t, grid_h, grid_w)`.

**Acceptance:** decoded+preprocessed tensors match the reference
`Qwen3VLImageProcessor` to within 1e-5 max abs error on the E-V2 fixture images.
Corrupt/truncated JPEG, a 20,000×20,000 image, a 1×1 image, a CMYK JPEG, and a
16-bit PNG each produce a clear error, never a crash and never silent garbage.

### V2 — ViT forward pass in C  ~4–5 days

`src/vision.c` + `src/vision.h`. Load the `model.visual.*` tensors through the
existing `st.h` reader and the existing `qload`/`matmul_qt` int8 path. Implement
LayerNorm, GELU-tanh, the conv3d patch embed, interpolated pos-embed, 27 attention
blocks (head-at-a-time or streaming attention per E-V5), and the merger.

**Acceptance:** every E-V2 fixture matches the reference with cosine ≥ 0.999 at
each dumped stage. Stage-by-stage, not just end-to-end — an end-to-end-only test
cannot localize a bug in a 27-block tower. Peak RSS during a 512×512 forward
stays under the E-V5 measured budget with zero new swap.

### V3 — Embedding splice and prompt assembly  ~2–3 days  **Depends on E-V3**

Extend prefill to accept precomputed embeddings at `<|image_pad|>` positions.
Build the image prompt (`<|vision_start|>` + N × `<|image_pad|>` + `<|vision_end|>`,
N = grid_t·grid_h·grid_w / merge²) in the hardcoded C template at
[qwen36b.c:3440](../src/qwen36b.c#L3440) — there is no template engine, and the
template must match upstream exactly.

**Acceptance:** with **no image attached, output is byte-identical to the current
engine on a fixed seed** — this is the non-negotiable gate, the same one A3.3
sets for tool injection. With an image, greedy output matches the HF reference
on ≥ 8/10 fixture prompts.

### V4 — API and UI  ~2 days

`POST /v1/chat/completions` accepts OpenAI-shaped `image_url` content parts with
`data:` URIs. **Note the existing 4 MiB body cap**
([samosa_http.h:20](../src/samosa_http.h#L20)) — base64 inflates by ~33%, so the
effective image limit is ~3 MB. Either raise the cap deliberately or document the
limit; do not let it fail obscurely. The UI ([assets/app.html](../assets/app.html))
has no file input, no paste handler, and no drag-drop today — all three are new.
Show the measured cost from E-V5 *before* the user commits.

**Acceptance:** attach → answer works end-to-end in the browser against the real
model. Oversized images are rejected with a clear message. The cost estimate
shown is within ±20% of actual.

### V5 — Honest documentation  ~0.5 day

README/model card get a measured vision section: supported formats, the
resolution cap and why, seconds-per-image on the reference machine, and the
quantization provenance — including that the tower was quantized whole-row int8
by a converter that meant to skip it, and what E-V1 measured about that. If E-V1
found degradation and it shipped anyway, that belongs in the model card.

Fix the comment at [convert_qwen36.py:508](../tools/convert_qwen36.py#L508)
regardless of outcome: it currently claims a skip that does not happen.

## Non-goals

- **Video.** `<|video_pad|>` and `temporal_patch_size: 2` exist, but video is
  many images and the per-image cost is already the binding constraint. Not v1.
- **Grounding/box outputs.** `<|box_start|>`, `<|object_ref_start|>` etc. exist
  in the tokenizer. Out of scope until basic VQA is measured.
- **Image generation.** Not this model.
- **DeepStack.** `deepstack_visual_indexes` is empty. Do not implement it.

## Open questions

- **Is the 0.454 GB dead weight worth reclaiming if E-V1 fails?** Users are
  downloading it today for nothing. If vision is not shipping soon, stripping it
  is a ~0.45 GB saving on a 24 GB download — probably not worth a republication
  on its own, but worth folding into the next artifact change.
- **Does linear attention handle image tokens?** 30 of 40 language layers are
  `linear_attention` (DeltaNet), full attention only every 4th. Qwen validated
  this architecture with vision, but Samosa's DeltaNet implementation has only
  ever seen text. E-V2-style stage comparison on the *language* side, with image
  embeddings spliced in, is the check. Flag this to V3.
- **Was the vision tower's `.qs` scale computed over the right axis?** The
  generic converter path assumed the language model's [out, in] row convention.
  It reconciles arithmetically for every tensor (scales count == O in all six
  spot-checks), but `patch_embed`'s 5-D shape is the one that could have been
  flattened unexpectedly — E-V1's per-tensor check will catch it.
