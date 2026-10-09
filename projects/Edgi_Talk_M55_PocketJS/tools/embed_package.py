#!/usr/bin/env python3
"""Generate PocketJS package embed sources without machine-specific paths."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--host-profile", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    generator = args.generator.resolve()
    sys.path.insert(0, str(generator.parent))
    import embed_package as upstream

    upstream.generate(
        args.package.resolve(),
        args.host_profile.resolve(),
        args.name,
        args.output_dir.resolve(),
    )
    relative_package = os.path.relpath(args.package.resolve(), project_root)
    assemblies = sorted(args.output_dir.glob(f"pocketjs_package_{args.name}*.S"))
    if not assemblies:
        raise RuntimeError(f"generator produced no assembly for {args.name}")
    for assembly in assemblies:
        text = assembly.read_text(encoding="utf-8")
        lines = []
        for line in text.splitlines(keepends=True):
            if line.startswith('.incbin "'):
                line = f'.incbin "{relative_package.replace(os.sep, "/")}"\n'
            lines.append(line)
        assembly.write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
