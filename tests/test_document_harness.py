#!/usr/bin/env python3
"""Exercise model-directed document I/O through a real gateway and HTTP backend.

Set SAMOSA_REAL_DOCUMENT_EXTRACT to a PDFium reader to repeat against a real
100-page PDF. Assertions inspect actual reader invocations and model inputs,
not just a canned final answer.
"""
import base64
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = Path(os.environ.get("BUILD_DIR", "build"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = ROOT / BUILD_DIR
sys.path.insert(0, str(ROOT / "tools"))
from gen_multipage_pdf import build


def run():
    with tempfile.TemporaryDirectory(prefix="samosa-document-harness-") as temporary:
        work = Path(temporary)
        home = work / "home"
        model = home / "qwen-model"
        model.mkdir(parents=True)
        (home / "run").mkdir()
        (model / "experts.bin").write_text("fixture")
        (work / "app.html").write_text("<!doctype html><title>Harness</title>")
        (work / "logo.png").write_bytes(b"png")
        (work / "tokenizer.json").write_text("fixture")
        reader_log = work / "reader.jsonl"
        model_log = work / "model.jsonl"
        ocr_script = work / "ocr-spy.py"
        ocr_fail_trigger = work / "ocr-fail.trigger"
        ocr_sequence = work / "ocr-sequence"
        ocr_script.write_text("#!/usr/bin/env python3\nimport json, os, sys\nif sys.argv[1:] == ['--version']:\n    print('samosa-ocr 0 (harness)')\nelif os.environ.get('SAMOSA_OCR_FAIL_FILE') and os.path.exists(os.environ['SAMOSA_OCR_FAIL_FILE']):\n    sys.exit(23)\nelif os.environ.get('SAMOSA_OCR_SEQUENCE_FILE') and os.path.exists(os.environ['SAMOSA_OCR_SEQUENCE_FILE']) and open(os.environ['SAMOSA_OCR_SEQUENCE_FILE']).read().strip() == 'malformed':\n    print('{\\\"ok\\\":true,\\\"lines\\\":[}', end='')\nelif os.environ.get('SAMOSA_OCR_SEQUENCE_FILE') and os.path.exists(os.environ['SAMOSA_OCR_SEQUENCE_FILE']) and open(os.environ['SAMOSA_OCR_SEQUENCE_FILE']).read().strip() == 'repaired':\n    print(json.dumps({'ok': True, 'lines': [{'text': 'REPAIRED TEXT 77', 'conf': 0.99, 'bbox': [0, 0, 1, 1]}]}))\nelse:\n    print(json.dumps({'ok': True, 'lines': [{'text': 'UNCERTAIN SENTINEL 30', 'conf': 0.30, 'bbox': [0, 0, 1, 1]}]}))\n")
        ocr_script.chmod(0o755)
        uncertain_trigger = work / "uncertain.trigger"
        native_failure_trigger = work / "native-failure.trigger"
        reader_failure_trigger = work / "reader-failure.trigger"
        incomplete_trigger = work / "incomplete.trigger"
        block_reader = work / "block-reader"
        block_ready = work / "block-reader.ready"
        # Keep listeners outside the OS ephemeral client-port range and check
        # both gateway/backend ports before launch.
        port = 20000 + 2 * (os.getpid() % 1000)
        for _ in range(100):
            try:
                with socket.socket() as probe, socket.socket() as backend_probe:
                    probe.bind(("127.0.0.1", port))
                    backend_probe.bind(("127.0.0.1", port + 1))
                break
            except OSError:
                port += 2
        else:
            raise AssertionError("No available test port pair")
        env = dict(os.environ, SAMOSA_HOME=str(home), SAMOSA_PORT=str(port),
                   SAMOSA_DOCUMENT_FAILURE=str(reader_failure_trigger),
                   SAMOSA_BACKEND_PORT=str(port + 1),
                   SAMOSA_APP_HTML=str(work / "app.html"), SAMOSA_APP_LOGO=str(work / "logo.png"),
                   SAMOSA_QWEN_ENGINE=str(BUILD_DIR / "test_fake_openai_backend"),
                   SAMOSA_QWEN_MODEL=str(model), SAMOSA_TOKENIZER=str(work / "tokenizer.json"),
                   SAMOSA_EXTRACT=str(ROOT / "tests/document_reader_spy.py"),
                   SAMOSA_OCR=str(ocr_script), SAMOSA_DOCUMENT_READ_LOG=str(reader_log),
                   SAMOSA_DOCUMENT_UNCERTAIN=str(uncertain_trigger),
                   SAMOSA_DOCUMENT_NATIVE_FAILURE=str(native_failure_trigger),
                   SAMOSA_DOCUMENT_INCOMPLETE=str(incomplete_trigger),
                   SAMOSA_OCR_FAIL_FILE=str(ocr_fail_trigger),
                   SAMOSA_OCR_SEQUENCE_FILE=str(ocr_sequence),
                   SAMOSA_FAKE_DOCUMENT_REQUEST_LOG=str(model_log),
                   SAMOSA_DOCUMENT_BLOCK=str(block_reader),
                   SAMOSA_DOCUMENT_BLOCK_READY=str(block_ready),
                   SAMOSA_READ_CACHE_DIR=str(work / "cache"))
        token = ""

        def request(path, data=None, extra=None):
            headers = {"X-Samosa-Token": token, **(extra or {})}
            if isinstance(data, dict):
                headers["Content-Type"] = "application/json"
                data = json.dumps(data).encode()
            with urllib.request.urlopen(urllib.request.Request(
                    f"http://127.0.0.1:{port}{path}", data=data, headers=headers), timeout=30) as reply:
                return json.load(reply)

        def log(path):
            return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

        def upload(filename, content):
            return request("/v1/attachments", content, {
                "X-Samosa-Filename-B64": base64.b64encode(filename.encode()).decode()})["id"]

        def chat(question, ids=None, conversation=None, prior=None):
            body = {"model_id": "qwen", "model_version": "qwen-model", "analysis_depth": "fast",
                    "messages": [*(prior or []), {"role": "user", "content": question}], "stream": False}
            if ids is not None:
                body["attachment_ids"] = ids
            if conversation:
                body["conversation_id"] = conversation
            request("/v1/chat/completions", body)
            return log(model_log)[-1]

        def stream_chat(question, ids):
            body = {"model_id": "qwen", "model_version": "qwen-model", "analysis_depth": "fast",
                    "messages": [{"role": "user", "content": question}], "stream": True, "attachment_ids": ids}
            headers = {"X-Samosa-Token": token, "Content-Type": "application/json"}
            events = []
            with urllib.request.urlopen(urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/chat/completions", data=json.dumps(body).encode(), headers=headers), timeout=30) as reply:
                for line in reply:
                    if line.startswith(b"data: ") and line.strip() != b"data: [DONE]":
                        events.append(json.loads(line[6:]))
            return events

        with (work / "gateway.log").open("w") as output:
            gateway = subprocess.Popen([str(BUILD_DIR / "samosa-gateway")], env=env, stdout=output, stderr=output)
            try:
                last_health = None
                for _ in range(150):
                    try:
                        health = request("/healthz")
                        last_health = health
                        if health.get("ready"):
                            break
                    except OSError as error:
                        last_health = repr(error)
                    time.sleep(.05)
                else:
                    raise AssertionError((last_health, (work / "gateway.log").read_text()))
                token = (home / "run/ui-token").read_text().strip()
                # The fixture has no real resident tensors for the identity
                # probe; readiness, not active_model_id, is its serving signal.
                time.sleep(.1)
                pdf = upload("Pride and Prejudice - Jane Austen.pdf", build(100))
                reader_log.write_text("")
                last = chat("harness metadata probe: who is the author of this book?", [pdf], "metadata-test")
                assert log(reader_log) == [], log(reader_log)
                assert "Jane Austen" in json.dumps(last) and "No document content read" in json.dumps(last)
                manifest = json.loads((home / "chats/metadata-test/documents.json").read_text())
                assert '"mode": "full"' not in json.dumps(manifest), manifest

                last = chat("harness title probe: verify the author on the opening pages", conversation="metadata-test")
                reads = log(reader_log)
                assert [(r[2], r[3]) for r in reads] == [("1", "2")], reads
                assert "[PDF page 1]" in json.dumps(last) and "[PDF page 2]" in json.dumps(last)
                assert "[PDF page 3]" not in json.dumps(last)
                assert "mode=selected" in json.dumps(last) and "mode=full" not in json.dumps(last)
                snapshot = home / "chats/metadata-test/document-context.txt"
                assert "[PDF page 2]" in snapshot.read_text()
                request("/v1/compact", {"conversation_id": "metadata-test"})
                assert "[PDF page 2]" in json.dumps(log(model_log)[-1])
                assert log(reader_log) == reads, "compaction started reading"
                last = chat("harness reuse probe: what do those pages say?", conversation="metadata-test")
                assert log(reader_log) == reads, "follow-up discarded already inspected evidence"
                reused_user = "\n".join(m["content"] for m in last["messages"] if m["role"] == "user")
                assert "[PDF page 2]" in reused_user and "No document content read" not in reused_user

                # Cache hit preserves absolute page numbering and performs no I/O.
                chat("harness title probe", [pdf])
                assert log(reader_log) == reads, (reads, log(reader_log))
                last = chat("harness jump probe: find the referenced section", [pdf])
                new_reads = log(reader_log)[len(reads):]
                assert [(r[2], r[3]) for r in new_reads] == [("1", "1"), ("73", "1")], new_reads
                assert "[PDF page 73]" in json.dumps(last) and "[PDF page 72]" not in json.dumps(last)
                before = len(log(reader_log))
                chat("harness invalid probe", [pdf])
                assert len(log(reader_log)) == before, ("invalid plan bypassed the bounded cache", log(reader_log)[before:], log(model_log)[-4:])
                last = chat("harness repeat probe", [pdf])
                assert len(log(reader_log)) == before + 1, "duplicate operation executed"
                assert "Stopped repeated planning" in json.dumps(last)

                before = len(log(reader_log))
                model_before = len(log(model_log))
                events = stream_chat("harness batch probe", [pdf])
                assert [(r[2], r[3]) for r in log(reader_log)[before:]] == [("1", "5"), ("6", "4")]
                planners = [r for r in log(model_log)[model_before:]
                            if "Plan the next document evidence action" in json.dumps(r)]
                assert len(planners) == 2, "each extraction batch incurred another model decision"
                activities = [e["choices"][0]["delta"]["file_activity"] for e in events
                              if e.get("choices") and "file_activity" in e["choices"][0].get("delta", {})]
                reading = next(a for a in activities if a["stage"] == "reading")
                assert "pages 1–9" in reading["message"] and "find the first chapter" in reading["message"], reading
                checking = next(a for a in activities if a["stage"] == "checking")
                assert "Read PDF pages 1–9" in checking["message"]
                assert activities.index(reading) < activities.index(checking)
                assert reading["indeterminate"], "unknown duration must not pretend to measure completion"

                before = len(log(reader_log))
                last = chat("harness overlap probe", [pdf])
                assert [(r[2], r[3]) for r in log(reader_log)[before:]] == [("1", "3"), ("4", "2")]
                assert json.dumps(last["messages"]).count("[PDF page 3]") == 1

                # The full-read action remains available and cannot use a partial cache as full.
                before = len(log(reader_log))
                last = chat("harness full probe: summarize the entire book", [pdf])
                assert [(r[2], r[3]) for r in log(reader_log)[before:]] == [
                    (str(page), "5") for page in range(1, 101, 5)
                ], "partial cache poisoned full read"
                reads = log(reader_log)
                assert all(r[0] == "--json-pages" and 1 <= int(r[3]) <= 5 for r in reads), reads

                text_id = upload("notes - Jane Austen.txt", b"Opening text\n" + b"z" * 9000 + b"UNREAD_SENTINEL")
                last = chat("harness text probe: inspect the opening", [text_id])
                assert "Opening text" in json.dumps(last) and "UNREAD_SENTINEL" not in json.dumps(last)
                large_pdf = upload("large-paper.pdf", build(43, padding_bytes=21 * 1024 * 1024))
                before = len(log(reader_log))
                last = chat("harness title probe: verify the author on the opening pages", [large_pdf])
                assert [(r[2], r[3]) for r in log(reader_log)[before:]] == [("1", "2")]
                assert "[PDF page 1]" in json.dumps(last)
                assert "[PDF page 3]" not in json.dumps(last)

                for code, expected_status in (("file_too_large", 413), ("pdf_encrypted", 422)):
                    failed_pdf = upload("failed.pdf", build(3) + ("\n%" + code).encode())
                    reader_failure_trigger.write_text(code)
                    before = len(log(reader_log))
                    try:
                        chat("harness title probe", [failed_pdf])
                        raise AssertionError("reader failure was turned into a model answer")
                    except urllib.error.HTTPError as error:
                        assert error.code == expected_status, (error.code, error.read())
                        detail = error.read().decode()
                        assert ("limit" if code == "file_too_large" else code) in detail, detail
                    assert len(log(reader_log)) == before + 1, "failed reader was retried"
                    events = stream_chat("harness title probe", [failed_pdf])
                    assert any("error" in e for e in events), events
                    assert any(code in json.dumps(e) and "read_failed" in json.dumps(e) for e in events), events
                    assert not any(e.get("choices", [{}])[0].get("delta", {}).get("content") for e in events if e.get("choices")), events
                    assert len(log(reader_log)) == before + 2, "streaming failed reader was retried"
                    reader_failure_trigger.unlink()
                    before = len(log(reader_log))
                    last = chat("harness title probe", [failed_pdf])
                    assert len(log(reader_log)) == before + 1, "failed read poisoned the cache"
                    assert "[PDF page 1]" in json.dumps(last)

                if os.environ.get("SAMOSA_REAL_DOCUMENT_EXTRACT"):
                    print("test_document_harness.py: PASS (real extractor; metadata, bounded pages, adaptive jump, cache, follow-up, compaction, full read, text, >20 MiB PDF, structured failures and recovery)")
                    return
                uncertain_id = upload("uncertain.pdf", build(1))
                uncertain_trigger.write_text("on")
                model_before = len(log(model_log))
                last = chat("harness uncertainty probe: read this page", [uncertain_id])
                uncertain_trigger.unlink()
                uncertainty_requests = json.dumps(log(model_log)[model_before:])
                assert "UNCERTAIN SENTINEL 30" in uncertainty_requests, uncertainty_requests
                assert "Reader warning: OCR confidence/coverage is uncertain" in uncertainty_requests
                uncertain_trigger.write_text("on")
                before_refresh = len(log(reader_log))
                refresh_events = stream_chat("harness refresh probe: retry the OCR now", [uncertain_id])
                refresh_reads = log(reader_log)[before_refresh:]
                assert any(r[0] == "--json-pages" for r in refresh_reads), refresh_reads
                refresh_activities = [e["choices"][0]["delta"]["file_activity"] for e in refresh_events
                    if e.get("choices") and "file_activity" in e["choices"][0].get("delta", {})]
                assert any(a["stage"] == "reading" and "Retrying PDF pages" in a["message"]
                           for a in refresh_activities), refresh_activities
                uncertain_trigger.unlink()
                incomplete_id = upload("incomplete.pdf", build(1) + b"\n\n")
                incomplete_trigger.write_text("on")
                incomplete_model_before = len(log(model_log))
                last = chat("harness incomplete probe: read this page", [incomplete_id])
                incomplete_trigger.unlink()
                incomplete_requests = json.dumps(log(model_log)[incomplete_model_before:])
                assert "INCOMPLETE INSPECTION SENTINEL 88" in incomplete_requests, incomplete_requests
                assert "page inspection was incomplete" in incomplete_requests, incomplete_requests
                assert "partial/retryable" in incomplete_requests, incomplete_requests
                native_failure_id = upload("native-failure.pdf", build(1) + b"\n")
                native_failure_trigger.write_text("on")
                ocr_fail_trigger.write_text("on")
                native_model_before = len(log(model_log))
                last = chat("harness native failure probe: read this page", [native_failure_id])
                native_failure_trigger.unlink()
                ocr_fail_trigger.unlink()
                native_requests = json.dumps(log(model_log)[native_model_before:])
                assert "PRESERVED NATIVE SENTINEL 99" in native_requests, native_requests
                assert "OCR failed or was unavailable" in native_requests, native_requests
                assert "partial/retryable" in native_requests, native_requests
                native_failure_trigger.write_text("on")
                ocr_sequence.write_text("malformed")
                before_retry = len(log(reader_log))
                first_retry = chat("harness native malformed retry probe", [native_failure_id], "native-retry-1")
                first_retry_requests = json.dumps(log(model_log)[native_model_before:])
                assert "PRESERVED NATIVE SENTINEL 99" in first_retry_requests, first_retry_requests
                assert "OCR failed or was unavailable" in first_retry_requests, first_retry_requests
                ocr_sequence.write_text("repaired")
                second_retry = chat("harness native malformed retry probe", [native_failure_id], "native-retry-2")
                retry_reads = log(reader_log)[before_retry:]
                assert len(retry_reads) == 4 and sum(r[0] == "--json-pages" for r in retry_reads) == 2, retry_reads
                assert "REPAIRED TEXT 77" in json.dumps(second_retry), second_retry
                native_failure_trigger.unlink()
                ocr_sequence.unlink()
                before = len(log(reader_log))
                chat("harness irrelevant probe: which filename is a PDF?", [pdf, text_id])
                assert len(log(reader_log)) == before
                with ThreadPoolExecutor(max_workers=1) as executor:
                    pending = executor.submit(chat, "harness cancellation probe", [pdf])
                    for _ in range(100):
                        if "Plan the next document evidence action" in json.dumps(log(model_log)[-1]) and \
                                "harness cancellation probe" in json.dumps(log(model_log)[-1]):
                            break
                        time.sleep(.01)
                    assert request("/v1/cancel", {})["cancelled"]
                    try:
                        pending.result()
                        raise AssertionError("cancelled document task returned success")
                    except urllib.error.HTTPError as error:
                        assert error.code == 409 and "document_cancelled" in error.read().decode()
                assert len(log(reader_log)) == before, "cancellation launched document reads"
                # Cancellation must interrupt an already-running extractor,
                # including one that emits no stdout. The spy writes a marker
                # and then blocks until the gateway's process-group kill.
                block_reader.write_text("start")
                with ThreadPoolExecutor(max_workers=1) as executor:
                    pending = executor.submit(chat, "harness child cancellation probe", [pdf])
                    for _ in range(200):
                        if block_ready.exists():
                            break
                        time.sleep(.01)
                    assert block_ready.exists(), "extractor did not reach the blocking boundary"
                    assert request("/v1/cancel", {})["cancelled"]
                    started = time.monotonic()
                    try:
                        pending.result(timeout=5)
                        raise AssertionError("active extractor cancellation returned success")
                    except (urllib.error.HTTPError, TimeoutError) as error:
                        if isinstance(error, urllib.error.HTTPError):
                            assert error.code == 409 and "document_cancelled" in error.read().decode()
                    elapsed = time.monotonic() - started
                    assert elapsed < 2.5, elapsed
                block_reader.unlink(missing_ok=True)
                print("test_document_harness.py: PASS (metadata, bounded pages, adaptive jump, cache, follow-up, compaction, invalid/repeated plans, full read, text, multiple sources, cancellation)")
            finally:
                gateway.terminate()
                try:
                    gateway.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    gateway.kill()
                    gateway.wait()


if __name__ == "__main__":
    run()
