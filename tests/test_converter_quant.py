import importlib.util
from pathlib import Path
import unittest

import numpy as np


MODULE_PATH = Path(__file__).parents[1] / "tools" / "convert_qwen36.py"
SPEC = importlib.util.spec_from_file_location("convert_qwen36", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class GroupwiseQuantizationTests(unittest.TestCase):
    def test_groupwise_round_trip_and_scale_layout(self):
        source = np.array([
            [-8.0, -4.0, 0.0, 4.0, -0.75, -0.25, 0.25, 0.75],
            [0.125, -0.125, 0.25, -0.25, 8.0, -8.0, 4.0, -4.0],
        ], dtype=np.float32)
        packed, scales = MODULE.quant_int4_grouped(source, 4)
        reconstructed = MODULE.dequantize(packed, scales, source.shape, 4, 4)

        self.assertEqual(packed.shape, (source.size // 2,))
        self.assertEqual(scales.shape, (source.shape[0] * 2,))
        self.assertEqual(reconstructed.shape, source.shape)
        np.testing.assert_allclose(reconstructed[:, (0, 3)], source[:, (0, 3)], atol=0.6)
        self.assertLess(np.sqrt(np.mean((reconstructed - source) ** 2)), 0.3)

    def test_group_must_be_even_divisor(self):
        source = np.ones((2, 8), dtype=np.float32)
        for invalid in (1, 3, 6):
            with self.assertRaises(ValueError):
                MODULE.quant_int4_grouped(source, invalid)

    def test_mixed_expert_blob_layout_uses_q8_down(self):
        gate = np.arange(32, dtype=np.float32).reshape(4, 8) / 13 - 1
        up = np.flip(gate, axis=1).copy()
        down = np.linspace(-3, 3, 32, dtype=np.float32).reshape(8, 4)
        blob = MODULE.quantize_expert_blob(
            gate, up, down, gate_up_bits=4, down_bits=8, group_size=4)

        gate_q_bytes = gate.size // 2
        gate_scale_bytes = gate.shape[0] * (gate.shape[1] // 4) * 4
        up_q_bytes = up.size // 2
        up_scale_bytes = up.shape[0] * (up.shape[1] // 4) * 4
        down_offset = gate_q_bytes + gate_scale_bytes + up_q_bytes + up_scale_bytes
        down_q = np.frombuffer(blob, dtype=np.uint8, count=down.size,
                               offset=down_offset).copy()
        down_scales = np.frombuffer(blob, dtype=np.float32,
                                    count=down.shape[0],
                                    offset=down_offset + down.size).copy()
        reconstructed = MODULE.dequantize(
            down_q, down_scales, down.shape, bits=8)

        self.assertEqual(len(blob) % MODULE.ALIGNMENT_BYTES, 0)
        self.assertLess(np.sqrt(np.mean((reconstructed - down) ** 2)), 0.02)
        self.assertEqual(len(blob), MODULE.ALIGNMENT_BYTES)

    def test_canonical_expert_manifest_detection(self):
        canonical = {"experts": {
            "model.layers.1.mlp.experts.0": {"offset": 16, "size": 8},
            "model.layers.0.mlp.experts.1": {"offset": 8, "size": 8},
            "model.layers.0.mlp.experts.0": {"offset": 0, "size": 8},
        }}
        self.assertTrue(MODULE.manifest_is_canonical(canonical))
        canonical["experts"]["model.layers.0.mlp.experts.1"]["offset"] = 12
        self.assertFalse(MODULE.manifest_is_canonical(canonical))

    def test_dense_noncanonical_manifest_can_publish_without_repack(self):
        noncanonical = {"experts": {
            "model.layers.1.mlp.experts.0": {"offset": 0, "size": 8},
            "model.layers.0.mlp.experts.0": {"offset": 8, "size": 8},
        }}
        self.assertFalse(MODULE.manifest_is_canonical(noncanonical))
        self.assertTrue(MODULE.manifest_is_dense(noncanonical, 16))
        self.assertFalse(MODULE.manifest_is_dense(noncanonical, 24))
        noncanonical["experts"]["model.layers.0.mlp.experts.0"]["offset"] = 12
        self.assertFalse(MODULE.manifest_is_dense(noncanonical, 20))


if __name__ == "__main__":
    unittest.main()
