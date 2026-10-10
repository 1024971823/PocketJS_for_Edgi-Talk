#!/usr/bin/env python3
"""Check the external source revisions and binary inputs used by the firmware."""
from __future__ import annotations

import argparse
import hashlib
import re
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
WORK = ROOT.parents[2]
LOCK_PATH = ROOT / "tools" / "dependencies.lock"
ROOTS = {
    "pocketjs": Path(os.environ.get("POCKETJS_ROOT", WORK / "pocketjs")).resolve(),
    "quickjs-ng": Path(
        os.environ.get("QUICKJS_ROOT", os.environ.get("POCKETJS_QUICKJS_ROOT", WORK / "quickjs-ng"))
    ).resolve(),
}
ARTIFACTS = {
    "pocketjs/apps/edgitalk-m55-smoke/dist/edgitalk-m55-smoke.pocket":
        ("pocketjs", "apps/edgitalk-m55-smoke/dist/edgitalk-m55-smoke.pocket"),
    "pocketjs/hosts/esp-idf/native/ui-core/target/thumbv8m.main-none-eabihf/release/libpocketjs_idf_ui_core.a":
        ("pocketjs", "hosts/esp-idf/native/ui-core/target/thumbv8m.main-none-eabihf/release/libpocketjs_idf_ui_core.a"),
    "pocketjs/hosts/esp-idf/native/render-rgb565/target/thumbv8m.main-none-eabihf/release/libpocketjs_idf_render_rgb565.a":
        ("pocketjs", "hosts/esp-idf/native/render-rgb565/target/thumbv8m.main-none-eabihf/release/libpocketjs_idf_render_rgb565.a"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


SOURCE_PATHS = (
    "README.md",
    "projects/Edgi_Talk_M55_PocketJS/tools/dependencies.lock",
    "projects/Edgi_Talk_M55_PocketJS/applications/pocketjs",
    "pocketjs-app/edgitalk-m55-smoke",
)


def check_source_only() -> None:
    """Validate the checked-in inputs without requiring the external BSP checkout."""
    try:
        lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {LOCK_PATH}: {error}") from error
    if lock.get("format") != 1:
        raise RuntimeError("dependency lock format must be 1")
    git = lock.get("git")
    hashes = lock.get("sha256")
    if not isinstance(git, dict) or set(git) != set(ROOTS):
        raise RuntimeError("dependency lock git keys must match pocketjs and quickjs-ng")
    if not isinstance(hashes, dict) or set(hashes) != set(ARTIFACTS):
        raise RuntimeError("dependency lock sha256 keys do not match the build inputs")
    for name, revision in git.items():
        if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise RuntimeError(f"invalid {name} revision in dependency lock")
    for name, digest in hashes.items():
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise RuntimeError(f"invalid SHA-256 for {name} in dependency lock")
    for relative in SOURCE_PATHS:
        if not (REPO_ROOT / relative).exists():
            raise RuntimeError(f"missing source input: {REPO_ROOT / relative}")


def current() -> dict[str, object]:
    revisions = {}
    for name, root in ROOTS.items():
        try:
            revisions[name] = subprocess.check_output(
                ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
            ).strip()
        except (OSError, subprocess.CalledProcessError) as error:
            raise RuntimeError(f"cannot read {name} revision at {root}: {error}") from error

    hashes = {}
    for lock_name, (root_name, relative) in ARTIFACTS.items():
        path = ROOTS[root_name] / relative
        if not path.is_file():
            raise RuntimeError(f"missing build input: {path}")
        hashes[lock_name] = sha256(path)
    return {"format": 1, "git": revisions, "sha256": hashes}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", action="store_true", help="replace the lock after intentional input changes")
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="validate checked-in metadata without requiring external PocketJS/BSP inputs",
    )
    args = parser.parse_args()
    if args.update and args.source_only:
        print("--update and --source-only cannot be combined", file=sys.stderr)
        return 2
    try:
        if args.source_only:
            check_source_only()
            print("dependency metadata passed (source-only)")
            return 0
        actual = current()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1

    if args.update:
        LOCK_PATH.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"updated {LOCK_PATH.relative_to(ROOT)}")
        return 0
    try:
        expected = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"cannot read {LOCK_PATH}: {error}", file=sys.stderr)
        return 1
    if expected != actual:
        print("dependency lock mismatch; current inputs:", file=sys.stderr)
        print(json.dumps(actual, indent=2, sort_keys=True), file=sys.stderr)
        print("run tools/check_dependencies.py --update after verifying the change", file=sys.stderr)
        return 1
    print("dependency lock passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
