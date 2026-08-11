#!/usr/bin/env python3
import sys
import os
import mlx.core as mx

# Add the vendor/maple-reference path so we can import the official implementation
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../vendor/maple-reference"))
from mlx_lm.models import maple
from mlx_lm.models.maple import ModelArgs, MapleModel

def test_add_rms_norm():
    print("Generating reference for add_rms_norm...")
    mx.random.seed(0)
    dim = 2048
    eps = 1e-6
    x = mx.random.normal((1, 1, dim), dtype=mx.bfloat16)
    r = mx.random.normal((1, 1, dim), dtype=mx.bfloat16)
    w = mx.random.normal((dim,), dtype=mx.bfloat16)

    h_out, hn_out = maple._add_rms_norm(x, r, w, eps)
    mx.eval(h_out, hn_out)
    return {
        "x": x,
        "r": r,
        "w": w,
        "h_out": h_out,
        "hn_out": hn_out
    }

def test_qk_norm_rope():
    print("Generating reference for qk_norm_rope...")
    mx.random.seed(0)
    n_q = 16
    n_kv = 4
    head_dim = 128
    eps = 1e-6
    rope_dim = 64

    # 20 heads total (16 q + 4 kv)
    qk = mx.random.normal((n_q + n_kv, head_dim), dtype=mx.bfloat16)
    w = mx.random.normal((n_q + n_kv, head_dim), dtype=mx.bfloat16)

    inv_freq = 10000.0 ** (-mx.arange(rope_dim // 2, dtype=mx.float32) / (rope_dim // 2))

    pos_eps = mx.array([7.0, eps], dtype=mx.float32)

    out = maple._qk_norm_rope_kernel(
        inputs=[qk, w, inv_freq, pos_eps],
        template=[
            ("T_", qk.dtype),
            ("HEAD_DIM", head_dim),
            ("ROPE_DIM", rope_dim),
        ],
        grid=(32, qk.shape[0], 1),
        threadgroup=(32, 1, 1),
        output_shapes=[qk.shape],
        output_dtypes=[qk.dtype],
    )[0]
    mx.eval(out)
    return {
        "qk": qk,
        "w": w,
        "inv_freq": inv_freq,
        "pos_eps": pos_eps,
        "out": out
    }

def test_fused_router():
    print("Generating reference for fused_router...")
    mx.random.seed(0)
    dim = 2048
    num_experts = 256

    x = mx.random.normal((1, dim), dtype=mx.bfloat16)
    w = mx.random.normal((num_experts, dim), dtype=mx.float32)
    ctr_in = mx.zeros((8,), dtype=mx.uint32)

    inds, scores, _ = maple._fused_router_kernel(
        inputs=[x.reshape(-1), w, ctr_in],
        template=[
            ("T_", w.dtype),
            ("NEXP", num_experts),
            ("DIM", dim),
        ],
        grid=((num_experts // 32) * 256, 1, 1),
        threadgroup=(256, 1, 1),
        output_shapes=[(8,), (8,), (num_experts,)],
        output_dtypes=[mx.int32, mx.float32, mx.float32],
    )
    mx.eval(inds, scores)
    return {
        "x": x,
        "w": w,
        "inds": inds,
        "scores": scores
    }

def test_swiglu():
    print("Generating reference for swiglu...")
    mx.random.seed(0)
    dim = 2048
    gate = mx.random.normal((1, dim), dtype=mx.bfloat16)
    x = mx.random.normal((1, dim), dtype=mx.bfloat16)
    out = maple.clamped_swiglu(gate, x)
    mx.eval(out)
    return {
        "gate": gate,
        "x": x,
        "out": out
    }

def to_mx(x):
    import mlx.core as mx
    if isinstance(x, mx.array):
        return x
    return mx.array(x)

def dump_npz():
    import mlx.core as mx
    t1 = test_add_rms_norm()
    t2 = test_qk_norm_rope()
    t3 = test_fused_router()
    t4 = test_swiglu()

    out_dict = {}
    for k, v in t1.items(): out_dict[f"add_rms_norm_{k}"] = to_mx(v)
    for k, v in t2.items(): out_dict[f"qk_norm_rope_{k}"] = to_mx(v)
    for k, v in t3.items(): out_dict[f"fused_router_{k}"] = to_mx(v)
    for k, v in t4.items(): out_dict[f"swiglu_{k}"] = to_mx(v)

    mx.save_safetensors("tests/maple_parity_fixtures.safetensors", out_dict)
    print("Wrote tests/maple_parity_fixtures.safetensors")

def generate_fake_model():
    import json
    import os
    import mlx.core as mx

    os.makedirs("tests/fake_model", exist_ok=True)

    config = {
        "model_type": "maple",
        "hidden_size": 128,
        "intermediate_size": 256,
        "moe_intermediate_size": 64,
        "num_hidden_layers": 2,
        "num_attention_heads": 4,
        "num_key_value_heads": 2,
        "head_dim": 32,
        "num_experts": 8,
        "num_experts_per_tok": 2,
        "first_k_dense_replace": 1,
        "rms_norm_eps": 1e-6,
        "vocab_size": 32000,
        "layer_types": ["full_attention", "sliding_attention"]
    }

    with open("tests/fake_model/config.json", "w") as f:
        json.dump(config, f)

    weights = {}
    weights["model.embed_tokens.weight"] = mx.random.normal((config["vocab_size"], config["hidden_size"]))
    weights["model.norm.weight"] = mx.ones((config["hidden_size"],))
    weights["lm_head.weight"] = mx.random.normal((config["vocab_size"], config["hidden_size"]))

    for l in range(config["num_hidden_layers"]):
        prefix = f"model.layers.{l}"
        weights[f"{prefix}.input_layernorm.weight"] = mx.ones((config["hidden_size"],))
        weights[f"{prefix}.post_attention_layernorm.weight"] = mx.ones((config["hidden_size"],))

        # Attention
        n_q = config["num_attention_heads"]
        n_kv = config["num_key_value_heads"]
        d = config["head_dim"]

        weights[f"{prefix}.self_attn.qkv_proj.weight"] = mx.random.normal(((n_q + 2*n_kv) * d, config["hidden_size"]))
        weights[f"{prefix}.self_attn.o_proj.weight"] = mx.random.normal((config["hidden_size"], n_q * d))
        weights[f"{prefix}.self_attn.q_norm.weight"] = mx.ones((d,))
        weights[f"{prefix}.self_attn.k_norm.weight"] = mx.ones((d,))

        if l < config["first_k_dense_replace"]:
            # Dense MLP
            weights[f"{prefix}.mlp.gate_proj.weight"] = mx.random.normal((config["intermediate_size"], config["hidden_size"]))
            weights[f"{prefix}.mlp.up_proj.weight"] = mx.random.normal((config["intermediate_size"], config["hidden_size"]))
            weights[f"{prefix}.mlp.down_proj.weight"] = mx.random.normal((config["hidden_size"], config["intermediate_size"]))
        else:
            # MoE MLP
            weights[f"{prefix}.mlp.gate.weight"] = mx.random.normal((config["num_experts"], config["hidden_size"]))

            # SwitchGLU weights (fused)
            weights[f"{prefix}.mlp.switch_mlp.up_gate_proj.weight"] = mx.random.normal((config["num_experts"], 2 * config["moe_intermediate_size"], config["hidden_size"]))
            weights[f"{prefix}.mlp.switch_mlp.down_proj.weight"] = mx.random.normal((config["num_experts"], config["hidden_size"], config["moe_intermediate_size"]))

    # Write a dummy tokenizer.json
    tokenizer = {
        "model": {
            "type": "BPE",
            "vocab": {
                "<|endoftext|>": 0,
                "<|im_start|>": 1,
                "<|im_end|>": 2,
                "a": 3,
                "b": 4,
                "ab": 5
            },
            "merges": ["a b"]
        },
        "added_tokens": [
            {"id": 0, "content": "<|endoftext|>"},
            {"id": 1, "content": "<|im_start|>"},
            {"id": 2, "content": "<|im_end|>"}
        ]
    }
    with open("tests/fake_model/tokenizer.json", "w") as f:
        json.dump(tokenizer, f)

    mx.save_safetensors("tests/fake_model/model.safetensors", weights)
    print("Wrote tests/fake_model/model.safetensors and tokenizer.json")

if __name__ == "__main__":
    dump_npz()
    generate_fake_model()
