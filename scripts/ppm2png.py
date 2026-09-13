#!/usr/bin/env python3
"""ppm2png.py — convert a P6 PPM into a PNG using only the Python standard library (zlib/struct).

The PPM this was written for came from the (since removed) vsg_color_probe's VINE_PROBE_CAPTURE;
it is a plain P6 reader, so any PPM writer can feed it.

Usage: python3 scripts/ppm2png.py in.ppm out.png
"""
import struct
import sys
import zlib


def load_ppm(path):
    data = open(path, "rb").read()
    pos = 0

    def tok():
        nonlocal pos
        while data[pos] in b" \t\r\n":
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos] not in b"\n":
                pos += 1
            return tok()
        start = pos
        while data[pos] not in b" \t\r\n":
            pos += 1
        out = data[start:pos]
        pos += 1
        return out

    assert tok() == b"P6"
    width = int(tok())
    height = int(tok())
    int(tok())  # maxval
    return width, height, data[pos:]


def write_png(path, width, height, rgb):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    width, height, rgb = load_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, rgb)
    print(f"wrote {sys.argv[2]} ({width}x{height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
