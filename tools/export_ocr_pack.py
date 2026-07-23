#!/usr/bin/env python3
"""Export the pinned PP-OCRv6 det+rec weights into a Samosa OCR flat pack.

R1 of docs/TASKS_READER.md. Offline, one-time. Produces the opt-in,
manifest-pinned pack the `samosa-ocr` sidecar loads at runtime:

    ocr-pack-v1/
      det.bin        # PP-OCRv6 small det weights, Samosa flat format
      rec.bin        # PP-OCRv6 small rec weights
      charset.txt    # one character per line, index order (blank, dict..., space)
      manifest.json  # contract version, pack fingerprint, per-file SHA, source pins

Decision 8 (owner, 2026-07-23): every downloaded file must be Apache-2.0 or
MIT, verified against the *exact files at the pinned revision*. This tool, at
download time: (1) re-reads the HF license tag at the pinned revision,
(2) downloads at that revision, (3) checks payload byte sizes against the
spec table, (4) records SHA-256 of every file. Any mismatch is stop-and-report,
never substitute.

Usage:
    python tools/export_ocr_pack.py --tier small --out ~/.samosa/models/ocr-pack-v1
    python tools/export_ocr_pack.py --tier medium --out /tmp/ocr-pack-medium   # E-R1(e)

No network at inference. This is the only place HF is touched.
"""

import argparse
import datetime
import hashlib
import json
import os
import struct
import sys
import urllib.request

# --- The pins. Single source of truth mirrors TASKS_READER.md "Model pins". ---
# license: expected HF `license` tag at the revision (decision 8).
# files: filename -> expected exact byte count (None = size not asserted).
PINS = {
    "small": {
        "det": {
            "repo": "PaddlePaddle/PP-OCRv6_small_det_safetensors",
            "revision": "eae2ee920a39fb3087637d3dbb58df1896ec1f24",
            "license": "apache-2.0",
            "files": {"model.safetensors": 9938124, "config.json": None,
                      "inference.yml": None, "preprocessor_config.json": None},
        },
        "rec": {
            "repo": "PaddlePaddle/PP-OCRv6_small_rec_safetensors",
            "revision": "fe049fb103f57443fe8840c54ed06b702f3c1de5",
            "license": "apache-2.0",
            "files": {"model.safetensors": 21204736, "config.json": None,
                      "inference.yml": 150579, "preprocessor_config.json": None},
        },
    },
    "medium": {
        "det": {
            "repo": "PaddlePaddle/PP-OCRv6_medium_det_safetensors",
            "revision": "4236c2b61741a259c091fd879dcc4edc339e916c",
            "license": "apache-2.0",
            "files": {"model.safetensors": 88020412, "config.json": None,
                      "inference.yml": None, "preprocessor_config.json": None},
        },
        "rec": {
            "repo": "PaddlePaddle/PP-OCRv6_medium_rec_safetensors",
            "revision": "024cad6a831de75c2c3c26e711ba8c4a82ccd24b",
            "license": "apache-2.0",
            "files": {"model.safetensors": 76741720, "config.json": None,
                      "inference.yml": None, "preprocessor_config.json": None},
        },
    },
}

CONTRACT_VERSION = "reader-v0"

# Detector preprocessing, verified against paddle's captured model input
# (RGB range [-2.118, 2.640] on a white/black fixture => these mean/std,
#  BGR channel read, max-side cap 960, size rounded to a /32 multiple).
DET_PRE = {
    "read_order": "BGR", "scale": 1.0 / 255.0,
    "mean": [0.485, 0.456, 0.406], "std": [0.229, 0.224, 0.225],
    "limit_type": "max", "limit_side_len": 960, "size_multiple": 32,
    "max_side_limit": 4000,
}
# DB postprocess defaults; the pinned det inference.yml overrides thresh /
# box_thresh / unclip_ratio / max_candidates at export time (verified: the
# PP-OCRv6_small_det pin ships thresh 0.2, box_thresh 0.45, unclip_ratio 1.4).
DET_POST = {"thresh": 0.3, "box_thresh": 0.6, "unclip_ratio": 2.0,
            "max_candidates": 1000, "min_size": 3, "score_mode": "fast"}


def extract_det_postprocess(det_inference_yml):
    import yaml
    y = yaml.safe_load(open(det_inference_yml))
    pp = y.get("PostProcess", {}) or {}
    post = dict(DET_POST)
    for k in ("thresh", "box_thresh", "unclip_ratio", "max_candidates"):
        if k in pp:
            post[k] = pp[k]
    return post
# Recognizer preprocessing (OCRReisizeNormImg): resize to h=48 keeping aspect,
# normalise to [-1, 1], right-pad width.
REC_PRE = {"img_h": 48, "img_w": 320, "max_wh_ratio": 320.0 / 48.0,
           "max_img_w": 3200, "norm": "pm1"}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def hf_license_at_revision(repo, revision):
    from huggingface_hub import HfApi

    info = HfApi().model_info(repo, revision=revision, files_metadata=False)
    if info.sha != revision:
        raise SystemExit(f"STOP: {repo} resolved sha {info.sha} != pin {revision}")
    return (info.card_data or {}).get("license") if info.card_data else None


def safetensors_header(path):
    with open(path, "rb") as f:
        (n,) = struct.unpack("<Q", f.read(8))
        return json.loads(f.read(n)), 8 + n


