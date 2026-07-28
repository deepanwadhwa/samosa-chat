#!/usr/bin/env python3
"""Metadata-only Kimi conversion preflight test."""
from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


FULL = [4, 8, 12, 16, 20, 24, 27]
KDA = [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 21, 22, 23, 25, 26]


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="samosa-kimi-plan-") as td:
        src, out = Path(td) / "model", Path(td) / "out"
        src.mkdir()
        config = {
            "model_type": "kimi_linear", "hidden_size": 2304,
            "num_hidden_layers": 27, "vocab_size": 163840,
            "num_attention_heads": 32, "num_key_value_heads": 32,
            "head_dim": 72, "num_experts": 256,
            "num_experts_per_token": 8, "moe_intermediate_size": 1024,
            "intermediate_size": 9216, "kv_lora_rank": 512,
            "qk_nope_head_dim": 128, "qk_rope_head_dim": 64,
            "v_head_dim": 128, "first_k_dense_replace": 1,
            "moe_router_activation_func": "sigmoid",
            "linear_attn_config": {"full_attn_layers": FULL, "kda_layers": KDA},
        }
        weight_map: dict[str, str] = {
            "model.embed_tokens.weight": "model-00001-of-00020.safetensors",
            "lm_head.weight": "model-00001-of-00020.safetensors",
            "model.norm.weight": "model-00001-of-00020.safetensors",
        }
        for i in range(27):
            p = f"model.layers.{i}"
            weight_map[f"{p}.input_layernorm.weight"] = "model-00001-of-00020.safetensors"
            weight_map[f"{p}.post_attention_layernorm.weight"] = "model-00001-of-00020.safetensors"
            roles = (
                ["kv_a_layernorm.weight", "kv_a_proj_with_mqa.weight", "kv_b_proj.weight", "o_proj.weight", "q_proj.weight"]
                if i + 1 in FULL else
                ["A_log", "b_proj.weight", "dt_bias", "f_a_proj.weight", "f_b_proj.weight", "g_a_proj.weight", "g_b_proj.weight", "k_conv1d.weight", "k_proj.weight", "o_norm.weight", "o_proj.weight", "q_conv1d.weight", "q_proj.weight", "v_conv1d.weight", "v_proj.weight"]
            )
            for role in roles:
                weight_map[f"{p}.self_attn.{role}"] = "model-00001-of-00020.safetensors"
            if i == 0:
                for role in ("gate_proj", "up_proj", "down_proj"):
                    weight_map[f"{p}.mlp.{role}.weight"] = "model-00001-of-00020.safetensors"
            else:
                for role in ("weight", "e_score_correction_bias"):
                    weight_map[f"{p}.block_sparse_moe.gate.{role}"] = "model-00001-of-00020.safetensors"
                for role in ("gate_proj", "up_proj", "down_proj"):
                    weight_map[f"{p}.block_sparse_moe.shared_experts.{role}.weight"] = "model-00001-of-00020.safetensors"
                for expert in range(256):
                    for role in ("w1", "w2", "w3"):
                        weight_map[f"{p}.block_sparse_moe.experts.{expert}.{role}.weight"] = "model-00001-of-00020.safetensors"
        (src / "config.json").write_text(json.dumps(config), encoding="utf-8")
        (src / "model.safetensors.index.json").write_text(json.dumps({"weight_map": weight_map}), encoding="utf-8")
        result = subprocess.run(
            ["python3", str(root / "tools/convert_kimi_linear.py"), "--indir", str(src), "--outdir", str(out), "--allow-partial"],
            text=True, capture_output=True, check=True,
        )
        assert "preflight PASS" in result.stdout
        plan = json.loads((out / "conversion_plan.json").read_text(encoding="utf-8"))
        assert plan["status"] == "preflight-only"
        assert plan["layer_layout"]["full_attention_1_based"] == FULL
        assert plan["tensor_count"] == len(weight_map)
    print("test_kimi_conversion_plan.py: PASS")


if __name__ == "__main__":
    main()
