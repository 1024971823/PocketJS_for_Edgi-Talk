#!/usr/bin/env python3
"""Validate release metadata and repository hygiene before publishing."""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
VERSION_PATH = REPO_ROOT / "VERSION"
CHANGELOG_PATH = REPO_ROOT / "CHANGELOG.md"
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")


def main() -> int:
    problems: list[str] = []
    try:
        version = VERSION_PATH.read_text(encoding="utf-8").strip()
    except OSError as error:
        problems.append(f"cannot read VERSION: {error}")
        version = ""
    if version and not SEMVER.fullmatch(version):
        problems.append(f"VERSION must be semantic versioning, got {version!r}")
    try:
        changelog = CHANGELOG_PATH.read_text(encoding="utf-8")
    except OSError as error:
        problems.append(f"cannot read CHANGELOG.md: {error}")
        changelog = ""
    if version and f"## [{version}]" not in changelog and f"## {version}" not in changelog:
        problems.append(f"CHANGELOG.md has no entry for {version}")
    try:
        diff = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "diff", "--check"],
            text=True,
            capture_output=True,
            check=False,
        )
    except OSError as error:
        problems.append(f"cannot run git diff --check: {error}")
    else:
        if diff.returncode:
            problems.append("git diff --check found whitespace errors")
            if diff.stdout:
                problems.append(diff.stdout.strip())
    if problems:
        print("\n".join(problems), file=sys.stderr)
        print("release check failed", file=sys.stderr)
        return 1
    print(f"release check passed ({version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
