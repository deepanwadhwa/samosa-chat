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
import platform

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
    "samosa_kokoro.h",
    "samosa_extract.c",
    "samosa_ocr.c",
    "visionpsy/samosa_visionpsy.cpp",
    "visionpsy/visionpsy_model.cpp",
    "visionpsy/visionpsy_model.h",
    # The compiled gateway and filesystem sidecar are the mandatory browser
    # control plane (docs/TASKS_UI_CHUTNI.md T1.0) -- every release includes
    # them unconditionally, not as an opt-in capability with a raw-Qwen
    # HTTP-serve fallback.
    "samosa_gateway.c",
    "samosa_multimodal.c",
    "samosa_multimodal.h",
    "samosa_summarizer.cpp",
    "samosa_fs.c",
    "read_cache.h",
    "durable_job.h",
]

CHUTNI_SOURCE_FILES = [
    "LICENSE",
    "NOTICE",
    # Chutni 0.3.0 compiles its release version in rather than hardcoding it in
    # a source file, and stamps it into the producer record of every artifact
    # it writes (SPEC §16.1). Ship VERSION so dist/install.sh can pass
    # -DCHUTNI_VERSION: without it the fallback is "0.0.0-unversioned", which
    # is not a build that exists and would be written permanently into the
    # user's own store.
    "VERSION",
    "include/chutni.h",
    "src/chutni.c",
    "src/scan.c",
    "src/cj.c",
    "src/cj.h",
    "src/mcp.c",
    "third_party/blake3/LICENSE_A2",
    "third_party/blake3/LICENSE_CC0",
    "third_party/blake3/blake3.c",
    "third_party/blake3/blake3.h",
    "third_party/blake3/blake3_dispatch.c",
    "third_party/blake3/blake3_impl.h",
    "third_party/blake3/blake3_portable.c",
    "third_party/sqlite/sqlite3.c",
    "third_party/sqlite/sqlite3.h",
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

def stage_native_summarizer(args, out: pathlib.Path,
                            staged: list[pathlib.Path]) -> bool:
    binary = args.summarizer_runtime_dir / "bin" / "samosa-summarizer"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print(
            "missing native summarizer runtime; run "
            "tools/stage_native_summarizer_runtime.sh",
            file=sys.stderr,
        )
        return False
    place(binary, out / "runtime" / "macos-arm64" / "samosa-summarizer", True)
    staged.append(out / "runtime" / "macos-arm64" / "samosa-summarizer")
    for name in (
        "libllama.0.dylib",
        "libggml.0.dylib",
        "libggml-cpu.0.dylib",
        "libggml-blas.0.dylib",
        "libggml-metal.0.dylib",
        "libggml-base.0.dylib",
    ):
        src = args.summarizer_runtime_dir / "lib" / name
        if not src.is_file():
            print(f"missing native summarizer library: {src}", file=sys.stderr)
            return False
        place(src, out / "runtime" / "macos-arm64" / "lib" / name, True)
        staged.append(out / "runtime" / "macos-arm64" / "lib" / name)
    if not args.summarizer_model.is_file():
        print(f"missing native summarizer model: {args.summarizer_model}", file=sys.stderr)
        return False
    package_fixture = os.environ.get("SAMOSA_PACKAGE_TEST") == "1"
    if not package_fixture and (args.summarizer_model.stat().st_size != 65364992 or \
            sha256_file(args.summarizer_model) != \
            "30c73e0b2ebd4f65538d87f5157aa7193261e8ff362e441df86c6941f3e9265c"):
        print("native summarizer model failed pinned byte/SHA-256 verification", file=sys.stderr)
        return False
    destination = out / "runtime" / "common" / "samosa-text-summarization-Q8_0.gguf"
    place(args.summarizer_model, destination, True)
    staged.append(destination)
    return True

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
    ap.add_argument(
        "--maple-runtime", type=pathlib.Path,
        default=ROOT / "build" / "samosa-maple",
        help="Apple-Silicon samosa-maple executable (defaults to the production build)")
    ap.add_argument(
        "--maple-metallib", type=pathlib.Path,
        default=(ROOT / "build" / "mlx-build" / "mlx" / "backend" /
                 "metal" / "kernels" / "mlx.metallib"),
        help="Metal library paired with --maple-runtime")
    ap.add_argument(
        "--visionpsy-runtime", type=pathlib.Path,
        default=ROOT / "build" / "samosa-visionpsy",
        help="Apple-Silicon native VisionPsy helper (defaults to the production build)")
    ap.add_argument(
        "--molmo2-runtime", type=pathlib.Path,
        default=ROOT / "build" / "samosa-molmo2",
        help="Apple-Silicon native on-demand Molmo2 helper")
    ap.add_argument(
        "--molmo2-pack", type=pathlib.Path,
        default=ROOT / "build" / "molmo2-pack",
        help="native pinned Molmo2 Q4 package builder")
    ap.add_argument(
        "--summarizer-model", type=pathlib.Path,
        default=ROOT / "build" / "samosa-text-summarization-Q8_0.gguf",
        help="verified Falconsai/text_summarization Q8_0 GGUF")
    ap.add_argument(
        "--summarizer-runtime-dir", type=pathlib.Path,
        default=ROOT / "build" / "native-summarizer-runtime",
        help="output from tools/stage_native_summarizer_runtime.sh")
    ap.add_argument("--runtime-only", action="store_true",
                    help="package the browser control plane (gateway, engine "
                         "sources, app shell) without optional chat-model weights; "
                         "the small native summarizer remains part of the runtime -- "
                         "docs/TASKS_UI_CHUTNI.md T1.0. Model artifacts are "
                         "installed separately through the in-app catalog.")
    args = ap.parse_args()
    out: pathlib.Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    version_file = ROOT / "VERSION"
    if not version_file.exists():
        print(f"missing VERSION file: {version_file}", file=sys.stderr)
        return 1
    version = version_file.read_text(encoding="utf-8").strip()

    staged: list[pathlib.Path] = []

    # Maple is an Apple-Silicon native runtime.  MLX first looks for its Metal
    # library beside the executable; shipping both makes the installed runtime
    # independent of the developer's build tree and its absolute CMake path.
    if sys.platform == "darwin" and platform.machine() == "arm64":
        for src, name in ((args.maple_runtime, "samosa-maple"),
                          (args.visionpsy_runtime, "samosa-visionpsy"),
                          (args.molmo2_runtime, "samosa-molmo2"),
                          (args.molmo2_pack, "molmo2-pack"),
                          (args.maple_metallib, "mlx.metallib")):
            if not src.is_file():
                print(f"missing Apple-Silicon Maple runtime: {src}", file=sys.stderr)
                return 1
            if name in {"samosa-maple", "samosa-visionpsy", "samosa-molmo2", "molmo2-pack"} and not os.access(src, os.X_OK):
                print(f"Apple-Silicon native runtime is not executable: {src}",
                      file=sys.stderr)
                return 1
            place(src, out / "runtime" / "macos-arm64" / name, link=True)
            staged.append(out / "runtime" / "macos-arm64" / name)

        molmo2_processor = ROOT / "assets" / "molmo2" / "processor.json"
        if not molmo2_processor.is_file():
            print(f"missing Molmo2 processor contract: {molmo2_processor}", file=sys.stderr)
            return 1
        processor_target = out / "runtime" / "common" / "molmo2-processor.json"
        place(molmo2_processor, processor_target, link=False)
        staged.append(processor_target)

        if not stage_native_summarizer(args, out, staged):
            return 1
    if not args.runtime_only:
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

    chutni_root = ROOT / "vendor" / "chutni"
    for name in CHUTNI_SOURCE_FILES:
        src = chutni_root / name
        if not src.exists():
            print(
                f"missing Chutni source: {src}; run git submodule update --init",
                file=sys.stderr,
            )
            return 1
        place(src, out / "engine" / "chutni" / name, link=False)
        staged.append(out / "engine" / "chutni" / name)

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
                     # Kept as a release runtime script rather than a bundled
                     # binary: it fetches and verifies the pinned Whisper.cpp
                     # source only after the user explicitly enables STT.
                     (ROOT / "tools" / "samosa_voice_runtime.sh", out / "engine" / "samosa_voice_runtime.sh"),
                     # Optional neural TTS is a separate opt-in native runtime;
                     # its pinned C library and model are fetched only after
                     # the user selects Download in Voice settings.
                     (ROOT / "tools" / "samosa_kokoro_runtime.sh", out / "engine" / "samosa_kokoro_runtime.sh"),
                     (ROOT / "dist" / "MODEL_CARD.md", out / "README.md"),
                     (ROOT / "assets" / "app.html", out / "app.html"),
                     (ROOT / "assets" / "samosa-chat.png", out / "samosa-chat.png"),
                     # The model catalog GET /v1/models/catalog serves (T2.1)
                     # is control-plane data, not a model weight -- staged
                     # unconditionally, runtime-only or not.
                     (ROOT / "assets" / "models.json", out / "models.json")):
        if not src.exists():
            print(f"missing dist file: {src}", file=sys.stderr)
            return 1
        place(src, dst, link=False)
        # Bake the version into the launcher so a packaged release reports it
        # without shipping the VERSION file into the release dir. The marker
        # appears once in the launcher (the empty assignment), so this whole-
        # string replace cannot disturb the runtime fallback logic.
        if dst.name == "samosa":
            text = dst.read_text(encoding="utf-8")
            if 'SAMOSA_VERSION=""' not in text:
                print("launcher is missing the SAMOSA_VERSION=\"\" marker",
                      file=sys.stderr)
                return 1
            dst.write_text(
                text.replace('SAMOSA_VERSION=""', f'SAMOSA_VERSION="{version}"'),
                encoding="utf-8")
        if (args.repo_id != "REPO_ID_PLACEHOLDER" and
                dst.name in {"install.sh", "samosa", "README.md"}):
            text = dst.read_text(encoding="utf-8")
            if "REPO_ID_PLACEHOLDER" in text:
                dst.write_text(text.replace("REPO_ID_PLACEHOLDER", args.repo_id),
                               encoding="utf-8")
        staged.append(dst)

    # Browser-local TTS runtimes are application assets, not Python packages.
    # Ship their JS/WASM adapters and tokenizer host, but keep the large model
    # weights out of the release; the app downloads those into browser-managed
    # storage only after the user chooses a model.
    browser_root = ROOT / "assets" / "voice" / "browser"
    if not browser_root.is_dir():
        print(f"missing browser voice assets: {browser_root}", file=sys.stderr)
        return 1
    for src in sorted(path for path in browser_root.rglob("*") if path.is_file()):
        relative = src.relative_to(browser_root)
        destination = out / "voice" / "browser" / relative
        place(src, destination, link=False)
        staged.append(destination)

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
