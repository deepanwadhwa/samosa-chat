import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).parents[1] / "tools" / "spec_accept.py"
SPEC = importlib.util.spec_from_file_location("spec_accept", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules["spec_accept"] = MODULE  # dataclasses resolve types via sys.modules
SPEC.loader.exec_module(MODULE)

VOCAB = 8


def build_teacher_stream(sequences, calibration_ordinals=()):
    """Byte-exact synthetic QWTFM001 stream mirroring run_teacher_capture.

    `sequences` is a list of lists of (target, argmax) pairs; the target's
    logit/lse are fixed so p(target) is exp(-1).
    """
    positions = sum(len(items) for items in sequences)
    header = bytearray(80)
    header[:8] = b"QWTFM001"
    struct.pack_into("<HHI", header, 8, 1, 0, 80)
    header[16:48] = hashlib.sha256(b"corpus").digest()
    struct.pack_into("<II", header, 48, VOCAB, len(sequences))
    struct.pack_into("<QIII", header, 56, positions, max(1, len(calibration_ordinals)), 1, 56)
    body = bytearray(header)
    ordinal = 0
    for sequence, items in enumerate(sequences):
        for position, (target, argmax) in enumerate(items):
            full = ordinal in calibration_ordinals
            record = bytearray(56)
            struct.pack_into("<IIIII", record, 0, sequence, position, target,
                             1 if full else 0, VOCAB if full else 0)
            struct.pack_into("<dd", record, 20, 2.0, 3.0)
            top5 = [argmax] + [t for t in range(VOCAB) if t != argmax][:4]
            struct.pack_into("<5I", record, 36, *top5)
            body += record
            if full:
                body += struct.pack(f"<{VOCAB}f", *([0.5] * VOCAB))
            ordinal += 1
    trailer = bytearray(48)
    trailer[:8] = b"QWTFEND1"
    struct.pack_into("<Q", trailer, 8, positions)
    trailer[16:48] = hashlib.sha256(bytes(body)).digest()
    return bytes(body) + bytes(trailer)


class TeacherStreamTests(unittest.TestCase):
    def test_parses_records_and_skips_calibration_blocks(self):
        payload = build_teacher_stream(
            [[(1, 1), (2, 2), (3, 4)]], calibration_ordinals={1})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "teacher.bin"
            path.write_bytes(payload)
            stream = MODULE.read_teacher_stream(path)
        self.assertEqual(stream.vocab, VOCAB)
        self.assertEqual(len(stream.records), 3)
        self.assertEqual([r.agree for r in stream.records], [True, True, False])
        self.assertAlmostEqual(stream.records[0].target_probability, 2.718281828 ** -1, places=6)

    def test_rejects_corrupted_stream(self):
        payload = bytearray(build_teacher_stream([[(1, 1), (2, 2)]]))
        payload[100] ^= 0xFF
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "teacher.bin"
            path.write_bytes(bytes(payload))
            with self.assertRaises(ValueError):
                MODULE.read_teacher_stream(path)


class SimulationTests(unittest.TestCase):
    def test_perfect_agreement_emits_window_plus_one(self):
        result = MODULE.simulate_windows([True] * 20, 4)
        self.assertEqual(result["steps"], 4)
        self.assertEqual(result["emitted_tokens"], 20)
        self.assertEqual(result["tokens_per_step"], 5.0)

    def test_no_agreement_emits_one_per_step(self):
        result = MODULE.simulate_windows([False] * 6, 4)
        self.assertEqual(result["steps"], 6)
        self.assertEqual(result["tokens_per_step"], 1.0)

    def test_leading_runs_and_speedup_model(self):
        runs = MODULE.leading_run_lengths([True, True, False, True], 2)
        self.assertEqual(runs, [2, 1, 0, 1])
        # 5 tokens/step, full 100 ms, draft 20 ms x W=4, verify 120 ms.
        self.assertAlmostEqual(MODULE.modeled_speedup(5.0, 4, 100.0, 20.0, 120.0), 2.5)


class TraceExtractionTests(unittest.TestCase):
    def write_trace(self, directory, records):
        path = Path(directory) / "trace.jsonl"
        meta = {"type": "meta", "schema": "qwen36-route-v2", "layers": 2,
                "experts": 4, "selected_k": 1}
        path.write_text("".join(json.dumps(item) + "\n" for item in [meta] + records))
        return path

    def test_orders_and_deduplicates_positions_across_layers(self):
        records = [
            {"type": "route", "position": 1, "token_id": 7, "layer": 0},
            {"type": "route", "position": 0, "token_id": 5, "layer": 0},
            {"type": "route", "position": 0, "token_id": 5, "layer": 1},
            {"type": "route", "position": 2, "token_id": 9, "layer": 1},
        ]
        with tempfile.TemporaryDirectory() as directory:
            tokens = MODULE.read_trace_tokens(self.write_trace(directory, records))
        self.assertEqual(tokens, [5, 7, 9])

    def test_rejects_gap_from_resumed_session(self):
        records = [
            {"type": "route", "position": 3, "token_id": 5, "layer": 0},
            {"type": "route", "position": 4, "token_id": 6, "layer": 0},
        ]
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                MODULE.read_trace_tokens(self.write_trace(directory, records))


if __name__ == "__main__":
    unittest.main()
