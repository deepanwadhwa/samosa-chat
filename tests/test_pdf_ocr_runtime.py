#!/usr/bin/env python3
"""Exercise real PDF rendering and OCR through the gateway on synthetic pixels."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import select
import shlex
from difflib import SequenceMatcher

from PIL import Image
from reportlab.pdfgen import canvas

ROOT = Path(__file__).resolve().parents[1]
OCR = Path(os.environ.get("SAMOSA_OCR", ROOT / "build/samosa-ocr"))
EXTRACT = Path(os.environ.get("SAMOSA_EXTRACT", ROOT / "build/samosa-extract"))
RUNNER = Path(os.environ.get("SAMOSA_OCR_TEST_RUNNER", ROOT / "build/test-document-reader-contract"))

def run(args, env):
    result = subprocess.run(list(map(str, args)), env=env, capture_output=True,
                            text=True, timeout=180)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout)

with tempfile.TemporaryDirectory(prefix="samosa-real-ocr-") as temporary:
    work = Path(temporary)
    home = work / "home"
    (home / "models").mkdir(parents=True)
    env = dict(os.environ, SAMOSA_HOME=str(home), SAMOSA_READ_CACHE_DIR=str(work / "cache"))
    env.pop("SAMOSA_MODELS_DIR", None)
    tiny = ROOT / "tools/testdata/ocr/tiny.png"
    result = run([OCR, "read", tiny], env)
    assert any("2019" in line["text"] for line in result["lines"]), result
    assert all(line["reader"] == "tesseract" for line in result["lines"]), result
    missing = dict(env, SAMOSA_TESSDATA=str(work / "missing-models"))
    result = subprocess.run([str(OCR), "read", str(tiny)], env=missing,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and json.loads(result.stdout)["error"] == "ocr_unavailable", result.stdout
    blank = work / "blank.png"
    Image.new("RGB", (600, 800), "white").save(blank)
    assert run([OCR, "read", blank], env)["lines"] == []
    link = work / "symlink.png"
    link.symlink_to(tiny)
    invalid = subprocess.run([str(OCR), "read", str(link)], env=env, capture_output=True, text=True)
    assert invalid.returncode != 0 and json.loads(invalid.stdout)["error"] == "image_invalid"
    crops = work / "crops"
    crops.mkdir()
    cropped = run([OCR, "read", tiny, "--emit-crops", crops, "--below", "1"], env)
    assert cropped["emitted_crops"] > 0 and list(crops.glob("*.ppm")), cropped
    assert "2019" in run([OCR, "recognize", tiny, "--box", "0,0,320,72"], env)["text"]
    assert run([OCR, "detect", tiny], env)["boxes"]

    # A full raster page, with no selectable PDF text. Only bundled fixture
    # pixels are used; no user attachment is opened.
    raster = Image.new("RGB", (1200, 1600), "white")
    with Image.open(tiny) as line:
        for y in (180, 460, 740, 1020):
            raster.paste(line.convert("RGB"), (120, y))
    png, pdf = work / "scan.png", work / "scan.pdf"
    raster.save(png)
    document = canvas.Canvas(str(pdf), pagesize=(600, 800))
    document.drawImage(str(png), 0, 0, width=600, height=800)
    document.showPage()
    document.save()
    inspected = run([EXTRACT, "--json-pages", pdf, 1, 1], env)
    assert inspected["pages"][0]["inspection"]["needs_ocr"], inspected
    started = time.monotonic()
    result = run([RUNNER, "--real-pdf", pdf, EXTRACT, OCR, home], env)
    assert "2019" in result["text"], result
    assert "STRUCTURED TEXT:" in result["text"] and "OCR:" in result["text"], result
    assert not list(home.glob("doc-read-*")), "temporary render directory leaked"

    # Native text and an embedded raster image must both survive the read,
    # with separate labels in the evidence passed onward to the model.
    mixed_pdf = work / "mixed.pdf"
    mixed_document = canvas.Canvas(str(mixed_pdf), pagesize=(600, 800))
    mixed_document.drawImage(str(png), 0, 0, width=600, height=800)
    mixed_document.setFont("Helvetica", 16)
    mixed_document.drawString(48, 760, "NATIVE STRUCTURED SENTINEL 84")
    mixed_document.showPage()
    mixed_document.save()
    mixed_result = run([RUNNER, "--real-pdf", mixed_pdf, EXTRACT, OCR, home], env)
    assert "NATIVE STRUCTURED SENTINEL 84" in mixed_result["text"], mixed_result
    assert "2019" in mixed_result["text"], mixed_result
    assert mixed_result["text"].count("STRUCTURED TEXT:") == 1, mixed_result
    assert mixed_result["text"].count("OCR:") == 1, mixed_result
    direct = subprocess.run(["sh", str(ROOT / "dist/samosa"), "ocr", str(pdf)],
                            env=dict(env, SAMOSA_EXTRACT=str(EXTRACT), SAMOSA_OCR=str(OCR),
                                     SAMOSA_HOME=str(home)), capture_output=True, text=True,
                            timeout=180)
    assert direct.returncode == 0, (direct.stdout, direct.stderr)
    assert "--- PAGE 1 ---" in direct.stdout and "2019" in direct.stdout, direct.stdout

    # Dense, long lines catch the fixed-320-width regression. Known text is
    # rendered into pixels; the OCR command cannot use the PDF's native text.
    dense = work / "dense.pdf"
    document = canvas.Canvas(str(dense), pagesize=(600, 800))
    expected = []
    sentences = [
        "Weather forecasts help people plan travel, protect crops and prepare for heavy rain.",
        "Residents should check the latest report before setting out on a long journey tomorrow.",
        "A clear account of changing temperatures gives farmers time to protect their plants.",
        "The research team compared several methods using observations collected last winter.",
        "During the afternoon, strong winds carried rain across the coast and into the valley.",
        "Better measurements can improve predictions for communities far from major cities.",
    ]
    for page in range(2):
        document.setFont("Helvetica", 10)
        for line in range(24):
            text = sentences[(page * 3 + line) % len(sentences)]
            expected.append(text)
            document.drawString(30, 755 - line * 29, text)
        document.showPage()
    document.save()
    ppm = work / "dense.ppm"
    rendered = subprocess.run([str(EXTRACT), "--render-ocr-ppm", str(dense), "1", str(ppm)],
                              env=env, capture_output=True, text=True, timeout=30)
    assert rendered.returncode == 0, rendered.stderr
    serial = run([OCR, "read", ppm], env)
    serial_text = " ".join(line["text"] for line in serial["lines"])
    normalize = lambda text: "".join(c.lower() for c in text if c.isalnum())
    similarity = SequenceMatcher(None, normalize(" ".join(expected[:24])),
                                 normalize(serial_text), autojunk=False).ratio()
    assert similarity > 0.97, (similarity, serial_text)
    direct_args = ["sh", str(ROOT / "dist/samosa"), "ocr", str(dense)]
    direct_env = dict(env, SAMOSA_EXTRACT=str(EXTRACT), SAMOSA_OCR=str(OCR))
    # Runtime must work even when invoking Python would fail.
    trap_bin = work / "trap-bin"
    trap_bin.mkdir()
    for name in ("python", "python3"):
        trap = trap_bin / name
        trap.write_text("#!/bin/sh\necho 'Python must not run during OCR' >&2\nexit 99\n")
        trap.chmod(0o700)
    direct_env["PATH"] = str(trap_bin) + ":/usr/bin:/bin"
    direct = subprocess.run(direct_args, env=direct_env, capture_output=True, text=True, timeout=180)
    assert direct.returncode == 0, (direct.stdout, direct.stderr)
    assert direct.stdout.count("--- PAGE ") == 2, direct.stdout
    for page in range(2):
        output = direct.stdout.split(f"--- PAGE {page + 1} ---")[1].split("--- PAGE")[0]
        similarity = SequenceMatcher(None, normalize(" ".join(expected[page * 24:(page + 1) * 24])),
                                     normalize(output), autojunk=False).ratio()
        assert similarity > 0.97, (page, similarity, output)
    failed = subprocess.run(direct_args, env=dict(direct_env, SAMOSA_TESSDATA=str(work / "missing")),
                            capture_output=True, text=True, timeout=30)
    assert failed.returncode != 0 and "ocr_unavailable" in failed.stdout, failed

    # Hold the second page's renderer until page one's text is observable.
    release = work / "continue"
    renderer = work / "gated-renderer"
    renderer.write_text("#!/bin/sh\n"
                        "if [ \"$1\" = --render-ocr-ppm ] && [ \"$3\" = 2 ]; then\n"
                        f"  while [ ! -f {shlex.quote(str(release))} ]; do sleep 0.02; done\nfi\n"
                        f"exec {shlex.quote(str(EXTRACT))} \"$@\"\n")
    renderer.chmod(0o700)
    process = subprocess.Popen(direct_args, env=dict(direct_env, SAMOSA_EXTRACT=str(renderer)),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    first = b""
    try:
        deadline = time.monotonic() + 30
        while b"protect crops" not in first and time.monotonic() < deadline:
            if select.select([process.stdout], [], [], 1)[0]:
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                first += chunk
        assert b"--- PAGE 1 ---" in first and b"protect crops" in first, first
        assert process.poll() is None, "page two should still be waiting"
        release.touch()
        rest, errors = process.communicate(timeout=30)
        assert process.returncode == 0 and b"--- PAGE 2 ---" in rest, (rest, errors)
    finally:
        release.touch()
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)
    print(json.dumps({"seconds": round(time.monotonic() - started, 2),
                      "text": result["text"]}), flush=True)
print("test_pdf_ocr_runtime.py: PASS", flush=True)
