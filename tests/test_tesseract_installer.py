#!/usr/bin/env python3
"""Install a release with real OCR into an empty home without host OCR tools."""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile

from reportlab.pdfgen import canvas

ROOT = Path(__file__).resolve().parents[1]

def run(args, env, expected=0):
    result = subprocess.run(list(map(str, args)), env=env, capture_output=True,
                            text=True, timeout=180)
    assert result.returncode == expected, (args, result.stdout[-3000:], result.stderr[-5000:])
    return result

assert platform.system() == "Darwin" and platform.machine() == "arm64", "macOS release test"
with tempfile.TemporaryDirectory(prefix="samosa-tesseract-install-") as temp:
    work = Path(temp)
    remote, home, sdk = work / "remote", work / "home", work / "pdfium"
    sdk.mkdir()
    candidates = [Path(os.environ.get("PDFIUM_LIBRARY", "/nonexistent")),
                  ROOT / "dist/libpdfium.dylib", ROOT / "build/libpdfium.dylib",
                  Path.home() / ".samosa/current/lib/libpdfium.dylib",
                  Path.home() / ".samosa/current/bin/libpdfium.dylib"]
    library = next((p for p in candidates if p.is_file()), None)
    assert library, "PDFIUM_LIBRARY must point to the reviewed macOS PDFium library"
    with tarfile.open(sdk / "pdfium-mac-arm64.tgz", "w:gz") as archive:
        archive.add(library.resolve(), arcname="lib/libpdfium.dylib")
        archive.add(ROOT / "vendor/pdfium-headers-152.0.7961.0", arcname="include")
    for platform_name in ("linux-x64", "linux-arm64"):
        (sdk / f"pdfium-{platform_name}.tgz").write_text("not selected on macOS")
    fixture = ROOT / "tests/fixtures/maple-runtime/samosa-maple"
    command = [sys.executable, ROOT / "tools/package_hf.py", "--out", remote,
               "--runtime-only", "--repo-id", "test/samosa", "--pdfium-dir", sdk,
               "--maple-metallib", ROOT / "tests/fixtures/maple-runtime/mlx.metallib",
               "--summarizer-model", ROOT / "tests/fixtures/native-summarizer/model.gguf",
               "--summarizer-runtime-dir", ROOT / "tests/fixtures/native-summarizer"]
    for flag in ("--maple-runtime", "--visionpsy-runtime", "--molmo2-runtime",
                 "--molmo2-pack", "--audio-decode-runtime"):
        command += [flag, fixture]
    # Chat-model fixtures keep this test small; the OCR executable, libraries,
    # language data and PDFium are real and checked through the installed CLI.
    run(command, dict(os.environ, SAMOSA_PACKAGE_TEST="1"))
    manifest = (remote / "release-manifest.tsv").read_text()
    assert "runtime/macos-arm64/ocr/bin/samosa-ocr" in manifest
    assert "runtime/macos-arm64/ocr/share/tessdata/eng.traineddata" in manifest

    traps = work / "traps"
    traps.mkdir()
    for name in ("brew", "pkg-config", "tesseract", "python", "python3"):
        path = traps / name
        path.write_text(f"#!/bin/sh\necho 'Unexpected host dependency: {name}' >&2\nexit 99\n")
        path.chmod(0o700)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SAMOSA_", "TESSDATA", "DYLD_"))}
    env.update(SAMOSA_HOME=str(home), SAMOSA_BASE_URL=remote.as_uri(),
               SAMOSA_INSTALL_TEST="1", SAMOSA_SKIP_PATH_SETUP="1", SAMOSA_MIN_FREE_AFTER_GB="0",
               PATH=f"{traps}:/usr/bin:/bin:/usr/sbin:/sbin")
    run(["/bin/sh", ROOT / "dist/install.sh"], env)
    current = home / "current"
    assert json.loads(run([current / "bin/samosa-ocr", "--check"], env).stdout)["ok"]
    loaded = run([current / "bin/samosa-ocr", "--check"], dict(env, DYLD_PRINT_LIBRARIES="1"))
    assert "/opt/homebrew" not in loaded.stderr and "/usr/local/" not in loaded.stderr, loaded.stderr
    assert "tesseract" in run([current / "bin/samosa-ocr", "--version"], env).stdout

    pdf = work / "two scans.pdf"
    doc = canvas.Canvas(str(pdf), pagesize=(600, 800))
    for _ in range(2):
        doc.drawImage(str(ROOT / "tools/testdata/ocr/tiny.png"), 72, 600, width=320, height=72)
        doc.showPage()
    doc.save()
    text = run([home / "bin/samosa", "ocr", pdf], env).stdout
    assert text.count("--- PAGE ") == 2 and text.count("2019") == 2, text

    # Missing mandatory language data must fail before replacing the live release.
    previous = current.readlink()
    (remote / "release-manifest.tsv").write_text("\n".join(
        line for line in manifest.splitlines() if not line.endswith("/eng.traineddata")) + "\n")
    rejected = run(["/bin/sh", ROOT / "dist/install.sh"], env, expected=1)
    assert "bundled Tesseract" in rejected.stderr, rejected.stderr
    assert current.readlink() == previous
print("test_tesseract_installer: PASS (real OCR, clean home, host OCR/Python disabled)")
