# Storage migration — 2026-07-14

This ledger records the cleanup that separated the Samosa Chat product from
the original colibrì development checkout. The migration was performed only
after explicit approval.

## Resulting layout

- `/Users/deepanwadhwa/Documents/samosa-chat` — current product repository
- `/Users/deepanwadhwa/Documents/samosa-models` — unpublished model artifacts
- `/Users/deepanwadhwa/Documents/samosa-lab` — retained experiments and
  benchmark evidence from the original workspace

All three moves stayed on the same APFS volume. They were directory-entry
renames rather than copies of the large model files.

## Preserved model

The local group-32 development artifact and tokenizer were preserved:

| file | bytes | SHA-256 before move |
|---|---:|---|
| `qwen36_group32_i8/experts.bin` | 20,942,159,872 | `00d64d44c39496e5ab5691f4cb27b67e27f3a10efd1f7c54024a9b43b130dbba` |
| `qwen36_group32_i8/resident.safetensors` | 3,015,056,192 | `52ff706830df2defaca591813810a8d19e1ba9b31d9b2d27b6ecf593b3a91627` |
| `qwen36_group32_i8/manifest.json` | 1,908,179 | `12ad73a9457e5d88b7cd4b00cae4a5c7ccb9031aa10d1111b80932d115f224d4` |
| `tokenizer_qwen36.json` | 28,142,621 | `6d56a5c681da15d38fb9f883016f86fa0638176e3f748a0acf5c7ba02725679b` |

The inode numbers and byte sizes were unchanged after the same-volume move.
No second full read of the 24 GB artifact was performed solely for validation,
avoiding unnecessary SSD traffic.

## Preserved lab evidence

Approximately 1.1 GiB was retained under `samosa-lab`, including:

- quantization and refinement notes;
- groupwise/refinable-q4 source and tools;
- route traces and route-cache analysis;
- teacher-forcing, precision-sweep, and performance benchmark captures;
- small fixtures and regression utilities not yet ported into the product
  repository.

The original upstream Git metadata was intentionally not retained. The
colibrì source remains publicly available from its upstream repository; local
uncommitted experiment files remain in `samosa-lab` as ordinary files.

## Removed data

- uv's re-downloadable package cache (roughly 12–13 GiB physically reclaimed)
- `/Users/deepanwadhwa/samosa_upload`, after its published-bundle checksum file
  verified every staged file
- the published legacy whole-row-q4 local model; it remains re-downloadable
  from the Samosa Chat Hugging Face repository
- the resident-int8 experiment
- the old outer `.venv`
- the outer colibrì `.git`, including 37.21 GiB of abandoned `tmp_pack_*`
  garbage and 23.48 GiB of large pack data

The large files in `samosa_upload` were hard links to the legacy model, so the
staging directory alone did not represent another physical 17 GiB allocation.
Both link locations were removed before that storage was reclaimed.

## Validation

- Samosa Chat Git status: clean, `main` synchronized with `origin/main` at
  `6d8852c`
- repository connectivity check: passed (three harmless dangling blobs were
  reported)
- group-32 artifact: 22 GiB on disk including resident weights and metadata
- retained lab: 1.1 GiB
- free space: increased from approximately 13 GiB to 95 GiB

