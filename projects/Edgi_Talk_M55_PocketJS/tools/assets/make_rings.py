#!/usr/bin/env python3
"""Generate the gauge ring sprites used by the Ring component (ui/kit.tsx).

Arcs are expensive on the board: the core rasterises them into a variable number of spans every
frame, and a changed span count repaints the whole screen. Sprites keep the draw list stable, so
a gauge update only repaints the gauge. Output goes next to the app sources:

    ring_track.png / ring_track@2x.png      grey track
    ring_00 .. ring_20 (+ @2x)              teal fill for 0, 5, ... 100 percent

Each image is 32x32 logical (64x64 at 2x); the base PNG is only the 1x fallback.
"""
import math
import os
import sys

from PIL import Image, ImageDraw

APP = os.path.abspath(os.path.join(os.path.dirname(__file__), *[".."] * 5, "pocketjs", "apps", "edgitalk-m55-smoke"))
TRACK = (0xD5, 0xE0, 0xE4)
FILL = (0x1D, 0x79, 0x74)
SUPER = 8
LOGICAL = 32
WIDTH = 4.0  # logical pixels


def render(size, percent, color):
    """Ring of `size` px with a clockwise arc from 12 o'clock covering `percent`."""
    big = size * SUPER
    image = Image.new("RGBA", (big, big), color + (0,))
    if percent <= 0:
        return image.resize((size, size), Image.LANCZOS)
    draw = ImageDraw.Draw(image)
    scale = size / LOGICAL * SUPER
    outer = big / 2.0
    width = WIDTH * scale
    box = [0, 0, big - 1, big - 1]
    if percent >= 100:
        draw.ellipse(box, fill=color + (255,))
        inner = [width, width, big - 1 - width, big - 1 - width]
        draw.ellipse(inner, fill=color + (0,))
    else:
        sweep = 360.0 * percent / 100.0
        draw.arc(box, -90, -90 + sweep, fill=color + (255,), width=int(round(width)))
        mid = outer - width / 2.0
        for angle in (-90.0, -90.0 + sweep):
            rad = math.radians(angle)
            cx = outer + math.cos(rad) * mid
            cy = outer + math.sin(rad) * mid
            r = width / 2.0
            draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color + (255,))
    return image.resize((size, size), Image.LANCZOS)


def save(name, percent, color):
    for suffix, size in (("", LOGICAL), ("@2x", LOGICAL * 2)):
        render(size, percent, color).save(os.path.join(APP, "%s%s.png" % (name, suffix)))


def main():
    if not os.path.isdir(APP):
        sys.exit("app directory not found: %s" % APP)
    save("ring_track", 100, TRACK)
    for step in range(21):
        save("ring_%02d" % step, step * 5, FILL)
    print("wrote 22 ring sprites to", APP)


if __name__ == "__main__":
    main()
