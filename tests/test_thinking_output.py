import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).parents[1] / "tools" / "check_thinking_output.py"
SPEC = importlib.util.spec_from_file_location("check_thinking_output", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ThinkingOutputTests(unittest.TestCase):
    def test_completed_answer_passes(self):
        result = MODULE.evaluate("reasoning\n</think>\n\nfinal answer", 0.45)
        self.assertTrue(result["passed"])

    def test_answer_string_inside_unfinished_thinking_fails(self):
        result = MODULE.evaluate("reasoning mentions Answer: 72 but never closes", 0.45)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["thinking_closed"])

    def test_empty_answer_fails(self):
        result = MODULE.evaluate("reasoning\n</think>\n", 0.45)
        self.assertFalse(result["passed"])

    def test_repetition_fails(self):
        text = "</think>\n" + "alpha beta gamma delta " * 20
        result = MODULE.evaluate(text, 0.20)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["repetition_within_limit"])

    def test_new_repeated_line_attractor_fails_before_global_ratio_does(self):
        coherent = "\n".join(f".rule-{number} {{ color: red; }}" for number in range(200))
        repeated = "\n".join([".flex-evenly { justify-content: space-evenly; }"] * 8)
        result = MODULE.evaluate(f"</think>\n{coherent}\n{repeated}", 0.45)
        self.assertLess(result["checks"]["repeated_4gram_fraction"], 0.45)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["repeated_line_run_within_limit"])

    def test_same_line_tail_attractor_fails(self):
        coherent = " ".join(f"unique-{number}" for number in range(1000))
        repeated = '<p class="tagline">coffee</p> ' * 70
        result = MODULE.evaluate(f"</think>\n{coherent}\n{repeated}", 0.45)
        self.assertLess(result["checks"]["repeated_4gram_fraction"], 0.45)
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["tail_repetition_within_limit"])

    def test_task_specific_completion_marker_is_enforced(self):
        result = MODULE.evaluate("</think>\n```html\n<body>", 0.45, ("</html>",))
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["required_substrings_present"])

    def test_task_specific_regex_accepts_equivalent_word_order(self):
        pattern = r"(?:\b7\W{0,12}red\b|\bred\b\D{0,24}\b7\b)"
        result = MODULE.evaluate("</think>\nRed balls: **7**", 0.45, (), (pattern,))
        self.assertTrue(result["passed"])
        self.assertTrue(result["checks"]["required_patterns_present"])


if __name__ == "__main__":
    unittest.main()
