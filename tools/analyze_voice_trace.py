#!/usr/bin/env python3
"""Summarize a local Samosa Voice timing JSONL trace.

This developer tool reads metadata only. Voice traces never contain audio,
transcripts, prompts, or generated reply text.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


SCHEMA = "samosa.voice.trace.v1"


def milliseconds(value: float | None) -> str:
    return "—" if value is None else f"{value:.0f}"


def first(events: list[dict], name: str) -> dict | None:
    return next((event for event in events if event.get("event") == name), None)


def browser_delta(events: list[dict], start_name: str, end_name: str) -> float | None:
    start = first(events, start_name)
    end = first(events, end_name)
    if not start or not end:
        return None
    a = start.get("browser_mono_ms")
    b = end.get("browser_mono_ms")
    return float(b) - float(a) if isinstance(a, (int, float)) and isinstance(b, (int, float)) else None


def field(events: list[dict], event_name: str, field_name: str) -> float | None:
    event = first(events, event_name)
    value = (event or {}).get("fields", {}).get(field_name)
    return float(value) if isinstance(value, (int, float)) else None


def fields_for(events: list[dict], event_name: str, field_name: str) -> list[float]:
    values: list[float] = []
    for event in events:
        if event.get("event") != event_name:
            continue
        value = event.get("fields", {}).get(field_name)
        if isinstance(value, (int, float)):
            values.append(float(value))
    return values


def read_rows(paths: list[Path]) -> list[dict]:
    rows: list[dict] = []
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
                if row.get("schema") != SCHEMA:
                    raise ValueError(f"{path}:{line_number}: unsupported schema {row.get('schema')!r}")
                rows.append(row)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize Samosa Voice timing JSONL")
    parser.add_argument("trace", nargs="+", type=Path, help="voice-trace-*.jsonl file")
    args = parser.parse_args()
    try:
        rows = read_rows(args.trace)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2

    turns: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        if row.get("turn_id"):
            turns[str(row["turn_id"])].append(row)
    if not turns:
        print("No completed Voice turns were found in this trace.")
        return 0

    headers = [
        "turn", "endpoint", "stt", "llm first", "llm total", "tts first",
        "tts compute", "audio", "gaps", "end→audio", "total", "outcome"
    ]
    output: list[list[str]] = []
    for turn_id, events in turns.items():
        events.sort(key=lambda event: (event.get("browser_mono_ms", float("inf")), event.get("sequence", 0)))
        endpoint = field(events, "speech_ended", "silence_ms")
        stt = browser_delta(events, "stt_request_started", "stt_response_received")
        llm_first = browser_delta(events, "llm_request_started", "llm_first_content_delta")
        llm_total = browser_delta(events, "llm_request_started", "llm_stream_complete")
        tts_first = browser_delta(events, "tts_request_started", "tts_first_audio_byte")
        tts_compute_values = fields_for(events, "tts_generation_complete", "server_duration_ms")
        audio_ms = 0.0
        for event in events:
            if event.get("event") != "tts_generation_complete":
                continue
            fields = event.get("fields", {})
            samples, rate = fields.get("sample_count"), fields.get("sample_rate")
            if isinstance(samples, (int, float)) and isinstance(rate, (int, float)) and rate:
                audio_ms += float(samples) * 1000.0 / float(rate)
        gaps = fields_for(events, "playback_buffer_underrun", "gap_ms")
        end_to_audio = browser_delta(events, "speech_ended", "playback_started")
        total = browser_delta(events, "speech_ended", "voice_turn_complete")
        complete = first(events, "voice_turn_complete") or first(events, "voice_turn_failed")
        outcome = str((complete or {}).get("fields", {}).get("outcome", "incomplete"))
        output.append([
            turn_id[-12:], milliseconds(endpoint), milliseconds(stt), milliseconds(llm_first),
            milliseconds(llm_total), milliseconds(tts_first), milliseconds(sum(tts_compute_values) if tts_compute_values else None),
            milliseconds(audio_ms if audio_ms else None), f"{len(gaps)}/{sum(gaps):.0f}",
            milliseconds(end_to_audio), milliseconds(total), outcome,
        ])

    widths = [max(len(headers[index]), *(len(row[index]) for row in output)) for index in range(len(headers))]
    print("  ".join(headers[index].ljust(widths[index]) for index in range(len(headers))))
    print("  ".join("-" * width for width in widths))
    for row in output:
        print("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)))
    print("\nAll durations are milliseconds. ‘llm first’ means first answer text, not reasoning; ‘gaps’ is count/total gap time.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
