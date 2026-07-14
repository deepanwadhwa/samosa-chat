import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).parents[1] / "tools" / "analyze_route_trace.py"
SPEC = importlib.util.spec_from_file_location("analyze_route_trace", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class RouteAnalysisTests(unittest.TestCase):
    def test_reads_effective_routes_and_improves_synthetic_adjacency(self):
        records = [
            {"type": "meta", "schema": "qwen36-route-v2", "layers": 1,
             "experts": 4, "selected_k": 2},
            {"type": "route", "layer": 0, "ids": [0, 2], "effective_k": 2},
            {"type": "route", "layer": 0, "ids": [0, 2], "effective_k": 2},
            {"type": "route", "layer": 0, "ids": [1, 3], "effective_k": 2},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(item) + "\n" for item in records))
            metadata, layers = MODULE.read_trace(path)
        result = MODULE.analyze(metadata, layers)
        current = result["aggregate"]["current_mean_adjacent_selected_pairs"]
        candidate = result["aggregate"]["candidate_mean_adjacent_selected_pairs"]
        self.assertEqual(result["aggregate"]["records"], 3)
        self.assertGreater(candidate, current)

    def test_rejects_duplicate_selected_expert(self):
        records = [
            {"type": "meta", "experts": 4, "selected_k": 2},
            {"type": "route", "layer": 0, "ids": [1, 1], "effective_k": 2},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(item) + "\n" for item in records))
            with self.assertRaises(ValueError):
                MODULE.read_trace(path)


if __name__ == "__main__":
    unittest.main()
