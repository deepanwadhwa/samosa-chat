#!/usr/bin/env python3
"""Assemble the Hugging Face distribution folder for Samosa Chat.

Collects the int4 container, tokenizer, engine sources, installer, and the
`samosa` wrapper into one folder, computes SHA-256 and byte sizes for atomic
installation, and verifies nothing is missing. Large model files are
HARD-LINKED (same volume) so the staging folder costs no extra disk space.

Usage:
  python3 tools/package_hf.py --out /path/to/staging [--repo-id user/name]

Upload afterwards (needs `pip install huggingface_hub` and a write token):
  hf upload <repo-id> /path/to/staging . --repo-type model
"""

import argparse
import hashlib
import os
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
MODEL_ROOT = ROOT.parent / "samosa-models"

MODEL_FILES = [
    "experts.bin",
    "resident.safetensors",
    "manifest.json",
    "config.json",
    "generation_config.json",
]

SOURCE_FILES = [
    "qwen36b.c",
    "expert_cache.c",
    "expert_cache.h",
    "vision.c",
    "vision.h",
    "stb_image.h",
    "kernels.h",
    "st.h",
    "json.h",
    "tok.h",
    "tok_unicode.h",
    "compat.h",
    "repetition_guard.h",
    "thinking_budget.h",
    "samosa_http.h",
    "samosa_extract.c",
]

PDFIUM_ARCHIVES = [
    "pdfium-mac-arm64.tgz",
    "pdfium-linux-x64.tgz",
    "pdfium-linux-arm64.tgz",
]

def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            h.update(block)
    return h.hexdigest()

def place(src: pathlib.Path, dst: pathlib.Path, link: bool) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    if link:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass
    shutil.copy2(src, dst)

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--snapshot", type=pathlib.Path,
                    default=MODEL_ROOT / "qwen36_group32_i8")
    ap.add_argument("--tokenizer", type=pathlib.Path,
                    default=MODEL_ROOT / "tokenizer_qwen36.json")
    ap.add_argument("--repo-id", default="REPO_ID_PLACEHOLDER")
    ap.add_argument("--pdfium-dir", type=pathlib.Path,
                    help="directory containing all SHA-reviewed PDFium archives")
    args = ap.parse_args()
    out: pathlib.Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    staged: list[pathlib.Path] = []
    for name in MODEL_FILES:
        src = args.snapshot / name
        if not src.exists():
            print(f"missing model file: {src}", file=sys.stderr)
            return 1
        place(src, out / name, link=True)
        staged.append(out / name)
    tok = args.tokenizer
    if not tok.exists():
        print(f"missing tokenizer: {tok}", file=sys.stderr)
        return 1
    place(tok, out / "tokenizer_qwen36.json", link=True)
    staged.append(out / "tokenizer_qwen36.json")

    for name in SOURCE_FILES:
        src = ROOT / "src" / name
        if not src.exists():
            print(f"missing source file: {src}", file=sys.stderr)
            return 1
        place(src, out / "engine" / name, link=False)
        staged.append(out / "engine" / name)

    if args.pdfium_dir:
        for name in PDFIUM_ARCHIVES:
            src = args.pdfium_dir / name
            if not src.is_file():
                print(f"missing PDFium archive: {src}", file=sys.stderr)
                return 1
            place(src, out / "pdfium" / name, link=False)
            staged.append(out / "pdfium" / name)

    for src, dst in ((ROOT / "dist" / "install.sh", out / "install.sh"),
                     (ROOT / "dist" / "samosa", out / "samosa"),
                     (ROOT / "dist" / "MODEL_CARD.md", out / "README.md"),
                     (ROOT / "assets" / "app.html", out / "app.html"),
                     (ROOT / "assets" / "samosa-chat.png", out / "samosa-chat.png")):
        if not src.exists():
            print(f"missing dist file: {src}", file=sys.stderr)
            return 1
        place(src, dst, link=False)
        if (args.repo_id != "REPO_ID_PLACEHOLDER" and
                dst.name in {"install.sh", "samosa", "README.md"}):
            text = dst.read_text(encoding="utf-8")
            if "REPO_ID_PLACEHOLDER" in text:
                dst.write_text(text.replace("REPO_ID_PLACEHOLDER", args.repo_id),
                               encoding="utf-8")
        staged.append(dst)

    lines = []
    release_lines = []
    for path in sorted(staged):
        digest = sha256_file(path)
        relative = path.relative_to(out)
        lines.append(f"{digest}  {relative}")
        release_lines.append(f"{digest}\t{path.stat().st_size}\t{relative}")
        print(f"{digest[:16]}  {relative}")
    (out / "checksums.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (out / "release-manifest.tsv").write_text(
        "\n".join(release_lines) + "\n", encoding="utf-8")

    total = sum(p.stat().st_size for p in staged) / 1e9
    print(f"\nstaged {len(staged)} files, {total:.2f} GB -> {out}")
    print(f"upload: hf upload {args.repo_id} {out} . --repo-type model")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
