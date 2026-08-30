import mlx.core as mx
import mlx.nn as nn
from mlx.utils import tree_flatten
import json
import gc
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../vendor/maple-reference")))
import maple as maple_ref

# Disable MLX free-memory cache
mx.set_cache_limit(0)

mx.random.seed(42)

def class_predicate(p, m):
    if not hasattr(m, "to_quantized"):
        return False
    return isinstance(m, nn.Linear)

def quantize_module(module):
    nn.quantize(
        module,
        group_size=64,
        bits=4,
        mode="affine",
        class_predicate=class_predicate,
    )
    mx.eval(module.parameters())

synthetic_config = {
    "hidden_size": 128,
    "intermediate_size": 256,
    "num_attention_heads": 16,
    "num_key_value_heads": 4,
    "num_hidden_layers": 2,
    "layer_types": ["sliding_attention", "attention"],
    "vocab_size": 512,
    "rms_norm_eps": 1e-5,
    "max_position_embeddings": 128,
    "rope_theta": 10000.0,
    "num_experts": 32,
    "num_experts_per_tok": 8,
    "first_k_dense_replace": 0,
    "sliding_window": 64,
}

synthetic_args = maple_ref.ModelArgs.from_dict(synthetic_config)

print("Generating Model fixture...")
model = maple_ref.Model(synthetic_args)
# randomize gates to avoid ties
for layer in model.model.layers:
    if hasattr(layer, "mlp") and hasattr(layer.mlp, "gate"):
        layer.mlp.gate.weight = mx.random.normal(layer.mlp.gate.weight.shape)

quantize_module(model)

x_seq = mx.random.randint(0, synthetic_args.vocab_size, (1, 8))

# 1. Prefill
from mlx_lm.models.cache import make_prompt_cache
caches = make_prompt_cache(model, max_kv_size=synthetic_args.sliding_window)
out_prefill = model(x_seq, cache=caches)

# 2. Decode 1
x_dec1 = mx.random.randint(0, synthetic_args.vocab_size, (1, 1))
out_dec1 = model(x_dec1, cache=caches)

# 3. Decode 2
x_dec2 = mx.random.randint(0, synthetic_args.vocab_size, (1, 1))
out_dec2 = model(x_dec2, cache=caches)

state = dict(tree_flatten(model.parameters()))
save_dict = {
    "x_seq": x_seq,
    "out_prefill": out_prefill,
    "x_dec1": x_dec1,
    "out_dec1": out_dec1,
    "x_dec2": x_dec2,
    "out_dec2": out_dec2,
}

# Split for C++ compatibility
for k in list(state.keys()):
    if "qkv_proj" in k:
        w = state.pop(k)
        suffix = k.split('.')[-1]
        prefix = k.replace(f"qkv_proj.{suffix}", "")
        q_size = synthetic_args.num_attention_heads * synthetic_args.head_dim
        k_size = synthetic_args.num_key_value_heads * synthetic_args.head_dim
        state[f"{prefix}q_proj.{suffix}"] = w[:q_size]
        state[f"{prefix}k_proj.{suffix}"] = w[q_size:q_size + k_size]
        state[f"{prefix}v_proj.{suffix}"] = w[q_size + k_size:]
    elif "up_gate_proj" in k:
        w = state.pop(k)
        suffix = k.split('.')[-1]
        prefix = k.replace(f"up_gate_proj.{suffix}", "")
        mid = w.shape[1] // 2
        state[f"{prefix}up_proj.{suffix}"] = w[:, :mid]
        state[f"{prefix}gate_proj.{suffix}"] = w[:, mid:]

for k, v in state.items():
    save_dict[k] = v

mx.save_safetensors("tests/fixtures/maple/components/model.safetensors", save_dict)
print("Done generating Model fixture!")
