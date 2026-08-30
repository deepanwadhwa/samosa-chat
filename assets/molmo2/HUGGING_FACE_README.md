---
license: apache-2.0
base_model:
  - allenai/Molmo2-4B
base_model_relation: quantized
pipeline_tag: image-text-to-text
tags:
  - molmo
  - mlx
  - vision-language
  - image-to-text
  - video-to-text
  - macos
  - apple-silicon
  - samosa
---

# Samosa Molmo2 4B — native MLX Q4

This repository contains the pinned, selectively quantized Molmo2 4B package
used by [Samosa Chat](https://github.com/deepanwa/samosa-chat) for local image
and video intelligence on Apple Silicon.

It is converted from
[`allenai/Molmo2-4B`](https://huggingface.co/allenai/Molmo2-4B) revision
`042abfa7a38879a376cec03d949eff0aefaa0600`, released by the Allen Institute
for AI under Apache-2.0.

## Important compatibility note

This is a **Samosa native runtime package**, not a drop-in Transformers,
`mlx-vlm`, GGUF, or OpenAI API checkpoint. It is intended for Samosa's bundled
C++20 MLX/Metal and AVFoundation runtime on macOS ARM64. The Samosa inference
path does not use Python, PyTorch, Transformers, vLLM, Conda, or a separate
model server.

## Package

| Property | Value |
| --- | --- |
| Package ID | `molmo2-4b-mlx-q4-v1` |
| Upstream model | `allenai/Molmo2-4B` |
| Upstream revision | `042abfa7a38879a376cec03d949eff0aefaa0600` |
| Quantization | affine Q4, group size 64, for eligible language matrices |
| Retained precision | BF16 vision tower, connector, normalization, and non-eligible tensors |
| Weight bytes | 3,360,724,138 |
| Complete runtime package bytes | 3,372,298,903 |
| Estimated resident bytes | 3,763,377,322 |
| Runtime ABI | `samosa-model-runtime-v1` |
| Supported platform | macOS ARM64 / Apple Silicon |

`manifest.json` pins the processor identity, upstream revision, quantization
contract, byte count, and SHA-256 of every runtime file. Samosa verifies that
manifest before admitting the model.

Qualified manifest SHA-256:

```text
a07230dd952e97969921223328e7812020bb58711728450cdfb2205e3a15b770
```

## Capabilities in Samosa

- image-to-text and visual question answering;
- image counting, pointing, and grounding-coordinate output;
- video-to-text using bounded timestamped AVFoundation frame sampling;
- temporal localization and visual tracking;
- direct visual chat, or on-demand delegation from a selected text model.

Molmo is loaded for one admitted visual turn and unloaded afterward. On the
qualified 16 GiB Apple Silicon machine, Samosa stops the resident text model
before starting Molmo, so the two models do not occupy memory simultaneously.

Molmo2 4B emits text and grounding coordinates. It does **not** generate new
raster images. Samosa can render the grounding coordinates as markers over the
input image.

## Installation

Use a Samosa release that supports package ID `molmo2-4b-mlx-q4-v1`. The Samosa
model catalog downloads the exact manifest-listed files into:

```text
~/.samosa/models/molmo2-4b-mlx-q4-v1/
```

For a manual full-repository download:

```sh
hf download deepanwa/Samosa-Chat-Molmo2-4B-MLX-Q4 \
  --local-dir ~/.samosa/models/molmo2-4b-mlx-q4-v1
```

The extra Hub documentation files are not part of the manifest and are ignored
by the package verifier.

## Qualification

The package was converted with Samosa's native `molmo2-pack` tool and verified
on an Apple M3 with 16 GiB unified memory. The qualification run recorded:

- native FP32-to-Q4 conversion with zero swap growth;
- real PNG inference and native AVFoundation MP4 inference;
- a real 12-chart grounding response in 24.99 seconds;
- approximately 3.95 GB Molmo process footprint during that grounding turn;
- zero swapins and swapouts, no thermal warning, and complete teardown;
- successful restoration of the previously selected Bonsai text model.

These measurements describe the qualified machine and Samosa runtime; they are
not universal performance guarantees.

## Limitations

- Samosa v1 retains at most 16 sampled frames per inference call and uses
  sequential bounded windows for longer videos.
- Sparse video sampling can miss events between frames; Samosa reports covered
  and unprocessed intervals.
- Audio and subtitle transcription are not part of this package.
- Broader BF16-versus-Q4 parity evaluation remains ongoing.
- The package is specifically versioned for Samosa's native runtime contract.

## License and attribution

The upstream Molmo2 4B model is copyright the Allen Institute for AI and
contributors and is licensed under the Apache License, Version 2.0. This
converted package preserves the upstream license and provenance. Samosa is an
independent project and is not affiliated with or endorsed by AllenAI or the
Molmo project.
