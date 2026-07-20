#!/usr/bin/env python3
import hashlib
import importlib.util
import os
from pathlib import Path
import tarfile
import tempfile
import time
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "samosa_models_test",
    Path(__file__).parents[1] / "tools/samosa_models.py",
)
models = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(models)


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    fixture = root / "fixture.bin"
    fixture.write_bytes(b"verified model payload\n")
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    curl = root / "curl"
    curl.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "target=''\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = --output ]; then shift; target=$1; fi\n"
        "  shift\n"
        "done\n"
        "cp \"$SAMOSA_TEST_FIXTURE\" \"$target\"\n"
    )
    curl.chmod(0o755)
    os.environ["SAMOSA_CURL"] = str(curl)
    os.environ["SAMOSA_TEST_FIXTURE"] = str(fixture)

    target = root / "installed" / "payload.bin"
    updates = []
    models._download_file(
        "https://invalid.test/payload", target, fixture.stat().st_size,
        digest, None, updates.append, "fixture",
    )
    assert target.read_bytes() == fixture.read_bytes()
    assert not target.with_name("payload.bin.partial").exists()
    assert {update["phase"] for update in updates} >= {"downloading", "verifying"}

    corrupt_target = root / "installed" / "corrupt.bin"
    try:
        models._download_file(
            "https://invalid.test/corrupt", corrupt_target, fixture.stat().st_size,
            "0" * 64, None, None, "corrupt fixture",
        )
    except RuntimeError as error:
        assert "SHA-256 mismatch" in str(error)
    else:
        raise AssertionError("a corrupt download was installed")
    assert not corrupt_target.exists()
    assert corrupt_target.with_name("corrupt.bin.partial").exists()

    same_size_corrupt = root / "installed" / "same-size.bin"
    same_size_corrupt.write_bytes(b"x" * fixture.stat().st_size)
    models._download_file(
        "https://invalid.test/same-size", same_size_corrupt, fixture.stat().st_size,
        digest, None, None, "same-size fixture",
    )
    assert same_size_corrupt.read_bytes() == fixture.read_bytes()

    archive = root / "unsafe.tar.gz"
    escaped = root / "escape"
    with tarfile.open(archive, "w:gz") as bundle:
        info = tarfile.TarInfo("../escape")
        info.size = 1
        import io
        bundle.addfile(info, io.BytesIO(b"x"))
    try:
        models._safe_extract(archive, root / "extract")
    except RuntimeError as error:
        assert "unsafe path" in str(error)
    else:
        raise AssertionError("unsafe runtime archive was extracted")
    assert not escaped.exists()

    original_catalog = models.MODEL_CATALOG
    tiny_catalog = {
        "tiny": {
            "label": "Tiny",
            "description": "test",
            "size_bytes": fixture.stat().st_size,
            "license": "test",
            "source": "https://invalid.test",
            "directory": "tiny",
            "files": [("tiny.bin", fixture.stat().st_size, digest)],
            "repository": "invalid/test",
            "revision": "pinned",
            "runtime": "samosa",
        }
    }
    models.MODEL_CATALOG = tiny_catalog
    completed = []
    manager = models.DownloadManager(root / "manager", completed.append)
    manager.start("tiny")
    deadline = time.time() + 5
    while manager.snapshot()["active"] and time.time() < deadline:
        time.sleep(0.02)
    assert manager.snapshot()["phase"] == "complete"
    assert completed == ["tiny"]
    assert (root / "manager/models/tiny/tiny.bin").read_bytes() == fixture.read_bytes()
    models.MODEL_CATALOG = original_catalog

print("verified model downloads: PASS")
