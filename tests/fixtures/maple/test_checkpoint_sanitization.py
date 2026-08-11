import os
import gc
import json
import numpy as np
import mlx.core as mx
from safetensors import safe_open

MODEL_PATH = "models/maple"

def get_shard_for_tensor(tensor_name):
    index_path = os.path.join(MODEL_PATH, "model.safetensors.index.json")
    if not os.path.exists(index_path):
        return None
    with open(index_path, "r") as f:
        index = json.load(f)
    return index.get("weight_map", {}).get(tensor_name)

def get_rss_mb():
    # Return RSS in MB
    try:
        with os.popen(f"ps -o rss= -p {os.getpid()}") as p:
            rss_kb = p.read().strip()
            if rss_kb:
                return int(rss_kb) // 1024
    except:
        pass
    return 0

def extract_and_sanitize(tensor_name, output_name):
    shard_name = get_shard_for_tensor(tensor_name)
    if not shard_name:
        print(f"Skipping {tensor_name} (not found in index)")
        return

    shard_path = os.path.join(MODEL_PATH, shard_name)
    print(f"Reading {tensor_name} from {shard_name}...")

    mem_before = get_rss_mb()

    with safe_open(shard_path, framework="mlx") as f:
        if tensor_name not in f.keys():
            print(f"Tensor {tensor_name} not found in shard header!")
            return

        view = f.get_slice(tensor_name)
        shape = view.get_shape()

        try:
            # Slicing the first 16 elements (or as many as possible along the first dim)
            if len(shape) == 1:
                tiny_slice = view[0:min(16, shape[0])]
            elif len(shape) == 2:
                tiny_slice = view[0:min(16, shape[0]), 0:1]
                tiny_slice = mx.flatten(tiny_slice)
            else:
                tiny_slice = view[0:min(16, shape[0]), ...]
                tiny_slice = mx.flatten(tiny_slice)[:16]
        except TypeError as e:
            if "bfloat16" in str(e).lower():
                print(f"  [Warning] safetensors get_slice lacks bfloat16 support without torch. Falling back to zeros for {tensor_name}.")
                tiny_slice = mx.zeros((min(16, shape[0]),), dtype=mx.bfloat16)
            else:
                raise

    mem_after = get_rss_mb()
    print(f"  Footprint delta for slicing: {mem_after - mem_before} MB")

    save_dict = {
        "slice": tiny_slice,
        "shape": mx.array(list(shape), dtype=mx.int32)
    }

    os.makedirs("tests/fixtures/maple/components", exist_ok=True)
    mx.save_safetensors(f"tests/fixtures/maple/components/{output_name}.safetensors", save_dict)
    print(f"Saved tiny sanitization fixture: {output_name}.safetensors")

    del tiny_slice, save_dict
    gc.collect()

# We only load ONE tensor at a time from the real checkpoint
tensors_to_check = [
    ("model.layers.0.self_attn.q_proj.weight", "sanitize_q_proj"),
    ("model.layers.0.self_attn.q_proj.scales", "sanitize_q_scales"),
    ("model.layers.0.mlp.switch_mlp.gate_proj.weight", "sanitize_gate_proj"),
    ("model.layers.0.mlp.switch_mlp.gate_proj.row_alpha", "sanitize_gate_alpha")
]

for name, out in tensors_to_check:
    extract_and_sanitize(name, out)

print("Sanitization fixtures extraction complete.")
