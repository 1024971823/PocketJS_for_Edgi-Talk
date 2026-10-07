#!/usr/bin/env python3
"""Screenshot the panel over SWD after a scripted sequence of touches.

    shot.py "wait:45000" "tap:332,20" "wait:2500" "shot:/tmp/calendar.png" ...

Steps: tap:x,y (logical 400x240 coordinates, debug touch build), wait:ms, shot:path.
Without --no-reset the board is reset first and the script starts at boot. The first step is
usually a wait that covers the boot and page warm-up.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import stage_bench as sb  # noqa: E402
import scanout_check as sc  # noqa: E402
import subprocess  # noqa: E402
import time  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sym = sb.symbols()
    touch = sym.get("g_pocketjs_debug_touch", 0)
    scan = sym["graphics_scanout_storage"]
    size = sc.STRIDE * sc.H * 2
    if "--no-reset" not in sys.argv:
        subprocess.run([sb.FLASH, "reset"], capture_output=True)
    script = []
    shots = []
    for step in args:
        kind, _, value = step.partition(":")
        if kind == "wait":
            script.append("sleep %d" % int(value))
        elif kind == "tap":
            x, y = (int(v) for v in value.split(","))
            script += ["mww 0x%08x 0x%08x" % (touch, 0x80000000 | (x << 16) | y), "sleep 250",
                       "mww 0x%08x 0" % touch, "sleep 400"]
        elif kind == "shot":
            raw = value + ".bin"
            script.append("dump_image %s 0x%08x %d" % (raw, scan, size))
            shots.append((raw, value))
    sb.openocd(script)
    for raw, path in shots:
        image, _ = sc.to_image(raw)
        image.save(path if path.endswith(".png") else path + ".png")
        os.remove(raw)
        print("saved", path)


if __name__ == "__main__":
    main()
