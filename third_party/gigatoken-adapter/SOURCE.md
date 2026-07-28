# Samosa Gigatoken adapter

This directory contains a minimized, local-only adapter derived from
Gigatoken v0.10.0 at commit
`34a1599f0c0ae7d7cd0d1c530e6522320158b360`.

The upstream project is MIT licensed; its license is retained in
`vendor/gigatoken/LICENSE`. The vendored Rust source is limited to the BPE
tokenizer, pretokenizers, HF `tokenizer.json` and Kimi `tiktoken.model` loaders,
and the small input types needed by those modules. Python/PyO3 bindings, NumPy, Hub/network loaders,
Parquet/Arrow, compression, training, benchmark, and file-source modules are
not present in the adapter tree.

The adapter accepts one local tokenizer file as an argv argument and serves a
versioned `GTK1` framed protocol on stdin/stdout. It accepts only bounded UTF-8
frames, returns exact token IDs, and treats document frames as ordinary text so
control-token spellings in source documents are not promoted to trusted prompt
controls. The gateway must supervise it, enforce cancellation, and compare the
tokenizer/template fingerprints before using direct token-ID ingestion.

The source snapshot was copied from `/Users/deepanwadhwa/Documents/giga/gigatoken`
by the Samosa build on 2026-07-28. Any future source update must change the
commit above, regenerate this snapshot, rerun differential parity tests, and
repeat the dependency/license audit.
