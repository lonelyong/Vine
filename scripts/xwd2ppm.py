#!/usr/bin/env python3
"""xwd2ppm.py — read an X window's pixels and report what is actually in them.

Usage:
    xwd2ppm.py <window-id | name substring> [out.ppm]

WHY IT EXISTS. The app gate (scripts/gfx_lavapipe_check.sh) judges the GUI app by its stderr evidence and by
"no validation error" — and a run that draws NOTHING satisfies both just as well. On 2026-09-16 a black
render area passed every gate until this tool read the window's pixels (0.84% non-black against 83.81% for
the same scene when it worked). The self-test has its own pixel assertions; the app had none.

HOW IT CAPTURES. `xwd -id <id> -out <file>`: the window's own contents, not a screen grab. `xwd -root` does
not work under XWayland (BadMatch), and a window chosen by name is looked up in `xwininfo -root -tree`
(the deepest match wins, which is the render area of a Qt container). The XWD file is decoded here —
bits_per_pixel, bytes_per_line and the red/green/blue masks — because ImageMagick and netpbm are not
assumed to be installed. Convert the PPM with scripts/ppm2png.py to look at it.

WHAT IT PRINTS. Size and depth, the share of pixels that are not near-black, the mean colour and the most
frequent colours: enough to tell "the picture is there" from "the window is black" without opening the image.
"""

import collections
import re
import struct
import subprocess
import sys

NEAR_BLACK = 8  # a pixel counts as content when its brightest channel passes this


def find_window(token: str) -> str:
    """Return the id of the window @p token names: an id verbatim, else the deepest window whose name contains it."""
    if token.lower().startswith("0x"):
        return token
    tree = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True, text=True, check=True).stdout
    deepest = None
    for line in tree.splitlines():
        if f'"{token}"' not in line:
            continue
        fields = line.split()
        size = next((field for field in fields if re.fullmatch(r"\d+x\d+\+\d+\+\d+", field)), None)
        if size is None:
            continue
        # The DEPTH is what finds a render area: a Qt container's render area is a child of the widget's
        # window, and it is the child the backend draws into. Qt also keeps a 1x1 helper window with the same
        # name, which sits higher up and would win any "largest last" rule.
        depth = len(line) - len(line.lstrip())
        if deepest is None or depth > deepest[0]:
            deepest = (depth, fields[0], size.split("+")[0])
    if deepest is None:
        sys.exit(f'xwd2ppm: no window whose name contains "{token}" (try xwininfo -root -tree)')
    print(f"# window {deepest[1]} ({deepest[2]})", file=sys.stderr)
    return deepest[1]


def capture(window_id: str, xwd_path: str) -> None:
    """Write the window's contents to @p xwd_path (dumping to stdout is not supported by xwd)."""
    subprocess.run(["xwd", "-silent", "-id", window_id, "-out", xwd_path], check=True)


def decode(xwd_path: str):
    """Decode the XWD file: return (width, height, depth, bpp, rgb bytes, colour histogram)."""
    raw = open(xwd_path, "rb").read()
    u32 = lambda offset: struct.unpack_from(">I", raw, offset)[0]
    header_size = u32(0)
    width, height = u32(16), u32(20)
    depth, bpp, stride = u32(12), u32(44), u32(48)
    red_mask, green_mask, blue_mask = u32(56), u32(60), u32(64)
    if width == 0 or height == 0:
        sys.exit("xwd2ppm: the window reports no size (is it mapped?)")

    def shift(mask):
        return (mask & -mask).bit_length() - 1

    def bits(mask):
        return bin(mask).count("1")

    shifts = (shift(red_mask), shift(green_mask), shift(blue_mask))
    widths = (bits(red_mask), bits(green_mask), bits(blue_mask))
    step = bpp // 8
    pixels = bytearray()
    histogram = collections.Counter()
    for y in range(height):
        row = header_size + y * stride
        for x in range(width):
            value = int.from_bytes(raw[row + x * step : row + x * step + 4], sys.byteorder)
            rgb = tuple(((value >> shifts[i]) & ((1 << widths[i]) - 1)) * 255 // ((1 << widths[i]) - 1) for i in range(3))
            pixels += bytes(rgb)
            histogram[rgb] += 1
    return width, height, depth, bpp, pixels, histogram


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    window_id = find_window(sys.argv[1])
    out_ppm = sys.argv[2] if len(sys.argv) > 2 else "/tmp/xwd2ppm.ppm"
    xwd_path = out_ppm + ".xwd"

    capture(window_id, xwd_path)
    width, height, depth, bpp, pixels, histogram = decode(xwd_path)

    total = width * height
    content = sum(count for rgb, count in histogram.items() if max(rgb) > NEAR_BLACK)
    mean = tuple(sum(rgb[i] * count for rgb, count in histogram.items()) // total for i in range(3))

    with open(out_ppm, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (width, height))
        fh.write(bytes(pixels))

    print(f"window {window_id}: {width}x{height} depth={depth} bpp={bpp}")
    print(f"content (not near-black): {content}/{total} = {100.0 * content / total:.2f}%")
    print(f"mean colour: {mean}")
    print("most frequent colours:")
    for rgb, count in histogram.most_common(6):
        print(f"  {rgb} x{count}")
    print(f"ppm: {out_ppm}  (png: python3 scripts/ppm2png.py {out_ppm} {out_ppm[:-4]}.png)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
