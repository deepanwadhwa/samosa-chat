import json
import os
import tempfile
import unittest
from unittest.mock import patch

from dist.samosa_jobs import EventLog, cmd_review_patch


class TestReviewPatch(unittest.TestCase):

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root_dir = self.temp_dir.name
        self.job_id = "test-patch-job"
        self.job_dir = os.path.join(self.root_dir, self.job_id)
        os.makedirs(os.path.join(self.job_dir, "results", "items"), exist_ok=True)
        os.makedirs(os.path.join(self.job_dir, "results", "review"), exist_ok=True)

        self.job_dict = {
            "schema_version": 1,
            "job_id": self.job_id,
            "name": "Test Patch Job",
            "input": {
                "folder": self.root_dir,
                "recursive": False,
                "types": ["text/plain"],
                "max_file_bytes": 10485760
            },
            "unit": "auto",
            "instruction": "Extract total.",
            "output_schema": {
                "type": "object",
                "required": ["merchant", "total"],
                "properties": {
                    "merchant": {"type": ["string", "null"]},
                    "total": {"type": ["number", "null"]}
                }
            },
            "output": {"dir": os.path.join(self.job_dir, "results"), "format": "jsonl"},
            "resources": {"max_attempts": 3, "run_on_battery": False, "pause_when_user_active": True, "min_free_gb": 5}
        }
        self.job_path = os.path.join(self.root_dir, "job.json")
        with open(self.job_path, "w") as f:
            f.write(json.dumps(self.job_dict))

        # Create initial item and review files
        self.unit_id = "unit_123"
        item_path = os.path.join(self.job_dir, "results", "items", f"{self.unit_id}.json")
        prov_path = os.path.join(self.job_dir, "results", "items", f"{self.unit_id}.provenance.json")
        review_path = os.path.join(self.job_dir, "results", "review", f"{self.unit_id}.json")

        with open(item_path, "w") as f:
            f.write(json.dumps({"merchant": "Unknown", "total": None}))
        with open(prov_path, "w") as f:
            f.write(json.dumps({"unit_id": self.unit_id, "validation": "review_required"}))
        with open(review_path, "w") as f:
            f.write(json.dumps({"merchant": "Unknown", "total": None}))

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_review_patch_updates_field(self):
        with patch("dist.samosa_jobs.get_jobs_root", return_value=self.root_dir):
            res = cmd_review_patch([
                self.job_path,
                "--unit", self.unit_id,
                "--field", "total",
                "--val", "42.50"
            ])
            self.assertEqual(res, 0)

            item_path = os.path.join(self.job_dir, "results", "items", f"{self.unit_id}.json")
            with open(item_path) as f:
                rec = json.loads(f.read())
            self.assertEqual(rec["total"], 42.50)

            # Check review file was removed
            review_path = os.path.join(self.job_dir, "results", "review", f"{self.unit_id}.json")
            self.assertFalse(os.path.exists(review_path))

            # Check event log
            event_log = EventLog(os.path.join(self.job_dir, "events.jsonl"))
            event_log.load()
            patched_evs = [ev for ev in event_log.events if ev.get("type") == "unit_patched"]
            self.assertEqual(len(patched_evs), 1)
            self.assertEqual(patched_evs[0]["val"], 42.50)


if __name__ == "__main__":
    unittest.main()
