import json
import os
import tempfile
import unittest

from dist.samosa_jobs import cmd_suggest_job, load_and_validate_job


class TestSuggestJob(unittest.TestCase):

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_folder = self.temp_dir.name
        self.old_serve_url = os.environ.get('SAMOSA_SERVE_URL')
        os.environ['SAMOSA_SERVE_URL'] = 'http://127.0.0.1:1'  # Force unreachable

    def tearDown(self):
        self.temp_dir.cleanup()
        if self.old_serve_url is not None:
            os.environ['SAMOSA_SERVE_URL'] = self.old_serve_url
        else:
            os.environ.pop('SAMOSA_SERVE_URL', None)

    def test_suggest_job_intent_matching_sort_type(self):
        out_job = os.path.join(self.test_folder, "suggested.job.json")
        res = cmd_suggest_job([
            "--description", "sort this folder by extension",
            "--folder", self.test_folder,
            "--output-job", out_job
        ])
        self.assertEqual(res, 0)
        self.assertTrue(os.path.exists(out_job))
        job, errors = load_and_validate_job(out_job)
        self.assertEqual(len(errors), 0)
        self.assertIn("organize", job)

    def test_suggest_job_intent_matching_photos_two_people(self):
        out_job = os.path.join(self.test_folder, "photos.job.json")
        res = cmd_suggest_job([
            "--description", "separate out pictures with 2 people",
            "--folder", self.test_folder,
            "--output-job", out_job
        ])
        self.assertEqual(res, 0)
        self.assertTrue(os.path.exists(out_job))
        job, errors = load_and_validate_job(out_job)
        self.assertEqual(len(errors), 0)
        self.assertIn("organize", job)

    def test_suggest_job_fallback_generation(self):
        out_job = os.path.join(self.test_folder, "custom.job.json")
        res = cmd_suggest_job([
            "--description", "extract medical record patient names and blood pressure",
            "--folder", self.test_folder,
            "--output-job", out_job
        ])
        self.assertEqual(res, 0)
        self.assertTrue(os.path.exists(out_job))
        job, errors = load_and_validate_job(out_job)
        self.assertEqual(len(errors), 0)
        self.assertEqual(job["schema_version"], 1)

    def test_suggest_job_missing_desc(self):
        res = cmd_suggest_job([])
        self.assertEqual(res, 2)


if __name__ == "__main__":
    unittest.main()
