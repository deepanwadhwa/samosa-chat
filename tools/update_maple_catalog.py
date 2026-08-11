#!/usr/bin/env python3
"""Pin the production Maple SSD-streaming pack in assets/models.json."""

import hashlib
import json
import subprocess
from pathlib import Path


REPO = "deepanwa/Samosa-Chat-Maple-2bit-SSD"
REVISION = "5567ec8bf083f6716a6311734da71a0f44aed929"
BASE_URL = f"https://huggingface.co/{REPO}/resolve/{REVISION}"
API_URL = f"https://huggingface.co/api/models/{REPO}/tree/{REVISION}"
CATALOG = Path(__file__).resolve().parents[1] / "assets/models.json"

FILES = {
    "maple-experts.bin": "weights",
    "maple-resident.safetensors": "weights",
    "maple-manifest.json": "configuration",
    "config.json": "configuration",
    "tokenizer.json": "tokenizer",
    "tokenizer_config.json": "tokenizer",
    "special_tokens_map.json": "tokenizer",
    "added_tokens.json": "tokenizer",
    "merges.txt": "tokenizer",
    "vocab.json": "tokenizer",
    "chat_template.jinja": "tokenizer",
}


def read_url(url: str) -> bytes:
    return subprocess.run(
        ["curl", "-fsSL", "--user-agent", "samosa-catalog/1", url],
        check=True,
        capture_output=True,
    ).stdout


tree = json.loads(read_url(API_URL))
tree_by_path = {item["path"]: item for item in tree}
missing = FILES.keys() - tree_by_path.keys()
if missing:
    raise SystemExit(f"published Maple pack is incomplete: {sorted(missing)}")

artifacts = []
for name, role in FILES.items():
    item = tree_by_path[name]
    lfs = item.get("lfs")
    if lfs:
        size = lfs["size"]
        sha256 = lfs["oid"]
    else:
        content = read_url(f"{BASE_URL}/{name}")
        size = len(content)
        sha256 = hashlib.sha256(content).hexdigest()
    artifacts.append(
        {
            "name": name,
            "role": role,
            "required": True,
            "url": f"{BASE_URL}/{name}",
            "install_path": f"models/maple/{name}",
            "file_mode": "0600",
            "bytes": size,
            "sha256": sha256,
        }
    )

with CATALOG.open() as source:
    catalog = json.load(source)

maple = next(model for model in catalog["models"] if model["id"] == "maple")
maple["version"] = REVISION
maple["license"]["url"] = f"https://huggingface.co/{REPO}"
maple["artifacts"] = artifacts
catalog["catalog_revision"] = "2026-08-11.1"

with CATALOG.open("w") as destination:
    json.dump(catalog, destination, indent=2)
    destination.write("\n")

print(f"Pinned {len(artifacts)} Maple streaming artifacts at {REVISION}")
