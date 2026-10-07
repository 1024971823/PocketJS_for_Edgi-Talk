#!/usr/bin/env python3
"""Capture the panel scanout buffer over SWD and cross-check the partial present path.

    scanout_check.py [--stage] [--out /tmp/scan]

Dumps the scanout buffer while the firmware presents partial rectangles, then sets
g_lcd_force_full so the next presents go through the full-frame GPU rotation, dumps again and
counts differing pixels (a correct partial path only differs where the screen changed in
between, e.g. clocks and gauges). Writes PNGs of both dumps in landscape orientation.
Needs a POCKETJS_DEBUG_TOUCH build for --stage.
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import stage_bench as sb  # noqa: E402

W, H, STRIDE = 480, 800, 512


def to_image(path):
    from PIL import Image

    raw = open(path, "rb").read()
    pix = struct.unpack("<%dH" % (len(raw) // 2), raw)
    image = Image.new("RGB", (H, W))
    out = image.load()
    for u in range(H):
        base = u * STRIDE
        for v in range(W):
            p = pix[base + (W - 1 - v)]
            out[u, v] = (((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31)
    return image, pix


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", action="store_true")
    parser.add_argument("--out", default="/tmp/scan")
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--single", choices=("partial", "full"),
                        help="one dump only; 'full' forces the GPU path from the start")
    parser.add_argument("--wait", type=float, default=4.2, help="seconds after stage start")
    args = parser.parse_args()

    sym = sb.symbols()
    touch = sym["g_pocketjs_debug_touch"]
    force = sym["g_lcd_force_full"]
    scan = sym["graphics_scanout_storage"]
    size = STRIDE * H * 2
    if not args.no_reset:
        subprocess.run([sb.FLASH, "reset"], capture_output=True)
        time.sleep(40)

    def tap(x, y):
        return ["mww 0x%08x 0x%08x" % (touch, 0x80000000 | (x << 16) | y), "sleep 250",
                "mww 0x%08x 0" % touch, "sleep 400"]

    script = []
    if args.single == "full":
        script += ["mww 0x%08x 1" % force]
    if args.stage:
        script += tap(107, 141) + ["sleep 1500"] + tap(56, 134) + ["sleep 1500"] + tap(200, 120) + ["sleep %d" % int(args.wait * 1000)]
    else:
        script += ["sleep 1500"]
    if args.single:
        script += ["dump_image %s_%s.bin 0x%08x %d" % (args.out, args.single, scan, size), "mww 0x%08x 0" % force]
        sb.openocd(script)
        image, _ = to_image("%s_%s.bin" % (args.out, args.single))
        image.save("%s_%s.png" % (args.out, args.single))
        print("saved %s_%s.png" % (args.out, args.single))
        return
    script += ["dump_image %s_partial.bin 0x%08x %d" % (args.out, scan, size),
               "mww 0x%08x 1" % force, "sleep 1200",
               "dump_image %s_full.bin 0x%08x %d" % (args.out, scan, size),
               "mww 0x%08x 0" % force]
    sb.openocd(script)

    a, pa = to_image(args.out + "_partial.bin")
    b, pb = to_image(args.out + "_full.bin")
    a.save(args.out + "_partial.png")
    b.save(args.out + "_full.png")
    diff = sum(1 for x, y in zip(pa, pb) if x != y)
    print("differing pixels: %d of %d (%.3f%%)" % (diff, W * H, 100.0 * diff / (W * H)))


if __name__ == "__main__":
    main()
