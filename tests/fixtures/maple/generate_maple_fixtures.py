import mlx.core as mx
import mlx.nn as nn
import os
import gc
import json
import importlib.util
from mlx.utils import tree_flatten

# Disable MLX free-memory cache
mx.set_cache_limit(0)

mx.random.seed(42)

MAPLE_PY = "vendor/maple-reference/maple.py"
spec = importlib.util.spec_from_file_location("maple_ref", MAPLE_PY)
maple_ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maple_ref)

# We use a synthetic config so we do not instantiate the full model dimensions!
synthetic_config = {
    "hidden_size": 128,
    "intermediate_size": 256,
    "num_attention_heads": 16,
    "num_key_value_heads": 4,
    "num_hidden_layers": 2,
    "rms_norm_eps": 1e-5,
    "vocab_size": 512,
    "max_position_embeddings": 128,
    "rope_theta": 10000.0,
    "rope_scaling": None,
    "num_experts_per_tok": 8,
    "num_experts": 256,
    "layer_types": ["sliding_attention", "attention"]
}
args = maple_ref.ModelArgs.from_dict(synthetic_config)

# For testing quantization behavior, match C++ QLinear defaults
config = {
    "quantization": {"group_size": 64, "bits": 4, "mode": "affine"}
}

def class_predicate(p, m):
    if "quantization" in config and p in config["quantization"]:
        return config["quantization"][p]
    if not hasattr(m, "to_quantized"):
        return False
    return True

def quantize_module(module):
    if "quantization" in config:
        quantization = config["quantization"]
        nn.quantize(
            module,
            group_size=quantization.get("group_size", 32),
            bits=quantization.get("bits", 2),
            mode=quantization.get("mode", "affine"),
            class_predicate=class_predicate,
        )
    mx.eval(module.parameters())

def save_fixture(name, input_dict, module, expected_outputs):
    save_dict = {}
    for k, v in input_dict.items():
        save_dict[f"input_{k}"] = v

    if module is not None:
        for k, v in tree_flatten(module.parameters()):
            save_dict[f"module.{k}"] = v

    for i, t in enumerate(expected_outputs):
        save_dict[f"expected_output_{i}"] = t

    os.makedirs("tests/fixtures/maple/components", exist_ok=True)
    mx.save_safetensors(f"tests/fixtures/maple/components/{name}.safetensors", save_dict)
    print(f"Saved {name}.safetensors")

# 1. RMSNorm
print("Generating RMSNorm fixture...")
rmsnorm = maple_ref.MapleRMSNorm(args.hidden_size, eps=args.rms_norm_eps)
quantize_module(rmsnorm)
x = mx.random.normal((2, 32, args.hidden_size))
out = rmsnorm(x)
save_fixture("rmsnorm", {"x": x}, rmsnorm, [out])
del rmsnorm, x, out
gc.collect()

# 2. RoPE
print("Generating RoPE fixture...")
rope = maple_ref.initialize_rope(
    args.head_dim,
    args.rope_theta,
    traditional=False,
    scaling_config=args.rope_scaling,
    max_position_embeddings=args.max_position_embeddings,
)
if rope is not None:
    q = mx.random.normal((2, 32, args.num_attention_heads, args.head_dim))
    k = mx.random.normal((2, 32, args.num_key_value_heads, args.head_dim))
    pos = mx.arange(32)[None, :]
    q_rope = rope(q, offset=0)
    k_rope = rope(k, offset=0)
    save_fixture("rope", {"q": q, "k": k, "pos": pos}, None, [q_rope, k_rope])
    del q, k, pos, q_rope, k_rope
del rope
gc.collect()

# 3. Router
print("Generating Router fixture...")
router = maple_ref.MapleGate(args)
quantize_module(router)
x_router = mx.random.normal((2, 32, args.hidden_size))
inds, scores = router(x_router)
save_fixture("router", {"x": x_router}, router, [inds, scores])
del router, x_router, inds, scores
gc.collect()

# 4. SwiGLU
print("Generating SwiGLU fixture...")
x_gate = mx.random.normal((2, 32, 128))
x_up = mx.random.normal((2, 32, 128))
out_swiglu = maple_ref.clamped_swiglu(x_gate, x_up)
save_dict_swiglu = {"input_x_gate": x_gate, "input_x_up": x_up, "expected_output_0": out_swiglu}
mx.save_safetensors("tests/fixtures/maple/components/swiglu.safetensors", save_dict_swiglu)
print("Saved swiglu.safetensors")
del x_gate, x_up, out_swiglu, save_dict_swiglu
gc.collect()

# 5. Decoder Block
print("Generating Decoder Block fixture...")
block = maple_ref.MapleDecoderLayer(args, layer_idx=0)
if hasattr(block, "mlp") and hasattr(block.mlp, "gate"):
    block.mlp.gate.weight = mx.random.normal(block.mlp.gate.weight.shape)
quantize_module(block)
x_block = mx.random.normal((1, 8, args.hidden_size))
# mask for 8 tokens
mask = nn.MultiHeadAttention.create_additive_causal_mask(8)
cache = None
out_block = block(x_block, mask, cache)

# The reference implementation natively uses fused qkv_proj and up_gate_proj.
# The C++ implementation uses split projections. Split them for the fixture.
from mlx.utils import tree_flatten
state = dict(tree_flatten(block.parameters()))

# Split self_attn.qkv_proj
num_heads = args.num_attention_heads
num_kv = args.num_key_value_heads
head_dim = args.head_dim

for suffix in ["weight", "scales", "biases"]:
    qkv_key = f"self_attn.qkv_proj.{suffix}"
    if qkv_key in state:
        w = state.pop(qkv_key)
        q_size = num_heads * head_dim
        k_size = num_kv * head_dim
        v_size = num_kv * head_dim
        q = w[:q_size]
        k = w[q_size:q_size + k_size]
        v = w[q_size + k_size:]
        state[f"self_attn.q_proj.{suffix}"] = q
        state[f"self_attn.k_proj.{suffix}"] = k
        state[f"self_attn.v_proj.{suffix}"] = v

# Split mlx.switch_mlp.up_gate_proj
for suffix in ["weight", "scales", "biases"]:
    up_gate_key = f"mlp.switch_mlp.up_gate_proj.{suffix}"
    if up_gate_key in state:
        w = state.pop(up_gate_key)
        # up and gate are concatenated along axis 1 (per-expert rows)
        # shape is (num_experts, 2 * intermediate_size, hidden_size) for weight
        # or for scales (num_experts, 2 * int_size, ...)
        mid = w.shape[1] // 2
        state[f"mlp.switch_mlp.up_proj.{suffix}"] = w[:, :mid]
        state[f"mlp.switch_mlp.gate_proj.{suffix}"] = w[:, mid:]

# Remove original parameters and replace with un-fused
save_dict = {"input_x": x_block, "input_mask": mask, "expected_output_0": out_block}
for k, v in state.items():
    save_dict[f"module.{k}"] = v

mx.save_safetensors("tests/fixtures/maple/components/decoder_block.safetensors", save_dict)
del block, x_block, mask, cache, out_block
gc.collect()

print("Done generating fixtures!")
