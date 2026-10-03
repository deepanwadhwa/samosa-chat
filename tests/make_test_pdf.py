#!/usr/bin/env python3
"""Write a small one-page, text-searchable PDF for offline integration tests."""
from pathlib import Path
import sys


def pdf_escape(text: str) -> bytes:
    data = text.encode("ascii")
    return data.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)")


def main() -> None:
    output = Path(sys.argv[1])
    text = sys.argv[2]
    stream = b"BT /F1 12 Tf 72 720 Td (" + pdf_escape(text) + b") Tj ET\n"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Length " + str(len(stream)).encode("ascii") + b" >>\nstream\n" + stream + b"endstream",
    ]
    document = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for index, body in enumerate(objects, 1):
        offsets.append(len(document))
        document.extend(f"{index} 0 obj\n".encode("ascii"))
        document.extend(body + b"\nendobj\n")
    xref = len(document)
    document.extend(f"xref\n0 {len(offsets)}\n".encode("ascii"))
    document.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        document.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    document.extend(
        f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii")
    )
    output.write_bytes(document)


if __name__ == "__main__":
    main()
