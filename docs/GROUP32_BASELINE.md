# Group-32 local baseline

Recorded 2026-07-14 before fail-fast regression work. This artifact is a local
experimental baseline, not a release candidate and not approved for upload.

## Provenance and format

- Upstream repository: `Qwen/Qwen3.6-35B-A3B`
- Upstream revision: `995ad96eacd98c81ed38be0c5b274b04031597b0`
- Local source commit before the uncommitted stabilization work:
  `39d7c054f8d34a5f47ca135a1f77f163a6c03382`
- Expert format: `groupwise-symmetric-q4-v1`, group size 32
- Expert entries: 10,496 (10,240 regular plus 256 MTP)
- `experts.bin`: 20,942,159,872 bytes
- `resident.safetensors`: 3,015,056,192 bytes, resident row-q8 payloads

## Fingerprints

- `manifest.json`:
  `12ad73a9457e5d88b7cd4b00cae4a5c7ccb9031aa10d1111b80932d115f224d4`
- `resident.safetensors`:
  `52ff706830df2defaca591813810a8d19e1ba9b31d9b2d27b6ecf593b3a91627`
- `config.json`:
  `93a4693fa9d8392fbfccd4b3c9873f4bfdcb14fdede978b123d07d19675efe99`
- `generation_config.json`:
  `e70c136c1b78ddc1fb0905bac8e733a4dc448d4f852a5dd75143fffc70be550e`

The manifest hash identifies the physical expert layout and all per-expert
SHA-256 digests. The converter validated every blob against those digests
before and after final publication. A second monolithic 20.94 GB hash was
deliberately not performed because it would add a redundant full-store read.

## Pre-gate observations

- One-token loader smoke: returned `OK`; peak RSS 3.88 GB; expert bytes read
  4.05 GB.
- Direct unsigned-loop control: correct, natural model stop after 193 tokens;
  7.27 tok/s; peak RSS 3.85 GB; 68.38 GB expert bytes read.
- General-thinking arithmetic control: correct reasoning before closure, but
  forced at the 256-token thinking budget and produced an incomplete final
  answer; 6.54 tok/s; peak RSS 3.85 GB; 114.94 GB expert bytes read.

These observations are directional only. The forced-close cell was later shown
to be an invalid quality gate: all six upstream FP8 controls exceeded its
256-token thinking budget, and the recalibrated local group-32 run closed
naturally, answered correctly, and stopped after 933 total tokens with a 1,024
thinking budget. See `UPSTREAM_CONTROL_2026-07-14.md`. Broad release stability
remains unproven, but this arithmetic cell no longer blocks it.
