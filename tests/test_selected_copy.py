#!/usr/bin/env python3
"""Guarded copy/retry/undo outcomes through the compiled filesystem sidecar."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import os
import signal
import time

FS = Path(__file__).resolve().parents[1] / "build/samosa-fs"
with tempfile.TemporaryDirectory(prefix="samosa-selected-copy-") as temporary:
    root = Path(temporary) / "root"
    root.mkdir()
    source = root / "input.txt"
    source.write_bytes(b"selected immutable evidence\n")
    st = source.stat()
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    dest = root / "organized/input.txt"
    flags = ["--root", str(root), "--operation-id", "copy-fixture-1", "--size", str(st.st_size),
             "--mtime", str(st.st_mtime), "--dev", str(st.st_dev), "--ino", str(st.st_ino), "--sha256", digest]

    def call(operation, destination=dest, operation_id=None):
        args = flags.copy()
        if operation_id:
            args[args.index("--operation-id") + 1] = operation_id
        result = subprocess.run([str(FS), operation, *args, str(source), str(destination)],
                                check=True, capture_output=True, text=True)
        return json.loads(result.stdout)

    first = call("copy")
    assert first["applied"], first
    assert dest.read_bytes() == source.read_bytes()
    assert call("copy")["already_completed"]
    assert call("copy", operation_id="different-operation")["reason"] == "dest_exists"
    dest.write_bytes(b"later user edit")
    assert not call("undo-copy")["applied"]
    assert dest.read_bytes() == b"later user edit"
    # Restore original bytes to exercise unchanged copy undo, then retry it.
    dest.write_bytes(source.read_bytes())
    assert call("undo-copy")["applied"]
    assert not dest.exists() and source.read_bytes() == b"selected immutable evidence\n"
    assert call("undo-copy")["already_completed"]
    assert call("copy", operation_id="copy-fixture-2")["applied"]
    dest.unlink()
    dest.write_bytes(source.read_bytes())
    assert call("undo-copy", operation_id="copy-fixture-2")["reason"] == "destination_replaced"
    assert dest.exists()
    outside = Path(temporary) / "outside"
    outside.mkdir()
    (root / "escape").symlink_to(outside, target_is_directory=True)
    assert not call("copy", root / "escape/stolen.txt", "copy-escape")["applied"]
    assert not (outside / "stolen.txt").exists()
    parallel = root / "parallel/input.txt"
    with ThreadPoolExecutor(max_workers=2) as pool:
        replies = list(pool.map(lambda _: call("copy", parallel, "copy-concurrent"), range(2)))
    assert all(reply["applied"] for reply in replies), replies
    assert sum(reply["already_completed"] for reply in replies) == 1, replies
    assert parallel.read_bytes() == source.read_bytes()
    if os.geteuid() != 0:
        denied = root / "denied"
        denied.mkdir()
        existing = denied / "existing.txt"
        existing.write_bytes(b"existing user file")
        denied.chmod(0)
        try:
            assert not call("copy", denied / "input.txt", "copy-permission")["applied"]
        finally:
            denied.chmod(0o700)
        assert not (denied / "input.txt").exists()
        assert existing.read_bytes() == b"existing user file"
        source_mode = source.stat().st_mode & 0o777
        source.chmod(0)
        try:
            assert not call("copy", root / "unreadable/input.txt", "copy-unreadable")["applied"]
        finally:
            source.chmod(source_mode)
        assert not (root / "unreadable/input.txt").exists()
        assert source.read_bytes() == b"selected immutable evidence\n"
        print("selected copy source/destination permission refusal: PASS")
    source.write_bytes(b"changed source\n")
    assert not call("copy", root / "new/input.txt", "copy-changed")["applied"]
    assert not (root / "new/input.txt").exists()
    # Kill the native helper while bytes are in private staging. Retry must
    # publish a complete copy, never expose partial destination bytes or
    # remove the unfinished staging file without ownership/version proof.
    source = root / "stage-source.bin"
    with source.open("wb") as stream:
        stream.truncate(64 << 20)
    st = source.stat()
    with source.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    flags = ["--root", str(root), "--operation-id", "copy-stage-crash", "--size", str(st.st_size),
             "--mtime", str(st.st_mtime), "--dev", str(st.st_dev), "--ino", str(st.st_ino), "--sha256", digest]
    destination = root / "stage-retry/data.bin"
    process = subprocess.Popen([str(FS), "copy", *flags, str(source), str(destination)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    staging = root / ".samosa-actions" / f"copy-stage-crash-{process.pid}.partial"
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and process.poll() is None:
            if staging.exists() and staging.stat().st_size > 0:
                os.kill(process.pid, signal.SIGSTOP)
                os.waitpid(process.pid, os.WUNTRACED)
                break
            time.sleep(.001)
        assert process.poll() is None and staging.exists(), "staging checkpoint was not observed"
        process.kill()
        process.wait(timeout=5)
        assert not destination.exists(), "partial bytes were published"
        orphan_inode = staging.stat().st_ino
        with staging.open("rb") as stream:
            orphan_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        assert call("copy", destination)["applied"]
        with destination.open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == digest
        assert staging.stat().st_ino == orphan_inode
        with staging.open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == orphan_hash
        assert call("undo-copy", destination)["applied"] and not destination.exists()
        with source.open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == digest
        print("selected copy staging interruption/retry/undo: PASS")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()
print("test_selected_copy.py: PASS")
