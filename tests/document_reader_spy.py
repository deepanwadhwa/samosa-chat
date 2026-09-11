#!/usr/bin/env python3
"""Extractor spy for document-harness integration tests; optionally runs PDFium."""
import json
import os
import subprocess
import sys

args = sys.argv[1:]
if args == ["--version"]:
    print("samosa-extract 0 (harness-test;pdfium)")
    sys.exit(0)
with open(os.environ["SAMOSA_DOCUMENT_READ_LOG"], "a") as log:
    log.write(json.dumps(args) + "\n")
failure = os.environ.get("SAMOSA_DOCUMENT_FAILURE")
if failure and os.path.exists(failure):
    with open(failure) as source:
        print(json.dumps({"ok": False, "error": source.read().strip()}))
    sys.exit(65)
real = os.environ.get("SAMOSA_REAL_DOCUMENT_EXTRACT")
if real:
    sys.exit(subprocess.call([real, *args]))
if args[0] == "--json-pages":
    start, count = map(int, args[2:4])
    assert 1 <= start <= 100 and 1 <= count <= 5, args
    block = os.environ.get("SAMOSA_DOCUMENT_BLOCK")
    ready = os.environ.get("SAMOSA_DOCUMENT_BLOCK_READY")
    if block and os.path.exists(block):
        if ready:
            open(ready, "w").close()
        while True:
            import time
            time.sleep(1)
    if os.environ.get("SAMOSA_DOCUMENT_UNCERTAIN") and os.path.exists(os.environ["SAMOSA_DOCUMENT_UNCERTAIN"]):
        print(json.dumps({"ok": True, "page_count": 1, "pages": [{
            "index": 1, "text_chars": 0, "tokens": 0, "has_raster_figure": True,
            "text": "", "inspection": {"version": 1, "kind": "visual_only",
            "reason": "no_usable_text", "blank": False, "incomplete": False,
            "needs_ocr": True, "ocr_region": False, "ocr_bounds": [0, 0, 1, 1]}
        }]}))
        sys.exit(0)
    if os.environ.get("SAMOSA_DOCUMENT_NATIVE_FAILURE") and os.path.exists(os.environ["SAMOSA_DOCUMENT_NATIVE_FAILURE"]):
        print(json.dumps({"ok": True, "page_count": 1, "pages": [{
            "index": 1, "text_chars": 31, "tokens": 6, "has_raster_figure": True,
            "text": "PRESERVED NATIVE SENTINEL 99", "inspection": {
                "version": 1, "kind": "mixed", "reason": "image_region_without_text",
                "blank": False, "incomplete": False, "needs_ocr": True,
                "ocr_region": True, "ocr_bounds": [0, 0, 1, 1]
            }
        }]}))
        sys.exit(0)
    if os.environ.get("SAMOSA_DOCUMENT_INCOMPLETE") and os.path.exists(os.environ["SAMOSA_DOCUMENT_INCOMPLETE"]):
        print(json.dumps({"ok": True, "page_count": 1, "pages": [{
            "index": 1, "text_chars": 31, "tokens": 6, "has_raster_figure": True,
            "text": "INCOMPLETE INSPECTION SENTINEL 88", "inspection": {
                "version": 1, "kind": "mixed", "reason": "inspection_limit",
                "blank": False, "incomplete": True, "needs_ocr": True,
                "ocr_region": False, "ocr_bounds": [0, 0, 1, 1]
            }
        }]}))
        sys.exit(0)
    pages = [{"index": page, "text_chars": 20, "text": f"Page {page} of 100",
              "has_raster_figure": False} for page in range(start, min(101, start + count))]
    print(json.dumps({"ok": True, "page_count": 100, "pages": pages}))
elif args[0] == "--json":
    with open(args[1]) as source:
        print(json.dumps({"ok": True, "text": source.read()}))
elif args[0] == "--render-ocr-ppm":
    with open(args[3], "wb") as ppm:
        ppm.write(b"P6\n1 1\n255\n\x00\x00\x00")
    sys.exit(0)
else:
    # Even a very short digital page must not trigger rendering or OCR.
    sys.exit(64)
