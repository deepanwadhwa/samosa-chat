#!/usr/bin/env python3
"""Synthetic FW acceptance folder; requires the existing PDF/OCR test venv."""
import json
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas
from gen_multipage_pdf import build

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=False)
relevant = ["Person X records.pdf", "Person X appointment.txt", "Person X scan.pdf"]
(root / relevant[0]).write_bytes(build(40).replace(b"Page 37 of 40", b"Code: QZ-731 ", 1))
(root / relevant[1]).write_text("Person X appointment date is 17 May 2031. Reference: WX-482.\n")
image = Image.new("RGB", (1200, 1600), "white")
font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 42)
ImageDraw.Draw(image).text((100, 180), "Person X scanned certificate\nScan reference: SC-913\nIssued 4 April 2030", fill="black", font=font, spacing=25)
scan = canvas.Canvas(str(root / relevant[2]), pagesize=(600, 800))
scan.drawImage(ImageReader(image), 0, 0, width=600, height=800); scan.showPage(); scan.save()
for index in range(9):
    relative = f"{('01', '02', 'misc')[index % 3]}/" + ("report.txt" if index < 2 else f"note-{index}.txt")
    path = root / relative; path.parent.mkdir(exist_ok=True)
    path.write_text(f"Person X document {index + 1}. Record reference: X-{index + 101}.\n")
    relevant.append(relative)
distractors = []
for index in range(8):
    person = "Person Y" if index < 7 else "Person Xavier"
    relative = f"{person} record-{index}.txt"
    (root / relative).write_text(f"{person} appointment date is 19 June 2032. Reference: YD-{index + 991}.\n")
    distractors.append(relative)
(root / "unsupported.bin").write_bytes(b"\0\xffunsupported synthetic bytes")
(root / "unreadable.txt").write_text("Person X inaccessible coverage case\n")
(root / "unreadable.txt").chmod(0)
outside = root.parent / (root.name + "-outside.txt")
outside.write_text("Person X outside authorized root. Never read or select.\n")
(root / "outside-link.txt").symlink_to(outside)
manifest = {"version": 2, "root": str(root), "relevant": relevant, "distractors": distractors,
            "coverage_cases": ["unsupported.bin", "unreadable.txt", "outside-link.txt"],
            "facts": {"Person X records.pdf": {"page": 37, "code": "QZ-731"},
                      "Person X appointment.txt": {"date": "17 May 2031", "reference": "WX-482"},
                      "Person X scan.pdf": {"page": 1, "code": "SC-913"}}}
manifest_path = root.parent / (root.name + "-manifest.json")
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
print(manifest_path)
