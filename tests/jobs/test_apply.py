import json
import os
import shutil
import tempfile
import unittest
from dist.samosa_jobs import cmd_organize, cmd_apply, EventLog

class TestApply(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.jobs_root = os.path.join(self.tmpdir, 'jobs')
        os.environ['SAMOSA_JOBS_DIR'] = self.jobs_root
        self.input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(self.input_folder)

        # Create test files
        self.f1 = os.path.join(self.input_folder, 'doc1.pdf')
        self.f2 = os.path.join(self.input_folder, 'doc2.txt')
        with open(self.f1, 'wb') as f: f.write(b'%PDF-1.4 pdf content')
        with open(self.f2, 'w') as f: f.write('text content')

        self.job_file = os.path.join(self.tmpdir, 'job.json')
        with open(self.job_file, 'w') as f:
            f.write(json.dumps({
                'job_id': 'test-apply-job',
                'schema_version': 1,
                'input': {'folder': self.input_folder},
                'organize': {'rule': {'by': 'extension'}}
            }))

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_apply_moves_end_to_end(self):
        code_org = cmd_organize([self.job_file])
        self.assertEqual(code_org, 0)

        code_apply = cmd_apply([self.job_file, '--yes'])
        self.assertEqual(code_apply, 0)

        self.assertFalse(os.path.exists(self.f1))
        self.assertFalse(os.path.exists(self.f2))

        dst1 = os.path.join(self.input_folder, 'Organized', 'PDF', 'doc1.pdf')
        dst2 = os.path.join(self.input_folder, 'Organized', 'TXT', 'doc2.txt')
        self.assertTrue(os.path.exists(dst1))
        self.assertTrue(os.path.exists(dst2))

        # Idempotent second apply
        code_apply2 = cmd_apply([self.job_file, '--yes'])
        self.assertEqual(code_apply2, 0)

    def test_apply_without_yes_non_tty_fails(self):
        cmd_organize([self.job_file])
        code = cmd_apply([self.job_file]) # no --yes, stdin is non-tty in test runner
        self.assertEqual(code, 2)
        # Tree untouched
        self.assertTrue(os.path.exists(self.f1))
        self.assertTrue(os.path.exists(self.f2))

    def test_crash_recovery_orphaned_move_applying(self):
        cmd_organize([self.job_file])
        dst1 = os.path.join(self.input_folder, 'Organized', 'PDF', 'doc1.pdf')
        os.makedirs(os.path.dirname(dst1), exist_ok=True)

        # Simulate crash: rename occurred but crash before move_applied event
        os.rename(self.f1, dst1)

        job_dir = os.path.join(self.jobs_root, 'test-apply-job')
        event_log = EventLog(os.path.join(job_dir, 'events.jsonl'))
        event_log.load()
        event_log.append('move_applying', src=self.f1, dst=dst1, input_sha256='')

        # Re-apply recovers the orphaned move_applying event and moves f2
        code = cmd_apply([self.job_file, '--yes'])
        self.assertEqual(code, 0)

        dst2 = os.path.join(self.input_folder, 'Organized', 'TXT', 'doc2.txt')
        self.assertTrue(os.path.exists(dst1))
        self.assertTrue(os.path.exists(dst2))

    def test_stale_plan_rejected(self):
        cmd_organize([self.job_file])

        # Append terminal event after plan creation
        job_dir = os.path.join(self.jobs_root, 'test-apply-job')
        event_log = EventLog(os.path.join(job_dir, 'events.jsonl'))
        event_log.load()
        event_log.append('item_discovered', input_path=os.path.join(self.input_folder, 'new.pdf'), input_sha256='9999')

        code = cmd_apply([self.job_file, '--yes'])
        self.assertEqual(code, 2)

if __name__ == '__main__':
    unittest.main()
