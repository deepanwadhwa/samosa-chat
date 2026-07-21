#!/usr/bin/env python3
"""E-X8 speculative-drafting acceptance analysis.

Two subcommands:

  extract-trace  Convert qwen36 route-v2 JSONL traces (one per draft
                 generation) into a QWTFM teacher corpus whose prompts are
                 exact token-id sequences ("tokens" items).  The trace records
                 every *input* token id at every routed layer, so the ordered,
                 deduplicated position -> token_id map is the templated prompt
                 plus the generated continuation, verbatim.

  analyze        Parse a QWTFM001 teacher stream produced by teacher-forcing
                 the full model over those token sequences and report
                 per-position greedy agreement (alpha), leading-run
                 statistics, the sequential speculative-window simulation
                 E[tokens/step] for the requested window sizes, and (when
                 timing inputs are supplied) the modeled end-to-end speedup
                 curve.

The offline simulation is exact only up to each window's first disagreement:
after a real rejection the verifier would emit a corrected token and the
draft would continue from a different prefix than the recorded trajectory.
Report the numbers as a model, not as a measured speedup.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path

TEACHER_MAGIC = b"QWTFM001"
TEACHER_TRAILER_MAGIC = b"QWTFEND1"
TEACHER_HEADER_BYTES = 80
TEACHER_RECORD_BYTES = 56
TEACHER_TRAILER_BYTES = 48


def read_trace_tokens(path: Path) -> list[int]:
    """Return the ordered input token ids recorded by a route-v2 trace."""
    meta = None
    by_position: dict[int, int] = {}
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("type") == "meta":
                if meta is not None:
                    raise ValueError(f"{path}: duplicate trace metadata")
                meta = record
                continue
            if meta is None or record.get("type") != "route":
                raise ValueError(f"{path}: invalid record at line {line_number}")
            position = int(record["position"])
            token = int(record["token_id"])
            if position < 0:
                raise ValueError(f"{path}: negative position at line {line_number}")
            known = by_position.get(position)
            if known is not None and known != token:
                raise ValueError(
                    f"{path}: position {position} has conflicting token ids "
                    f"{known} and {token}")
            by_position[position] = token
    if meta is None:
        raise ValueError(f"{path}: trace has no metadata")
    if not by_position:
        raise ValueError(f"{path}: trace has no route records")
    count = max(by_position) + 1
    missing = [p for p in range(count) if p not in by_position]
    if missing:
        raise ValueError(
            f"{path}: trace positions are not contiguous from 0 "
            f"(first missing: {missing[0]}); a resumed session does not "
            f"re-route its restored context — generate from a full prompt")
    return [by_position[p] for p in range(count)]


@dataclass
class TeacherRecord:
    sequence: int
    position: int
    target: int
    target_logit: float
    lse: float
    top5: tuple[int, ...]

    @property
    def argmax(self) -> int:
        return self.top5[0]

    @property
    def agree(self) -> bool:
        return self.argmax == self.target

    @property
    def target_probability(self) -> float:
        return math.exp(self.target_logit - self.lse)


@dataclass
class TeacherStream:
    vocab: int
    sequence_count: int
    positions: int
    calibration: int
    corpus_sha256: str
    records: list[TeacherRecord] = field(default_factory=list)

    def sequences(self) -> dict[int, list[TeacherRecord]]:
        grouped: dict[int, list[TeacherRecord]] = {}
        for record in self.records:
            grouped.setdefault(record.sequence, []).append(record)
        for sequence, items in grouped.items():
            items.sort(key=lambda record: record.position)
            expected = list(range(len(items)))
            if [record.position for record in items] != expected:
                raise ValueError(f"sequence {sequence} positions are not contiguous")
        return grouped


def read_teacher_stream(path: Path) -> TeacherStream:
    data = path.read_bytes()
    if len(data) < TEACHER_HEADER_BYTES + TEACHER_TRAILER_BYTES:
        raise ValueError(f"{path}: too short for a QWTFM001 stream")
    header = data[:TEACHER_HEADER_BYTES]
    if header[:8] != TEACHER_MAGIC:
        raise ValueError(f"{path}: bad magic {header[:8]!r}")
    version, _reserved, header_bytes = struct.unpack_from("<HHI", header, 8)
    if version != 1 or header_bytes != TEACHER_HEADER_BYTES:
        raise ValueError(f"{path}: unsupported header version={version} size={header_bytes}")
    corpus_sha = header[16:48].hex()
    vocab, sequence_count = struct.unpack_from("<II", header, 48)
    positions, calibration, logit_kind, record_bytes = struct.unpack_from("<QIII", header, 56)
    if logit_kind != 1 or record_bytes != TEACHER_RECORD_BYTES:
        raise ValueError(
            f"{path}: unsupported logit encoding {logit_kind} or record size {record_bytes}")
    trailer = data[-TEACHER_TRAILER_BYTES:]
    if trailer[:8] != TEACHER_TRAILER_MAGIC:
        raise ValueError(f"{path}: bad trailer magic {trailer[:8]!r}")
    (trailer_positions,) = struct.unpack_from("<Q", trailer, 8)
    if trailer_positions != positions:
        raise ValueError(f"{path}: trailer position count mismatch")
    digest = hashlib.sha256(data[:-TEACHER_TRAILER_BYTES]).hexdigest()
    if digest != trailer[16:48].hex():
        raise ValueError(f"{path}: stream sha256 mismatch — truncated or corrupt")

    stream = TeacherStream(vocab=vocab, sequence_count=sequence_count,
                           positions=positions, calibration=calibration,
                           corpus_sha256=corpus_sha)
    offset = TEACHER_HEADER_BYTES
    end = len(data) - TEACHER_TRAILER_BYTES
    while offset < end:
        if offset + TEACHER_RECORD_BYTES > end:
            raise ValueError(f"{path}: truncated record at offset {offset}")
        record = data[offset:offset + TEACHER_RECORD_BYTES]
        offset += TEACHER_RECORD_BYTES
        sequence, position, target, full, full_vocab = struct.unpack_from("<IIIII", record, 0)
        target_logit, lse = struct.unpack_from("<dd", record, 20)
        top5 = struct.unpack_from("<5I", record, 36)
        if full:
            if full_vocab != vocab:
                raise ValueError(f"{path}: calibration vocab mismatch at offset {offset}")
            offset += 4 * vocab  # full logits are not needed for acceptance
            if offset > end:
                raise ValueError(f"{path}: truncated calibration block")
        stream.records.append(TeacherRecord(
            sequence=sequence, position=position, target=target,
            target_logit=target_logit, lse=lse, top5=tuple(top5)))
    if len(stream.records) != positions:
        raise ValueError(
            f"{path}: header promises {positions} positions, found {len(stream.records)}")
    return stream


def leading_run_lengths(agreement: list[bool], window: int) -> list[int]:
    """Leading-agreement length of every window start (distribution view)."""
    runs = []
    for start in range(len(agreement)):
        accepted = 0
        for step in range(start, min(start + window, len(agreement))):
            if not agreement[step]:
                break
            accepted += 1
        runs.append(accepted)
    return runs


def simulate_windows(agreement: list[bool], window: int) -> dict:
    """Sequential speculative simulation over the recorded trajectory.

    Each step drafts `window` tokens and accepts the leading agreements; the
    verify pass contributes one further token (correction or bonus), so a
    step emits accepted+1 tokens.  Exact only up to each step's first
    disagreement — see the module docstring.
    """
    position = 0
    steps = 0
    emitted = 0
    accepted_total = 0
    while position < len(agreement):
        drafted = min(window, len(agreement) - position)
        accepted = 0
        for step in range(position, position + drafted):
            if not agreement[step]:
                break
            accepted += 1
        steps += 1
        accepted_total += accepted
        emitted += accepted + 1
        position += accepted + 1
    return {
        "window": window,
        "steps": steps,
        "emitted_tokens": emitted,
        "accepted_tokens": accepted_total,
        "tokens_per_step": emitted / steps if steps else 0.0,
    }


def modeled_speedup(tokens_per_step: float, window: int, t_full_ms: float,
                    t_draft_ms: float, t_verify_ms: float) -> float:
    """Speedup vs plain decode: emitted tokens per unit time, normalized."""
    step_ms = window * t_draft_ms + t_verify_ms
    if step_ms <= 0:
        raise ValueError("non-positive modeled step time")
    return tokens_per_step * t_full_ms / step_ms


def command_extract_trace(args: argparse.Namespace) -> int:
    prompts = []
    for trace in args.traces:
        tokens = read_trace_tokens(Path(trace))
        prompts.append({"tokens": tokens})
        print(f"{trace}: {len(tokens)} token ids")
    corpus = {"schema_version": 1, "prompts": prompts}
    Path(args.out).write_text(json.dumps(corpus) + "\n", encoding="utf-8")
    print(f"wrote {args.out}: {len(prompts)} sequence(s)")
    return 0


def parse_draft_starts(text: str, sequence_count: int) -> list[int]:
    starts = [int(item) for item in text.split(",")]
    if len(starts) != sequence_count:
        raise ValueError(
            f"--draft-start has {len(starts)} entries for {sequence_count} sequence(s)")
    if any(start < 1 for start in starts):
        raise ValueError("--draft-start entries must be >= 1 (index of first generated token)")
    return starts


def parse_verify_times(text: str | None) -> dict[int, float]:
    if not text:
        return {}
    result = {}
    for chunk in text.split(","):
        window, _, value = chunk.partition(":")
        result[int(window)] = float(value)
    return result


def command_analyze(args: argparse.Namespace) -> int:
    stream = read_teacher_stream(Path(args.teacher))
    grouped = stream.sequences()
    starts = parse_draft_starts(args.draft_start, len(grouped))
    windows = [int(item) for item in args.windows.split(",")]
    verify_ms = parse_verify_times(args.t_verify)

    tsv_rows = ["sequence\tposition\ttarget\targmax\tagree\tp_target"]
    combined_agreement: list[bool] = []
    per_sequence = []
    for (sequence, records), start in zip(sorted(grouped.items()), starts):
        # Record at position p predicts the token at p+1, so the draft
        # region (targets are generated tokens) is positions >= start-1.
        region = [record for record in records if record.position >= start - 1]
        if not region:
            raise ValueError(f"sequence {sequence}: draft start {start} leaves no positions")
        agreement = [record.agree for record in region]
        combined_agreement.extend(agreement)
        alpha = sum(agreement) / len(agreement)
        mean_p = sum(record.target_probability for record in region) / len(region)
        per_sequence.append({
            "sequence": sequence,
            "draft_positions": len(region),
            "alpha": alpha,
            "mean_p_draft_token": mean_p,
        })
        for record in region:
            tsv_rows.append(
                f"{record.sequence}\t{record.position}\t{record.target}\t"
                f"{record.argmax}\t{int(record.agree)}\t{record.target_probability:.6f}")

    report = {
        "teacher_stream": {
            "vocab": stream.vocab,
            "sequences": stream.sequence_count,
            "positions": stream.positions,
            "corpus_sha256": stream.corpus_sha256,
        },
        "per_sequence": per_sequence,
        "combined": {
            "draft_positions": len(combined_agreement),
            "alpha": sum(combined_agreement) / len(combined_agreement),
        },
        "windows": [],
        "note": ("offline trajectory simulation; exact only up to each "
                 "window's first disagreement"),
    }
    for window in windows:
        runs = leading_run_lengths(combined_agreement, window)
        simulated = simulate_windows(combined_agreement, window)
        cell = dict(simulated)
        cell["mean_leading_run"] = sum(runs) / len(runs)
        cell["leading_run_histogram"] = {
            str(length): runs.count(length) for length in range(window + 1)}
        if args.t_full and args.t_draft and window in verify_ms:
            cell["modeled_speedup_vs_full"] = modeled_speedup(
                simulated["tokens_per_step"], window, args.t_full,
                args.t_draft, verify_ms[window])
        report["windows"].append(cell)

    if args.tsv:
        Path(args.tsv).write_text("\n".join(tsv_rows) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    extract = commands.add_parser(
        "extract-trace", help="route-v2 JSONL trace(s) -> tokens corpus JSON")
    extract.add_argument("traces", nargs="+", help="ROUTE_TRACE JSONL, one per draft run")
    extract.add_argument("--out", required=True, help="corpus JSON to write")
    extract.set_defaults(handler=command_extract_trace)

    analyze = commands.add_parser(
        "analyze", help="QWTFM001 teacher stream -> acceptance report JSON")
    analyze.add_argument("teacher", help="teacher output from the full model")
    analyze.add_argument("--draft-start", required=True,
                         help="comma list, per sequence: index of the first generated token")
    analyze.add_argument("--windows", default="4,6,8", help="comma list of window sizes")
    analyze.add_argument("--t-full", type=float, default=None,
                         help="measured full-model ms/token (e.g. 131.6)")
    analyze.add_argument("--t-draft", type=float, default=None,
                         help="measured MOE_K=1 draft ms/token")
    analyze.add_argument("--t-verify", default=None,
                         help="comma list window:ms, measured batched verify cost")
    analyze.add_argument("--tsv", default=None, help="optional per-position TSV dump")
    analyze.set_defaults(handler=command_analyze)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
