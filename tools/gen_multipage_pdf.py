#!/usr/bin/env python3
"""Generate a tiny, hand-computed, valid multi-page PDF fixture.

Produces N pages, each with one line of distinguishable text
("Page K of N"), correct xref byte offsets computed programmatically
(not by hand), and a minimal object graph mirroring
tests/fixtures/documents/hello.pdf's style.
"""
import sys

def build(num_pages: int) -> bytes:
    objs = {}
    # 1: catalog, 2: pages, 3..3+num_pages-1: page objs,
    # 3+num_pages: font, 3+num_pages+1 .. : content stream per page
    page_ids = list(range(3, 3 + num_pages))
    font_id = 3 + num_pages
    content_ids = list(range(font_id + 1, font_id + 1 + num_pages))

    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    kids = " ".join(f"{pid} 0 R" for pid in page_ids)
    objs[2] = f"<< /Type /Pages /Kids [{kids}] /Count {num_pages} >>".encode()
    for i, pid in enumerate(page_ids):
        cid = content_ids[i]
        objs[pid] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            f"/Resources << /Font << /F1 {font_id} 0 R >> >> "
            f"/Contents {cid} 0 R >>"
        ).encode()
    objs[font_id] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    for i, cid in enumerate(content_ids):
        page_num = i + 1
        text = f"Page {page_num} of {num_pages}"
        stream = f"BT\n/F1 24 Tf\n72 720 Td\n({text}) Tj\nET\n".encode()
        objs[cid] = (f"<< /Length {len(stream)} >>\nstream\n".encode()
                      + stream + b"\nendstream")

    all_ids = sorted(objs.keys())
    max_id = all_ids[-1]

    out = bytearray()
    out += b"%PDF-1.4\n%Samosa\n"
    offsets = {0: 0}
    for oid in all_ids:
        offsets[oid] = len(out)
        out += f"{oid} 0 obj\n".encode()
        out += objs[oid]
        out += b"\nendobj\n"

    xref_offset = len(out)
    out += f"xref\n0 {max_id + 1}\n".encode()
    out += b"0000000000 65535 f \n"
    for oid in range(1, max_id + 1):
        off = offsets.get(oid)
        if off is None:
            out += b"0000000000 00000 f \n"
        else:
            out += f"{off:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {max_id + 1} /Root 1 0 R >>\n".encode()
    out += f"startxref\n{xref_offset}\n%%EOF\n".encode()
    return bytes(out)

if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 7
    out_path = sys.argv[2] if len(sys.argv) > 2 else f"multipage_{n}pages.pdf"
    data = build(n)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"wrote {out_path}: {len(data)} bytes, {n} pages")
