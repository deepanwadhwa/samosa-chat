#!/usr/bin/env python3
"""Reject unfinished or degenerate Samosa thinking output.

The original quality harness looked only for expected answer substrings.  A
substring mentioned inside an unfinished reasoning block therefore counted as
a pass.  This checker makes completion structure an explicit release gate.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
from pathlib import Path


def repeated_ngram_fraction(text: str, size: int = 4) -> float:
    words = re.findall(r"[\w'-]+", text.lower(), flags=re.UNICODE)
    return _repeated_ngram_fraction(words, size)


def _repeated_ngram_fraction(words: list[str], size: int) -> float:
    if len(words) < size:
        return 0.0
    grams = [tuple(words[index : index + size]) for index in range(len(words) - size + 1)]
    counts = collections.Counter(grams)
    repeated = sum(count - 1 for count in counts.values() if count > 1)
    return repeated / len(grams)


def tail_repeated_ngram_fraction(text: str, size: int = 4, window: int = 256) -> float:
    words = re.findall(r"[\w'-]+", text.lower(), flags=re.UNICODE)
    return _repeated_ngram_fraction(words[-window:], size)


def longest_repeated_line_run(text: str) -> int:
    """Return the longest run of the same non-empty generated line.

    A global n-gram ratio reacts too slowly when a long, otherwise valid
    answer has only just entered a repetition attractor.  Consecutive copies
    of a CSS rule (the observed failure) should fail immediately rather than
    being diluted by all of the coherent text before them.
    """
    longest = current = 0
    previous = None
    for raw_line in text.splitlines():
        line = " ".join(raw_line.split()).casefold()
        if not line:
            previous = None
            current = 0
        elif line == previous:
            current += 1
        else:
            previous = line
            current = 1
        longest = max(longest, current)
    return longest


def evaluate(text: str, repetition_limit: float,
             required_substrings: tuple[str, ...] = (),
             required_patterns: tuple[str, ...] = ()) -> dict[str, object]:
    if "--- risposta ---" in text:
        text = text.split("--- risposta ---", 1)[1]
    closed = "</think>" in text
    answer = text.split("</think>", 1)[1].strip() if closed else ""
    repetition = repeated_ngram_fraction(text)
    tail_repetition = tail_repeated_ngram_fraction(answer)
    repeated_lines = longest_repeated_line_run(answer)
    checks = {
        "thinking_closed": closed,
        "final_answer_nonempty": bool(answer),
        "repeated_4gram_fraction": repetition,
        "repetition_within_limit": repetition <= repetition_limit,
        "tail_repeated_4gram_fraction": tail_repetition,
        "tail_repetition_within_limit": tail_repetition <= 0.70,
        "longest_repeated_line_run": repeated_lines,
        "repeated_line_run_within_limit": repeated_lines < 8,
        "required_substrings_present": all(item in answer for item in required_substrings),
        "required_patterns_present": all(
            re.search(pattern, answer, flags=re.IGNORECASE | re.DOTALL)
            for pattern in required_patterns),
    }
    return {"passed": all(value for key, value in checks.items()
                          if key not in {"repeated_4gram_fraction",
                                         "tail_repeated_4gram_fraction",
                                         "longest_repeated_line_run"}),
            "checks": checks}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--repetition-limit", type=float, default=0.45)
    parser.add_argument("--require", action="append", default=[], metavar="TEXT",
                        help="also require TEXT in the final answer (repeatable)")
    parser.add_argument("--require-regex", action="append", default=[], metavar="REGEX",
                        help="also require REGEX in the final answer (repeatable)")
    args = parser.parse_args()
    result = evaluate(args.output.read_text(errors="replace"), args.repetition_limit,
                      tuple(args.require), tuple(args.require_regex))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
