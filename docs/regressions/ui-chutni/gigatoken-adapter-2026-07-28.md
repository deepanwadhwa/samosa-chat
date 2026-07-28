# Gigatoken adapter handoff — 2026-07-28

The local source at `/Users/deepanwadhwa/Documents/giga/gigatoken` was pinned
to commit `34a1599f0c0ae7d7cd0d1c530e6522320158b360` (MIT). Samosa now has a
minimized vendored native adapter under `third_party/gigatoken-adapter`.

Evidence:

- `make gigatoken-adapter` builds a release binary offline from the vendored
  lockfile; the build script uses Cargo's `--offline` mode and the adapter's
  SHA-256 implementation is local, so no new hashing crate or registry access
  is needed;
- GTK1/v2 now carries a request ID, pinned Gigatoken commit, model identity,
  tokenizer SHA-256, vocabulary size, policy fingerprint, source SHA-256,
  item/byte/token ceilings, and per-operation bounded payloads;
- `health`, `encode_batch`, typed-segment `encode_prompt`, `cancel`, and
  `shutdown` are implemented. Batch results contain per-document lengths and
  one flat little-endian `u32` token buffer. Errors carry stable numeric codes,
  retryability, request ID, and bounded detail;
- `src/samosa_gigatoken.h` provides the gateway-owned child supervisor with
  low-priority spawn, bounded framed I/O, response correlation, timeout kill,
  cancellation writes, and graceful shutdown. `make test-gigatoken-supervisor`
  exercises its health/cancel/shutdown boundary;
- `make test-gigatoken-adapter` passes the frozen Qwen differential corpus
  against a `src/tok.h` oracle, including prose, Unicode/CJK/emoji, code,
  whitespace, empty/long input, and printed control-token text under the
  special-token-disabled document policy. It also covers fingerprint rejection,
  invalid UTF-8, per-document lengths, trusted/untrusted prompt segments, and
  shutdown;
- the vendored dependency graph excludes PyO3, NumPy, Hugging Face Hub, ureq,
  Parquet, Arrow, and the upstream network/training paths;
- the adapter uses bounded GTK1 little-endian frames (8 MiB maximum), local
  files only, and no model-name or URL lookup;
- Kimi special-token IDs are read from the adjacent
  `tokenizer_config.json`; unnamed reserved IDs are synthesized as the Kimi
  tokenizer does.

The direct-ID adoption gate is still deliberately closed: the existing text
tokenization path remains authoritative until the parity corpus is expanded to
the complete frozen prompt-template/control-token set, repeated serial and
parallel runs, fuzz cases, binary/network inspection, and measured end-to-end
admission/throughput evidence. The C supervisor is currently a tested reusable
gateway boundary; wiring it into Chutni's T4.3/T4.4 ingestion worker is the
next integration step. No browser route accepts raw token IDs.
