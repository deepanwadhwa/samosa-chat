#!/usr/bin/env python3
"""Verified model/runtime catalog and atomic downloader for Samosa.

This module has no third-party Python dependencies. Downloads are resumable,
remain hidden behind .partial paths, and become visible only after byte-size and
SHA-256 verification.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import threading
import time
from typing import Callable


QWEN_REPOSITORY = "deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32"
QWEN_REVISION = "6de29dd71aa34628f1d0b4c005dbb03a53e0b655"
BONSAI_REPOSITORY = "prism-ml/Bonsai-27B-gguf"
BONSAI_REVISION = "f10afb355f104535e3e3e98cf7ab7795c72bd292"
ORNITH_REPOSITORY = "deepreinforce-ai/Ornith-1.0-9B-GGUF"
ORNITH_REVISION = "3296bc7a404871a72ac3f1903f561459c09b5c17"
PRISM_RELEASE = "prism-b9596-9fcaed7"
PRISM_VERSION = "9596 (9fcaed76)"
MIN_FREE_AFTER_BYTES = 2_000_000_000


def _hf_url(repository: str, revision: str, filename: str) -> str:
    return f"https://huggingface.co/{repository}/resolve/{revision}/{filename}?download=true"


MODEL_CATALOG = {
    "qwen": {
        "label": "Qwen3.6 35B A3B",
        "description": "Samosa's expert-streaming Qwen; strongest integration and image support.",
        "size_bytes": 23_987_270_752,
        "license": "Apache-2.0",
        "source": f"https://huggingface.co/{QWEN_REPOSITORY}",
        "directory": "qwen36-group32",
        "files": [
            ("experts.bin", 20_942_159_872, "00d64d44c39496e5ab5691f4cb27b67e27f3a10efd1f7c54024a9b43b130dbba"),
            ("resident.safetensors", 3_015_056_192, "52ff706830df2defaca591813810a8d19e1ba9b31d9b2d27b6ecf593b3a91627"),
            ("manifest.json", 1_908_179, "12ad73a9457e5d88c7cd4b00cae4a5c7ccb9031aa10d1111b80932d115f224d4"),
            ("config.json", 3_686, "93a4693fa9d8392fbfccd4b3c9873f4bfdcb14fdede978b123d07d19675efe99"),
            ("generation_config.json", 202, "e70c136c1b78ddc1fb0905bac8e733a4dc448d4f852a5dd75143fffc70be550e"),
            ("tokenizer_qwen36.json", 28_142_621, "6d56a5c681da15d38fb9f883016f86fa0638176e3f748a0acf5c7ba02725679b"),
        ],
        "repository": QWEN_REPOSITORY,
        "revision": QWEN_REVISION,
        "runtime": "samosa",
    },
    "bonsai": {
        "label": "Bonsai 27B 1-bit",
        "description": "A compact 27B reasoning model from PrismML; 3.54 GB of weights.",
        "size_bytes": 3_803_452_480,
        "license": "Apache-2.0",
        "source": f"https://huggingface.co/{BONSAI_REPOSITORY}",
        "directory": "bonsai-27b-1bit",
        "files": [
            ("Bonsai-27B-Q1_0.gguf", 3_803_452_480,
             "17ef842e47450caeb8eaa3ebfbbab5d2f2278b62b79be107985fb69a2f819aa0"),
        ],
        "repository": BONSAI_REPOSITORY,
        "revision": BONSAI_REVISION,
        "runtime": "prism",
    },
    "ornith": {
        "label": "Ornith 1.0 9B",
        "description": "A compact coding/reasoning model from DeepReinforce; MIT licensed.",
        "size_bytes": 5_629_108_704,
        "license": "MIT",
        "source": f"https://huggingface.co/{ORNITH_REPOSITORY}",
        "directory": "ornith-9b",
        "files": [
            ("ornith-1.0-9b-Q4_K_M.gguf", 5_629_108_704,
             "5720d1f671b4996481274fffe01868c3c36e87c135cc8538471cc7bd6087b106"),
        ],
        "repository": ORNITH_REPOSITORY,
        "revision": ORNITH_REVISION,
        "runtime": "prism",
    },
}


RUNTIME_ASSETS = {
    ("Darwin", "arm64"): (
        "llama-prism-b9596-9fcaed7-bin-macos-arm64.tar.gz",
        11_169_620,
        "9c14bcdba8c99378ca3fd4dbf3c28f94bb8c528186a80f8bc40b5fdd7edb9937",
    ),
    ("Darwin", "x86_64"): (
        "llama-prism-b9596-9fcaed7-bin-macos-x64.tar.gz",
        11_205_277,
        "06c182e47295cccfead6b3cff1a4fa91b347974141b3a8b26d2231b4396db2f8",
    ),
    ("Linux", "x86_64"): (
        "llama-prism-b9596-9fcaed7-bin-ubuntu-x64.tar.gz",
        16_089_328,
        "e361c09f128a407c659d07361b008155e1eab0cd0ed0a12ccdcf7147f7c22948",
    ),
    ("Linux", "aarch64"): (
        "llama-prism-b9596-9fcaed7-bin-ubuntu-arm64.tar.gz",
        13_067_159,
        "4bf7ac9514f78bf25bd09c4f70ee5c5b5885c4781bfe48d093c4374d141c11b8",
    ),
}


Progress = Callable[[dict], None]


def human_bytes(value: int) -> str:
    units = ("B", "KB", "MB", "GB", "TB")
    amount = float(value)
    for unit in units:
        if amount < 1000 or unit == units[-1]:
            return f"{amount:.1f} {unit}" if unit != "B" else f"{int(amount)} B"
        amount /= 1000
    return f"{value} B"


def model_directory(home: Path, name: str) -> Path:
    return home / "models" / MODEL_CATALOG[name]["directory"]


def model_primary_path(home: Path, name: str) -> Path:
    filename = MODEL_CATALOG[name]["files"][0][0]
    return model_directory(home, name) / filename


def qwen_tokenizer_path(home: Path) -> Path:
    return model_directory(home, "qwen") / "tokenizer_qwen36.json"


def prism_runtime_directory(home: Path) -> Path:
    return home / "backends" / PRISM_RELEASE


def prism_server_path(home: Path) -> Path:
    return prism_runtime_directory(home) / "llama-server"


def legacy_prism_server_path(home: Path) -> Path:
    return home / "backends" / "prism-llama.cpp" / "build" / "bin" / "llama-server"


def discover_prism_server(home: Path) -> Path:
    override = os.environ.get("SAMOSA_BONSAI_SERVER")
    if override:
        return Path(override).expanduser()
    installed = prism_server_path(home)
    return installed if installed.is_file() else legacy_prism_server_path(home)


def discover_model_path(home: Path, name: str, preferred: Path | None = None) -> Path:
    if preferred and preferred.is_file():
        return preferred
    canonical = model_primary_path(home, name)
    if canonical.is_file():
        return canonical
    if name == "ornith":
        legacy = model_directory(home, name) / "Ornith-1.0-9B-Q4_K_M.gguf"
        if legacy.is_file():
            return legacy
    return canonical


def discover_qwen_directory(home: Path, preferred: Path | None = None) -> Path:
    if preferred and (preferred / "experts.bin").is_file():
        return preferred
    return model_directory(home, "qwen")


def _files_present(directory: Path, files: list[tuple[str, int, str]]) -> bool:
    return all(
        (directory / filename).is_file()
        and (directory / filename).stat().st_size == size
        for filename, size, _digest in files
    )


def catalog_status(home: Path, configured_qwen: Path | None = None,
                   configured_tokenizer: Path | None = None,
                   configured_runtime: Path | None = None,
                   configured_models: dict[str, Path] | None = None) -> list[dict]:
    runtime = configured_runtime or discover_prism_server(home)
    model_overrides = configured_models or {}
    result = []
    for name, entry in MODEL_CATALOG.items():
        if name == "qwen":
            directory = discover_qwen_directory(home, configured_qwen)
            if configured_qwen and directory == configured_qwen and configured_tokenizer:
                model_downloaded = (
                    _files_present(directory, entry["files"][:-1])
                    and configured_tokenizer.is_file()
                    and configured_tokenizer.stat().st_size == entry["files"][-1][1]
                )
            else:
                model_downloaded = _files_present(directory, entry["files"])
            runtime_ready = True
        else:
            path = discover_model_path(home, name, model_overrides.get(name))
            expected = entry["files"][0][1]
            model_downloaded = path.is_file() and path.stat().st_size == expected
            # Preserve recognition of the previously-qualified Ornith artifact.
            if name == "ornith" and path.is_file() and path.stat().st_size == 5_701_067_872:
                model_downloaded = True
            runtime_ready = runtime.is_file() and os.access(runtime, os.X_OK)
        result.append({
            "id": name,
            "label": entry["label"],
            "description": entry["description"],
            "size_bytes": entry["size_bytes"],
            "size": human_bytes(entry["size_bytes"]),
            "license": entry["license"],
            "source": entry["source"],
            "model_downloaded": model_downloaded,
            "runtime_ready": runtime_ready,
            "installed": model_downloaded and runtime_ready,
        })
    return result


def _sha256(path: Path, cancel: threading.Event | None = None) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            if cancel and cancel.is_set():
                raise RuntimeError("download cancelled")
            chunk = stream.read(4 * 1024 * 1024)
            if not chunk:
                return digest.hexdigest()
            digest.update(chunk)


def _notify(callback: Progress | None, **values: object) -> None:
    if callback:
        callback(values)


def _download_file(url: str, target: Path, size: int, digest: str,
                   cancel: threading.Event | None, callback: Progress | None,
                   label: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and target.stat().st_size == size:
        _notify(callback, phase="verifying", file=label,
                file_downloaded=size, file_total=size)
        if _sha256(target, cancel) == digest:
            return
        target.unlink()
    partial = target.with_name(target.name + ".partial")
    if partial.exists() and partial.stat().st_size > size:
        partial.unlink()
    _notify(callback, phase="downloading", file=label,
            file_downloaded=partial.stat().st_size if partial.exists() else 0,
            file_total=size)
    command = [
        os.environ.get("SAMOSA_CURL", "curl"),
        "-fL", "--retry", "5", "--retry-delay", "3", "-C", "-",
        "--output", str(partial), url,
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    while process.poll() is None:
        if cancel and cancel.is_set():
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
            raise RuntimeError("download cancelled")
        downloaded = partial.stat().st_size if partial.exists() else 0
        _notify(callback, phase="downloading", file=label,
                file_downloaded=downloaded, file_total=size)
        time.sleep(0.25)
    stderr = (process.stderr.read() if process.stderr else b"").decode("utf-8", "replace")
    if process.returncode:
        raise RuntimeError(f"download failed for {label}: {stderr.strip()[-300:]}")
    if not partial.is_file() or partial.stat().st_size != size:
        actual = partial.stat().st_size if partial.exists() else 0
        raise RuntimeError(
            f"downloaded size mismatch for {label}: expected {size}, received {actual}"
        )
    _notify(callback, phase="verifying", file=label,
            file_downloaded=size, file_total=size)
    actual_digest = _sha256(partial, cancel)
    if actual_digest != digest:
        raise RuntimeError(f"SHA-256 mismatch for {label}; the partial file was not installed")
    os.replace(partial, target)


def _safe_extract(archive: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise RuntimeError("runtime archive contains an unsafe path")
            if member.issym() or member.islnk():
                link = Path(member.linkname)
                if link.is_absolute() or ".." in link.parts:
                    raise RuntimeError("runtime archive contains an unsafe link")
        bundle.extractall(destination)
    roots = [entry for entry in destination.iterdir() if entry.is_dir()]
    if len(roots) != 1 or not (roots[0] / "llama-server").is_file():
        raise RuntimeError("runtime archive did not contain the expected llama-server")
    return roots[0]


def ensure_prism_runtime(home: Path, cancel: threading.Event | None = None,
                         callback: Progress | None = None) -> Path:
    existing = discover_prism_server(home)
    if existing.is_file() and os.access(existing, os.X_OK):
        return existing
    key = (platform.system(), platform.machine())
    if key not in RUNTIME_ASSETS:
        raise RuntimeError(
            f"no verified Prism runtime is published for {key[0]} {key[1]}"
        )
    filename, size, digest = RUNTIME_ASSETS[key]
    url = (
        "https://github.com/PrismML-Eng/llama.cpp/releases/download/"
        f"{PRISM_RELEASE}/{filename}"
    )
    downloads = home / "downloads"
    archive = downloads / filename
    _download_file(url, archive, size, digest, cancel, callback, "Prism runtime")
    stage_parent = home / "backends"
    stage_parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{PRISM_RELEASE}.", dir=stage_parent))
    try:
        extracted = _safe_extract(archive, stage)
        final = prism_runtime_directory(home)
        if final.exists():
            shutil.rmtree(final)
        os.replace(extracted, final)
        shutil.rmtree(stage)
        server = final / "llama-server"
        if not server.is_file():
            raise RuntimeError("Prism runtime installation did not produce llama-server")
        return server
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _required_download_bytes(home: Path, name: str, include_runtime: bool) -> int:
    entry = MODEL_CATALOG[name]
    directory = model_directory(home, name)
    needed = 0
    for filename, size, _digest in entry["files"]:
        partial = (directory / filename).with_name(filename + ".partial")
        present = min(size, partial.stat().st_size) if partial.exists() else 0
        target = directory / filename
        if not target.is_file() or target.stat().st_size != size:
            needed += size - present
    if include_runtime and not discover_prism_server(home).is_file():
        asset = RUNTIME_ASSETS.get((platform.system(), platform.machine()))
        if asset:
            archive = home / "downloads" / asset[0]
            partial = archive.with_name(archive.name + ".partial")
            present = min(asset[1], partial.stat().st_size) if partial.exists() else 0
            if not archive.is_file():
                needed += asset[1] - present
    return needed


def install_model(home: Path, name: str, cancel: threading.Event | None = None,
                  callback: Progress | None = None) -> None:
    if name not in MODEL_CATALOG:
        raise ValueError(f"unknown model {name!r}")
    entry = MODEL_CATALOG[name]
    include_runtime = entry["runtime"] == "prism"
    needed = _required_download_bytes(home, name, include_runtime)
    free = shutil.disk_usage(home if home.exists() else home.parent).free
    if free < needed + MIN_FREE_AFTER_BYTES:
        raise RuntimeError(
            f"{entry['label']} needs {human_bytes(needed + MIN_FREE_AFTER_BYTES)} "
            f"free including safety reserve; only {human_bytes(free)} is available"
        )
    if include_runtime:
        ensure_prism_runtime(home, cancel, callback)
    directory = model_directory(home, name)
    directory.mkdir(parents=True, exist_ok=True)
    completed = 0
    for filename, size, digest in entry["files"]:
        target = directory / filename

        def file_progress(value: dict, base: int = completed) -> None:
            current = int(value.get("file_downloaded", 0))
            _notify(
                callback, **value, model=name,
                downloaded_bytes=base + current,
                total_bytes=entry["size_bytes"],
            )

        url = _hf_url(entry["repository"], entry["revision"], filename)
        _download_file(url, target, size, digest, cancel, file_progress, filename)
        completed += size
    marker = directory / ".installed.json"
    marker.write_text(json.dumps({
        "model": name,
        "source": entry["source"],
        "revision": entry["revision"],
        "installed_at": int(time.time()),
        "files": [
            {"name": filename, "size": size, "sha256": digest}
            for filename, size, digest in entry["files"]
        ],
    }, indent=2) + "\n")
    _notify(callback, phase="complete", model=name,
            downloaded_bytes=entry["size_bytes"], total_bytes=entry["size_bytes"])


class DownloadManager:
    def __init__(self, home: Path, on_complete: Callable[[str], None] | None = None):
        self.home = home
        self.on_complete = on_complete
        self.lock = threading.RLock()
        self.cancel_event = threading.Event()
        self.thread: threading.Thread | None = None
        self.state: dict = {"active": False, "phase": "idle"}

    def snapshot(self) -> dict:
        with self.lock:
            return dict(self.state)

    def start(self, name: str) -> dict:
        if name not in MODEL_CATALOG:
            raise ValueError("unknown model")
        with self.lock:
            if self.thread and self.thread.is_alive():
                raise RuntimeError(f"{self.state.get('model')} is already downloading")
            self.cancel_event = threading.Event()
            self.state = {
                "active": True, "phase": "starting", "model": name,
                "downloaded_bytes": 0,
                "total_bytes": MODEL_CATALOG[name]["size_bytes"],
                "error": None,
            }
            self.thread = threading.Thread(
                target=self._run, args=(name,), daemon=True,
                name=f"samosa-download-{name}",
            )
            self.thread.start()
            return dict(self.state)

    def _progress(self, update: dict) -> None:
        with self.lock:
            self.state.update(update)

    def _run(self, name: str) -> None:
        try:
            install_model(self.home, name, self.cancel_event, self._progress)
            if self.on_complete:
                self.on_complete(name)
            with self.lock:
                self.state.update({"active": False, "phase": "complete"})
        except Exception as error:
            with self.lock:
                self.state.update({
                    "active": False,
                    "phase": "cancelled" if self.cancel_event.is_set() else "error",
                    "error": str(error),
                })

    def cancel(self) -> bool:
        with self.lock:
            if not self.thread or not self.thread.is_alive():
                return False
            self.cancel_event.set()
            return True
