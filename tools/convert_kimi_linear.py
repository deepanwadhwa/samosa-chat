#!/usr/bin/env python3
"""Validate and describe a Kimi Linear safetensors conversion.

This is intentionally a metadata-only first stage.  Kimi Linear cannot be
converted by the Qwen converter: its KDA recurrence, MLA latent KV cache, and
sigmoid router need a new C runtime and a new snapshot schema.  This tool
therefore refuses malformed/partial inputs and emits the exact tensor plan a
future quantizing converter must implement; it never downloads weights or
pretends that a Qwen snapshot is Kimi-compatible.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

EXPECTED_FULL = [4, 8, 12, 16, 20, 24, 27]
EXPECTED_KDA = [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 21, 22, 23, 25, 26]
EXPECTED = {
    "model_type": "kimi_linear",
    "hidden_size": 2304,
    "num_hidden_layers": 27,
    "vocab_size": 163840,
    "num_attention_heads": 32,
    "num_key_value_heads": 32,
    "head_dim": 72,
    "num_experts": 256,
    "num_experts_per_token": 8,
    "moe_intermediate_size": 1024,
    "intermediate_size": 9216,
    "kv_lora_rank": 512,
    "qk_nope_head_dim": 128,
    "qk_rope_head_dim": 64,
    "v_head_dim": 128,
}
KDA_ROLES = (
    "A_log", "b_proj.weight", "dt_bias", "f_a_proj.weight", "f_b_proj.weight",
    "g_a_proj.weight", "g_b_proj.weight", "k_conv1d.weight", "k_proj.weight",
    "o_norm.weight", "o_proj.weight", "q_conv1d.weight", "q_proj.weight",
    "v_conv1d.weight", "v_proj.weight",
)
MLA_ROLES = (
    "kv_a_layernorm.weight", "kv_a_proj_with_mqa.weight", "kv_b_proj.weight",
    "o_proj.weight", "q_proj.weight",
)


def read_json(path: Path) -> dict:
    if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError(f"missing or oversized JSON file: {path}")
    with path.open("r", encoding="utf-8") as f:
        value = json.load(f)
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def safe_relpath(name: str) -> str:
    p = Path(name)
    if p.is_absolute() or ".." in p.parts or p.name != name:
        raise ValueError(f"unsafe shard path in index: {name!r}")
    if p.suffix != ".safetensors":
        raise ValueError(f"non-safetensors shard in index: {name!r}")
    return name


def validate(indir: Path, allow_partial: bool) -> tuple[dict, dict, list[str]]:
    config = read_json(indir / "config.json")
    index = read_json(indir / "model.safetensors.index.json")
    errors: list[str] = []
    for key, wanted in EXPECTED.items():
        if config.get(key) != wanted:
            errors.append(f"config.{key}={config.get(key)!r}, expected {wanted!r}")
    linear = config.get("linear_attn_config")
    if not isinstance(linear, dict) or linear.get("full_attn_layers") != EXPECTED_FULL or linear.get("kda_layers") != EXPECTED_KDA:
        errors.append("config.linear_attn_config layer lists do not match the pinned Kimi layout")
    if config.get("first_k_dense_replace") != 1 or config.get("moe_router_activation_func") != "sigmoid":
        errors.append("config must use first_k_dense_replace=1 and sigmoid MoE routing")
    weight_map = index.get("weight_map")
    if not isinstance(weight_map, dict) or not weight_map:
        errors.append("index.weight_map is missing or empty")
        weight_map = {}
    shards = sorted({safe_relpath(str(v)) for v in weight_map.values()})
    if len(shards) != 20 and not allow_partial:
        errors.append(f"index lists {len(shards)} shards, expected the complete 20-shard model")
    missing = [s for s in shards if not (indir / s).is_file()]
    if missing and not allow_partial:
        errors.append(f"missing local safetensors shards: {', '.join(missing[:4])}")

    def has(name: str) -> bool:
        return name in weight_map

    required = {"model.embed_tokens.weight", "lm_head.weight", "model.norm.weight"}
    required |= {f"model.layers.{i}.input_layernorm.weight" for i in range(27)}
    required |= {f"model.layers.{i}.post_attention_layernorm.weight" for i in range(27)}
    for i in range(27):
        one_based = i + 1
        attn_roles = MLA_ROLES if one_based in EXPECTED_FULL else KDA_ROLES
        required |= {f"model.layers.{i}.self_attn.{role}" for role in attn_roles}
        if i == 0:
            required |= {f"model.layers.{i}.mlp.{role}.weight" for role in ("gate_proj", "up_proj", "down_proj")}
        else:
            required |= {f"model.layers.{i}.block_sparse_moe.gate.{role}" for role in ("weight", "e_score_correction_bias")}
            required |= {f"model.layers.{i}.block_sparse_moe.shared_experts.{role}.weight" for role in ("gate_proj", "up_proj", "down_proj")}
            for expert in range(256):
                required |= {f"model.layers.{i}.block_sparse_moe.experts.{expert}.{role}.weight" for role in ("w1", "w2", "w3")}
    missing_tensors = sorted(name for name in required if not has(name))
    if missing_tensors:
        errors.append(f"index is missing {len(missing_tensors)} required tensors (first: {missing_tensors[0]})")
    if errors:
        raise ValueError("Kimi conversion preflight failed:\n- " + "\n- ".join(errors))
    normalized = {k: config[k] for k in sorted(config)}
    plan = {
        "schema_version": 1,
        "status": "preflight-only",
        "source_model": "moonshotai/Kimi-Linear-48B-A3B-Instruct",
        "source_format": "safetensors-indexed",
        "quantization": "not implemented",
        "runtime_required": ["KDA", "MLA", "sigmoid-MoE"],
        "shards": shards,
        "tensor_count": len(weight_map),
        "layer_layout": {"full_attention_1_based": EXPECTED_FULL, "kda_1_based": EXPECTED_KDA, "dense_layers_0_based": [0], "moe_layers_0_based": list(range(1, 27))},
        "next_steps": ["implement KDA recurrence/state kernels", "implement MLA latent KV cache and RoPE", "implement Kimi sigmoid/top-k MoE", "quantize and emit a kimi_linear snapshot", "add C runtime parity tests"],
    }
    return normalized, plan, missing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--indir", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--allow-partial", action="store_true", help="validate index/config without requiring all 20 shard files")
    args = parser.parse_args()
    try:
        normalized, plan, missing = validate(args.indir.resolve(), args.allow_partial)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / "config.normalized.json").write_text(json.dumps(normalized, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.outdir / "conversion_plan.json").write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if missing:
        print(f"Kimi preflight PASS (partial metadata only; {len(missing)} shard files absent)")
    else:
        print("Kimi preflight PASS (metadata and all shard files present)")
    print("No weights were converted: pure-C KDA/MLA runtime support is required before quantization.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
