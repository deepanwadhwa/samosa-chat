import os
import sys
import torch
import numpy as np
import safetensors.torch
from huggingface_hub import hf_hub_download
from transformers import AutoProcessor, AutoConfig
from transformers.models.qwen3_5_moe.modeling_qwen3_5_moe import Qwen3_5MoeVisionModel, Qwen3_5MoeVisionConfig
from PIL import Image, ImageDraw

def log(msg):
    print(msg, flush=True)

def main():
    repo = "Qwen/Qwen3.6-35B-A3B"
    shipped_path = "/Users/deepanwadhwa/Documents/samosa-models/qwen36_group32_i8/resident.safetensors"
    out_dir = "/Users/deepanwadhwa/Documents/samosa-chat/docs/regressions/vision-validation"
    os.makedirs(out_dir, exist_ok=True)

    log("Step 1: Downloading index and locating reference shards...")
    index_path = hf_hub_download(repo_id=repo, filename="model.safetensors.index.json")
    with open(index_path, "r") as f:
        import json
        index_data = json.load(f)
    weight_map = index_data["weight_map"]
    
    # We download the shards that contain the visual weights
    shards_to_download = sorted(list(set(v for k, v in weight_map.items() if "visual" in k)))
    log(f"Visual shards found: {shards_to_download}")
    
    local_shards = []
    for shard in shards_to_download:
        log(f"Downloading/loading shard {shard}...")
        local_path = hf_hub_download(repo_id=repo, filename=shard)
        local_shards.append(local_path)
    
    log("Loading reference visual weights from shards...")
    ref_weights = {}
    for local_path in local_shards:
        with safetensors.safe_open(local_path, framework="pt", device="cpu") as f:
            for k in f.keys():
                if "visual" in k:
                    ref_weights[k] = f.get_tensor(k)
    log(f"Loaded {len(ref_weights)} visual weights from reference shards.")
    
    log("Loading shipped visual weights from resident.safetensors...")
    shipped_tensors = {}
    with safetensors.safe_open(shipped_path, framework="pt", device="cpu") as f:
        for k in f.keys():
            if "visual" in k:
                shipped_tensors[k] = f.get_tensor(k)
    log(f"Loaded {len(shipped_tensors)} visual keys from shipped weights.")

    # We need to perform the numerical comparison
    # Tensors are either:
    # 1. Unquantized: they are present directly as float32 in shipped_tensors
    # 2. Quantized: they are present as U8 in shipped_tensors, and have a corresponding .weight.qs key
    # Wait, let's identify the 111 quantized weights
    quantized_weights_names = []
    for k in shipped_tensors.keys():
        if k.endswith(".weight") and (k + ".qs") in shipped_tensors:
            quantized_weights_names.append(k)
            
    log(f"Identified {len(quantized_weights_names)} quantized tensors (carrying .qs scales).")
    
    per_tensor_results = []
    
    # Helper for dequantizing symmetric row-wise int8
    # q is uint8 tensor (flat or reshaped), qs is scales (float32)
    def dequantize_rowwise_int8(q_tensor, qs_tensor, ref_shape):
        # Cast to signed int8
        q_np = q_tensor.numpy()
        q_signed = q_np.view(np.int8).astype(np.float32)
        
        # Reshape to O, I
        O = qs_tensor.shape[0]
        I = q_signed.size // O
        q_2d = q_signed.reshape(O, I)
        
        # Multiply by scales
        scales_np = qs_tensor.numpy()
        dequant = q_2d * scales_np[:, np.newaxis]
        
        # Reshape to final reference shape
        return torch.from_numpy(dequant).reshape(ref_shape)

    for k in quantized_weights_names:
        q_w = shipped_tensors[k]
        scales = shipped_tensors[k + ".qs"]
        ref_w = ref_weights[k].to(torch.float32)
        
        dequant = dequantize_rowwise_int8(q_w, scales, ref_w.shape)
        
        # Compute cosine similarity
        ref_flat = ref_w.flatten()
        dequant_flat = dequant.flatten()
        
        dot = torch.dot(ref_flat, dequant_flat).item()
        norm_ref = torch.norm(ref_flat).item()
        norm_dequant = torch.norm(dequant_flat).item()
        
        if norm_ref * norm_dequant == 0:
            cosine = 1.0 if norm_ref == norm_dequant else 0.0
        else:
            cosine = dot / (norm_ref * norm_dequant)
            
        # Max absolute relative error: max(|A - B| / (|B| + 1e-8))
        abs_diff = torch.abs(dequant - ref_w)
        rel_diff = abs_diff / (torch.abs(ref_w) + 1e-8)
        max_rel_error = torch.max(rel_diff).item()
        max_abs_error = torch.max(abs_diff).item()
        
        per_tensor_results.append({
            "tensor_name": k,
            "cosine_similarity": cosine,
            "max_abs_relative_error": max_rel_error,
            "max_abs_error": max_abs_error
        })
        
    # Analyze the distribution of cosine similarity
    cosines = [r["cosine_similarity"] for r in per_tensor_results]
    rel_errors = [r["max_abs_relative_error"] for r in per_tensor_results]
    
    log("\n--- Per-Tensor Statistics ---")
    log(f"Cosine Similarity:")
    log(f"  Min:  {np.min(cosines):.6f}")
    log(f"  Mean: {np.mean(cosines):.6f}")
    log(f"  Max:  {np.max(cosines):.6f}")
    log(f"  p50:  {np.percentile(cosines, 50):.6f}")
    log(f"  p90:  {np.percentile(cosines, 90):.6f}")
    log(f"  p10:  {np.percentile(cosines, 10):.6f}")
    
    log(f"Max Absolute Relative Error:")
    log(f"  Min:  {np.min(rel_errors):.6f}")
    log(f"  Mean: {np.mean(rel_errors):.6f}")
    log(f"  Max:  {np.max(rel_errors):.6f}")
    log(f"  p50:  {np.percentile(rel_errors, 50):.6f}")
    log(f"  p90:  {np.percentile(rel_errors, 90):.6f}")
    
    # Now let's do the end-to-end forward pass check
    log("\nStep 2: Preparing 20 synthetic images for forward pass checks...")
    synthetic_images = []
    for i in range(20):
        # Create diverse images (different sizes, patterns, text, colors)
        size = (256 + (i * 16), 256 + ((i % 3) * 32))
        img = Image.new("RGB", size, color=(i * 12, 255 - i * 10, (i * 20) % 256))
        draw = ImageDraw.Draw(img)
        # draw a pattern
        draw.rectangle([10, 10, size[0]-10, size[1]-10], outline="white", width=3)
        draw.ellipse([50, 50, size[0]-50, size[1]-50], fill="yellow" if i % 2 == 0 else "green")
        synthetic_images.append(img)
        
    config = AutoConfig.from_pretrained(repo, trust_remote_code=True)
    processor = AutoProcessor.from_pretrained(repo, trust_remote_code=True)
    
    # 1. Forward pass using original BF16 weights
    log("Instantiating reference model...")
    ref_model = Qwen3_5MoeVisionModel(config.vision_config)
    ref_model.eval()
    
    # Load original weights (we need to clean keys: strip 'model.visual.' prefix)
    ref_state_dict = {}
    for k, v in ref_weights.items():
        stripped_key = k.replace("model.visual.", "")
        ref_state_dict[stripped_key] = v
    ref_model.load_state_dict(ref_state_dict)
    
    # 2. Forward pass using dequantized weights
    log("Instantiating dequantized model...")
    dequant_model = Qwen3_5MoeVisionModel(config.vision_config)
    dequant_model.eval()
    
    dequant_state_dict = {}
    # Build complete state dict of dequantized weights
    for k, v in shipped_tensors.items():
        stripped_key = k.replace("model.visual.", "")
        if stripped_key.endswith(".qs"):
            continue
        
        # If it's a quantized weight, dequantize it
        if k in quantized_weights_names:
            ref_w = ref_weights[k]
            dequant_w = dequantize_rowwise_int8(v, shipped_tensors[k + ".qs"], ref_w.shape)
            dequant_state_dict[stripped_key] = dequant_w.to(ref_w.dtype) # match original dtype (bfloat16)
        else:
            # It's an unquantized weight or bias
            dequant_state_dict[stripped_key] = v
            
    dequant_model.load_state_dict(dequant_state_dict)
    
    # Run end-to-end forward passes and compare output embeddings
    image_similarity_results = []
    
    log("Running forward pass comparisons on images...")
    for idx, img in enumerate(synthetic_images):
        inputs = processor(images=img, text="Describe", return_tensors="pt")
        pixel_values = inputs["pixel_values"]
        grid_thw = inputs["image_grid_thw"]
        
        with torch.no_grad():
            ref_out = ref_model(pixel_values, grid_thw)[0]
            dequant_out = dequant_model(pixel_values, grid_thw)[0]
            
        # Compute cosine similarity of the merger output
        ref_out_flat = ref_out.flatten()
        dequant_out_flat = dequant_out.flatten()
        
        dot = torch.dot(ref_out_flat, dequant_out_flat).item()
        norm_ref = torch.norm(ref_out_flat).item()
        norm_dequant = torch.norm(dequant_out_flat).item()
        cosine = dot / (norm_ref * norm_dequant)
        
        abs_diff = torch.abs(dequant_out - ref_out)
        max_abs = torch.max(abs_diff).item()
        mean_abs = torch.mean(abs_diff).item()
        
        image_similarity_results.append({
            "image_index": idx,
            "cosine_similarity": cosine,
            "max_abs_error": max_abs,
            "mean_abs_error": mean_abs
        })
        log(f"  Image {idx}: cosine_similarity={cosine:.6f}, max_abs_error={max_abs:.6f}")
        
    image_cosines = [r["cosine_similarity"] for r in image_similarity_results]
    mean_img_cosine = np.mean(image_cosines)
    min_img_cosine = np.min(image_cosines)
    
    log("\n--- Image Forward Pass Statistics ---")
    log(f"Merger Output Cosine Similarity:")
    log(f"  Min:  {min_img_cosine:.6f}")
    log(f"  Mean: {mean_img_cosine:.6f}")
    log(f"  Max:  {np.max(image_cosines):.6f}")

    # Write report
    report_path = os.path.join(out_dir, "report.md")
    with open(report_path, "w", encoding="utf-8") as f:
        f.write("# E-V1: Shipped Vision Tower Numerical Parity Report\n\n")
        f.write(f"Analyzed on: 2026-07-15\n")
        f.write(f"Reference Model: `{repo}`\n")
        f.write(f"Shipped Model: `deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32` (row-wise int8 visual tower)\n\n")
        
        f.write("## Per-Tensor Similarity (111 Quantized Tensors)\n\n")
        f.write("| Metric | Cosine Similarity | Max Absolute Relative Error |\n")
        f.write("|---|---|---|\n")
        f.write(f"| **Min** | {np.min(cosines):.6f} | {np.min(rel_errors):.6f} |\n")
        f.write(f"| **Mean** | {np.mean(cosines):.6f} | {np.mean(rel_errors):.6f} |\n")
        f.write(f"| **Max** | {np.max(cosines):.6f} | {np.max(rel_errors):.6f} |\n")
        f.write(f"| **p50** | {np.percentile(cosines, 50):.6f} | {np.percentile(rel_errors, 50):.6f} |\n")
        f.write(f"| **p90** | {np.percentile(cosines, 90):.6f} | {np.percentile(rel_errors, 90):.6f} |\n\n")
        
        # Check acceptance criteria:
        # "Acceptance: per-tensor cosine >= 0.99 for all 111; merger-output cosine >= 0.99 mean with no image below 0.97"
        tensor_pass = np.all(np.array(cosines) >= 0.99)
        merger_pass = (mean_img_cosine >= 0.99) and (min_img_cosine >= 0.97)
        
        f.write("## End-to-End ViT Merger Embeddings Similarity (20 Images)\n\n")
        f.write(f"**Mean Cosine Similarity:** {mean_img_cosine:.6f}\n")
        f.write(f"**Min Cosine Similarity:** {min_img_cosine:.6f}\n\n")
        
        f.write("## Verdict\n\n")
        if tensor_pass and merger_pass:
            f.write("> [!TIP]\n")
            f.write("> **PASS**: The shipped row-wise int8 visual weights are numerically usable and meet the accuracy criteria (per-tensor cosine >= 0.99, merger cosine >= 0.99 mean, min >= 0.97).\n")
        else:
            f.write("> [!WARNING]\n")
            f.write("> **FAIL**: The shipped row-wise int8 visual weights do NOT meet the accuracy requirements. Re-quantization using groupwise-symmetric-q4-v1 is recommended.\n")
            
        f.write("\n### Details by Tensor\n\n")
        f.write("| Tensor Name | Cosine Similarity | Max Abs Relative Error | Max Abs Error |\n")
        f.write("|---|---|---|---|\n")
        for r in per_tensor_results:
            f.write(f"| `{r['tensor_name']}` | {r['cosine_similarity']:.6f} | {r['max_abs_relative_error']:.6f} | {r['max_abs_error']:.6f} |\n")
            
    log(f"Report written to {report_path}")

if __name__ == "__main__":
    main()
