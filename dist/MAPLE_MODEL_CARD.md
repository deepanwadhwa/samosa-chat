---
license: mit
base_model: deepgrove/maple-preview-2bit-mlx
pipeline_tag: text-generation
tags:
  - maple
  - moe
  - apple-silicon
  - ssd-streaming
  - local-inference
---

# Samosa Chat Maple 2-bit SSD streaming pack

This repository contains the production artifact layout used by Samosa Chat's
native Maple runtime. It is a storage-only repack of the MIT-licensed
[`deepgrove/maple-preview-2bit-mlx`](https://huggingface.co/deepgrove/maple-preview-2bit-mlx)
checkpoint pinned at revision
`361db5da5e74ff6fcdd852d478e1f266ce11013a`.

The model's numerical weights are unchanged. Expert tensors are stored in
fixed-size aligned records in `maple-experts.bin`, allowing Samosa to read only
the routed experts from SSD. Non-expert tensors are stored in
`maple-resident.safetensors`. `maple-manifest.json` describes and validates the
packed layout.

These files are intended for the bundled `samosa-maple` runtime. They are not a
drop-in MLX checkpoint because the original expert shards have been replaced by
Samosa's streaming container.

## Runtime files

- `maple-experts.bin`: aligned expert records streamed on demand from SSD
- `maple-resident.safetensors`: non-expert tensors retained by the runtime
- `maple-manifest.json`: strict layout metadata
- `config.json` and tokenizer files: model configuration and prompt encoding

Samosa verifies every artifact's byte length and SHA-256 digest before atomic
installation.
