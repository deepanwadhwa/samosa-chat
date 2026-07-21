#!/usr/bin/env python3
"""tests/jobs/test_jobs_fs.py — deterministic filesystem core (tools/jobs_fs.py).

Ports the move-engine and organize-plan coverage from the original runner and
adds discovery/dedup/count/revert/downscale tests, all against the extracted
pure module. No model, no network.
"""

import inspect
import os
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools'))
import jobs_fs as fs


class TestMoveEngine(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(self.input_folder)

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_dst_exists_skips(self):
        src = os.path.join(self.input_folder, 'src.txt')
        dst_dir = os.path.join(self.input_folder, 'TEXT')
        os.mkdir(dst_dir)
        dst = os.path.join(dst_dir, 'src.txt')
        with open(src, 'w') as f:
            f.write('source text')
        with open(dst, 'w') as f:
            f.write('dest text')

        plan = {'src': src, 'dst': dst, 'size': 11, 'mtime': os.path.getmtime(src)}
        ok, reason = fs.apply_move(plan, input_folder=self.input_folder)

        self.assertFalse(ok)
        self.assertEqual(reason, 'dest_exists')
        self.assertTrue(os.path.exists(src))
        with open(src) as f:
            self.assertEqual(f.read(), 'source text')
        with open(dst) as f:
            self.assertEqual(f.read(), 'dest text')

    def test_symlink_src_skips(self):
        real_file = os.path.join(self.input_folder, 'real.txt')
        with open(real_file, 'w') as f:
            f.write('content')
        sym = os.path.join(self.input_folder, 'sym.txt')
        os.symlink(real_file, sym)

        dst = os.path.join(self.input_folder, 'TEXT', 'sym.txt')
        plan = {'src': sym, 'dst': dst, 'size': 7, 'mtime': os.path.getmtime(real_file)}
        ok, reason = fs.apply_move(plan, input_folder=self.input_folder)

        self.assertFalse(ok)
        self.assertTrue(reason.startswith('cannot_open_src') or reason == 'not_regular_file', reason)

    def test_src_modified_skips(self):
        src = os.path.join(self.input_folder, 'src.txt')
        with open(src, 'w') as f:
            f.write('original')
        plan = {'src': src, 'dst': os.path.join(self.input_folder, 'TEXT', 'src.txt'),
                'size': 8, 'mtime': os.path.getmtime(src)}
        with open(src, 'w') as f:
            f.write('modified content')
        ok, reason = fs.apply_move(plan, input_folder=self.input_folder)
        self.assertFalse(ok)
        self.assertEqual(reason, 'changed_since_scan')

    def test_successful_move(self):
        src = os.path.join(self.input_folder, 'doc.txt')
        with open(src, 'w') as f:
            f.write('valid doc')
        dst = os.path.join(self.input_folder, 'TEXT', 'doc.txt')
        plan = {'src': src, 'dst': dst, 'size': 9, 'mtime': os.path.getmtime(src)}
        ok, reason = fs.apply_move(plan, input_folder=self.input_folder)
        self.assertTrue(ok, reason)
        self.assertFalse(os.path.exists(src))
        with open(dst) as f:
            self.assertEqual(f.read(), 'valid doc')

    def test_exdev_cross_device_skips(self):
        src = os.path.join(self.input_folder, 'doc.txt')
        with open(src, 'w') as f:
            f.write('valid doc')
        dst = os.path.join(self.input_folder, 'TEXT', 'doc.txt')
        plan = {'src': src, 'dst': dst, 'size': 9, 'mtime': os.path.getmtime(src)}
        with patch('jobs_fs.atomic_no_clobber_rename', return_value=(False, 'cross_device')):
            ok, reason = fs.apply_move(plan, input_folder=self.input_folder)
        self.assertFalse(ok)
        self.assertEqual(reason, 'cross_device')
        self.assertTrue(os.path.exists(src))

    def test_fallback_link_path_and_inode_assertion(self):
        src = os.path.join(self.input_folder, 'fallback.txt')
        with open(src, 'w') as f:
            f.write('fallback content')
        dst_dir = os.path.join(self.input_folder, 'TEXT')
        os.mkdir(dst_dir)
        dst = os.path.join(dst_dir, 'fallback.txt')
        with patch('jobs_fs._libc', None):
            ok, reason = fs.atomic_no_clobber_rename(src, dst)
        self.assertTrue(ok, reason)
        self.assertFalse(os.path.exists(src))
        with open(dst) as f:
            self.assertEqual(f.read(), 'fallback content')

    def test_no_forbidden_unlink_during_move(self):
        src = os.path.join(self.input_folder, 'audit.txt')
        with open(src, 'w') as f:
            f.write('audit content')
        dst = os.path.join(self.input_folder, 'TEXT', 'audit.txt')
        plan = {'src': src, 'dst': dst, 'size': 13, 'mtime': os.path.getmtime(src)}

        orig_unlink = os.unlink

        def guarded_unlink(path, *args, **kwargs):
            stack = [frame.function for frame in inspect.stack()]
            if 'atomic_no_clobber_rename' in stack:
                return orig_unlink(path, *args, **kwargs)
            raise RuntimeError(f"FORBIDDEN UNLINK of user file: {path}")

        with patch('os.unlink', side_effect=guarded_unlink), \
             patch('os.remove', side_effect=RuntimeError("FORBIDDEN REMOVE")), \
             patch('shutil.rmtree', side_effect=RuntimeError("FORBIDDEN RMTREE")):
            ok, reason = fs.apply_move(plan, input_folder=self.input_folder)
        self.assertTrue(ok, reason)
        self.assertTrue(os.path.exists(dst))

    def test_revert_restores_source(self):
        src = os.path.join(self.input_folder, 'r.txt')
        with open(src, 'w') as f:
            f.write('revert me')
        dst = os.path.join(self.input_folder, 'TEXT', 'r.txt')
        plan = {'src': src, 'dst': dst, 'size': 9, 'mtime': os.path.getmtime(src)}
        ok, _ = fs.apply_move(plan, input_folder=self.input_folder)
        self.assertTrue(ok)
        self.assertFalse(os.path.exists(src))

        ok, reason = fs.revert_move(plan)
        self.assertTrue(ok, reason)
        self.assertTrue(os.path.exists(src))
        self.assertFalse(os.path.exists(dst))
        with open(src) as f:
            self.assertEqual(f.read(), 'revert me')


class TestDiscoveryAndTyping(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.inp = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(self.inp)

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def _w(self, name, data):
        p = os.path.join(self.inp, name)
        mode = 'wb' if isinstance(data, bytes) else 'w'
        with open(p, mode) as f:
            f.write(data)
        return p

    def test_magic_byte_typing_and_utf8_fallback(self):
        self.assertEqual(fs.detect_media_type(b'\xff\xd8\xff\xe0'), 'image/jpeg')
        self.assertEqual(fs.detect_media_type(b'\x89PNG\r\n'), 'image/png')
        self.assertEqual(fs.detect_media_type(b'%PDF-1.7'), 'application/pdf')
        self.assertIsNone(fs.detect_media_type(b'plain te'))
        self.assertTrue(fs.is_valid_utf8_text(b'hello\tworld\n'))
        self.assertFalse(fs.is_valid_utf8_text(b'\x00\x01\x02'))

    def test_discovery_dedup_symlink_and_count(self):
        self._w('a.txt', 'hello world')
        self._w('dup.txt', 'hello world')          # identical content -> deduped
        self._w('b.pdf', b'%PDF-1.4 body')
        self._w('c.jpg', b'\xff\xd8\xff\xe0 jpeg')
        os.symlink(os.path.join(self.inp, 'a.txt'), os.path.join(self.inp, 'link.txt'))

        items, skipped = fs.discover_files({'folder': self.inp}, is_metadata_only=True)
        names = sorted(os.path.basename(i['input_path']) for i in items)
        self.assertEqual(names, ['a.txt', 'b.pdf', 'c.jpg'])
        reasons = {os.path.basename(p): r for p, r in skipped}
        self.assertIn('dup.txt', reasons)
        self.assertIn('duplicate', reasons['dup.txt'])
        self.assertIn('link.txt', reasons)  # symlink rejected

        summary = fs.count_by_type(items)
        self.assertEqual(summary['text/plain']['count'], 1)
        self.assertEqual(summary['application/pdf']['count'], 1)
        self.assertEqual(summary['image/jpeg']['count'], 1)

    def test_max_bytes_and_empty_skipped(self):
        self._w('big.txt', 'x' * 100)
        self._w('empty.txt', '')
        items, skipped = fs.discover_files({'folder': self.inp, 'max_file_bytes': 10})
        self.assertEqual(items, [])
        reasons = {os.path.basename(p): r for p, r in skipped}
        self.assertIn('exceeds max_file_bytes', reasons['big.txt'])
        self.assertEqual(reasons['empty.txt'], 'empty file')


class TestOrganizePlan(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.inp = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(self.inp)

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def _fixtures(self):
        specs = {
            'doc.pdf': b'%PDF-1.4 header',
            'img.jpg': b'\xff\xd8\xff\xe0 jpeg',
            'graphic.png': b'\x89PNG\r\n\x1a\n png',
            'notes.txt': 'plain text',
            'photo_no_ext': b'\x89PNG\r\n\x1a\n magic png, no ext',
            'text_renamed.jpg': 'this is actually text, jpg extension',
        }
        for name, data in specs.items():
            mode = 'wb' if isinstance(data, bytes) else 'w'
            with open(os.path.join(self.inp, name), mode) as f:
                f.write(data)

    def test_by_extension_deterministic(self):
        self._fixtures()
        job = {'input': {'folder': self.inp}, 'organize': {'rule': {'by': 'extension'}}}
        plan1, err1 = fs.build_organize_plan(job, self.tmpdir)
        plan2, err2 = fs.build_organize_plan(job, self.tmpdir)
        self.assertIsNone(err1)
        self.assertEqual(plan1, plan2, "plan must be byte-identical across runs")

        dest = {os.path.basename(m['src']): os.path.relpath(m['dst'], self.inp)
                for m in plan1 if 'dst' in m}
        self.assertEqual(dest['doc.pdf'], 'Organized/PDF/doc.pdf')
        self.assertEqual(dest['img.jpg'], 'Organized/JPG/img.jpg')
        self.assertEqual(dest['graphic.png'], 'Organized/PNG/graphic.png')
        self.assertEqual(dest['notes.txt'], 'Organized/TXT/notes.txt')
        # extensionless file typed by magic bytes -> PNG
        self.assertEqual(dest['photo_no_ext'], 'Organized/PNG/photo_no_ext')
        # extension wins for the folder name even when content is text
        self.assertEqual(dest['text_renamed.jpg'], 'Organized/JPG/text_renamed.jpg')

    def test_by_media_type_uses_magic_bytes(self):
        self._fixtures()
        job = {'input': {'folder': self.inp}, 'organize': {'rule': {'by': 'media_type'}}}
        plan, err = fs.build_organize_plan(job, self.tmpdir)
        self.assertIsNone(err)
        dest = {os.path.basename(m['src']): os.path.relpath(m['dst'], self.inp)
                for m in plan if 'dst' in m}
        # the mislabeled .jpg is really text -> TEXT by content
        self.assertEqual(dest['text_renamed.jpg'], 'Organized/TEXT/text_renamed.jpg')
        self.assertEqual(dest['img.jpg'], 'Organized/JPEG/img.jpg')

    def test_where_rule_from_records(self):
        self._fixtures()
        # 'where people == 2' using provided extraction records
        job = {
            'input': {'folder': self.inp},
            'instruction': 'count people',
            'output_schema': {'type': 'object'},
            'organize': {'rule': {'by': 'where', 'field': 'people', 'op': 'eq',
                                  'value': 2, 'dest': 'Two people'},
                         'unmatched': 'leave'},
        }
        img = os.path.join(self.inp, 'img.jpg')
        png = os.path.join(self.inp, 'graphic.png')
        records = [
            {'input_path': img, 'status': 'passed', 'people': 2},
            {'input_path': png, 'status': 'passed', 'people': 1},
        ]
        plan, err = fs.build_organize_plan(job, self.tmpdir, results=records)
        self.assertIsNone(err)
        moves = {m['src']: m for m in plan if 'dst' in m}
        skips = {m['src']: m for m in plan if 'skip' in m}
        self.assertIn(img, moves)
        self.assertTrue(moves[img]['dst'].endswith('Two people/img.jpg'))
        self.assertEqual(skips.get(png, {}).get('skip'), 'unmatched')

    def test_collision_skip_vs_suffix(self):
        self._fixtures()
        # Pre-create a colliding destination for doc.pdf under PDF/.
        pdf_dir = os.path.join(self.inp, 'Organized', 'PDF')
        os.makedirs(pdf_dir)
        with open(os.path.join(pdf_dir, 'doc.pdf'), 'w') as f:
            f.write('existing')

        base = {'input': {'folder': self.inp}}
        skip_plan, _ = fs.build_organize_plan(
            {**base, 'organize': {'rule': {'by': 'extension'}, 'on_collision': 'skip'}}, self.tmpdir)
        doc_skip = [m for m in skip_plan if os.path.basename(m['src']) == 'doc.pdf'][0]
        self.assertEqual(doc_skip.get('skip'), 'dest_exists')

        suffix_plan, _ = fs.build_organize_plan(
            {**base, 'organize': {'rule': {'by': 'extension'}, 'on_collision': 'suffix_sha8'}}, self.tmpdir)
        doc_move = [m for m in suffix_plan if os.path.basename(m['src']) == 'doc.pdf'][0]
        self.assertIn('dst', doc_move)
        self.assertRegex(os.path.basename(doc_move['dst']), r'^doc\.[0-9a-f]{8}\.pdf$')


class TestEventLog(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_append_and_reload_ignores_torn_line(self):
        path = os.path.join(self.tmpdir, 'events.jsonl')
        log = fs.EventLog(path)
        log.append('decode_intent', goal='sort by type')
        log.append('move_applied', src='a', dst='b')
        with open(path, 'a') as f:
            f.write('{ this is a torn line')  # no newline / invalid JSON

        reloaded = fs.EventLog(path)
        reloaded.load()
        self.assertEqual([e['type'] for e in reloaded.events], ['decode_intent', 'move_applied'])
        self.assertEqual(reloaded.seq, 2)


FIXTURES = os.path.join(os.path.dirname(__file__), '..', 'fixtures', 'documents')


class TestDocumentExtraction(unittest.TestCase):
    def setUp(self):
        if fs.find_extractor() is None:
            self.skipTest('samosa-extract sidecar not built in this environment')

    def test_extracts_real_pdf_text(self):
        result, error = fs.extract_document(os.path.join(FIXTURES, 'hello.pdf'))
        self.assertIsNone(error)
        self.assertEqual(result['input_type'], 'application/pdf')
        self.assertEqual(result['text'], 'Hello PDFium')
        self.assertEqual(len(result['pages']), 1)
        self.assertEqual(result['pages'][0]['index'], 1)

    def test_extracts_plain_text_too(self):
        result, error = fs.extract_document(os.path.join(FIXTURES, 'notes.txt'))
        self.assertIsNone(error)
        self.assertEqual(result['input_type'], 'text/plain')
        self.assertIn('Ada Lovelace', result['text'])

    def test_missing_file_is_a_clean_error(self):
        result, error = fs.extract_document('/no/such/file.pdf')
        self.assertIsNone(result)
        self.assertIsInstance(error, str)
        self.assertNotIn('Traceback', error)

    def test_unsupported_format_reports_clearly_not_garbage(self):
        with tempfile.NamedTemporaryFile(suffix='.docx', delete=False) as f:
            f.write(b'PK\x03\x04 not really a docx')
            path = f.name
        try:
            result, error = fs.extract_document(path)
            self.assertIsNone(result)
            self.assertIn('docx', error)
            self.assertIn('not supported', error)
        finally:
            os.unlink(path)

    def test_explicit_bad_extractor_path_reports_cleanly(self):
        result, error = fs.extract_document(os.path.join(FIXTURES, 'hello.pdf'),
                                            extractor='/no/such/binary')
        self.assertIsNone(result)
        self.assertIn('document reader could not be run', error)


class TestDocumentExtractionNoBinary(unittest.TestCase):
    """extractor_unavailable path: exercised regardless of whether the real
    sidecar happens to be built in this environment, by forcing find_extractor
    to report none found."""

    def test_no_extractor_found_is_a_clean_capability_gap(self):
        with patch('jobs_fs.find_extractor', return_value=None):
            result, error = fs.extract_document(os.path.join(FIXTURES, 'hello.pdf'))
        self.assertIsNone(result)
        self.assertIn('not installed', error)


class TestDownscale(unittest.TestCase):
    def test_small_image_unchanged(self):
        data = b'\xff\xd8\xff' + b'x' * 100
        out, mime, changed = fs.auto_downscale_image_bytes(data, 'image/jpeg', target_max_bytes=1024)
        self.assertFalse(changed)
        self.assertEqual(out, data)

    def test_oversized_png_shrinks_when_pillow_available(self):
        try:
            import io
            from PIL import Image
        except Exception:
            self.skipTest('Pillow not available')
        img = Image.new('RGB', (2000, 2000), (123, 200, 50))
        buf = io.BytesIO()
        img.save(buf, format='PNG')
        big = buf.getvalue()
        out, mime, changed = fs.auto_downscale_image_bytes(big, 'image/png', target_max_bytes=64 * 1024)
        self.assertTrue(changed)
        self.assertLess(len(out), len(big))


if __name__ == '__main__':
    unittest.main()
