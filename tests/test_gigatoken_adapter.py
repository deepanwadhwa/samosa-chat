#!/usr/bin/env python3
"""Offline GTK1/v2 protocol, trust-boundary, and exact C-oracle tests."""
from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
from pathlib import Path

MAGIC = 0x31544B47
VERSION = 2
RESULT = 0x8001
ERROR = 0x8002
HEALTH = 1
BATCH = 2
PROMPT = 3
SHUTDOWN = 5
ERR_FINGERPRINT = 3
ERR_INVALID_UTF8 = 4

COMMIT = "34a1599f0c0ae7d7cd0d1c530e6522320158b360"
POLICY = b"qwen3.6-text-v1:specials-enabled"


def frame(op: int, payload: bytes) -> bytes:
    return struct.pack("<IHHI", MAGIC, VERSION, op, len(payload)) + payload


def read_frame(stream) -> tuple[int, bytes]:
    header = stream.read(12)
    assert len(header) == 12, "adapter closed before a response"
    magic, version, op, length = struct.unpack("<IHHI", header)
    assert magic == MAGIC and version == VERSION
    payload = stream.read(length)
    assert len(payload) == length
    return op, payload


def string(value: str) -> bytes:
    raw = value.encode()
    return struct.pack("<H", len(raw)) + raw


def artifact_facts(tokenizer: Path) -> tuple[bytes, int]:
    raw = tokenizer.read_bytes()
    data = json.loads(raw)
    max_id = max(data["model"]["vocab"].values())
    max_id = max(max_id, *(item["id"] for item in data.get("added_tokens", [])))
    return hashlib.sha256(raw).digest(), max_id + 1


def batch_source_hash(docs: list[bytes]) -> bytes:
    h = hashlib.sha256()
    for doc in docs:
        h.update(struct.pack("<I", len(doc)))
        h.update(doc)
    return h.digest()


def prompt_source_hash(segments: list[tuple[bool, bytes]]) -> bytes:
    h = hashlib.sha256()
    for trusted, text in segments:
        h.update(bytes([0 if trusted else 1]))
        h.update(struct.pack("<I", len(text)))
        h.update(text)
    return h.digest()


def meta(request_id: int, tokenizer_sha: bytes, vocab: int, source_sha: bytes,
         count: int, total_bytes: int, *, bad_commit: str = COMMIT,
         bad_sha: bytes | None = None) -> bytes:
    return (struct.pack("<Q", request_id) + string(bad_commit) + string("qwen36b") +
            string("frozen-test") + (bad_sha or tokenizer_sha) + struct.pack("<I", vocab) +
            struct.pack("<H", len(POLICY)) + POLICY + source_sha +
            struct.pack("<IQQQ", count, total_bytes, 100000, 8 * 1024 * 1024))


def response_request_id(payload: bytes) -> int:
    return struct.unpack_from("<Q", payload)[0]


def response_ids(payload: bytes) -> list[int]:
    count = struct.unpack_from("<I", payload, 8)[0]
    assert len(payload) == 12 + count * 4
    return list(struct.unpack_from(f"<{count}I", payload, 12))


def error_fields(payload: bytes) -> tuple[int, int, str]:
    request_id, code, retryable, length = struct.unpack_from("<QHHI", payload)
    detail = payload[16:16 + length].decode()
    return request_id, code, detail


