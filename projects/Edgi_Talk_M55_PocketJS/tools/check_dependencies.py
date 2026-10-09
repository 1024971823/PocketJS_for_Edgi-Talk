#!/usr/bin/env python3
"""Check the external source revisions and binary inputs used by the firmware."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parents[2]
LOCK_PATH = ROOT / "tools" / "dependencies.lock"
ROOTS = {
    "pocketjs": Path(os.environ.get("POCKETJS_ROOT", WORK / "pocketjs")).resolve(),
    "quickjs-ng": Path(os.environ.get("QUICKJS_ROOT", WORK / "quickjs-ng")).resolve(),
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
    args = parser.parse_args()
    try:
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
