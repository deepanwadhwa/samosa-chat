#!/usr/bin/env python3
"""Generate independent memory-sampling fixtures (test-only ReportLab/Pillow)."""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=False)
font_path = next(path for path in (
    Path("/System/Library/Fonts/Supplemental/Arial.ttf"),
    Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
) if path.is_file())
font = ImageFont.truetype(str(font_path), 28)
line = "The harbor crew checked blue lanterns and signed the register."


def draw_native(document, rows, size=10):
    document.setFont("Helvetica", size)
    for row in range(rows):
        document.drawString(32, 760 - row * (size + 1), f"{row + 1:02d}. {line}")


def scanned_image(rows):
    image = Image.new("RGB", (1600, 2100), "white")
    draw = ImageDraw.Draw(image)
    for row in range(rows):
        draw.text((70, 70 + row * 42), f"{row + 1:02d}. {line}", font=font, fill="black")
    return image


for name in ("Native pages.pdf", "Mixed text and scans.pdf", "Large first page.pdf", "Blank pages.pdf"):
    document = canvas.Canvas(str(root / name), pagesize=(612, 792), invariant=1)
    for page in range(20):
        if name == "Blank pages.pdf":
            pass
        elif name == "Large first page.pdf":
            draw_native(document, 75, 8)
        elif name == "Mixed text and scans.pdf" and page in (1, 2):
            document.drawImage(ImageReader(scanned_image(20)), 0, 0, width=612, height=792)
        else:
            draw_native(document, 8 if name == "Mixed text and scans.pdf" and page == 0 else 16)
        if page >= 10 and name != "Blank pages.pdf":
            document.drawString(32, 40, "UNREAD_TAIL_SENTINEL")
        document.showPage()
    document.save()
scanned_image(45).save(root / "Single scanned image.png")
(root / "Long Unicode text.txt").write_text(
    "Café résumé: The harbor crew checked the blue lanterns and signed the safety register.\n" * 120
    + "UNREAD_TEXT_TAIL_SENTINEL", encoding="utf-8")
print(f"Created six independent fixtures: {root}")
