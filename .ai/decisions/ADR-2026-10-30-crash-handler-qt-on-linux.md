# ADR-2026-10-30: the Linux crash window uses Qt Widgets

- Status: **Superseded** (on the same day by `ADR-2026-10-30-crash-handler-x11-on-linux.md`: the window can be drawn with Xlib + Xft alone, so the `base` layer need not bring in a UI toolkit for one dialog)
- Date: 2026-10-30
- Related: `../design/base-crash.md` (section 2.1 boundary, 2.9 capture, 3.4 window, 9 status) · `../backlog/base-crash.md`

## Background

`vn::Crash`'s handler follows a written boundary: **link only `vn::Crash` and system libraries, not Qt and not appfw** (`base-crash.md` section 2.1; the reason is in a comment in `src/base/crash/CMakeLists.txt`: a handler that has learned Qt or appfw precisely fails to start when those things have just died).

Once the Linux side had to gain capture and a window, that boundary met a fact: **the window is the handler's entire output**. On Windows the system provides that window (`TaskDialogIndirect`, shipped with comctl32); Linux has no equivalent - either bring in a window toolkit or give up the window. This was not "write one more file" but a trade-off about the boundary itself.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Qt Widgets (chosen) | The repository already uses Qt6 (`Vine` is itself a Qt application), so no new toolchain; a `QDialog` plus a `QPlainTextEdit` is all that is needed; consistent with how `src/apps` is built | **Violates G2**: an executable target on the `base` layer gains a Qt dependency; the handler needs Qt libraries and platform plugins at runtime; library-level gates such as `check_export_annotations` do not cover it, so this ADR is the record |
| B. GTK3 | Does not touch Qt, and desktop distributions usually ship it | Brings a dependency into the repository that has no user at all (G8); the styling differs greatly; both machines (developer and user) may lack `gtk3` |
| C. A terminal interface (report to stdout, `xdg-open` the directory) | Zero dependencies, works over ssh, fits the existing boundary exactly | A double-click start has no terminal and the user sees nothing - which is precisely why the window exists (G4) |
| D. Let appfw provide the window | Reuses the existing UI capability | The handler would then depend on appfw, the very thing it must not rely on when crashing (G2/G10, the original reason for this boundary) |

## Decision

1. **Only the single executable target `CrashHandler` links `Qt6::Widgets`, and only on Linux**; the `vn::Crash` library, the crash process and the Windows handler's dependencies are unchanged (`N1`: no Qt on the crash path).
2. Windows keeps the system TaskDialog (this ADR does not change it): do not install a dependency tree for the user when it can be avoided.
3. With no display (`DISPLAY`, `WAYLAND_DISPLAY` and `QT_QPA_PLATFORM` all unset) no window is created and the text goes to stdout; the `.txt` is written before the window opens, so no path loses the report.
4. The boundary violation is **recorded in exactly one place**: section 2.1 of the design page points at this ADR and the CMake comment points at this ADR; neither restates the reason.

## Consequences

- Good: Linux has a crash window equivalent to Windows, with no new toolchain; the handler stays an independent, healthy little process that only reads a report.
- Bad / cost: the `base` layer has a Qt dependency for the first time (though on one executable target only). Someone following the dependency will see `src/base/crash` linking Qt and has to read this ADR to learn the scope: **it does not spread to the library**.
- Bad / cost: deploying the handler means shipping Qt's Widgets and platform plugins (a distribution repository provides them; a self-built static environment is the trigger below).
- Trigger (when to reopen): (1) a distribution shape appears whose target machine has no Qt (static link, trimmed image); (2) another GTK user appears in the repository, making GTK a dependency already paid for; (3) the handler must stay resident (the Crashpad shape) and Qt's start-up cost becomes unacceptable.
