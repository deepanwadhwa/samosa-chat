#!/usr/bin/env python3
"""Independent 40-page PDF: native text, scans, and native/scan mixed pages.

No user documents are inputs. The manifest records every expected page fact.
"""
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


def generate(destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    font_path = next((path for path in (
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ) if Path(path).is_file()), "DejaVuSans.ttf")
    font = ImageFont.truetype(font_path, 30)
    document = canvas.Canvas(str(destination), pagesize=(612, 792), invariant=1)
    manifest = []
    for page in range(1, 41):
        kind = ("native", "scanned", "mixed")[(page - 1) % 3]
        # End with a scan so reading only early/native pages cannot pass.
        if page == 40:
            kind = "scanned"
        fact = f"Station {page:02d} approved {7100 + page} blue lanterns."
        lines = [f"Harbor inspection report - page {page} of 40", fact]
        lines += [
            f"Record {page:02d}.{row:02d}: the crew checked the storage room and signed the register."
            for row in range(1, 11)
        ]
        if kind in ("scanned", "mixed"):
            raster = Image.new("RGB", (1530, 1740), "white")
            draw = ImageDraw.Draw(raster)
            for row, line in enumerate(lines):
                draw.text((95, 110 + row * 75), line, font=font, fill="black")
            document.drawImage(ImageReader(raster), 0, 0, width=612, height=696)
        else:
            document.setFont("Helvetica", 12)
            for row, line in enumerate(lines):
                document.drawString(38, 705 - row * 30, line)
        if kind == "mixed":
            document.setFont("Helvetica", 14)
            document.drawString(38, 752, f"Native cover label for station {page:02d}")
        document.showPage()
        manifest.append({"page": page, "kind": kind, "fact": fact})
    document.save()
    destination.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    generate(sys.argv[1])
    print(f"Created synthetic 40-page PDF: {sys.argv[1]}")
