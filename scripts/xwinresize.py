#!/usr/bin/env python3
"""xwinresize.py — resize the TOP-LEVEL parent of an X window, like a user dragging its edge.

Usage:
    xwinresize.py <window-id> <width> <height>

WHY IT EXISTS. The gates judge the app by what it draws, and the one picture that matters most is the one
after a size change: the surface, the swapchain, the off-screen chain and every pass' viewport all move, and
a backend can get every one of them wrong while a still frame looks perfect (measured 2026-09-23: after the
first resize the deferred chain drew nothing and the window showed only the sky, and a gate that read a
single settled frame before any resize called that green). So something has to CHANGE THE SIZE from outside
the app, exactly as a window manager's drag would.

WHICH WINDOW IS RESIZED, AND WHY IT IS THE PARENT. The backend logs the window it draws into, and that
window is a CHILD: Qt puts the render area inside the host's top-level (the test suite and the app shell both
do this). Resizing the child directly would be undone by the parent's layout on the next event; resizing the
parent is what makes the app receive a resize it has to follow, which is the path being judged. The window is
found by walking the tree from the root, so an id that is already a top-level is resized through its own
parent only when the id names a child (a top-level id fails with a message rather than resizing the root).

WHAT IT DOES NOT DO. It does not wait for the app to have FOLLOWED the resize: the app is another process,
and how long it takes is what the caller's own settle window is for. It flushes and syncs, so the request is
at the server before it returns.
"""

import ctypes
import ctypes.util
import sys

lib = ctypes.CDLL(ctypes.util.find_library("X11"))
lib.XOpenDisplay.restype = ctypes.c_void_p
lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
lib.XDefaultRootWindow.restype = ctypes.c_ulong
lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
lib.XQueryTree.restype = ctypes.c_int
lib.XQueryTree.argtypes = [
    ctypes.c_void_p,
    ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
    ctypes.POINTER(ctypes.c_uint),
]
lib.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
lib.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.XFree.argtypes = [ctypes.c_void_p]


def main() -> int:
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip().splitlines()[3].strip())
    display = lib.XOpenDisplay(None)
    if not display:
        sys.exit("no X display: set DISPLAY (the app and this tool must be on the same one)")
    root = lib.XDefaultRootWindow(display)
    child = int(sys.argv[1], 0)
    width, height = int(sys.argv[2]), int(sys.argv[3])

    parent = None
    stack = [root]
    while stack:
        window = stack.pop()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        root_return, parent_return = ctypes.c_ulong(), ctypes.c_ulong()
        if not lib.XQueryTree(
            display, window, ctypes.byref(root_return), ctypes.byref(parent_return),
            ctypes.byref(children), ctypes.byref(count)
        ):
            continue
        kids = [children[i] for i in range(count.value)]
        if children:
            lib.XFree(children)
        if child in kids:
            parent = window
            break
        stack.extend(kids)

    if parent is None:
        sys.exit(f"0x{child:x} is not a child of any window on this display")
    if parent == root:
        sys.exit(f"0x{child:x} is a top-level: there is no parent to resize (the gate resizes the host's "
                 f"top-level around the render area it logs)")
    lib.XResizeWindow(display, parent, width, height)
    lib.XSync(display, 0)
    print(f"resized parent 0x{parent:x} of 0x{child:x} to {width}x{height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
