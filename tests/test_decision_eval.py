#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parent / "decision" / "evaluate.py"
SPEC = importlib.util.spec_from_file_location("samosa_decision_evaluate", MODULE_PATH)
EVALUATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATOR)


class DecisionEvaluatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gold = EVALUATOR.validate_gold(
            EVALUATOR.read_jsonl(Path(__file__).parent / "decision" / "gold.jsonl"))

    def perfect_predictions(self):
        predictions = []
        for case_id, case in self.gold.items():
            scores = {option: float(option == case["gold"]) for option in case["options"]}
            abstained = bool(case.get("review_expected", False))
            predictions.append({
                "case_id": case_id,
                "selected_option": case["gold"],
                "scores": scores,
                "abstained": abstained,
                "abstention_reason": "insufficient evidence" if abstained else "",
                "model_revision": "fixture@sha256:abc",
                "token_budget": 512,
                "input_tokens": 32,
                "latency_ms": 10,
                "peak_rss_bytes": 4096,
                "swap_delta_bytes": 0,
                "thermal_celsius": 35,
                "thermal_observed": True,
                "memory_pressure": "normal",
            })
        return predictions

    def test_perfect_run_metrics_and_groups(self):
        predictions, revision = EVALUATOR.validate_predictions(
            self.gold, self.perfect_predictions(), 512)
        report = EVALUATOR.summarize(self.gold, predictions, revision, 512)
        self.assertEqual(report["cases"], len(self.gold))
        self.assertEqual(report["accuracy"], 1.0)
        self.assertEqual(report["macro_f1"], 1.0)
        self.assertEqual(report["expected_calibration_error_10_bins"], 0.0)
        self.assertEqual(report["abstention_precision"], 1.0)
        self.assertEqual(report["abstention_recall"], 1.0)
        self.assertIn("12", report["accuracy_by_class_count_and_position"])
        self.assertIn("topics:12_options", report["metrics_by_task_and_class_count"])
        self.assertIn("inbox:5_options", report["metrics_by_task_and_class_count"])
        self.assertEqual(report["thermal_celsius_max"], 35)
        for placement in ("begin", "middle", "end"):
            self.assertIn("position:" + placement,
                          report["accuracy_by_class_count_and_position"])

    def test_missing_predictions_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing predictions"):
            EVALUATOR.validate_predictions(self.gold, [], 512)

    def test_inconsistent_revision_is_rejected(self):
        rows = self.perfect_predictions()
        rows[-1]["model_revision"] = "un-pinned"
        with self.assertRaisesRegex(ValueError, "single pinned model_revision"):
            EVALUATOR.validate_predictions(self.gold, rows, 512)

    def test_input_over_budget_is_rejected(self):
        rows = self.perfect_predictions()
        rows[0]["input_tokens"] = 513
        with self.assertRaisesRegex(ValueError, "input_tokens exceeds token_budget"):
            EVALUATOR.validate_predictions(self.gold, rows, 512)

    def test_unavailable_thermal_readings_are_reported_as_unavailable(self):
        rows = self.perfect_predictions()
        for row in rows:
            row["thermal_celsius"] = None
            row["thermal_observed"] = False
        predictions, revision = EVALUATOR.validate_predictions(self.gold, rows, 512)
        report = EVALUATOR.summarize(self.gold, predictions, revision, 512)
        self.assertIsNone(report["thermal_celsius_max"])
        self.assertEqual(report["thermal_observed_cases"], 0)


if __name__ == "__main__":
    unittest.main()
