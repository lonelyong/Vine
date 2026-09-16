#!/usr/bin/env python3
"""xwin2ppm.py — read an X window's pixels and report what is actually in them.

Usage:
    xwin2ppm.py <window-id | name substring> [out.ppm]

WHY IT EXISTS. The app gate (scripts/gfx_lavapipe_check.sh) judges the GUI app by its stderr evidence and by
"no validation error" — and a run that draws NOTHING satisfies both just as well. On 2026-09-16 a black
render area passed every gate until this tool read the window's pixels (0.84% non-black against 83.81% for
the same scene when it worked). The self-test has its own pixel assertions; the app had none.

WHICH WINDOW. The backend logs the window it draws into ("[VsgHostWindow] attached to the host window
0x60004a (378x247, mapped=true)") and that id is the one to read: a Qt container keeps the RENDER AREA as a
child of the named top-level window, so reading the top-level measures Qt's own chrome, which stays bright
even when the render area is black — i.e. the case this tool exists for would stay hidden. A name substring
is resolved to the DEEPEST window whose name contains it (normally the top-level); the viewable children of
that window are listed on stderr, so the render area can be named by id.

HOW IT CAPTURES. libX11 directly through ctypes (XGetImage), so the only requirement is a reachable X
server. It used to shell out to `xwd` and decode the file it wrote, which made the app gate depend on
x11-apps — not part of a default install (measured 2026-09-16 on a WSLg box that lacks it and whose sudo
needs a password, i.e. the gate would have skipped exactly where it was needed). `xwd -root` also fails
under XWayland (BadMatch) and `xwd -out -` (stdout) is not supported. Convert the PPM with
scripts/ppm2png.py to look at the picture.

WHAT IT PRINTS. Size and depth, the share of pixels that are not near-black, the mean colour and the most
frequent colours: enough to tell "the picture is there" from "the window is black" without opening the image.
"""

import collections
import ctypes
import ctypes.util
import itertools
import re
import sys

NEAR_BLACK = 8  # a pixel counts as content when its brightest channel passes this

ALL_PLANES = 0xFFFFFFFFFFFFFFFF  # ~0UL: every plane of the drawable
Z_PIXMAP = 2  # XImage format: the pixels as the server stores them
LSB_FIRST = 0  # XImage byte order
IS_VIEWABLE = 2  # MapState of a window that is on screen (and therefore drawn into)

_LIB = None


class XImage(ctypes.Structure):
    """The leading fields of Xlib's XImage, up to the colour masks: everything a capture needs to decode."""

    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int),
        ("data", ctypes.c_void_p),
        ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
    ]


class XWindowAttributes(ctypes.Structure):
    """Xlib's XWindowAttributes as the server lays it out (size, depth and map state are what is used here)."""

    _fields_ = [
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong),
        ("class_", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


def x11():
    """Return libX11 with every entry point this tool calls declared: restype/argtypes matter on 64-bit."""
    global _LIB
    if _LIB is None:
        found = ctypes.util.find_library("X11")
        if found is None:
            sys.exit("xwin2ppm: libX11 not found — the pixels are read through XGetImage")
        _LIB = ctypes.CDLL(found)
        _LIB.XOpenDisplay.restype = ctypes.c_void_p
        _LIB.XOpenDisplay.argtypes = [ctypes.c_char_p]
        _LIB.XDefaultRootWindow.restype = ctypes.c_ulong
        _LIB.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        _LIB.XQueryTree.restype = ctypes.c_int
        _LIB.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                                    ctypes.POINTER(ctypes.c_uint)]
        _LIB.XFetchName.restype = ctypes.c_int
        _LIB.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
        _LIB.XFree.argtypes = [ctypes.c_void_p]
        _LIB.XGetWindowAttributes.restype = ctypes.c_int
        _LIB.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(XWindowAttributes)]
        _LIB.XGetImage.restype = ctypes.c_void_p
        _LIB.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                   ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
        _LIB.XDestroyImage.argtypes = [ctypes.c_void_p]
    return _LIB


def open_display():
    """Open the display DISPLAY names: the pixels are only reachable through it."""
    display = x11().XOpenDisplay(None)
    if not display:
        sys.exit("xwin2ppm: cannot open the display (is DISPLAY set?)")
    return display


def children(display, window: int):
    """Return the direct children of @p window (empty when the server will not answer for it)."""
    root, parent = ctypes.c_ulong(), ctypes.c_ulong()
    kids = ctypes.POINTER(ctypes.c_ulong)()
    count = ctypes.c_uint()
    if not x11().XQueryTree(display, window, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids),
                            ctypes.byref(count)):
        return []
    out = [kids[i] for i in range(count.value)]
    if kids:
        x11().XFree(kids)
    return out


def window_name(display, window: int) -> str:
    """Return @p window's WM_NAME, or "" when it has none."""
    name = ctypes.c_char_p()
    if x11().XFetchName(display, window, ctypes.byref(name)) and name.value:
        return name.value.decode("utf-8", "replace")
    if name:
        x11().XFree(name)
    return ""


def window_attributes(display, window: int):
    """Return @p window's attributes, or None when the window is gone."""
    attrs = XWindowAttributes()
    if not x11().XGetWindowAttributes(display, window, ctypes.byref(attrs)):
        return None
    return attrs


