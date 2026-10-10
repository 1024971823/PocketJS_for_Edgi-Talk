#!/usr/bin/env python3
"""Fail when source/config contains a user-specific absolute path."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PATTERN = re.compile(r"/home/[^/\s\"']+|/Users/[^/\s\"']+|[A-Za-z]:\\\\")
IGNORED_NAMES = {".sconsign.dblite", "rt-thread.elf", "rtthread.map"}
IGNORED_SUFFIXES = {".a", ".dblite", ".dep", ".d", ".elf", ".hex", ".map", ".o", ".obj", ".pyc"}
errors = []
for path in ROOT.rglob("*"):
    if not path.is_file():
        continue
    if path == Path(__file__).resolve() or any(
        part in {".git", "__pycache__", "build", "Debug"} for part in path.parts
    ) or path.name in IGNORED_NAMES or path.suffix in IGNORED_SUFFIXES:
        continue
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        continue
    for line_no, line in enumerate(text.splitlines(), 1):
        if PATTERN.search(line):
            errors.append(f"{path.relative_to(ROOT)}:{line_no}: {line.strip()}")
if errors:
    print("\n".join(errors), file=sys.stderr)
    raise SystemExit(1)
print("path check passed")
