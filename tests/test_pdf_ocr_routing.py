#!/usr/bin/env python3
"""Deterministic PDFium page-inspection and OCR-routing contract checks."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from reportlab.lib.pagesizes import letter
from reportlab.pdfgen import canvas
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_multipage_pdf import build
EXTRACT = Path(__import__("os").environ.get("SAMOSA_EXTRACT", ROOT / "build/samosa-extract"))


def run(*args):
    return subprocess.run([str(EXTRACT), *map(str, args)], check=True,
                          text=True, capture_output=True).stdout


def inspect(path):
    result = json.loads(run("--json-pages", path, 1, 1))
    assert result["ok"]
    return result["pages"][0], result


def make_pdf(path, image=None, native=None, blank=False):
    doc = canvas.Canvas(str(path), pagesize=letter)
    if native:
        doc.setFont("Helvetica", 16)
        doc.drawString(72, 735, native)
    if image:
        doc.drawImage(str(image), 72, 100, width=letter[0] - 144,
                      height=letter[1] - 220)
    doc.showPage()
    doc.save()


def make_tiled_pdf(path, native=None):
    """Two raster tiles covering a page, optionally behind native text."""
    doc = canvas.Canvas(str(path), pagesize=(600, 800))
    tile = Image.new("RGB", (600, 800), "white")
    if native is None:
        ImageDraw.Draw(tile).text((120, 300), "TILED SCANNED SENTINEL 43", fill="black")
    tile_path = Path(path).with_suffix(".tile.png")
    tile.save(tile_path)
    reader = str(tile_path)
    doc.drawImage(reader, 0, 0, width=300, height=800)
    doc.drawImage(reader, 300, 0, width=300, height=800)
    if native:
        doc.setFont("Helvetica", 10)
        for y in range(20, 800, 16):
            doc.drawString(8, y, "Selectable native text already covers this image. " * 2)
    doc.showPage()
    doc.save()
    tile_path.unlink()


def make_adversarial_tiled_pdf(path, upper_native=False):
    """Many small scan tiles with native text outside (or above) the scan."""
    doc = canvas.Canvas(str(path), pagesize=(600, 800))
    tile = Image.new("RGB", (400, 400), "white")
    ImageDraw.Draw(tile).text((24, 170), "SCAN FACT 42", fill="black")
    tile_path = Path(path).with_suffix(".tile.png")
    tile.save(tile_path)
    if upper_native:
        doc.drawImage(str(tile_path), 30, 70, width=540, height=390)
        doc.setFont("Helvetica", 9)
        for y in range(510, 781, 12):
            doc.drawString(30, y, "Native text only in this top region does not cover the scan below. " * 2)
    else:
        for row in range(4):
            for col in range(4):
                doc.drawImage(str(tile_path), 30 + col * 135, 170 + row * 140,
                              width=135, height=140)
        doc.setFont("Helvetica", 9)
        doc.drawString(30, 35, "Native footer has over fifty characters but does not cover scanned facts above.")
    doc.showPage()
    doc.save()
    tile_path.unlink()


def ppm_size(path):
    with open(path, "rb") as stream:
        assert stream.readline().strip() == b"P6"
        width, height = map(int, stream.readline().split())
    return width, height


def main():
    if ";pdfium)" not in run("--version"):
        print("test_pdf_ocr_routing.py: SKIP (no PDFium extractor)")
        return
    with tempfile.TemporaryDirectory(prefix="samosa-ocr-routing-") as temp:
        root = Path(temp)
        large = root / "large.pdf"
        large.write_bytes(build(43, padding_bytes=21 * 1024 * 1024))
        page, result = inspect(large)
        assert result["page_count"] == 43 and len(result["pages"]) == 1
        assert "Page 1 of 43" in page["text"] and not page["inspection"]["needs_ocr"]
        limited = subprocess.run([str(EXTRACT), "--json-pages", str(large), "1", "1"],
                                 env=dict(os.environ, SAMOSA_EXTRACT_MAX_BYTES=str(20 * 1024 * 1024)),
                                 text=True, capture_output=True)
        assert limited.returncode == 65 and json.loads(limited.stdout)["error"] == "file_too_large"
        for name, signature, size in (("large-native.txt", b"text", 21 * 1024 * 1024),
                                      ("over-upload-limit.pdf", b"%PDF-1.4\n", (4 << 30) + 1)):
            oversized = root / name
            with oversized.open("wb") as stream:
                stream.write(signature)
                stream.truncate(size)  # Sparse file: enforce ceilings without a 4 GiB allocation.
            rejected = subprocess.run([str(EXTRACT), "--json", str(oversized)],
                                      text=True, capture_output=True)
            assert rejected.returncode == 65 and json.loads(rejected.stdout)["error"] == "file_too_large"
        image = root / "scan.png"
        raster = Image.new("RGB", (1200, 1600), "white")
        ImageDraw.Draw(raster).text((120, 300), "SCANNED SENTINEL 42", fill="black")
        raster.save(image)
        digital, blank, scanned, mixed, tiled, tiled_covered, tiled_footer, uncovered_scan = (root / name for name in
                                          ("digital.pdf", "blank.pdf", "scanned.pdf", "mixed.pdf", "tiled.pdf", "tiled-covered.pdf", "tiled-footer.pdf", "uncovered-scan.pdf"))
        make_pdf(digital, native="DIGITAL SENTINEL - no OCR")
        make_pdf(blank, blank=True)
        make_pdf(scanned, image=image)
        make_pdf(mixed, image=image, native="NATIVE FOOTER SENTINEL")
        make_tiled_pdf(tiled)
        make_tiled_pdf(tiled_covered, native=True)
        make_adversarial_tiled_pdf(tiled_footer)
        make_adversarial_tiled_pdf(uncovered_scan, upper_native=True)

        page, _ = inspect(digital)
        assert page["inspection"]["kind"] == "digital_text"
        assert not page["inspection"]["needs_ocr"]
        assert page["inspection"]["image_count"] == 0

        page, _ = inspect(blank)
        assert page["inspection"]["blank"]
        assert not page["inspection"]["needs_ocr"]

        page, _ = inspect(scanned)
        assert page["inspection"]["kind"] == "visual_only"
        assert page["inspection"]["needs_ocr"]
        assert page["inspection"]["image_coverage"] > 0.4

        page, _ = inspect(mixed)
        check = page["inspection"]
        assert check["kind"] == "mixed" and check["needs_ocr"] and check["ocr_region"]
        assert check["image_coverage"] > 0.4
        assert page["text"] == "NATIVE FOOTER SENTINEL"

        page, _ = inspect(tiled)
        assert page["inspection"]["needs_ocr"], "small scan tiles must still route to OCR"
        page, _ = inspect(tiled_covered)
        assert not page["inspection"]["needs_ocr"], "covered raster tiles must reuse native text"
        page, _ = inspect(tiled_footer)
        assert page["inspection"]["needs_ocr"], "a native footer must not cover tiled scanned regions"
        page, _ = inspect(uncovered_scan)
        assert page["inspection"]["needs_ocr"], "native text outside a scan must not suppress OCR"

        full_ppm = root / "full.ppm"
        crop_ppm = root / "crop.ppm"
        run("--render-ppm", scanned, 1, full_ppm)
        run("--render-ocr-ppm", mixed, 1, crop_ppm,
            *check["ocr_bounds"])
        full_w, full_h = ppm_size(full_ppm)
        crop_w, crop_h = ppm_size(crop_ppm)
        assert full_w <= 768 and full_h <= 768
        assert crop_w < 2000 and crop_h < 2000
        assert crop_w > full_w // 2 and crop_h > full_h // 2
        ocr_bin = Path(os.environ.get("SAMOSA_OCR", ROOT / "build/samosa-ocr"))
        ocr_env = dict(os.environ)
        ocr_env.setdefault("SAMOSA_OCR_PACK", str(Path.home() / ".samosa/models/ocr-pack-v1"))
        ocr = subprocess.run([str(ocr_bin), "read", str(crop_ppm)], env=ocr_env,
                             check=True, text=True, capture_output=True)
        ocr_result = json.loads(ocr.stdout)
        assert ocr_result["ok"]
        recovered = " ".join(line["text"] for line in ocr_result["lines"])
        assert "SCANNED SENTINEL 42" in recovered

    print("test_pdf_ocr_routing.py: PASS")


if __name__ == "__main__":
    main()
