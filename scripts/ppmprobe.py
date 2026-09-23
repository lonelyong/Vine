#!/usr/bin/env python3
"""ppmprobe.py — what one RECTANGLE of a PPM holds.

Usage:
    ppmprobe.py <file.ppm> <x> <y> <width> <height>

Prints one line:
    rect <x>,<y> <w>x<h>: max <max-channel> mean <mean-channel>

WHY IT EXISTS. The gates judge pictures that already exist as files (scripts/xwin2ppm.py writes them), and the
questions they ask are about a REGION: "does the demo's G-buffer preview hold anything at all", "is the HUD's
corner dark", "how much did this band change". A whole-image statistic cannot answer those — a window that is
84% non-black still passes when the half of it that matters drew nothing (measured 2026-09-23: the deferred
chain stopped drawing on a resize and the sky alone kept the window bright).

WHAT "max" AND "mean" MEAN HERE. Both are channel values 0..255 over the rectangle's pixels, the MAXIMUM of
the three channels per pixel. A region whose attachment was CLEARED reads max 0 (the demo's extra G-buffer
attachments clear to transparent black), while anything drawn into it — a lit platform, a normal vector, a
plane of the sky — reads far above that, so "max > 0" is the cheapest honest question about "did a pixel of
content land here".

The rectangle is clamped to the image, so a probe aimed at a window that turned out smaller reports on what
is there instead of failing the caller's arithmetic; the printed size says what was actually read.
"""

import sys


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":  # a comment line: skip it, header fields may follow
            while data[pos : pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    width, height = int(fields[1]), int(fields[2])
    if fields[0] != b"P6":
        raise SystemExit(f"{path}: only binary PPM (P6) is read, got {fields[0].decode(errors='replace')}")
    return width, height, data[pos : pos + width * height * 3]


def main() -> int:
    if len(sys.argv) != 6:
        sys.exit("usage: ppmprobe.py <file.ppm> <x> <y> <width> <height>")
    path = sys.argv[1]
    want_x, want_y, want_w, want_h = (int(value) for value in sys.argv[2:6])
    width, height, pixels = read_ppm(path)

    x0, y0 = max(0, want_x), max(0, want_y)
    x1, y1 = min(width, want_x + want_w), min(height, want_y + want_h)
    if x1 <= x0 or y1 <= y0:
        sys.exit(f"{path}: the rectangle {want_x},{want_y} {want_w}x{want_h} is outside the {width}x{height} image")

    best, total, count = 0, 0, 0
    for y in range(y0, y1):
        row = y * width
        for x in range(x0, x1):
            index = (row + x) * 3
            best = max(best, pixels[index], pixels[index + 1], pixels[index + 2])
            total += pixels[index] + pixels[index + 1] + pixels[index + 2]
            count += 1
    print(f"rect {x0},{y0} {x1 - x0}x{y1 - y0}: max {best} mean {total / max(1, count * 3):.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
