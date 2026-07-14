import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).parents[1] / "tools" / "run_openrouter_control.py"
SPEC = importlib.util.spec_from_file_location("run_openrouter_control", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class OpenRouterControlTests(unittest.TestCase):
    def test_payload_has_no_forced_reasoning_budget(self):
        config = {
            "model": "qwen/test", "provider": "Provider", "defaults": {},
        }
        case = {"id": "a", "prompt": "test", "seed": 11}
        payload = MODULE.request_payload(config, case)
        self.assertEqual(payload["reasoning"], {"enabled": True, "exclude": False})
        self.assertNotIn("max_tokens", payload["reasoning"])
        self.assertFalse(payload["provider"]["allow_fallbacks"])

    def test_normalizes_reasoning_and_correct_final(self):
        case = {"id": "a", "seed": 11,
                "require_regex": [r"(?:7\W+red|red\D{0,20}7)",
                                  r"(?:4\W+blue|blue\D{0,20}4)", "11"]}
        response = {
            "model": "qwen/test", "provider": "Provider",
            "choices": [{"finish_reason": "stop", "native_finish_reason": "stop",
                         "message": {"reasoning": "check", "content":
                                     "7 red, 4 blue, total 11"}}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 20,
                      "completion_tokens_details": {"reasoning_tokens": 8},
                      "cost": 0.001},
        }
        result = MODULE.normalize_response(case, response, 1.0)
        self.assertTrue(result["passed"])
        self.assertEqual(result["usage"]["reasoning_tokens"], 8)

    def test_pilot_percentile_is_labeled(self):
        config = {"model": "qwen/test", "provider_quantization": "fp8"}
        results = []
        for index, tokens in enumerate((10, 20, 30)):
            results.append({
                "case_id": str(index), "seed": index, "passed": True,
                "checks": {"natural_model_stop": True,
                           "required_substrings_present": True},
                "usage": {"reasoning_tokens": tokens, "cost": 0.001},
            })
        summary = MODULE.summarize(results, config)
        self.assertEqual(summary["reasoning_tokens_p90_nearest_rank"], 30)
        self.assertTrue(summary["p90_is_pilot_only"])
        self.assertTrue(summary["not_bf16"])


if __name__ == "__main__":
    unittest.main()
