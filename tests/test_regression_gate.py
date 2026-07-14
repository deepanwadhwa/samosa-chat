import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).parents[1] / "tools" / "run_regression_gate.py"
SPEC = importlib.util.spec_from_file_location("run_regression_gate", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class RegressionGateTests(unittest.TestCase):
    def test_parses_engine_metrics(self):
        stderr = (
            "[stats] prompt=48 generated=272 stop=model thinking=forced-close "
            "prefill=5.399s (8.89 tok/s) decode=41.414s (6.54 tok/s) "
            "total=46.813s expert_hit=1/2 (50.0%) expert_disk=1s "
            "expert_mm=1s peak_rss=3.85 GB\n"
            "[ecache] budget=2.07 GB payload=1.29 GB peak=1.29 GB entries=656 "
            "evictions=1 bytes_read=114.94 GB bytes_avoided=63.24 GB "
            "failed_admissions=0 pressure_warn=0 pressure_critical=0\n"
        )
        metrics = MODULE.parse_engine_metrics(stderr)
        self.assertEqual(metrics["generated"], 272)
        self.assertEqual(metrics["thinking"], "forced-close")
        self.assertEqual(metrics["bytes_read_gb"], 114.94)

    def test_forced_thinking_closure_fails_even_with_correct_final(self):
        case = {"profile": "think", "require": ["Answer: 11"]}
        stdout = "--- risposta ---\nreasoning</think>\nAnswer: 11"
        stderr = (
            "[stats] prompt=1 generated=2 stop=model thinking=forced-close "
            "prefill=1s (1 tok/s) decode=1s (1 tok/s) total=2s "
            "expert_hit=0/1 (0%) expert_disk=1s expert_mm=1s peak_rss=1 GB\n"
        )
        result = MODULE.evaluate_case(case, stdout, stderr)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["natural_closure"])

    def test_direct_requires_model_stop_and_answer_marker(self):
        case = {"profile": "direct", "require": ["safe loop"]}
        stdout = "--- risposta ---\nsafe loop"
        stderr = (
            "[stats] prompt=1 generated=2 stop=model thinking=model-controlled "
            "prefill=1s (1 tok/s) decode=1s (1 tok/s) total=2s "
            "expert_hit=0/1 (0%) expert_disk=1s expert_mm=1s peak_rss=1 GB\n"
        )
        self.assertTrue(MODULE.evaluate_case(case, stdout, stderr)["passed"])

    def test_safety_rejects_swap_growth(self):
        before = {"vm": {"swapouts": 100}}
        current = {"vm": {"swapouts": 5000, "pages_throttled": 0},
                   "memory_free_percent": 80, "disk_free_gb": 30,
                   "thermal": ("No thermal warning level has been recorded\n"
                               "No performance warning level has been recorded")}
        violation = MODULE.safety_violation(before, current, 15, 25, 64)
        self.assertIn("swapouts grew", violation)


if __name__ == "__main__":
    unittest.main()