def find_window(display, token: str) -> int:
    """Return the window @p token names: an id verbatim, else the deepest window whose name contains it."""
    if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", token):
        return int(token, 0)
    deepest = None
    stack = [(x11().XDefaultRootWindow(display), 0)]
    while stack:
        window, depth = stack.pop()
        if token in window_name(display, window):
            attrs = window_attributes(display, window)
            if attrs is not None and (deepest is None or depth > deepest[0]):
                deepest = (depth, window, attrs.width, attrs.height)
        stack.extend((child, depth + 1) for child in children(display, window))
    if deepest is None:
        sys.exit(f'xwin2ppm: no window whose name contains "{token}"')
    print(f"# window 0x{deepest[1]:x} ({deepest[2]}x{deepest[3]})", file=sys.stderr)
    # Say which viewable children the named window has: the render area is usually one of them, and naming
    # it by id is the only unambiguous way to read it (the name belongs to the container).
    for child in children(display, deepest[1]):
        attrs = window_attributes(display, child)
        if attrs is not None and attrs.map_state == IS_VIEWABLE:
            print(f"#   child 0x{child:x} ({attrs.width}x{attrs.height})", file=sys.stderr)
    return deepest[1]


def capture(display, window_id: int):
    """Read @p window_id's pixels: return (width, height, depth, bpp, rgb bytes, colour histogram)."""
    attrs = window_attributes(display, window_id)
    if attrs is None:
        sys.exit(f"xwin2ppm: cannot read window 0x{window_id:x} (is it still there?)")
    if attrs.map_state != IS_VIEWABLE:
        sys.exit(f"xwin2ppm: window 0x{window_id:x} is not viewable (nothing is drawn into an unmapped window)")
    if attrs.width == 0 or attrs.height == 0:
        sys.exit("xwin2ppm: the window reports no size (is it mapped?)")

    image_ptr = x11().XGetImage(display, window_id, 0, 0, attrs.width, attrs.height, ALL_PLANES, Z_PIXMAP)
    if not image_ptr:
        sys.exit(f"xwin2ppm: XGetImage failed for window 0x{window_id:x}")
    image = ctypes.cast(image_ptr, ctypes.POINTER(XImage)).contents
    # Take everything needed out of the image BEFORE destroying it: XDestroyImage frees the struct too.
    width, height = image.width, image.height
    depth, bpp, stride = image.depth, image.bits_per_pixel, image.bytes_per_line
    shifts = (shift(image.red_mask), shift(image.green_mask), shift(image.blue_mask))
    widths = (bits(image.red_mask), bits(image.green_mask), bits(image.blue_mask))
    data = ctypes.string_at(image.data, stride * height)
    byte_order = image.byte_order
    x11().XDestroyImage(image_ptr)

    step = bpp // 8
    if step == 4 and widths == (8, 8, 8):
        # The ordinary 32-bit truecolour case: slice each row's channels out in C instead of per pixel.
        red, green, blue = bytearray(), bytearray(), bytearray()
        for y in range(height):
            row = data[y * stride : y * stride + width * step]
            red += row[shifts[0] // 8 :: step]
            green += row[shifts[1] // 8 :: step]
            blue += row[shifts[2] // 8 :: step]
        histogram = collections.Counter(zip(red, green, blue))
        return width, height, depth, bpp, bytes(itertools.chain.from_iterable(zip(red, green, blue))), histogram

    # Anything else (16-bit, 24-bit packed, unusual masks): decode pixel by pixel, which always works.
    pixels = bytearray()
    histogram = collections.Counter()
    order = "little" if byte_order == LSB_FIRST else "big"
    for y in range(height):
        start = y * stride
        for x in range(width):
            value = int.from_bytes(data[start + x * step : start + x * step + step], order)
            rgb = tuple(((value >> shifts[i]) & ((1 << widths[i]) - 1)) * 255 // ((1 << widths[i]) - 1) for i in range(3))
            pixels += bytes(rgb)
            histogram[rgb] += 1
    return width, height, depth, bpp, bytes(pixels), histogram


def shift(mask: int) -> int:
    """Return the number of bits @p mask's channel starts at."""
    return (mask & -mask).bit_length() - 1


def bits(mask: int) -> int:
    """Return how many bits wide @p mask's channel is."""
    return bin(mask).count("1")


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    display = open_display()
    window_id = find_window(display, sys.argv[1])
    out_ppm = sys.argv[2] if len(sys.argv) > 2 else "/tmp/xwin2ppm.ppm"

    width, height, depth, bpp, pixels, histogram = capture(display, window_id)

    total = width * height
    content = sum(count for rgb, count in histogram.items() if max(rgb) > NEAR_BLACK)
    mean = tuple(sum(rgb[i] * count for rgb, count in histogram.items()) // total for i in range(3))

    with open(out_ppm, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (width, height))
        fh.write(bytes(pixels))

    print(f"window 0x{window_id:x}: {width}x{height} depth={depth} bpp={bpp}")
    print(f"content (not near-black): {content}/{total} = {100.0 * content / total:.2f}%")
    print(f"mean colour: {mean}")
    print("most frequent colours:")
    for rgb, count in histogram.most_common(6):
        print(f"  {rgb} x{count}")
    print(f"ppm: {out_ppm}  (png: python3 scripts/ppm2png.py {out_ppm} {out_ppm[:-4]}.png)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
