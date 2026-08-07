import urllib.request
import json
import hashlib

base_url = "https://huggingface.co/deepgrove/maple-preview-2bit-mlx/resolve/361db5da5e74ff6fcdd852d478e1f266ce11013a"
api_url = "https://huggingface.co/api/models/deepgrove/maple-preview-2bit-mlx/tree/361db5da5e74ff6fcdd852d478e1f266ce11013a"

files = [
    "model-00001-of-00003.safetensors",
    "model-00002-of-00003.safetensors",
    "model-00003-of-00003.safetensors",
    "model.safetensors.index.json",
    "config.json",
    "tokenizer.json",
    "tokenizer_config.json",
    "special_tokens_map.json",
    "added_tokens.json",
    "merges.txt",
    "vocab.json",
    "chat_template.jinja"
]

artifacts = []

req = urllib.request.Request(api_url)
with urllib.request.urlopen(req) as response:
    tree = json.loads(response.read().decode())

tree_map = {item['path']: item for item in tree}

for f in files:
    item = tree_map[f]
    size = item.get('size', 0)
    
    if 'lfs' in item:
        sha256 = item['lfs']['oid']
        size = item['lfs']['size']
    else:
        req = urllib.request.Request(f"{base_url}/{f}")
        with urllib.request.urlopen(req) as response:
            content = response.read()
        sha256 = hashlib.sha256(content).hexdigest()
        size = len(content)
        
    artifacts.append({
        "name": f,
        "role": "weights" if ("model" in f or f.endswith(".safetensors")) else "tokenizer" if ("token" in f or "vocab" in f or "merges" in f or "chat_template" in f) else "configuration",
        "required": True,
        "url": f"{base_url}/{f}",
        "install_path": f"models/maple/{f}",
        "file_mode": "0600",
        "bytes": size,
        "sha256": sha256
    })

print(json.dumps(artifacts, indent=2))
