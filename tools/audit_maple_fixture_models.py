#!/usr/bin/env python3
"""Reject fixture generators that instantiate production-sized Maple models."""

from __future__ import annotations

import ast
import pathlib
import sys


LIMITS = {
    "hidden_size": 512,
    "intermediate_size": 2048,
    "num_hidden_layers": 4,
    "vocab_size": 4096,
    "num_experts": 256,
}


def dotted_name(node: ast.AST) -> str:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        prefix = dotted_name(node.value)
        return f"{prefix}.{node.attr}" if prefix else node.attr
    return ""


def assigned_dicts(tree: ast.AST) -> dict[str, dict[str, object]]:
    configs: dict[str, dict[str, object]] = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name) or not isinstance(node.value, ast.Dict):
            continue
        try:
            value = ast.literal_eval(node.value)
        except (ValueError, TypeError):
            continue
        if isinstance(value, dict):
            configs[target.id] = value
    return configs


def synthetic_model_args(tree: ast.AST, configs: dict[str, dict[str, object]]) -> set[str]:
    safe: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        call = node.value
        if not isinstance(target, ast.Name) or not isinstance(call, ast.Call):
            continue
        if not dotted_name(call.func).endswith("ModelArgs.from_dict") or len(call.args) != 1:
            continue
        source = call.args[0]
        if not isinstance(source, ast.Name) or source.id not in configs:
            continue
        config = configs[source.id]
        if all(isinstance(config.get(key), int) and 0 < config[key] <= limit
               for key, limit in LIMITS.items()):
            safe.add(target.id)
    return safe


def audit(path: pathlib.Path) -> list[str]:
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, SyntaxError) as error:
        return [f"{path}: cannot audit fixture: {error}"]
    configs = assigned_dicts(tree)
    safe_args = synthetic_model_args(tree, configs)
    findings: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not dotted_name(node.func).endswith("maple_ref.Model"):
            continue
        argument = node.args[0] if len(node.args) == 1 else None
        if not isinstance(argument, ast.Name) or argument.id not in safe_args:
            findings.append(
                f"{path}:{node.lineno}: Maple model dimensions are not proven synthetic"
            )
    return findings


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/maple")
    findings: list[str] = []
    for path in root.rglob("*.py"):
        if "361db5da5e74ff6fcdd852d478e1f266ce11013a" in path.parts:
            continue
        findings.extend(audit(path))
    if findings:
        print("\n".join(findings))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
