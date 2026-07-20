import errno
import inspect
import os
import shutil
import tempfile
import unittest
from unittest.mock import patch
from dist.samosa_jobs import apply_move, atomic_no_clobber_rename

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

        with open(src, 'w') as f: f.write('source text')
        with open(dst, 'w') as f: f.write('dest text')

        plan = {'src': src, 'dst': dst, 'size': 11, 'mtime': os.path.getmtime(src)}
        ok, reason = apply_move(plan, input_folder=self.input_folder)

        self.assertFalse(ok)
        self.assertEqual(reason, 'dest_exists')
        self.assertTrue(os.path.exists(src))
        self.assertTrue(os.path.exists(dst))
        with open(src) as f: self.assertEqual(f.read(), 'source text')
        with open(dst) as f: self.assertEqual(f.read(), 'dest text')

    def test_symlink_src_skips(self):
        real_file = os.path.join(self.input_folder, 'real.txt')
        with open(real_file, 'w') as f: f.write('content')
        sym = os.path.join(self.input_folder, 'sym.txt')
        os.symlink(real_file, sym)

        dst = os.path.join(self.input_folder, 'TEXT', 'sym.txt')
        plan = {'src': sym, 'dst': dst, 'size': 7, 'mtime': os.path.getmtime(real_file)}
        ok, reason = apply_move(plan, input_folder=self.input_folder)

        self.assertFalse(ok)
        self.assertTrue(reason.startswith('cannot_open_src') or reason == 'not_regular_file', f"Got reason: {reason}")

    def test_src_modified_skips(self):
        src = os.path.join(self.input_folder, 'src.txt')
        with open(src, 'w') as f: f.write('original')
        plan = {'src': src, 'dst': os.path.join(self.input_folder, 'TEXT', 'src.txt'), 'size': 8, 'mtime': os.path.getmtime(src)}

        # Modify file
        with open(src, 'w') as f: f.write('modified content')
        ok, reason = apply_move(plan, input_folder=self.input_folder)

        self.assertFalse(ok)
        self.assertEqual(reason, 'changed_since_scan')

    def test_successful_move(self):
        src = os.path.join(self.input_folder, 'doc.txt')
        with open(src, 'w') as f: f.write('valid doc')
        dst = os.path.join(self.input_folder, 'TEXT', 'doc.txt')

        plan = {'src': src, 'dst': dst, 'size': 9, 'mtime': os.path.getmtime(src)}
        ok, reason = apply_move(plan, input_folder=self.input_folder)

        self.assertTrue(ok, f"Expected success, got error: {reason}")
        self.assertFalse(os.path.exists(src))
        self.assertTrue(os.path.exists(dst))
        with open(dst) as f: self.assertEqual(f.read(), 'valid doc')

    def test_exdev_cross_device_skips(self):
        src = os.path.join(self.input_folder, 'doc.txt')
        with open(src, 'w') as f: f.write('valid doc')
        dst = os.path.join(self.input_folder, 'TEXT', 'doc.txt')
        plan = {'src': src, 'dst': dst, 'size': 9, 'mtime': os.path.getmtime(src)}

        with patch('dist.samosa_jobs.atomic_no_clobber_rename', return_value=(False, 'cross_device')):
            ok, reason = apply_move(plan, input_folder=self.input_folder)
            self.assertFalse(ok)
            self.assertEqual(reason, 'cross_device')
            self.assertTrue(os.path.exists(src))

    def test_fallback_link_path_and_inode_assertion(self):
        src = os.path.join(self.input_folder, 'fallback.txt')
        with open(src, 'w') as f: f.write('fallback content')
        dst_dir = os.path.join(self.input_folder, 'TEXT')
        os.mkdir(dst_dir)
        dst = os.path.join(dst_dir, 'fallback.txt')

        # Test atomic_no_clobber_rename fallback path directly when _libc is None
        with patch('dist.samosa_jobs._libc', None):
            ok, reason = atomic_no_clobber_rename(src, dst)
            self.assertTrue(ok, f"Fallback failed with: {reason}")
            self.assertFalse(os.path.exists(src))
            self.assertTrue(os.path.exists(dst))
            with open(dst) as f: self.assertEqual(f.read(), 'fallback content')

    def test_no_delete_audit_traps_armed(self):
        src = os.path.join(self.input_folder, 'audit.txt')
        with open(src, 'w') as f: f.write('audit content')
        dst = os.path.join(self.input_folder, 'TEXT', 'audit.txt')
        plan = {'src': src, 'dst': dst, 'size': 13, 'mtime': os.path.getmtime(src)}

        orig_unlink = os.unlink
        def guarded_unlink(path, *args, **kwargs):
            # Inspect stack to allow unlinking only inside atomic_no_clobber_rename fallback
            stack = [frame.function for frame in inspect.stack()]
            if 'atomic_no_clobber_rename' in stack:
                return orig_unlink(path, *args, **kwargs)
            raise RuntimeError(f"FORBIDDEN UNLINK of user file: {path}")

        with patch('os.unlink', side_effect=guarded_unlink), \
             patch('os.remove', side_effect=RuntimeError("FORBIDDEN REMOVE")), \
             patch('shutil.rmtree', side_effect=RuntimeError("FORBIDDEN RMTREE")):
            ok, reason = apply_move(plan, input_folder=self.input_folder)
            self.assertTrue(ok)
            self.assertTrue(os.path.exists(dst))

if __name__ == '__main__':
    unittest.main()
