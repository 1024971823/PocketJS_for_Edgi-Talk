#!/usr/bin/env python3
"""Statistical PC profiler over SWD (DWT_PCSR, no halting).

    pc_profile.py [--samples 1500] [--script transition|stage|none] [--top 25]

Boots the board (reset), runs a touch script through the debug touch word, reads DWT_PCSR in a
tight loop and prints the hottest functions via arm-none-eabi-addr2line. Needs a
POCKETJS_DEBUG_TOUCH build. Rust functions are shown demangled when symbols are available.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import stage_bench as sb  # noqa: E402

PCSR = 0xE000101C


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=1500)
    parser.add_argument("--script", default="transition", choices=["transition", "stage", "none"])
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--no-reset", action="store_true")
    args = parser.parse_args()

    sym = sb.symbols()
    touch = sym["g_pocketjs_debug_touch"]
    if not args.no_reset:
        subprocess.run([sb.FLASH, "reset"], capture_output=True)
        time.sleep(40)

    def tap(x, y):
        return ["mww 0x%08x 0x%08x" % (touch, 0x80000000 | (x << 16) | y), "sleep 250",
                "mww 0x%08x 0" % touch]

    script = []
    if args.script == "transition":
        script += tap(107, 141)
    elif args.script == "stage":
        script += tap(107, 141) + ["sleep 1800"] + tap(56, 134) + ["sleep 1800"] + tap(200, 120) + ["sleep 3000"]
    script.append("for {set i 0} {$i < %d} {incr i} { mdw 0x%08x 1 }" % (args.samples, PCSR))
    started = time.time()
    text = sb.openocd(script)
    print("openocd run %.1f s" % (time.time() - started))

    pcs = [int(m.group(1), 16) for m in re.finditer(r"0x%08x:\s+([0-9a-f]{8})" % PCSR, text)]
    pcs = [p for p in pcs if p not in (0, 0xFFFFFFFF)]
    if not pcs:
        print(text[-1500:])
        sys.exit("no PC samples (PCSR unavailable?)")
    prefix = os.path.dirname(sb.NM) + "/arm-none-eabi-"
    unique = sorted(set(pcs))
    out = subprocess.run([prefix + "addr2line", "-f", "-C", "-e", sb.ELF] + ["0x%x" % p for p in unique],
                         capture_output=True, text=True).stdout.splitlines()
    names = {}
    for index, pc in enumerate(unique):
        func, location = out[2 * index], out[2 * index + 1]
        names[pc] = func if func != "??" else "?? @0x%08x" % (pc & ~0xFFF)
    counts = collections.Counter(names[p] for p in pcs)
    total = len(pcs)
    print("%d samples" % total)
    for name, count in counts.most_common(args.top):
        print("%5.1f%%  %s" % (100.0 * count / total, name[:110]))


if __name__ == "__main__":
    main()
