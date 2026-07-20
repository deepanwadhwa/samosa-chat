#!/usr/bin/env python3
"""tests/jobs/test_host_tuning.py — Host-adaptive tuning tests."""

import os
import sys
import tempfile
import unittest
import json

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'dist'))
import samosa_jobs


class TestHostTuning(unittest.TestCase):

    def test_default_host_profile_detection(self):
        profile = samosa_jobs.get_host_profile()
        self.assertIn('tier', profile)
        self.assertIn('thread_budget', profile)
        self.assertIn('prefill_budget', profile)
        self.assertGreaterEqual(profile['thread_budget'], 2)

    def test_custom_host_profile_env(self):
        with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as f:
            f.write(json.dumps({
                'tier': 'desktop-cooled',
                'ram_gb': 64,
                'phys_perf_cores': 8,
                'thread_budget': 6,
            }))
            path = f.name
        try:
            os.environ['SAMOSA_HOST_PROFILE'] = path
            profile = samosa_jobs.get_host_profile()
            self.assertEqual(profile['tier'], 'desktop-cooled')
            self.assertEqual(profile['thread_budget'], 6)
        finally:
            os.environ.pop('SAMOSA_HOST_PROFILE', None)
            os.unlink(path)


if __name__ == '__main__':
    unittest.main()