def load_safetensors_f32(path):
    """Return {name: numpy float32 array}. Minimal header+buffer parser, no
    torch/paddle (decision 8 binding note: safetensors payloads only)."""
    import numpy as np

    hdr, data_off = safetensors_header(path)
    hdr.pop("__metadata__", None)
    dt = {"F32": np.float32, "F16": np.float16, "BF16": None}
    out = {}
    raw = open(path, "rb").read()
    for name, e in hdr.items():
        s, t = e["data_offsets"]
        buf = raw[data_off + s : data_off + t]
        if e["dtype"] == "F32":
            a = np.frombuffer(buf, dtype="<f4")
        elif e["dtype"] == "F16":
            a = np.frombuffer(buf, dtype="<f2").astype(np.float32)
        elif e["dtype"] == "BF16":
            u = np.frombuffer(buf, dtype="<u2").astype(np.uint32) << 16
            a = u.view(np.float32)
        else:
            raise SystemExit(f"STOP: unexpected dtype {e['dtype']} in {name}")
        out[name] = a.reshape(e["shape"]).astype(np.float32)
    return out


def download_and_verify(role_cfg, dest_dir):
    """Decision-8 gate: re-read license, download at revision, size + SHA."""
    from huggingface_hub import hf_hub_download

    repo, rev = role_cfg["repo"], role_cfg["revision"]
    lic = hf_license_at_revision(repo, rev)
    if lic != role_cfg["license"]:
        raise SystemExit(
            f"STOP: {repo}@{rev} license tag is {lic!r}, expected "
            f"{role_cfg['license']!r} (decision 8). Report to owner; do not substitute.")
    records = []
    os.makedirs(dest_dir, exist_ok=True)
    for fname, exp_bytes in role_cfg["files"].items():
        p = hf_hub_download(repo_id=repo, filename=fname, revision=rev, local_dir=dest_dir)
        n = os.path.getsize(p)
        if exp_bytes is not None and n != exp_bytes:
            raise SystemExit(
                f"STOP: {repo}@{rev}/{fname} is {n} bytes, expected {exp_bytes} "
                f"(decision 8 size check). Report to owner; do not substitute.")
        records.append({"repo": repo, "revision": rev, "file": fname,
                        "bytes": n, "sha256": sha256_file(p), "license": lic})
    return records, dest_dir


def build_pack(safetensors_path, config_path, extra_meta):
    from ocr_pack import write_pack  # noqa: local import so --help works w/o numpy

    tensors = load_safetensors_f32(safetensors_path)
    config = json.load(open(config_path))
    meta = {"arch": config, **extra_meta}
    return tensors, meta


def extract_charset(rec_inference_yml):
    import yaml

    y = yaml.safe_load(open(rec_inference_yml))
    char_dict = y["PostProcess"]["character_dict"]
    # CTCLabelDecode / BaseRecLabelDecode ordering: dict, then space (use_space_char),
    # then "blank" prepended => index 0 blank, 1..N dict, N+1 space.
    return ["blank"] + list(char_dict) + [" "]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["small", "medium"], default="small")
    ap.add_argument("--out", required=True, help="pack output directory")
    ap.add_argument("--cache", default=None, help="HF download cache dir")
    ap.add_argument("--t-accept", type=float, default=0.80)
    ap.add_argument("--t-decide", type=float, default=0.90)
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ocr_pack import write_pack

    tier = PINS[args.tier]
    cache = args.cache or os.path.join(args.out, "_src")
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    all_records = []
    file_shas = {}
    det_post = dict(DET_POST)
    for role in ("det", "rec"):
        print(f"[{role}] verifying + downloading {tier[role]['repo']}@{tier[role]['revision'][:12]}")
        recs, ddir = download_and_verify(tier[role], os.path.join(cache, role))
        all_records += recs
        pre = DET_PRE if role == "det" else REC_PRE
        if role == "det":
            det_post = extract_det_postprocess(os.path.join(ddir, "inference.yml"))
        post = det_post if role == "det" else {}
        tensors, meta = build_pack(
            os.path.join(ddir, "model.safetensors"),
            os.path.join(ddir, "config.json"),
            {"role": role, "tier": args.tier, "preprocess": pre, "postprocess": post},
        )
        pack_path = os.path.join(out, f"{role}.bin")
        write_pack(pack_path, tensors, meta)
        file_shas[f"{role}.bin"] = sha256_file(pack_path)
        print(f"[{role}] wrote {pack_path}  ({os.path.getsize(pack_path)} bytes, "
              f"{len(tensors)} tensors)")

    charset = extract_charset(os.path.join(cache, "rec", "inference.yml"))
    charset_path = os.path.join(out, "charset.txt")
    with open(charset_path, "w", encoding="utf-8") as f:
        for ch in charset:
            f.write(ch.replace("\n", "\\n") + "\n")
    file_shas["charset.txt"] = sha256_file(charset_path)
    print(f"[charset] wrote {charset_path}  ({len(charset)} classes)")

    thresholds = {"t_accept": args.t_accept, "t_decide": args.t_decide,
                  "max_escalations_per_page": 8, **det_post}
    fp_material = json.dumps(
        {"files": file_shas, "thresholds": thresholds,
         "contract": CONTRACT_VERSION, "samosa_ocr_version": "0"},
        sort_keys=True).encode()
    pack_fingerprint = hashlib.sha256(fp_material).hexdigest()

    manifest = {
        "contract_version": CONTRACT_VERSION,
        "pack_fingerprint": pack_fingerprint,
        "tier": args.tier,
        "created": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "num_classes": len(charset),
        "thresholds": thresholds,
        "det_preprocess": DET_PRE, "det_postprocess": det_post, "rec_preprocess": REC_PRE,
        "files": file_shas,
        "source_pins": all_records,
        "samosa_ocr_version": "0",
        "license_policy": "Apache-2.0 or MIT only (TASKS_READER.md decision 8)",
    }
    manifest_path = os.path.join(out, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"[manifest] wrote {manifest_path}")
    print(f"[manifest] pack_fingerprint {pack_fingerprint}")
    print("OK")


if __name__ == "__main__":
    main()
