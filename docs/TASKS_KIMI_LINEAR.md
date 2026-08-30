# Kimi Linear pure-C conversion plan

Source model: [moonshotai/Kimi-Linear-48B-A3B-Instruct](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct). The model card and repository are MIT-licensed. The repository is a custom Transformers implementation, not a Qwen checkpoint: it combines Kimi Delta Attention (KDA), Multi-head Latent Attention (MLA), and a 256-expert sigmoid-routed MoE.

## Current state

`tools/convert_kimi_linear.py` is a metadata-only preflight. It validates the
model config, 20-shard safetensors index, layer partition, and required tensor
names, then emits `config.normalized.json` and `conversion_plan.json`. It does
not quantize weights and no Kimi model is selectable in the gateway yet. Run:

```sh
python3 tools/convert_kimi_linear.py --indir /path/to/Kimi-Linear-48B-A3B-Instruct \
  --outdir build/kimi-linear-plan
```

`--allow-partial` is only for metadata tests; it must not be used for a release
conversion.

Pinned architectural facts from `config.json`:

- 27 layers, hidden size 2304, vocabulary 163840, head dimension 72;
- full MLA layers (1-based): 4, 8, 12, 16, 20, 24, 27;
- KDA layers (1-based): 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 21, 22, 23, 25, 26;
- KDA has 32 heads, head dimension 128, and short convolution kernel 4;
- MLA has `kv_lora_rank=512`, `qk_nope_head_dim=128`, `qk_rope_head_dim=64`, and `v_head_dim=128`;
- layers after the first use 256 experts, top-8 routing, sigmoid router, and routed scaling factor 2.446.

## Ordered implementation work

1. **Freeze artifacts and parity corpus.** Download the complete repository by
   an immutable revision, record SHA-256 for all 20 shards, pin the MIT custom
   code snapshot for reference only, and add tokenizer parity fixtures for
   `tiktoken.model` plus `tokenizer_config.json`.
2. **Define a new snapshot schema.** Store tensor dtype/shape, layer kind,
   quantization scales, expert offsets, RoPE parameters, and a schema version.
   Do not reuse the Qwen snapshot magic or tensor-name assumptions.
3. **Implement KDA in C.** Add the three depthwise short-convolution states,
   `A_log`, `dt_bias`, `b_proj`, `f_*`, `g_*`, gated RMS output normalization,
   and the recurrent per-head state. Test one layer against Transformers on
   single-token and multi-token sequences, including reset/continuation.
4. **Implement MLA in C.** Add the latent KV projection/layernorm, split
   no-position/rope query and key paths, latent KV cache, RoPE, value projection,
   and output projection. Test cache append, decode, and context-boundary cases.
5. **Implement Kimi MoE.** Add sigmoid scores, correction bias, top-8 selection,
   renormalization, routed scaling, one shared expert, and 256 expert weight
   ranges. Validate expert ordering and deterministic ties.
6. **Build the converter.** Extend the preflight into a safetensors reader and
   quantizer that emits the new snapshot, with shape checks before allocation,
   bounded temporary memory, and resumable per-shard progress.
7. **Wire the engine.** Add a separate `kimi_linear.c` backend and explicit
   catalog/runtime capability. Keep the existing Qwen backend unchanged and do
   not silently route Kimi weights through `qwen36b.c`.
8. **Gate release.** Require numerical parity against the reference for logits,
   greedy tokens, KV continuation, special-token handling, malformed snapshots,
   memory admission, and cancellation. Add a model manifest and only then
   expose Kimi in the UI catalog.

The removed Gigatoken prototype demonstrated local `tiktoken.model` parsing,
but it was never part of the runtime. Any future Kimi backend must provide and
test its own maintained tokenizer path alongside the pure-C KDA/MLA runtime.