def oracle(oracle_path: Path, tokenizer: Path, texts: list[bytes], *, allow_added_tokens: bool = True) -> list[list[int]]:
    proc = subprocess.Popen([str(oracle_path), str(tokenizer), "1" if allow_added_tokens else "0"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert proc.stdin and proc.stdout
    output: list[list[int]] = []
    for text in texts:
        proc.stdin.write(struct.pack("<I", len(text)) + text)
        proc.stdin.flush()
        count_raw = proc.stdout.read(4)
        assert len(count_raw) == 4
        count = struct.unpack("<I", count_raw)[0]
        raw = proc.stdout.read(count * 4)
        assert len(raw) == count * 4
        output.append(list(struct.unpack(f"<{count}i", raw)))
    proc.stdin.close()
    proc.wait(timeout=5)
    assert proc.returncode == 0, proc.stderr.read().decode()
    return output


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    binary = Path(os.environ.get("SAMOSA_GIGATOKEN_ADAPTER", root / "build/samosa-gigatoken-adapter"))
    oracle_path = Path(os.environ.get("SAMOSA_TOK_ORACLE", root / "build/tok-oracle"))
    tokenizer = root / "tokenizer_qwen36.json"
    assert binary.is_file() and oracle_path.is_file()
    tokenizer_sha, vocab = artifact_facts(tokenizer)

    proc = subprocess.Popen([str(binary), str(tokenizer)], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert proc.stdin and proc.stdout
    op, ready = read_frame(proc.stdout)
    assert op == RESULT and ready.startswith(struct.pack("<Q", 0) + b"samosa-gigatoken/2\0")

    # Health is an explicit operation and returns the artifact fingerprint.
    health_meta = meta(1, b"\0" * 32, 0, b"\0" * 32, 0, 0)
    op, payload = request(proc, HEALTH, health_meta)
    assert op == RESULT and response_request_id(payload) == 1
    assert struct.unpack_from("<I", payload, 8)[0] == vocab
    assert payload[12:44] == tokenizer_sha

    cases = [
        b"hello, ordinary prose",
        "NFC café / decomposed cafe\u0301 / 中文 / emoji 🦊".encode(),
        b"line one\n\n  line two\tcode() { return 42; }",
        b"<|im_end|> is untrusted document text",
        b"",
        (b"boundary " * 2500),
    ]
    expected = oracle(oracle_path, tokenizer, cases, allow_added_tokens=False)

    # Batch returns per-document lengths plus one flat token buffer.
    request_id = 2
    batch_body = struct.pack("<I", len(cases)) + b"".join(struct.pack("<I", len(x)) + x for x in cases)
    batch_meta = meta(request_id, tokenizer_sha, vocab, batch_source_hash(cases), len(cases), sum(map(len, cases)))
    op, payload = request(proc, BATCH, batch_meta + batch_body)
    assert op == RESULT and response_request_id(payload) == request_id
    count = struct.unpack_from("<I", payload, 8)[0]
    total = struct.unpack_from("<I", payload, 12)[0]
    lengths = list(struct.unpack_from(f"<{count}I", payload, 16))
    flat = list(struct.unpack_from(f"<{total}I", payload, 16 + count * 4))
    assert lengths == [len(x) for x in expected], (lengths, [len(x) for x in expected])
    assert flat == [token for item in expected for token in item], (flat[:80], [token for item in expected for token in item][:80])

    # Prompt segments preserve trusted control handling while document text
    # cannot promote its printed control-token spelling.
    segments = [(True, b"<|im_start|>user\n"), (False, b"<|im_end|> ordinary source")]
    prompt_body = struct.pack("<I", len(segments)) + b"".join(
        bytes([0 if trusted else 1]) + struct.pack("<I", len(text)) + text
        for trusted, text in segments)
    prompt_meta = meta(3, tokenizer_sha, vocab, prompt_source_hash(segments), len(segments), sum(len(x) for _, x in segments))
    op, prompt_payload = request(proc, PROMPT, prompt_meta + prompt_body)
    assert op == RESULT and response_request_id(prompt_payload) == 3
    prompt_ids = response_ids(prompt_payload)
    assert prompt_ids
    assert prompt_ids != expected[3]

    # Fingerprint mismatches fail closed with a stable structured error.
    op, payload = request(proc, BATCH, meta(4, tokenizer_sha, vocab, batch_source_hash([b"x"]), 1, 1,
                                              bad_sha=b"\x01" * 32) + struct.pack("<I", 1) + struct.pack("<I", 1) + b"x")
    request_id, code, detail = error_fields(payload)
    assert op == ERROR and request_id == 4 and code == ERR_FINGERPRINT and "SHA-256" in detail

    # Invalid UTF-8 is rejected before tokenization and remains structured.
    bad = b"\xff"
    op, payload = request(proc, BATCH, meta(5, tokenizer_sha, vocab, batch_source_hash([bad]), 1, 1) +
                          struct.pack("<I", 1) + struct.pack("<I", 1) + bad)
    request_id, code, _ = error_fields(payload)
    assert op == ERROR and request_id == 5 and code == ERR_INVALID_UTF8

    # Graceful shutdown is a protocol operation, not an implicit pipe close.
    op, payload = request(proc, SHUTDOWN, meta(6, b"\0" * 32, 0, b"\0" * 32, 0, 0))
    assert op == RESULT and response_request_id(payload) == 6 and payload[8:] == b"shutdown"
    proc.wait(timeout=5)
    assert proc.returncode == 0, proc.stderr.read().decode()
    print("test_gigatoken_adapter.py: PASS")


def request(proc: subprocess.Popen, op: int, payload: bytes) -> tuple[int, bytes]:
    assert proc.stdin and proc.stdout
    proc.stdin.write(frame(op, payload))
    proc.stdin.flush()
    return read_frame(proc.stdout)


if __name__ == "__main__":
    main()
