# ADR-2026-10-30: the Linux crash window uses Xlib + Xft, not a UI toolkit

- Status: Accepted (it **superseded** the Qt-based crash window the same day; git holds that record)
- Date: 2026-10-30
- Related: `../design/base-crash.md` (section 2.1 boundary, 2.9 capture, 3.4 window, 6.5 degradation, 9 status)

## Background

The earlier ADR that same day had the Linux crash window use Qt Widgets, arguing that "the window is the handler's whole output, and the `base` layer cannot draw a window with the C library alone".
It did run, but it replaced a boundary that had been clean: the `base` layer where `vn::Crash` lives gained a UI toolkit dependency for the first time.

Ask again what the window actually needs: a run of text with line breaks, a scrollable area, four buttons, one clipboard write and two `xdg-open` calls.
None of that needs a toolkit - it needs **font rendering** and an **event loop**, and the X11 platform brings both.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Qt Widgets (the earlier decision) | Written once, mature rendering, automatic CJK font fallback | A UI toolkit dependency on the `base` layer (G2); the handler's dependency surface stops being "system libraries" |
| B. Xlib + Xft (chosen) | System libraries only: Xlib draws the window and handles events, Xft does the text and font matching (fontconfig finds a CJK font); the clipboard uses X selections, opening a directory or a mail client uses `xdg-open` | Layout, scrolling, hit testing and the clipboard service have to be written here (about 500 lines); no theming and no rich text; the build needs `libx11-dev` + `libxft-dev` |
| C. Xlib + X core font | Not even Xft | Core fonts are only things like `-misc-fixed-*-iso8859-1` and **cannot draw Chinese**; the window text would have to be English |
| D. GTK3 | Does not touch Qt and has no trouble with Chinese | Brings a dependency into the repository that has no other user (G8) |
| E. A terminal interface | Zero dependencies | A double-click start has no terminal and the user sees nothing (G4) |

## Decision

1. **The Linux window is drawn with Xlib and typed with Xft**: the window, the scroll area, the buttons and the clipboard are all implemented by the handler, which links no UI toolkit. The `base` layer's dependency returns to "`vn::Crash` plus system libraries" (the original boundary of section 2.1), and this ADR withdraws the G2 violation.
2. **The build needs `libx11-dev` + `libxft-dev`** (both development packages); at runtime it needs only `libXft.so.2` and one font fontconfig can match (the interface first asks for a family with `:lang=zh-cn` and falls back to a plain family).
3. **With no usable window the text is printed**: with no `DISPLAY`, when `XOpenDisplay` fails, or when the font or drawing context cannot be created, `showWindow` returns false and the entry point prints the whole report to stdout; the `.txt` is written before that, so no path loses the report.
4. CMake **does not build** `CrashHandler` when the X11/Xft development packages are missing, and warns: the library still captures, writes the report and writes the `.txt`, there is just no dialog.

## Consequences

- Good: the handler uses system libraries only, symmetric with "TaskDialog is a system library" on Windows; there is no longer a dependency that only a read ADR explains ("the `base` layer links Qt").
- Bad / cost: the window is hand-written - scrolling, hit testing and the clipboard protocol (`TARGETS`/`UTF8_STRING`/`STRING`) are maintained by this module; there is no theming and no trace of the desktop's style; a pure Wayland session without XWayland gets no window (it degrades to stdout).
- Bad / cost: whether Chinese appears depends on a CJK font being installed (Noto CJK on this machine); without one the glyphs become missing-glyph boxes - but the report text and the `.txt` are unaffected.
- Trigger (when to reopen): (1) selectable text, rich text or full CJK paragraph shaping is needed (Xft has no per-glyph fallback); (2) the target session is pure Wayland without XWayland; (3) maintaining the hand-written window costs more than a toolkit is worth.
