import json
import os
import tempfile
import unittest
from unittest.mock import patch

from dist.samosa_jobs import cmd_arm, cmd_preview, estimate_job_cost


class TestEstimatePreview(unittest.TestCase):

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root_dir = self.temp_dir.name
        self.inputs_dir = os.path.join(self.root_dir, "inputs")
        os.makedirs(self.inputs_dir, exist_ok=True)

        # Create 3 test files
        with open(os.path.join(self.inputs_dir, "doc1.txt"), "w") as f:
            f.write("Hello world " * 100)
        with open(os.path.join(self.inputs_dir, "doc2.txt"), "w") as f:
            f.write("Sample document content " * 50)
        with open(os.path.join(self.inputs_dir, "doc3.txt"), "w") as f:
            f.write("Third file " * 30)

        self.job_dict = {
            "schema_version": 1,
            "job_id": "test-estimate-job",
            "name": "Test Estimate Job",
            "input": {
                "folder": self.inputs_dir,
                "recursive": False,
                "types": ["text/plain"],
                "max_file_bytes": 10485760
            },
            "unit": "auto",
            "instruction": "Extract text summary.",
            "output_schema": {
                "type": "object",
                "required": ["summary"],
                "properties": {
                    "summary": {"type": ["string", "null"]}
                }
            },
            "output": {"dir": os.path.join(self.root_dir, "results"), "format": "jsonl"},
            "resources": {"max_attempts": 3, "run_on_battery": False, "pause_when_user_active": True, "min_free_gb": 5}
        }
        self.job_path = os.path.join(self.root_dir, "job.json")
        with open(self.job_path, "w") as f:
            json.dumps(self.job_dict)
            f.write(json.dumps(self.job_dict))

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_estimate_job_cost(self):
        est = estimate_job_cost(self.job_dict)
        self.assertEqual(est["total_files"], 3)
        self.assertEqual(est["total_units"], 3)
        self.assertGreater(est["total_input_tokens"], 0)
        self.assertTrue(isinstance(est["formatted_time"], str) and len(est["formatted_time"]) > 0)

    def test_cmd_arm_prints_estimate(self):
        with patch("dist.samosa_jobs.get_jobs_root", return_value=self.root_dir):
            res = cmd_arm([self.job_path])
            self.assertEqual(res, 0)

    def test_cmd_preview_multi_samples(self):
        fake_response = {
            "choices": [{"message": {"content": '{"summary": "Test summary"}'}}],
            "usage": {"prompt_tokens": 100, "completion_tokens": 20}
        }
        with patch("dist.samosa_jobs.get_jobs_root", return_value=self.root_dir), \
             patch("dist.samosa_jobs.call_serve", return_value=(fake_response, None)):
            res = cmd_preview([self.job_path, "--samples", "2"])
            self.assertEqual(res, 0)


if __name__ == "__main__":
    unittest.main()
