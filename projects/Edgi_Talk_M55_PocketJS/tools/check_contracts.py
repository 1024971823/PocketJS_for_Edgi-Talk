#!/usr/bin/env python3
"""Check that the Python companion and firmware enforce the same limits."""
from __future__ import annotations

import ast
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
CONTRACT_PATH = REPO_ROOT / "contracts" / "chart-contract.json"
PYTHON_PATH = ROOT / "tools" / "edgitalk-companion" / "edgitalk.py"
C_PATHS = (
    ROOT / "applications" / "pocketjs" / "include" / "pocketjs_synth.h",
    ROOT / "applications" / "pocketjs" / "include" / "pocketjs_game.h",
)
EXPECTED = {
    "song_limit_bytes": ("SONG_LIMIT", "POCKETJS_GAME_SONG_LIMIT"),
    "player_wav_limit_bytes": ("PLAYER_WAV_LIMIT", "POCKETJS_GAME_PLAYER_WAV_LIMIT"),
    "music_name_max_bytes": ("PLAYER_NAME_MAX", "POCKETJS_GAME_MUSIC_NAME_MAX"),
    "max_events": ("MAX_EVENTS", "POCKETJS_GAME_MAX_EVENTS"),
    "max_notes": ("MAX_NOTES", "POCKETJS_GAME_MAX_NOTES"),
    "max_chart_steps": ("MAX_CHART_STEPS", "POCKETJS_GAME_STEP_MAX"),
    "bpm_min": ("BPM_MIN", "POCKETJS_GAME_BPM_MIN"),
    "bpm_max": ("BPM_MAX", "POCKETJS_GAME_BPM_MAX"),
    "title_max_code_points": ("TITLE_MAX_CODE_POINTS", "POCKETJS_GAME_TITLE_MAX"),
    "level_min": ("LEVEL_MIN", "POCKETJS_GAME_LEVEL_MIN"),
    "level_max": ("LEVEL_MAX", "POCKETJS_GAME_LEVEL_MAX"),
}


class ContractError(RuntimeError):
    pass


def _safe_eval(expression: str) -> int:
    expression = re.sub(r"(?<=\d)[uUlL]\b", "", expression).strip()
    tree = ast.parse(expression, mode="eval")

    def visit(node: ast.AST) -> int:
        if isinstance(node, ast.Expression):
            return visit(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
            value = visit(node.operand)
            return value if isinstance(node.op, ast.UAdd) else -value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult)):
            left, right = visit(node.left), visit(node.right)
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            return left * right
        raise ContractError(f"unsupported contract expression: {expression}")

    return visit(tree)


def python_constants() -> dict[str, int]:
    tree = ast.parse(PYTHON_PATH.read_text(encoding="utf-8"), filename=str(PYTHON_PATH))
    values: dict[str, int] = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            try:
                value = _safe_eval(ast.get_source_segment(PYTHON_PATH.read_text(encoding="utf-8"), node.value) or "")
            except (ContractError, SyntaxError):
                continue
            values[node.targets[0].id] = value
    return values


def c_constants() -> dict[str, int]:
    values: dict[str, int] = {}
    pattern = re.compile(r"^\s*#define\s+([A-Z][A-Z0-9_]*)\s+(.+?)\s*(?:/\*.*\*/)?$")
    for path in C_PATHS:
        for line in path.read_text(encoding="utf-8").splitlines():
            match = pattern.match(line)
            if not match:
                continue
            name, expression = match.groups()
            try:
                values[name] = _safe_eval(expression)
            except (ContractError, SyntaxError):
                continue
    return values


def load_contract() -> dict[str, Any]:
    try:
        data = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError(f"cannot read {CONTRACT_PATH}: {error}") from error
    if data.get("format") != 1 or not isinstance(data.get("limits"), dict):
        raise ContractError("contract must contain format: 1 and a limits object")
    return data


def check() -> list[str]:
    contract = load_contract()
    limits = contract["limits"]
    expected_keys = set(EXPECTED)
    actual_keys = set(limits)
    problems: list[str] = []
    if actual_keys != expected_keys:
        problems.append(
            "contract limits keys differ: "
            f"missing={sorted(expected_keys - actual_keys)} extra={sorted(actual_keys - expected_keys)}"
        )
    python_values = python_constants()
    c_values = c_constants()
    for key, (python_name, c_name) in EXPECTED.items():
        expected = limits.get(key)
        if not isinstance(expected, int):
            problems.append(f"{key}: contract value must be an integer")
            continue
        actual_python = python_values.get(python_name)
        actual_c = c_values.get(c_name)
        if actual_python != expected:
            problems.append(f"{key}: Python {python_name}={actual_python!r}, expected {expected}")
        if actual_c != expected:
            problems.append(f"{key}: C {c_name}={actual_c!r}, expected {expected}")
    return problems


def main() -> int:
    try:
        problems = check()
    except ContractError as error:
        print(error, file=sys.stderr)
        return 1
    if problems:
        print("\n".join(problems), file=sys.stderr)
        print("contract check failed", file=sys.stderr)
        return 1
    print("contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
