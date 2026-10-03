#!/usr/bin/env python3
"""Real 40-page OCR through HTTP chat; inspect evidence sent to the model.

Uses only a generated document and an isolated home. The model is deliberately
unhelpful at planning; complete reading must not depend on its cooperation.
"""
import base64
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("BUILD_DIR", "build"))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD
sys.path.insert(0, str(ROOT / "tools"))
from gen_mixed_document_fixture import generate


def run():
    with tempfile.TemporaryDirectory(prefix="samosa-mixed-40-") as temporary:
        work = Path(temporary)
        home = work / "home"
        model = home / "qwen-model"
        model.mkdir(parents=True)
        (home / "run").mkdir()
        (model / "experts.bin").write_text("fixture")
        (work / "tokenizer.json").write_text("fixture")
        document = work / "mixed.pdf"
        manifest = generate(document)
        reader_log, model_log, ocr_log = (work / name for name in ("reader.jsonl", "model.jsonl", "ocr.log"))
        ocr = work / "ocr-wrapper"
        ocr.write_text("#!/bin/sh\n"
                       f"printf '%s\\n' \"$1\" >> {shlex.quote(str(ocr_log))}\n"
                       f"exec {shlex.quote(str(BUILD / 'samosa-ocr'))} \"$@\"\n")
        ocr.chmod(0o700)
        port = 23000 + 2 * (os.getpid() % 1000)
        for _ in range(100):
            try:
                with socket.socket() as first, socket.socket() as second:
                    first.bind(("127.0.0.1", port))
                    second.bind(("127.0.0.1", port + 1))
                break
            except OSError:
                port += 2
        env = dict(os.environ, SAMOSA_HOME=str(home), SAMOSA_PORT=str(port),
                   SAMOSA_CONTEXT_TOKENS=os.environ.get("SAMOSA_MIXED_TEST_CONTEXT", "32768"),
                   SAMOSA_BACKEND_PORT=str(port + 1), SAMOSA_QWEN_MODEL=str(model),
                   SAMOSA_QWEN_ENGINE=str(BUILD / "test_fake_openai_backend"),
                   SAMOSA_TOKENIZER=str(work / "tokenizer.json"),
                   SAMOSA_EXTRACT=str(ROOT / "tests/document_reader_spy.py"),
                   SAMOSA_REAL_DOCUMENT_EXTRACT=str(BUILD / "samosa-extract"),
                   SAMOSA_OCR=str(ocr), SAMOSA_DOCUMENT_READ_LOG=str(reader_log),
                   SAMOSA_FAKE_DOCUMENT_REQUEST_LOG=str(model_log),
                   SAMOSA_READ_CACHE_DIR=str(work / "cache"))
        token = ""

        def request(path, data=None, headers=None):
            headers = {"X-Samosa-Token": token, **(headers or {})}
            if isinstance(data, dict):
                headers["Content-Type"] = "application/json"
                data = json.dumps(data).encode()
            with urllib.request.urlopen(urllib.request.Request(
                    f"http://127.0.0.1:{port}{path}", data=data, headers=headers), timeout=240) as reply:
                return json.load(reply)

        def log(path):
            return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

        with (work / "gateway.log").open("w") as output:
            gateway = subprocess.Popen([str(BUILD / "samosa-gateway")], env=env, stdout=output, stderr=output)
            try:
                for _ in range(150):
                    try:
                        if request("/healthz").get("ready"):
                            break
                    except OSError:
                        pass
                    time.sleep(.05)
                else:
                    raise AssertionError((work / "gateway.log").read_text())
                token = (home / "run/ui-token").read_text().strip()
                attachment = request("/v1/attachments", document.read_bytes(), {
                    "X-Samosa-Filename-B64": base64.b64encode(b"Harbor report.pdf").decode()})["id"]
                for depth in ("fast", "detailed"):
                    started = time.monotonic()
                    # Existing fake model returns invalid planning JSON for this
                    # suffix. The ordinary user instruction must win anyway.
                    body = {
                        "model_id": "qwen", "model_version": "qwen-model",
                        "analysis_depth": depth, "conversation_id": "synthetic-40",
                        "attachment_ids": [attachment] if depth == "fast" else [],
                        "messages": [{"role": "user", "content":
                            "Read this document carefully. OCR it. Understand the text. harness invalid probe"
                            if depth == "fast" else "Check in more detail."}], "stream": False}
                    if os.environ.get("SAMOSA_FAKE_DOCUMENT_REVIEW_FAIL"):
                        try:
                            request("/v1/chat/completions", body)
                        except urllib.error.HTTPError as error:
                            failure = json.load(error)
                            assert error.code == 422 and failure["error"]["code"] == "document_review_incomplete", failure
                            requests = log(model_log)
                            assert requests and all("Read this consecutive section of a document" in json.dumps(r) for r in requests), "failed review still generated an answer"
                            print("test_document_mixed_pdf.py: PASS (failed section review blocks synthesis)", flush=True)
                            return
                        raise AssertionError("Failed review incorrectly returned a complete answer")
                    request("/v1/chat/completions", body)
                    requests = log(model_log)
                    final = requests[-1]
                    evidence = "\n".join(message.get("content", "") for message in final["messages"])
                    reviews = [r for r in requests if "Read this consecutive section of a document" in json.dumps(r)]
                    reading_input = "\n".join(m.get("content", "") for r in reviews for m in r["messages"]) if reviews else evidence
                    missing = [page["page"] for page in manifest if page["fact"] not in reading_input]
                    assert not missing, f"{depth}: missing page facts in actual model input: {missing}"
                    assert ("mode=reviewed" if reviews else "mode=full") in evidence and "mode=retrieval" not in evidence
                    assert "[PDF page 40 of 40]" in reading_input, "full-reader citations lost page numbering"
                    if reviews:
                        assert "SYNTHETIC SECTION REVIEW" in evidence and "not verbatim source text" in evidence
                        assert len(reviews) >= 3, "long document was not read in consecutive sections"
                    assert not any("Plan the next document evidence action" in json.dumps(r) for r in requests), "broad reading still asks a planner for permission to read"
                    reads = [r for r in log(reader_log) if r[0] == "--json-pages"]
                    assert [(int(r[2]), int(r[3])) for r in reads] == [(n, 5) for n in range(1, 41, 5)], reads
                    ocr_reads = ocr_log.read_text().splitlines().count("read")
                    assert ocr_reads == sum(p["kind"] != "native" for p in manifest), ocr_reads
                    assert not list(home.glob("doc-read-*")), "render directory leaked"
                    print(json.dumps({"depth": depth, "pages": 40, "ocr_pages": ocr_reads,
                                      "seconds": round(time.monotonic() - started, 2),
                                      "review_calls": len(reviews),
                                      "all_page_facts_reached_model": True}), flush=True)
            finally:
                gateway.terminate()
                try:
                    gateway.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    gateway.kill()
                    gateway.wait()
    print("test_document_mixed_pdf.py: PASS", flush=True)


if __name__ == "__main__":
    run()
