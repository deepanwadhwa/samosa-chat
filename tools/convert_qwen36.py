import os
import sys
import json
import glob
import shutil
import argparse
import hashlib
import gc
import errno
import numpy as np

STATE_SCHEMA = 2
STATE_FILE = ".conversion_state.json"
EXPERTS_PARTIAL = ".experts.partial.bin"
RESIDENT_PARTIAL_PREFIX = ".resident.partial."
ALIGNMENT_BYTES = 16 * 1024


def sha256_file(path, chunk_size=8 * 1024 * 1024):
    """Return a file digest without mapping the whole file into memory."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk_size)
            if not block:
                return h.hexdigest()
            h.update(block)


def stable_json_sha256(value):
    return hashlib.sha256(json.dumps(
        value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()


def fsync_file(path):
    """Force a completed file's bytes to stable storage before journalling it."""
    with open(path, "rb") as f:
        os.fsync(f.fileno())


def fsync_dir(path):
    """Persist an atomic rename as well as the file it renamed.

    APFS supports fsync on directory descriptors.  Some unusual filesystems do
    not; the file fsync above is still useful there, so only ignore EINVAL.
    """
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        try:
            os.fsync(fd)
        except OSError as e:
            if e.errno != getattr(os, "EINVAL", 22):
                raise
    finally:
        os.close(fd)


