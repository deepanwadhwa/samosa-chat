import hashlib
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path
from dist.samosa_jobs import cmd_organize, cmd_apply, cmd_undo, EventLog

def snapshot_folder_hashes(root):
    hashes = {}
    for p in Path(root).rglob('*'):
        if p.is_file():
            rel = p.relative_to(root).as_posix()
            hashes[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return hashes

class TestUndo(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.jobs_root = os.path.join(self.tmpdir, 'jobs')
        os.environ['SAMOSA_JOBS_DIR'] = self.jobs_root
        self.input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(self.input_folder)

        self.f1 = os.path.join(self.input_folder, 'doc1.pdf')
        self.f2 = os.path.join(self.input_folder, 'doc2.txt')
        with open(self.f1, 'wb') as f: f.write(b'%PDF-1.4 pdf content')
        with open(self.f2, 'w') as f: f.write('text content')

        self.job_file = os.path.join(self.tmpdir, 'job.json')
        with open(self.job_file, 'w') as f:
            f.write(json.dumps({
                'job_id': 'test-undo-job',
                'schema_version': 1,
                'input': {'folder': self.input_folder},
                'organize': {'rule': {'by': 'extension'}}
            }))

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_apply_then_undo_restores_files_hash_inventory(self):
        pre_apply_hashes = snapshot_folder_hashes(self.input_folder)

        cmd_organize([self.job_file])
        cmd_apply([self.job_file, '--yes'])

        # Confirm moved
        self.assertFalse(os.path.exists(self.f1))
        self.assertFalse(os.path.exists(self.f2))

        # Perform undo
        code = cmd_undo([self.job_file, '--yes'])
        self.assertEqual(code, 0)

        # Confirm restored files match pre-apply hash inventory byte-for-byte
        post_undo_hashes = snapshot_folder_hashes(self.input_folder)
        for rel_path, expected_hash in pre_apply_hashes.items():
            self.assertIn(rel_path, post_undo_hashes)
            self.assertEqual(post_undo_hashes[rel_path], expected_hash)

        # Assert empty created destination directories remain present-but-empty (JO-D1 no rmdir rule)
        pdf_dir = os.path.join(self.input_folder, 'Organized', 'PDF')
        txt_dir = os.path.join(self.input_folder, 'Organized', 'TXT')
        self.assertTrue(os.path.exists(pdf_dir))
        self.assertTrue(os.path.exists(txt_dir))
        self.assertEqual(os.listdir(pdf_dir), [])
        self.assertEqual(os.listdir(txt_dir), [])

    def test_undo_skips_modified_file(self):
        cmd_organize([self.job_file])
        cmd_apply([self.job_file, '--yes'])

        dst1 = os.path.join(self.input_folder, 'Organized', 'PDF', 'doc1.pdf')
        # Modify dst1
        with open(dst1, 'w') as f: f.write('modified pdf content')

        cmd_undo([self.job_file, '--yes'])

        # f1 was modified at dst, so it skips revert; f2 reverts
        self.assertTrue(os.path.exists(dst1))
        self.assertTrue(os.path.exists(self.f2))

if __name__ == '__main__':
    unittest.main()
