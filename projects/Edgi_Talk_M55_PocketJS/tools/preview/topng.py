#!/usr/bin/env python3
"""Convert binary PPM (P6) files to PNG using only the standard library."""
import struct
import sys
import zlib


def convert(source: str, target: str) -> None:
    with open(source, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    raw = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(target, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    for path in sys.argv[1:]:
        convert(path, path[:-4] + ".png")