def atomic_json(path, value):
    """Write a small state/manifest file as a durable replace operation."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(value, f, indent=2, sort_keys=True)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fsync_dir(os.path.dirname(path) or ".")


def atomic_save_safetensors(path, tensors, save_file):
    """Publish a resident checkpoint only after a complete safetensors write."""
    tmp = path + ".tmp"
    try:
        save_file(tensors, tmp)
        fsync_file(tmp)
        os.replace(tmp, path)
        fsync_dir(os.path.dirname(path) or ".")
    finally:
        # A failed save must never be mistaken for a snapshot on the next run.
        if os.path.exists(tmp):
            os.remove(tmp)


def atomic_copy_file(source, destination):
    """Durably copy an input/config/snapshot into the final container."""
    if os.path.abspath(source) == os.path.abspath(destination):
        return
    tmp = destination + ".tmp"
    try:
        with open(source, "rb") as src, open(tmp, "wb") as dst:
            shutil.copyfileobj(src, dst, length=8 * 1024 * 1024)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(tmp, destination)
        fsync_dir(os.path.dirname(destination) or ".")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def atomic_link_or_copy_file(source, destination):
    """Publish a large immutable artifact without duplicating it when possible."""
    if os.path.abspath(source) == os.path.abspath(destination):
        return
    tmp = destination + ".tmp"
    try:
        remove_if_exists(tmp)
        try:
            os.link(source, tmp)
        except OSError as error:
            if error.errno not in (errno.EXDEV, errno.EPERM, errno.EACCES):
                raise
            with open(source, "rb") as src, open(tmp, "wb") as dst:
                shutil.copyfileobj(src, dst, length=8 * 1024 * 1024)
                dst.flush()
                os.fsync(dst.fileno())
        os.replace(tmp, destination)
        fsync_dir(os.path.dirname(destination) or ".")
    finally:
        remove_if_exists(tmp)


def validate_manifest_blobs(path, manifest, committed_bytes=None):
    """Validate the append-only part of the container recorded by state.

    A state record is authoritative about the file length.  Bytes after that
    length are an interrupted, uncommitted append and are deliberately ignored
    (and truncated during resume), not treated as more converted experts.
    """
    if not os.path.isfile(path):
        return False
    actual_bytes = os.path.getsize(path)
    if committed_bytes is None:
        committed_bytes = actual_bytes
    if actual_bytes != committed_bytes or committed_bytes < 0:
        return False

    entries = []
    for name, item in manifest.get("experts", {}).items():
        try:
            offset = int(item["offset"])
            size = int(item["size"])
            digest = item["sha256"]
        except (KeyError, TypeError, ValueError):
            return False
        if offset < 0 or size <= 0 or offset + size > committed_bytes:
            return False
        entries.append((offset, size, digest, name))

    # A malformed/replayed manifest must not make two experts claim one blob.
    last_end = 0
    for offset, size, _, _ in sorted(entries):
        if offset < last_end:
            return False
        last_end = offset + size
    # The append journal never has holes: accepting an unreferenced committed
    # tail would make a later resume silently retain data not covered by state.
    if last_end != committed_bytes:
        return False

    with open(path, "rb") as f:
        for offset, size, digest, _ in entries:
            f.seek(offset)
            blob = f.read(size)
            if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
                return False
    return True


def resident_snapshot_path(outdir, generation):
    return os.path.join(outdir, f"{RESIDENT_PARTIAL_PREFIX}{generation:05d}.safetensors")


def validate_resident_snapshot(outdir, info):
    """Check that state points at a complete, immutable resident generation."""
    if not isinstance(info, dict):
        return None
    rel = info.get("path")
    if not isinstance(rel, str) or os.path.basename(rel) != rel:
        return None
    path = os.path.join(outdir, rel)
    try:
        expected_size = int(info["size"])
        expected_sha = info["sha256"]
    except (KeyError, TypeError, ValueError):
        return None
    if not os.path.isfile(path) or os.path.getsize(path) != expected_size:
        return None
    if sha256_file(path) != expected_sha:
        return None
    return path


def remove_if_exists(path):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


class QuantStats:
    """Streaming quantization error summary, without retaining source tensors."""
    def __init__(self):
        self.groups = {}

    def add(self, group, original, reconstructed):
        # Relative error is intentionally floored near zero: otherwise exact
        # zeros/denormals make a useful conversion diagnostic meaningless.
        rel = np.abs(original - reconstructed) / np.maximum(np.abs(original), 1e-8)
        state = self.groups.setdefault(group, [0, 0.0, 0.0])
        state[0] += rel.size
        state[1] += float(rel.sum(dtype=np.float64))
        state[2] = max(state[2], float(rel.max(initial=0.0)))

    def report(self):
        print("\nQuantization round-trip relative error (per-row scales):")
        for group, (count, total, maximum) in sorted(self.groups.items()):
            print(f"  {group}: mean={total / count:.6g} max={maximum:.6g} values={count}")

    def to_state(self):
        return {name: [int(count), float(total), float(maximum)]
                for name, (count, total, maximum) in self.groups.items()}

    @classmethod
    def from_state(cls, value):
        out = cls()
        if not isinstance(value, dict):
            return out
        for name, item in value.items():
            if (isinstance(name, str) and isinstance(item, list) and len(item) == 3):
                try:
                    out.groups[name] = [int(item[0]), float(item[1]), float(item[2])]
                except (TypeError, ValueError):
                    pass
        return out

def quant_int8(w):
    """Quantize float32 matrix w [O,I] to int8 (Q8_0 style).
    Returns (packed_uint8_array, scales_float32_array).
    """
    O, I = w.shape
    qmax = 127
    amax = np.abs(w).max(axis=1, keepdims=True)
    s = np.maximum(amax / qmax, 1e-8)
    q = np.clip(np.rint(w / s), -128, qmax).astype(np.int8)
    return q.reshape(-1).view(np.uint8).copy(), s[:, 0].astype(np.float32)

def quant_int4_rowwise(w):
    """Legacy whole-row symmetric int4 (not GGML/GGUF Q4_0).

    One scale covers every input value in an output row. This function is
    retained only for compatibility with already-published containers; new
    expert artifacts should use quant_int4_grouped.

    Valori in [-8, 7] memorizzati come v+8 (0..15).
    Returns (packed_uint8_array, scales_float32_array).
    """
    O, I = w.shape
    qmax = 7
    amax = np.abs(w).max(axis=1, keepdims=True)
    s = np.maximum(amax / qmax, 1e-8)
    q = np.clip(np.rint(w / s), -8, qmax).astype(np.int32) # nibble [-8, 7]
    rb = (I + 1) // 2
    out = np.zeros((O, rb), np.uint8)
    v0 = (q[:, 0::2] + 8).astype(np.uint8)
    out[:, :v0.shape[1]] = v0
    if I > 1:
        v1 = (q[:, 1::2] + 8).astype(np.uint8)
        out[:, :v1.shape[1]] |= (v1 << 4)
    return out.reshape(-1), s[:, 0].astype(np.float32)


def quant_int4_grouped(w, group_size):
    """Symmetric int4 with one scale per contiguous input group.

    Packed codes retain the established row-major nibble layout; only the
    scale plane changes from [O] to [O, ceil(I/group_size)].  Requiring an even
    divisor keeps every group byte-aligned for the integer-dot kernel.
    """
    O, I = w.shape
    if group_size < 2 or group_size % 2 or I % group_size:
        raise ValueError(f"group_size {group_size} must be an even divisor of {I}")
    groups = I // group_size
    blocks = w.reshape(O, groups, group_size)
    scales = np.maximum(np.abs(blocks).max(axis=2) / 7, 1e-8).astype(np.float32)
    q = np.clip(np.rint(blocks / scales[:, :, None]), -8, 7).astype(np.int32)
    q = q.reshape(O, I)
    packed = ((q[:, 0::2] + 8).astype(np.uint8) |
              ((q[:, 1::2] + 8).astype(np.uint8) << 4))
    return packed.reshape(-1), scales.reshape(-1)

def dequantize(q, scales, shape, bits, group_size=0):
    """Mirror kernels.h's on-disk int4/int8 interpretation for diagnostics."""
    rows, cols = shape
    if bits == 8:
        values = q.view(np.int8).reshape(rows, cols).astype(np.float32)
    else:
        packed = q.reshape(rows, (cols + 1) // 2)
        values = np.empty((rows, cols), dtype=np.float32)
        values[:, 0::2] = (packed & 0x0F).astype(np.int16) - 8
        if cols > 1:
            values[:, 1::2] = (packed[:, :cols // 2] >> 4).astype(np.int16) - 8
    if bits == 4 and group_size:
        groups = (cols + group_size - 1) // group_size
        expanded = np.repeat(scales.reshape(rows, groups), group_size, axis=1)[:, :cols]
        return values * expanded
    return values * scales[:, None]

def quantize_with_stats(w, bits, stats, group, group_size=0):
    if bits == 8:
        q, scales = quant_int8(w)
    elif group_size:
        q, scales = quant_int4_grouped(w, group_size)
    else:
        q, scales = quant_int4_rowwise(w)
    if stats is not None:
        stats.add(group, w, dequantize(q, scales, w.shape, bits, group_size))
    return q, scales


def quantize_expert_blob(gate_w, up_w, down_w, gate_up_bits=4,
                         down_bits=4, group_size=0, stats=None):
    """Encode one aligned expert blob, including the mixed q4/q8 candidate.

    Projection order remains gate weights/scales, up weights/scales, then down
    weights/scales. Regular mixed experts use grouped q4 for gate/up and
    row-q8 for down; MTP experts continue to pass 8/8 with group_size zero.
    """
    if gate_up_bits not in (4, 8) or down_bits not in (4, 8):
        raise ValueError("expert projection bits must be 4 or 8")
    q_group = group_size if gate_up_bits == 4 else 0
    gate_group = f"experts-int{gate_up_bits}" + (f"-group{q_group}" if q_group else "-row")
    down_group = f"experts-int{down_bits}" + (
        f"-group{group_size}" if down_bits == 4 and group_size else "-row")
    q_gate, s_gate = quantize_with_stats(
        gate_w, gate_up_bits, stats, gate_group, q_group)
    q_up, s_up = quantize_with_stats(
        up_w, gate_up_bits, stats, gate_group, q_group)
    q_down, s_down = quantize_with_stats(
        down_w, down_bits, stats, down_group,
        group_size if down_bits == 4 else 0)
    blob = (q_gate.tobytes() + s_gate.tobytes() +
            q_up.tobytes() + s_up.tobytes() +
            q_down.tobytes() + s_down.tobytes())
    pad = (-len(blob)) % ALIGNMENT_BYTES
    return blob + (b"\x00" * pad)

def quantize_tensor_rows(handle, bits, stats, group, rows_per_chunk):
    """Quantize a safetensors slice without materializing a full bf16 tensor.

    The embedding and lm-head matrices are each gigabytes when expanded to
    float32.  Reading output rows in bounded chunks keeps the conversion
    usable on the 16 GB target machine.
    """
    import torch
    shape = tuple(handle.get_shape())
    if len(shape) != 2:
        raise ValueError(f"expected matrix tensor, got shape {shape}")
    rows, cols = shape
    row_bytes = cols if bits == 8 else (cols + 1) // 2
    # Preallocate the final container layout.  Accumulating chunk arrays and
    # concatenating them temporarily doubles the embedding/lm-head footprint.
    packed = np.empty(rows * row_bytes, dtype=np.uint8)
    scales = np.empty(rows, dtype=np.float32)
    for start in range(0, rows, rows_per_chunk):
        stop = min(rows, start + rows_per_chunk)
        w = handle[start:stop].to(torch.float32).numpy()
        q, chunk_scales = quantize_with_stats(w, bits, stats, group)
        packed[start * row_bytes:stop * row_bytes] = q
        scales[start:stop] = chunk_scales
        del w
        gc.collect()
    return packed, scales

def expert_sort_key(name):
    """Canonical numeric order, rather than safetensors' lexical key order."""
    parts = name.split(".")
    return int(parts[2]), int(parts[5])


def repack_experts(source_path, destination_path, append_manifest):
    """Build the final canonical experts.bin without mutating resumable input.

    The converter writes blobs as soon as their source tensors become
    available.  That order differs between the fused and individual checkpoint
    layouts.  A separate final repack gives both layouts identical output while
    leaving the append file and its checkpoint state intact if final publishing
    is interrupted.
    """
    final_manifest = {key: value for key, value in append_manifest.items()
                      if key != "experts"}
    final_manifest["alignment_kb"] = ALIGNMENT_BYTES // 1024
    final_manifest["experts"] = {}
    tmp = destination_path + ".tmp"
    try:
        offset = 0
        with open(source_path, "rb") as src, open(tmp, "wb") as dst:
            for name in sorted(append_manifest["experts"], key=expert_sort_key):
                entry = append_manifest["experts"][name]
                src.seek(entry["offset"])
                blob = src.read(entry["size"])
                if (len(blob) != entry["size"] or
                        hashlib.sha256(blob).hexdigest() != entry["sha256"]):
                    raise RuntimeError(f"invalid committed expert blob: {name}")
                dst.write(blob)
                final_manifest["experts"][name] = {
                    "offset": offset,
                    "size": len(blob),
                    "sha256": entry["sha256"],
                }
                offset += len(blob)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(tmp, destination_path)
        fsync_dir(os.path.dirname(destination_path) or ".")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return final_manifest


def manifest_is_canonical(manifest):
    """Return true when append order already matches numeric layer/expert order."""
    cursor = 0
    items = manifest.get("experts", {})
    try:
        for name in sorted(items, key=expert_sort_key):
            entry = items[name]
            if int(entry["offset"]) != cursor:
                return False
            cursor += int(entry["size"])
    except (KeyError, TypeError, ValueError, IndexError):
        return False
    return True


def manifest_is_dense(manifest, committed_bytes=None):
    """Return true when manifest regions exactly tile the append container.

    Runtime lookup is manifest-driven, so numeric layer order is not a format
    requirement.  Publishing a validated dense append container directly
    avoids a second full-store write solely to reorder otherwise identical
    expert blobs.
    """
    cursor = 0
    items = manifest.get("experts", {})
    if not items:
        return False
    try:
        for entry in sorted(items.values(), key=lambda item: int(item["offset"])):
            offset = int(entry["offset"])
            size = int(entry["size"])
            if offset != cursor or size <= 0:
                return False
            cursor += size
    except (KeyError, TypeError, ValueError):
        return False
    return committed_bytes is None or cursor == committed_bytes

def get_layer_idx(name):
    """Extract layer index from tensor name."""
    parts = name.split(".")
    for i, p in enumerate(parts):
        if p == "layers" and i > 0:
            try:
                return int(parts[i+1])
            except ValueError:
                pass
    return -1

def is_mtp(name):
    """Check if tensor belongs to MTP head."""
    return name.startswith("mtp.")

def parse_expert_name(name):
    """Parse name like 'model.layers.0.mlp.experts.3.gate_proj.weight'.
    Returns (layer_idx, expert_idx, 'gate'|'up'|'down') or None.
    """
    parts = name.split(".")
    for i, p in enumerate(parts):
        if p == "experts" and i > 0:
            try:
                expert_idx = int(parts[i+1])
                proj = parts[i+2] # 'gate_proj', 'up_proj' or 'down_proj'
                proj_type = proj.replace("_proj", "").replace(".weight", "")
                if proj_type not in ("gate", "up", "down"):
                    continue
                
                # find layer idx
                layer_idx = -1
                for j in range(i):
                    if parts[j] == "layers":
                        layer_idx = int(parts[j+1])
                        break
                return layer_idx, expert_idx, proj_type
            except (ValueError, IndexError):
                pass
    return None

def classify_tensor(name, has_moe, mtp_layer_idx):
    """Classify tensor type.
    Returns: 'skip', 'f32', 'x_gup', 'x_down', 'x_ind', 'q'
    """
    # Skip vision tower and indexers
    if any(k in name for k in ["vision", "indexer", "indexers_proj", "eh_proj", "shared_head"]):
        return "skip"
    if name.endswith("_scale_inv"):
        return "skip"
        
    li = get_layer_idx(name)
    
    # Check for MoE experts
    if ".mlp.experts." in name:
        if parse_expert_name(name) is not None:
            return "x_ind"
        if name.endswith("gate_up_proj") or name.endswith("gate_up_proj.weight"):
            return "x_gup"
        if name.endswith("down_proj") or name.endswith("down_proj.weight"):
            return "x_down"
        return "skip"
        
    # Check for norms and router gate/bias
    if name.endswith("norm.weight") or name == "model.norm.weight" or name.endswith("layernorm.weight"):
        return "f32"
    if name.endswith("mlp.gate.weight") or name.endswith("e_score_correction_bias") or name.endswith("mlp.shared_expert_gate.weight"):
        return "f32"
        
    # Embeddings, LM Head, and linear projections are quantized
    if name.endswith(".weight") or name in ("model.embed_tokens.weight", "lm_head.weight"):
        return "q"
        
    return "f32"

def process_and_quantize(name, w, is_mtp_tensor, stats):
    """Quantize tensor depending on its type and whether it belongs to MTP."""
    if w.ndim != 2:
        return w.astype(np.float32), None # norm/bias/1D stays float32
        
    bits = 8 if is_mtp_tensor else 4
    q, s = quantize_with_stats(w, bits, stats, f"resident-int{bits}")
    return q, s

def main():
    """Convert with a shard-boundary write-ahead checkpoint.

    Final artifacts retain the original public layout.  The only additional
    files are dot-prefixed work files, which make a killed conversion
    resumable without ever presenting a half-normalized experts.bin as final.
    """
    parser = argparse.ArgumentParser(description="Convert Qwen3.6 weights to Stage-B format.")
    parser.add_argument("--repo", default="Qwen/Qwen3.6-35B-A3B", help="HF Repo ID")
    parser.add_argument("--revision", default=None, help="Optional immutable HF revision")
    parser.add_argument("--indir", default=None, help="Local directory containing BF16 safetensors")
    parser.add_argument("--outdir", required=True, help="Output directory for Stage-B weights")
    parser.add_argument("--min-free-gb", type=float, default=25.0,
                        help="Minimum free disk space margin")
    parser.add_argument("--rows-per-chunk", type=int, default=256,
                        help="Maximum matrix rows expanded to float32 at once (default: 256)")
    parser.add_argument("--staging-dir", default=None,
                        help="Disposable local shard directory (default: OUTDIR/.staging)")
    parser.add_argument("--resident-bits", type=int, choices=(4, 8), default=4,
                        help="Quantization bits for non-MTP resident matrices (default: 4)")
    parser.add_argument("--expert-group-size", type=int, default=0,
                        help="Expert int4 scale group (0 keeps legacy whole-row scales; recommended: 32)")
    parser.add_argument("--expert-down-bits", type=int, choices=(4, 8), default=4,
                        help="Regular expert down-projection bits (8 requires grouped q4 gate/up)")
    parser.add_argument("--reuse-experts-from", default=None, metavar="DIR",
                        help="Reuse DIR/experts.bin and manifest.json; convert resident tensors only")
    parser.add_argument("--no-resume", action="store_true", help="Discard saved conversion work")
    parser.add_argument("--stop-after-shard", type=int, default=0,
                        help="Test hook: exit after checkpointing this 1-based shard index")
    args = parser.parse_args()
    if args.expert_group_size < 0 or (args.expert_group_size and
                                     (args.expert_group_size < 2 or args.expert_group_size % 2)):
        parser.error("--expert-group-size must be 0 or a positive even integer >= 2")
    if args.expert_down_bits == 8 and not args.expert_group_size:
        parser.error("--expert-down-bits 8 requires --expert-group-size")
    if args.rows_per_chunk < 1:
        parser.error("--rows-per-chunk must be positive")
    if args.stop_after_shard < 0:
        parser.error("--stop-after-shard must not be negative")
    if (args.reuse_experts_from and
            os.path.abspath(args.reuse_experts_from) == os.path.abspath(args.outdir)):
        parser.error("--reuse-experts-from must differ from --outdir")

    from safetensors import safe_open
    from safetensors.numpy import save_file
    import torch

    os.makedirs(args.outdir, exist_ok=True)
    staging_dir = args.staging_dir or os.path.join(args.outdir, ".staging")
    if not args.indir:
        os.makedirs(staging_dir, exist_ok=True)

    reuse_experts_path = None
    reuse_manifest = None
    reuse_identity = None
    if args.reuse_experts_from:
        reuse_dir = os.path.abspath(args.reuse_experts_from)
        reuse_experts_path = os.path.join(reuse_dir, "experts.bin")
        reuse_manifest_path = os.path.join(reuse_dir, "manifest.json")
        if not os.path.isfile(reuse_experts_path) or not os.path.isfile(reuse_manifest_path):
            raise RuntimeError("--reuse-experts-from requires experts.bin and manifest.json")
        with open(reuse_manifest_path, "r", encoding="utf-8") as f:
            reuse_manifest = json.load(f)
        reuse_bytes = os.path.getsize(reuse_experts_path)
        if not validate_manifest_blobs(reuse_experts_path, reuse_manifest, reuse_bytes):
            raise RuntimeError("reused expert container fails manifest validation")
        reuse_quant = reuse_manifest.get("expert_quantization")
        reuse_group = int(reuse_quant.get("group_size", 0)) if reuse_quant else 0
        reuse_down_bits = int(reuse_quant.get("down_bits", 4)) if reuse_quant else 4
        if (reuse_group != args.expert_group_size or
                reuse_down_bits != args.expert_down_bits):
            raise RuntimeError("expert quantization options do not match the reused expert container")
        reuse_identity = {
            "manifest_sha256": stable_json_sha256(reuse_manifest),
            "experts_bytes": reuse_bytes,
        }
        print(f"Reusing validated expert container: {reuse_experts_path}")

    # Resolve the index.  A local single-file fixture deliberately has no
    # index, so synthesize the same mapping used by a real indexed checkpoint.
    index_filename = "model.safetensors.index.json"
    index_data = None
    if args.indir:
        single_path = os.path.join(args.indir, "model.safetensors")
        index_path = os.path.join(args.indir, index_filename)
        if os.path.exists(index_path):
            with open(index_path, "r", encoding="utf-8") as f:
                index_data = json.load(f)
        elif os.path.exists(single_path):
            print("No index found, but detected single model.safetensors. Processing it directly...")
            with safe_open(single_path, framework="pt") as f:
                index_data = {"weight_map": {k: "model.safetensors" for k in f.keys()}}
    else:
        from huggingface_hub import hf_hub_download
        print(f"Downloading index {index_filename} from HF repo {args.repo}...")
        try:
            index_path = hf_hub_download(repo_id=args.repo, filename=index_filename,
                                         revision=args.revision)
            with open(index_path, "r", encoding="utf-8") as f:
                index_data = json.load(f)
        except Exception as e:
            print(f"Failed to download index: {e}. Looking in cache...")
    if not index_data:
        cache_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                  "qwen_metadata_cache.json")
        if os.path.exists(cache_path):
            with open(cache_path, "r", encoding="utf-8") as f:
                cache = json.load(f)
            if args.repo in cache:
                index_data = {"weight_map": {k: "dummy" for k in cache[args.repo]}}
    if not index_data or "weight_map" not in index_data:
        raise RuntimeError("could not retrieve model index mapping")

    # Config is both a conversion input (MTP quantization selection) and a
    # required final runtime artifact.  Retain its path for atomic publishing.
    config_data = None
    cfg_path = None
    generation_cfg_path = None
    if args.indir:
        cfg_path = os.path.join(args.indir, "config.json")
        if os.path.exists(cfg_path):
            with open(cfg_path, "r", encoding="utf-8") as f:
                config_data = json.load(f)
        candidate = os.path.join(args.indir, "generation_config.json")
        generation_cfg_path = candidate if os.path.exists(candidate) else None
    else:
        from huggingface_hub import hf_hub_download
        try:
            cfg_path = hf_hub_download(repo_id=args.repo, filename="config.json",
                                       revision=args.revision)
            with open(cfg_path, "r", encoding="utf-8") as f:
                config_data = json.load(f)
        except Exception as e:
            print(f"Failed to download config.json: {e}")
        try:
            generation_cfg_path = hf_hub_download(
                repo_id=args.repo, filename="generation_config.json", revision=args.revision)
        except Exception:
            # Optional for the C runner; config.json is not optional.
            generation_cfg_path = None
    if not config_data or not cfg_path:
        raise RuntimeError("could not load config.json to determine num_hidden_layers")
    text_cfg = config_data.get("text_config", config_data)
    num_hidden_layers = int(text_cfg["num_hidden_layers"])
    mtp_layer_idx = num_hidden_layers
    print(f"num_hidden_layers={num_hidden_layers} -> MTP layer index = {mtp_layer_idx}")

    weight_map = index_data["weight_map"]
    shard_to_weights = {}
    for weight_name, shard_name in weight_map.items():
        shard_to_weights.setdefault(shard_name, []).append(weight_name)
    shards = sorted(shard_to_weights)
    print(f"Found {len(shards)} shards.")

    signature = {
        "input": {
            "repo": None if args.indir else args.repo,
            "indir": os.path.abspath(args.indir) if args.indir else None,
            "revision": args.revision,
            "weight_map_sha256": stable_json_sha256(weight_map),
            "config_sha256": stable_json_sha256(config_data),
            "shards": shards,
        },
        "format": {
            "alignment_bytes": ALIGNMENT_BYTES,
            "quantization": "rowwise-symmetric-q4-q8-v1",
            "resident_bits": args.resident_bits,
            "expert_group_size": args.expert_group_size,
            "expert_down_bits": args.expert_down_bits,
            "mtp_layer_idx": mtp_layer_idx,
            "rows_per_chunk": args.rows_per_chunk,
            "reused_experts": reuse_identity,
        },
    }

    state_path = os.path.join(args.outdir, STATE_FILE)
    experts_work_path = os.path.join(args.outdir, EXPERTS_PARTIAL)
    final_experts_path = os.path.join(args.outdir, "experts.bin")
    final_resident_path = os.path.join(args.outdir, "resident.safetensors")
    final_manifest_path = os.path.join(args.outdir, "manifest.json")

    def clear_work():
        remove_if_exists(state_path)
        remove_if_exists(state_path + ".tmp")
        remove_if_exists(experts_work_path)
        remove_if_exists(experts_work_path + ".tmp")
        for path in glob.glob(os.path.join(args.outdir, RESIDENT_PARTIAL_PREFIX + "*.safetensors*")):
            remove_if_exists(path)
        fsync_dir(args.outdir)

    if args.no_resume:
        clear_work()

    manifest = {"alignment_kb": ALIGNMENT_BYTES // 1024, "experts": {}}
    if args.expert_group_size:
        manifest["expert_quantization"] = {
            "format": ("groupwise-q4-gate-up-row-q8-down-v1"
                       if args.expert_down_bits == 8
                       else "groupwise-symmetric-q4-v1"),
            "group_size": args.expert_group_size,
            "down_bits": args.expert_down_bits,
        }
    resident_dict = {}
    pending_fused = {}
    pending_individual = {}
    quant_stats = QuantStats()
    next_shard = 0
    resident_info = None

    if not args.no_resume and os.path.exists(state_path):
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
        if state.get("schema") != STATE_SCHEMA or state.get("signature") != signature:
            raise RuntimeError("resume state belongs to different input/options; rerun with --no-resume")
        try:
            next_shard = int(state["next_shard"])
            append = state["append"]
            committed_bytes = int(append["bytes"])
            manifest = append["manifest"]
            resident_info = state["resident"]
            pending = state.get("pending", {})
            pending_fused = pending.get("fused", {})
            pending_individual = pending.get("individual", {})
        except (KeyError, TypeError, ValueError) as e:
            raise RuntimeError("malformed resume state; rerun with --no-resume") from e
        if not (0 <= next_shard <= len(shards)):
            raise RuntimeError("resume state has an invalid next shard")
        if state.get("completed_shards") != shards[:next_shard]:
            raise RuntimeError("resume state does not describe a shard prefix; rerun with --no-resume")
        if not isinstance(pending_fused, dict) or not isinstance(pending_individual, dict):
            raise RuntimeError("resume state has malformed pending expert references")
        if not os.path.isfile(experts_work_path):
            raise RuntimeError("resume state is missing its expert append file; rerun with --no-resume")
        actual_bytes = os.path.getsize(experts_work_path)
        if actual_bytes < committed_bytes:
            raise RuntimeError("expert append file is shorter than committed state; rerun with --no-resume")
        if actual_bytes > committed_bytes:
            print(f"Discarding {actual_bytes - committed_bytes} uncommitted expert bytes...")
            with open(experts_work_path, "r+b") as f:
                f.truncate(committed_bytes)
                f.flush()
                os.fsync(f.fileno())
        if not validate_manifest_blobs(experts_work_path, manifest, committed_bytes):
            raise RuntimeError("committed expert blobs fail validation; rerun with --no-resume")
        resident_path = validate_resident_snapshot(args.outdir, resident_info)
        if not resident_path:
            raise RuntimeError("committed resident snapshot fails validation; rerun with --no-resume")
        with safe_open(resident_path, framework="np") as f:
            resident_dict = {name: f.get_tensor(name) for name in f.keys()}
        quant_stats = QuantStats.from_state(state.get("quant_stats"))
        print(f"Resuming: {next_shard}/{len(shards)} shard(s), "
              f"{len(manifest['experts'])} expert blobs")
    else:
        # Work left without a state was never committed and must not be reused.
        clear_work()
        # `local_dir` is intentionally disposable.  A new conversion must not
        # trust a same-named shard left by a different aborted source/revision.
        if not args.indir:
            for shard_name in shards:
                stale = os.path.join(staging_dir, shard_name)
                if os.path.isfile(stale):
                    os.remove(stale)
        with open(experts_work_path, "wb") as f:
            f.flush()
            os.fsync(f.fileno())

    def require_free_space():
        total, used, free = shutil.disk_usage(args.outdir)
        free_gb = free / 1e9
        if free_gb < args.min_free_gb:
            raise RuntimeError(f"disk space margin low ({free_gb:.2f} GB free, "
                               f"requested {args.min_free_gb} GB)")

    def shard_path(shard_name):
        if args.indir:
            path = os.path.join(args.indir, shard_name)
            if os.path.exists(path):
                return path
            matches = glob.glob(os.path.join(args.indir, shard_name))
            if matches:
                return matches[0]
            raise RuntimeError(f"shard file {path} not found")
        candidate = os.path.join(staging_dir, shard_name)
        if os.path.isfile(candidate):
            return candidate
        require_free_space()
        from huggingface_hub import hf_hub_download
        print(f"Downloading shard {shard_name} from HF...")
        return hf_hub_download(repo_id=args.repo, filename=shard_name,
                               local_dir=staging_dir, revision=args.revision)

    def make_ref(shard_name, tensor_name):
        return {"shard": shard_name, "tensor": tensor_name}

    def resolve_ref(ref):
        if not isinstance(ref, dict):
            raise RuntimeError("malformed pending expert reference")
        shard_name = ref.get("shard")
        tensor_name = ref.get("tensor")
        if not isinstance(shard_name, str) or not isinstance(tensor_name, str):
            raise RuntimeError("malformed pending expert reference")
        return shard_path(shard_name), tensor_name

    def pending_shards():
        result = set()
        for collection in (pending_fused, pending_individual):
            for projections in collection.values():
                if not isinstance(projections, dict):
                    raise RuntimeError("malformed pending expert references")
                for ref in projections.values():
                    if isinstance(ref, dict) and isinstance(ref.get("shard"), str):
                        result.add(ref["shard"])
        return result

    def cleanup_staging():
        if args.indir:
            return
        keep = pending_shards()
        for shard_name in shards:
            path = os.path.join(staging_dir, shard_name)
            if shard_name not in keep and os.path.isfile(path):
                print(f"Deleting staged shard {path}...")
                os.remove(path)

    def expert_blob(gate_w, up_w, down_w, bits):
        if bits == 8:
            return quantize_expert_blob(gate_w, up_w, down_w, 8, 8, 0, quant_stats)
        return quantize_expert_blob(gate_w, up_w, down_w, 4,
                                    args.expert_down_bits,
                                    args.expert_group_size, quant_stats)

    def append_expert(layer_idx, expert_idx, blob):
        key = f"model.layers.{layer_idx}.mlp.experts.{expert_idx}"
        if key in manifest["experts"]:
            raise RuntimeError(f"duplicate expert blob: {key}")
        offset = os.path.getsize(experts_work_path)
        if offset % ALIGNMENT_BYTES:
            raise RuntimeError("expert append file lost its 16 KB alignment")
        digest = hashlib.sha256(blob).hexdigest()
        with open(experts_work_path, "ab") as f:
            f.write(blob)
            f.flush()
        manifest["experts"][key] = {
            "offset": offset,
            "size": len(blob),
            "sha256": digest,
        }

    def load_ref_tensor(ref):
        path, tensor_name = resolve_ref(ref)
        with safe_open(path, framework="pt") as f:
            return f.get_tensor(tensor_name).to(torch.float32).numpy()

    def write_individual_expert(layer_idx, expert_idx, refs):
        required = {"gate", "up", "down"}
        if set(refs) != required:
            raise RuntimeError(f"incomplete individual expert {layer_idx}/{expert_idx}")
        gate_w = load_ref_tensor(refs["gate"])
        up_w = load_ref_tensor(refs["up"])
        down_w = load_ref_tensor(refs["down"])
        try:
            bits = 8 if layer_idx == mtp_layer_idx else 4
            append_expert(layer_idx, expert_idx, expert_blob(gate_w, up_w, down_w, bits))
        finally:
            del gate_w, up_w, down_w

    def write_fused_layer(layer_idx, refs):
        gup_path, gup_name = resolve_ref(refs["x_gup"])
        down_path, down_name = resolve_ref(refs["x_down"])
        print(f"-> Processing experts for layer {layer_idx}...")
        with safe_open(gup_path, framework="pt") as fg, safe_open(down_path, framework="pt") as fd:
            gup = fg.get_slice(gup_name)
            down = fd.get_slice(down_name)
            gshape, dshape = tuple(gup.get_shape()), tuple(down.get_shape())
            if (len(gshape) != 3 or len(dshape) != 3 or gshape[0] != dshape[0] or
                    gshape[1] % 2 or dshape[1] != gshape[2] or dshape[2] != gshape[1] // 2):
                raise ValueError(f"unexpected fused expert shapes: {gshape}, {dshape}")
            num_experts, two_inter, _ = gshape
            moe_inter = two_inter // 2
            bits = 8 if layer_idx == mtp_layer_idx else 4
            for expert_idx in range(num_experts):
                gate_w = gup[expert_idx, :moe_inter, :].to(torch.float32).numpy()
                up_w = gup[expert_idx, moe_inter:, :].to(torch.float32).numpy()
                down_w = down[expert_idx, :, :].to(torch.float32).numpy()
                try:
                    append_expert(layer_idx, expert_idx,
                                  expert_blob(gate_w, up_w, down_w, bits))
                finally:
                    del gate_w, up_w, down_w

    def checkpoint(after_shard):
        """Commit all effects of a shard in data-before-journal order."""
        fsync_file(experts_work_path)
        snapshot = resident_snapshot_path(args.outdir, after_shard)
        atomic_save_safetensors(snapshot, resident_dict, save_file)
        info = {
            "path": os.path.basename(snapshot),
            "size": os.path.getsize(snapshot),
            "sha256": sha256_file(snapshot),
        }
        state = {
            "schema": STATE_SCHEMA,
            "signature": signature,
            "next_shard": after_shard,
            "completed_shards": shards[:after_shard],
            "append": {
                "bytes": os.path.getsize(experts_work_path),
                "manifest": manifest,
            },
            "resident": info,
            "pending": {
                "fused": pending_fused,
                "individual": pending_individual,
            },
            "quant_stats": quant_stats.to_state(),
        }
        atomic_json(state_path, state)
        # State now points at this generation; old snapshots are expendable.
        for path in glob.glob(os.path.join(args.outdir, RESIDENT_PARTIAL_PREFIX + "*.safetensors")):
            if os.path.abspath(path) != os.path.abspath(snapshot):
                remove_if_exists(path)
        return info

    # Convert in stable shard order.  Only compact JSON-safe tensor references
    # survive a boundary, never a bf16/f32 expert array.
    for idx in range(next_shard, len(shards)):
        shard_name = shards[idx]
        print(f"\nProcessing shard [{idx + 1}/{len(shards)}]: {shard_name}")
        current_path = shard_path(shard_name)
        with safe_open(current_path, framework="pt") as f_in:
            for name in f_in.keys():
                kind = classify_tensor(name, has_moe=True, mtp_layer_idx=mtp_layer_idx)
                if kind == "skip":
                    continue
                if reuse_manifest is not None and kind in ("x_ind", "x_gup", "x_down"):
                    continue
                is_mtp_tensor = is_mtp(name) or get_layer_idx(name) == mtp_layer_idx
                if kind == "f32":
                    resident_dict[name] = f_in.get_tensor(name).to(torch.float32).numpy()
                elif kind == "q":
                    bits = 8 if is_mtp_tensor else args.resident_bits
                    handle = f_in.get_slice(name)
                    if len(handle.get_shape()) == 2:
                        q, scales = quantize_tensor_rows(handle, bits, quant_stats,
                                                         f"resident-int{bits}", args.rows_per_chunk)
                        resident_dict[name] = q
                        resident_dict[name + ".qs"] = scales
                    else:
                        resident_dict[name] = f_in.get_tensor(name).to(torch.float32).numpy()
                elif kind == "x_ind":
                    parsed = parse_expert_name(name)
                    if parsed is None:
                        raise RuntimeError(f"could not parse individual expert tensor {name}")
                    layer_idx, expert_idx, projection = parsed
                    if is_mtp_tensor:
                        layer_idx = mtp_layer_idx
                    key = f"{layer_idx}:{expert_idx}"
                    refs = pending_individual.setdefault(key, {})
                    ref = make_ref(shard_name, name)
                    if projection in refs and refs[projection] != ref:
                        raise RuntimeError(f"duplicate individual expert projection {key}/{projection}")
                    refs[projection] = ref
                    if set(refs) == {"gate", "up", "down"}:
                        write_individual_expert(layer_idx, expert_idx, refs)
                        del pending_individual[key]
                elif kind in ("x_gup", "x_down"):
                    layer_idx = mtp_layer_idx if is_mtp_tensor else get_layer_idx(name)
                    if layer_idx < 0:
                        raise RuntimeError(f"could not determine fused expert layer for {name}")
                    key = str(layer_idx)
                    refs = pending_fused.setdefault(key, {})
                    ref = make_ref(shard_name, name)
                    if kind in refs and refs[kind] != ref:
                        raise RuntimeError(f"duplicate fused expert projection {key}/{kind}")
                    refs[kind] = ref
                    if set(refs) == {"x_gup", "x_down"}:
                        write_fused_layer(layer_idx, refs)
                        del pending_fused[key]

        resident_info = checkpoint(idx + 1)
        cleanup_staging()  # Never delete a staged source before its state is durable.
        gc.collect()
        if args.stop_after_shard == idx + 1:
            print(f"Stopped after durable checkpoint {idx + 1} (test hook).")
            raise SystemExit(75)

    if pending_fused or pending_individual:
        pending_names = sorted(list(pending_fused) + list(pending_individual))
        raise RuntimeError(f"checkpoint ended with incomplete expert projections: {pending_names[:5]}")
    if resident_info is None:
        raise RuntimeError("conversion produced no resident checkpoint")

    committed_bytes = os.path.getsize(experts_work_path)
    if not validate_manifest_blobs(experts_work_path, manifest, committed_bytes):
        raise RuntimeError("cannot finalize: committed expert blobs fail validation")
    resident_path = validate_resident_snapshot(args.outdir, resident_info)
    if not resident_path:
        raise RuntimeError("cannot finalize: resident snapshot fails validation")

    # Finalization is intentionally separate from the append journal.  If a
    # process dies here, state and the original append offsets still resume.
    if reuse_manifest is not None:
        atomic_link_or_copy_file(reuse_experts_path, final_experts_path)
        final_manifest = reuse_manifest
        if (not os.path.samefile(reuse_experts_path, final_experts_path) and
                not validate_manifest_blobs(final_experts_path, final_manifest,
                                            os.path.getsize(final_experts_path))):
            raise RuntimeError("copied expert container fails validation")
    else:
        if manifest_is_dense(manifest, committed_bytes):
            # Offsets are authoritative at runtime; numeric layer order is not
            # a container invariant. A hard link publishes validated dense
            # append bytes atomically without temporarily doubling a 20+ GB
            # expert store merely to reorder identical blobs. The append
            # journal remains linked until manifest publication, so an
            # interrupted finalization is still resumable.
            atomic_link_or_copy_file(experts_work_path, final_experts_path)
            final_manifest = manifest
        else:
            final_manifest = repack_experts(experts_work_path, final_experts_path, manifest)
        if not validate_manifest_blobs(final_experts_path, final_manifest,
                                       os.path.getsize(final_experts_path)):
            raise RuntimeError("final expert repack validation failed")
    atomic_copy_file(resident_path, final_resident_path)
    atomic_copy_file(cfg_path, os.path.join(args.outdir, "config.json"))
    if generation_cfg_path and os.path.exists(generation_cfg_path):
        atomic_copy_file(generation_cfg_path,
                         os.path.join(args.outdir, "generation_config.json"))
    if args.indir:
        tokenizer_path = os.path.join(args.indir, "tokenizer.json")
        if os.path.exists(tokenizer_path):
            atomic_copy_file(tokenizer_path, os.path.join(args.outdir, "tokenizer.json"))

    # Manifest publication is the commit marker visible to qwen36b: it is
    # deliberately last, after every referenced artifact is durable.
    atomic_json(final_manifest_path, final_manifest)

    remove_if_exists(state_path)
    remove_if_exists(experts_work_path)
    for path in glob.glob(os.path.join(args.outdir, RESIDENT_PARTIAL_PREFIX + "*.safetensors")):
        remove_if_exists(path)
    fsync_dir(args.outdir)
    quant_stats.report()
    print("\nConversion Completed successfully!")


if __name__ == "__main__":
    main()
