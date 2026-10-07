#!/usr/bin/env python3
"""On-board frame budget bench (needs a POCKETJS_DEBUG_TOUCH build and a KitProg3 probe).

Resets the board, waits for boot, taps Home -> PLAY -> first song -> stage start through the
debug touch word, then samples the frame counters before and after a window and prints the
per-frame averages. Symbol addresses are read from the ELF with arm-none-eabi-nm.

    stage_bench.py [--song 0] [--settle 3.5] [--window 3.0] [--home-only]
"""
import argparse
import os
import re
import subprocess
import sys
import time

WORK = os.path.abspath(os.path.join(os.path.dirname(__file__), *[".."] * 5))
SDK = os.path.join(WORK, "sdk-bsp-psoc_e84-edgi-talk")
ELF = os.path.join(SDK, "projects/Edgi_Talk_M55_PocketJS/rt-thread.elf")
NM = os.path.join(WORK, "toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm")
OPENOCD_ROOT = os.path.join(WORK, "tools/openocd")
FLASH = os.path.join(WORK, "flash-xiaozhi-daplink.sh")
CLOCK_HZ = float(os.environ.get("CORE_HZ", "400000000"))


def symbols():
    out = subprocess.check_output([NM, ELF], text=True)
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


def openocd(commands):
    args = [
        os.path.join(OPENOCD_ROOT, "bin/openocd"),
        "-s", os.path.join(OPENOCD_ROOT, "scripts"),
        "-s", os.path.join(SDK, "projects/libs/TARGET_APP_KIT_PSE84_EVAL_EPC2/config/GeneratedSource"),
        "-f", "interface/kitprog3.cfg",
        "-c", "transport select swd",
        "-c", "set ENABLE_CM55 1",
        "-c", "set QSPI_FLASHLOADER " + os.path.join(OPENOCD_ROOT, "flm/infineon/pse8x6/PSE84_SMIF.FLM"),
        "-f", "target/infineon/pse84xgxs2.cfg",
        "-c", "adapter speed 12000",
        "-c", "init; targets cat1d.cm55; " + "; ".join(commands) + "; shutdown",
    ]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=240)
    return proc.stdout + proc.stderr


def words(text, address, count):
    pattern = re.compile(r"0x%08x:\s+((?:[0-9a-f]{8}\s*)+)" % address)
    match = pattern.search(text)
    if not match:
        raise RuntimeError("no dump for %08x:\n%s" % (address, text[-800:]))
    return [int(w, 16) for w in match.group(1).split()][:count]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--song", type=int, default=0)
    parser.add_argument("--settle", type=float, default=3.5)
    parser.add_argument("--window", type=float, default=3.0)
    parser.add_argument("--home-only", action="store_true")
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--transition", action="store_true", help="measure Home -> Songs -> Home instead")
    args = parser.parse_args()

    sym = symbols()
    touch = sym["g_pocketjs_debug_touch"]
    counters = sym["g_pocketjs_rows_total"]
    prof = sym["g_lcd_prof"]

    if not args.no_reset:
        subprocess.run([FLASH, "reset"], capture_output=True)
        time.sleep(40)

    def tap(x, y, hold=250):
        return [
            "mww 0x%08x 0x%08x" % (touch, 0x80000000 | (x << 16) | y),
            "sleep %d" % hold,
            "mww 0x%08x 0" % touch,
            "sleep 400",
        ]

    def snapshot():
        return ["mdw 0x%08x 8" % counters, "mdw 0x%08x 6" % prof]

    script = []
    if args.transition:
        script += ["sleep 1000"] + snapshot() + tap(107, 141) + ["sleep 3000"] + snapshot()
        script += tap(40, 20) + ["sleep 3000"] + snapshot()
    elif not args.home_only:
        script += tap(107, 141) + ["sleep 1500"]
        script += tap(56 + 96 * args.song, 134) + ["sleep 1500"]
        script += tap(200, 120) + ["sleep %d" % int(args.settle * 1000)]
    else:
        script += ["sleep 1000"]
    if not args.transition:
        script += snapshot() + ["sleep %d" % int(args.window * 1000)] + snapshot()
    text = openocd(script)
    if args.transition:
        chunks = text.split("0x%08x:" % counters)
        names = ["rows", "regions", "lcd_ms", "render_ms", "prepare_ms", "present_ms", "ui_ms", "frames"]
        snaps = [[int(w, 16) for w in c.split("\n")[0].split()][:8] for c in chunks[1:]]
        for label, a, b in (("Home->Songs", snaps[0], snaps[1]), ("Songs->Home", snaps[1], snaps[2])):
            d = {n: (y - x) & 0xFFFFFFFF for n, x, y in zip(names, a, b)}
            print("%s: frames=%d render=%d ms ui=%d ms present=%d ms regions=%d" % (
                label, d["frames"], d["render_ms"], d["ui_ms"], d["present_ms"], d["regions"]))
        return

    first = re.findall(r"0x%08x:" % counters, text)
    if len(first) < 2:
        print(text[-1500:])
        sys.exit("counters not read")
    chunks = text.split("0x%08x:" % counters)
    a = [int(w, 16) for w in chunks[1].split("\n")[0].split()][:8]
    b = [int(w, 16) for w in chunks[2].split("\n")[0].split()][:8]
    pchunks = text.split("0x%08x:" % prof)
    pa = [int(w, 16) for w in pchunks[1].split("\n")[0].split()][:6]
    pb = [int(w, 16) for w in pchunks[2].split("\n")[0].split()][:6]
    names = ["rows", "regions", "lcd_ms", "render_ms", "prepare_ms", "present_ms", "ui_ms", "frames"]
    d = {n: (y - x) & 0xFFFFFFFF for n, x, y in zip(names, a, b)}
    frames = max(d["frames"], 1)
    print("frames in window: %d  (%.1f fps)" % (d["frames"], d["frames"] / args.window))
    for key in ("ui_ms", "prepare_ms", "render_ms", "lcd_ms", "present_ms"):
        print("  %-11s %6.1f ms/frame" % (key, d[key] / frames))
    print("  regions/frame %.1f  rows/frame %.0f" % (d["regions"] / frames, d["rows"] / frames))
    pf = max((pb[0] - pa[0]) & 0xFFFFFFFF, 1)
    labels = ["frames", "clean", "rotate", "invalidate", "set-fb", "wait"]
    print("  LCD presents in window: %d" % pf)
    for i in range(1, 6):
        cycles = (pb[i] - pa[i]) & 0xFFFFFFFF
        print("    %-10s %6.2f ms/present" % (labels[i], cycles / CLOCK_HZ * 1000.0 / pf))


if __name__ == "__main__":
    main()
