#!/usr/bin/env python3
"""tests/jobs/test_samosa_fs.py — the shipped samosa-fs sidecar (src/samosa_fs.c).

Direct CLI coverage of the compiled binary that the release actually ships:
magic-byte typing, UTF-8 text fallback, content dedup, O_NOFOLLOW symlink
rejection, the metadata-only oversized-read cap, and per-file metadata.

This is the survivor of the old dual-mode ``test_jobs_fs.py``. That file
cross-checked the binary against the now-removed Python ``jobs_fs`` module and
resolved the binary at ``<repo>/samosa-fs`` (which does not exist), so its
sidecar class silently *skipped*. Here the expectations are pinned to the
binary's own observed output and ``SAMOSA_FS`` is honored (defaulting to
``build/samosa-fs``), so the coverage runs instead of skipping.

Gate 11 (see docs/TASKS_JOBS.md): the Python jobs_fs/samosa_jobs/samosa_gateway/
samosa_tools modules were removed after native parity; the C gateway's job
routes are covered by tests/test_compiled_gateway.sh.
"""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest

_HERE = os.path.dirname(__file__)
SAMOSA_FS = os.environ.get(
    "SAMOSA_FS", os.path.abspath(os.path.join(_HERE, "..", "..", "build", "samosa-fs"))
)


class TestSamosaFsSidecar(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(SAMOSA_FS):
            self.skipTest(f"samosa-fs sidecar not built at {SAMOSA_FS}")
        self.tmpdir = tempfile.mkdtemp()
        self.inp = os.path.join(self.tmpdir, "inputs")
        os.mkdir(self.inp)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _w(self, name, data):
        p = os.path.join(self.inp, name)
        mode = "wb" if isinstance(data, bytes) else "w"
        with open(p, mode) as f:
            f.write(data)
        return p

    def _run(self, *args):
        proc = subprocess.run(
            [SAMOSA_FS, *args], text=True, capture_output=True, check=False
        )
        self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        payload = json.loads(proc.stdout)
        self.assertTrue(payload.get("ok"), payload)
        return payload

    def test_list_typing_dedup_and_symlink_rejection(self):
        self._w("a.txt", "hello world")
        self._w("dup.txt", "hello world")  # byte-identical to a.txt
        self._w("b.pdf", b"%PDF-1.4 body")
        self._w("c.jpg", b"\xff\xd8\xff\xe0 jpeg")
        os.symlink(os.path.join(self.inp, "a.txt"), os.path.join(self.inp, "link.txt"))

        payload = self._run("list", self.inp)
        by_name = {i["name"]: i for i in payload["items"]}

        # Magic-byte typing + UTF-8 text fallback, exact sizes.
        self.assertEqual(by_name["a.txt"]["media_type"], "text/plain")
        self.assertEqual(by_name["a.txt"]["magic_type"], "text/plain")
        self.assertEqual(by_name["a.txt"]["rel_path"], "a.txt")
        self.assertEqual(by_name["a.txt"]["size"], 11)
        self.assertEqual(by_name["b.pdf"]["media_type"], "application/pdf")
        self.assertEqual(by_name["b.pdf"]["size"], 13)
        self.assertEqual(by_name["c.jpg"]["media_type"], "image/jpeg")
        self.assertEqual(by_name["c.jpg"]["size"], 9)
        self.assertIn("mtime", by_name["a.txt"])
        # Real SHA-256 of the content.
        self.assertEqual(
            by_name["a.txt"]["input_sha256"],
            hashlib.sha256(b"hello world").hexdigest(),
        )

        # dup.txt (content duplicate) and link.txt (symlink) are excluded, not listed.
        self.assertNotIn("dup.txt", by_name)
        self.assertNotIn("link.txt", by_name)
        skipped = {os.path.basename(s["path"]): s["reason"] for s in payload["skipped"]}
        self.assertIn("dup.txt", skipped)
        self.assertIn("duplicate", skipped["dup.txt"].lower())
        self.assertIn("link.txt", skipped)
        self.assertIn("symlink", skipped["link.txt"].lower())

    def test_survey_counts_by_type(self):
        self._w("a.txt", "hello")
        self._w("b.pdf", b"%PDF-1.4")
        self._w("c.jpg", b"\xff\xd8\xff\xe0")

        payload = self._run("survey", self.inp)
        self.assertEqual(payload["total"], 3)
        self.assertEqual(payload["by_type"]["text/plain"]["count"], 1)
        self.assertEqual(payload["by_type"]["application/pdf"]["count"], 1)
        self.assertEqual(payload["by_type"]["image/jpeg"]["count"], 1)

    def test_metadata_only_scan_caps_oversized_reads(self):
        # A binary blob larger than the cap is still typed and sized correctly
        # without reading the whole file into memory.
        self._w("huge.bin", b"\xff" * 200)
        self._w("normal.txt", "hello world")

        payload = self._run("list", "--max-file-bytes", "50", self.inp)
        by_name = {i["name"]: i for i in payload["items"]}
        self.assertEqual(by_name["huge.bin"]["media_type"], "application/octet-stream")
        self.assertEqual(by_name["huge.bin"]["size"], 200)
        self.assertEqual(by_name["normal.txt"]["media_type"], "text/plain")

    def test_metadata_one_file(self):
        path = self._w("note.txt", "hello")
        payload = self._run("metadata", path)
        self.assertEqual(payload["name"], "note.txt")
        self.assertEqual(payload["media_type"], "text/plain")
        self.assertEqual(payload["size"], 5)
        self.assertEqual(
            payload["input_sha256"], hashlib.sha256(b"hello").hexdigest()
        )


if __name__ == "__main__":
    unittest.main()
