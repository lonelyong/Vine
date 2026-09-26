#!/usr/bin/env python3
"""xwinmap.py — 列出根的顶层窗口：名字 / 尺寸 / map_state。启动期验收用（X11）。

为什么需要它：**"启动期屏幕上只有启动框"** 这类断言只能问 X 服务端。
窗口在 Qt 里 `isVisible()==false` 不等于它不存在（主窗照样是根子窗），所以判据是
`XGetWindowAttributes::map_state`（2 = IsViewable）——启动期应当只有启动框是 IsViewable，
主窗（已创建、尺寸已定）是 IsUnmapped。

用法（应用在跑的时候读两次：启动期一次，启动结束后一次）：

    python3 scripts/xwinmap.py            # 当前
    sleep 2; python3 scripts/xwinmap.py   # 启动结束后

注意：`XQueryTree` 的**子窗顺序不代表 z 序**（WSLg 上实测过，见 `.ai/design/appfw-startup-splash.md`），
所以这里只用"在不在 + 什么状态"，不推断谁盖着谁。

手写结构体偏移是个坑：`XWindowAttributes` 按 64 位布局 **map_state 在偏移 92**，
不是想当然的 100（读错会让所有窗口都长得像 IsUnmapped）。布局依据：
x,y,width,height,border_width,depth = 6×int → 24；Visual* → 24；Window root → 32；
class,bit_gravity,win_gravity,backing_store = 4×int → 40..55；backing_planes/backing_pixel
= 2×unsigned long → 56/64；Bool save_under → 72；(对齐) Colormap → 80；Bool map_installed → 88；
int map_state → 92。
"""
import ctypes
import sys

lib = ctypes.CDLL("libX11.so.6")
Window = ctypes.c_ulong

lib.XOpenDisplay.restype = ctypes.c_void_p
lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
lib.XDefaultRootWindow.restype = Window
lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
lib.XQueryTree.argtypes = [ctypes.c_void_p, Window, ctypes.POINTER(Window), ctypes.POINTER(Window),
                           ctypes.POINTER(ctypes.POINTER(Window)), ctypes.POINTER(ctypes.c_uint)]
lib.XFetchName.argtypes = [ctypes.c_void_p, Window, ctypes.POINTER(ctypes.c_char_p)]
lib.XGetWindowAttributes.argtypes = [ctypes.c_void_p, Window, ctypes.c_void_p]
lib.XFree.argtypes = [ctypes.c_void_p]

MAP_STATE_OFFSET = 92
STATE_NAME = {0: "IsUnmapped", 1: "IsUnviewable", 2: "IsViewable"}


def main() -> int:
    display = sys.argv[1].encode() if len(sys.argv) > 1 else None
    dpy = lib.XOpenDisplay(display)
    if not dpy:
        print("xwinmap: cannot open the display", file=sys.stderr)
        return 2

    root = lib.XDefaultRootWindow(dpy)
    root_return = Window()
    parent_return = Window()
    children = ctypes.POINTER(Window)()
    count = ctypes.c_uint()
    if not lib.XQueryTree(dpy, root, ctypes.byref(root_return), ctypes.byref(parent_return),
                          ctypes.byref(children), ctypes.byref(count)):
        print("xwinmap: XQueryTree failed", file=sys.stderr)
        return 2

    attrs = ctypes.create_string_buffer(256)
    for i in range(count.value):
        window = children[i]
        title = ""
        name = ctypes.c_char_p()
        if lib.XFetchName(dpy, window, ctypes.byref(name)) and name.value:
            title = name.value.decode("utf-8", "replace")
            lib.XFree(name)

        ok = lib.XGetWindowAttributes(dpy, window, attrs)
        if not ok:
            print(f"0x{window:x} (attributes unavailable)")
            continue

        width = ctypes.c_int.from_buffer(attrs, 8).value
        height = ctypes.c_int.from_buffer(attrs, 12).value
        state = ctypes.c_int.from_buffer(attrs, MAP_STATE_OFFSET).value
        print(f"0x{window:x} {width}x{height} {STATE_NAME.get(state, '?'):<12} '{title}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
