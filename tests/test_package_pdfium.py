#!/usr/bin/env python3
"""Exercise PDFium release-artifact selection without a model download."""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "tools" / "package_hf.py"
MAPLE_RUNTIME = ROOT / "tests" / "fixtures" / "maple-runtime" / "samosa-maple"
MAPLE_METALLIB = ROOT / "tests" / "fixtures" / "maple-runtime" / "mlx.metallib"
ARCHIVES = (
    "pdfium-mac-arm64.tgz",
    "pdfium-linux-x64.tgz",
    "pdfium-linux-arm64.tgz",
)


class PackagePdfiumTest(unittest.TestCase):
    def prepare_model(self, directory: pathlib.Path) -> pathlib.Path:
        snapshot = directory / "snapshot"
        snapshot.mkdir()
        for name in ("experts.bin", "resident.safetensors", "manifest.json",
                     "config.json", "generation_config.json"):
            (snapshot / name).write_bytes(b"fixture\n")
        tokenizer = directory / "tokenizer.json"
        tokenizer.write_text("{}\n", encoding="utf-8")
        return tokenizer

    def command(self, root: pathlib.Path, tokenizer: pathlib.Path,
                pdfium: pathlib.Path, output: pathlib.Path) -> list[str]:
        return ["python3", str(PACKAGE), "--out", str(output), "--snapshot",
                str(root / "snapshot"), "--tokenizer", str(tokenizer),
                "--pdfium-dir", str(pdfium),
                "--maple-runtime", str(MAPLE_RUNTIME),
                "--maple-metallib", str(MAPLE_METALLIB)]

    def test_requires_the_complete_platform_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tokenizer = self.prepare_model(root)
            pdfium = root / "pdfium"
            pdfium.mkdir()
            (pdfium / ARCHIVES[0]).write_bytes(b"one archive")
            result = subprocess.run(self.command(root, tokenizer, pdfium, root / "out"),
                                    text=True, capture_output=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing PDFium archive", result.stderr)

    def test_includes_each_archive_in_the_verified_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tokenizer = self.prepare_model(root)
            pdfium = root / "pdfium"
            pdfium.mkdir()
            expected: dict[str, str] = {}
            for name in ARCHIVES:
                content = f"fixture {name}\n".encode()
                (pdfium / name).write_bytes(content)
                expected[f"pdfium/{name}"] = hashlib.sha256(content).hexdigest()
            output = root / "out"
            subprocess.run(self.command(root, tokenizer, pdfium, output), check=True,
                           text=True, capture_output=True)
            manifest = {}
            for line in (output / "release-manifest.tsv").read_text(encoding="utf-8").splitlines():
                digest, _size, name = line.split("\t")
                manifest[name] = digest
            self.assertEqual({name: manifest[name] for name in expected}, expected)

    def test_bakes_the_version_into_the_launcher(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tokenizer = self.prepare_model(root)
            pdfium = root / "pdfium"
            pdfium.mkdir()
            for name in ARCHIVES:
                (pdfium / name).write_bytes(f"fixture {name}\n".encode())
            output = root / "out"
            subprocess.run(self.command(root, tokenizer, pdfium, output), check=True,
                           text=True, capture_output=True)
            version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
            launcher = (output / "samosa").read_text(encoding="utf-8")
            # The real number is baked in; the empty marker must be gone so the
            # packaged launcher never falls back to reporting "unknown".
            self.assertIn(f'SAMOSA_VERSION="{version}"', launcher)
            self.assertNotIn('SAMOSA_VERSION=""', launcher)


if __name__ == "__main__":
    unittest.main()
