#!/usr/bin/env python3
"""tests/jobs/test_daemon.py — Tests for samosa-jobsd daemon & scheduler."""

import os
import sys
import unittest
import tempfile
import json
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'dist'))
import samosa_jobs


class TestDaemonScheduler(unittest.TestCase):

    def test_generate_launchd_plist(self):
        plist = samosa_jobs.generate_launchd_plist('/path/to/samosa_jobs.py')
        self.assertIn('com.samosa.jobsd', plist)
        self.assertIn('run-loop', plist)

    def test_cmd_daemon_status(self):
        code = samosa_jobs.cmd_daemon(['status'])
        self.assertEqual(code, 0)

    def test_check_and_run_scheduled_job(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            job_dir = Path(tmpdir) / 'test-scheduled-job'
            job_dir.mkdir(parents=True)
            job_file = job_dir / 'job.json'
            job_data = {
                'job_id': 'test-scheduled-job',
                'schedule': {
                    'interval_seconds': 1,
                    'missed_window_policy': 'catch_up'
                }
            }
            job_file.write_text(json.dumps(job_data))
            
            samosa_jobs._check_and_run_scheduled_job(job_file)
            state_file = job_dir / 'schedule_state.json'
            self.assertTrue(state_file.exists())


if __name__ == '__main__':
    unittest.main()
