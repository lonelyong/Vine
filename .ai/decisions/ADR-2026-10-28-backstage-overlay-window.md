# ADR-2026-10-28: the Backstage is a top-level tool window, not a SARibbon child overlay

- Status: Accepted
- Date: 2026-10-28
- Related: `../design/appfw-backstage.md`, `../bugs/docking-flyout-under-render.md`, `tests/test_gui/test_gui.cpp`

## Background

The application button needs a whole-window page like the Office Backstage. SARibbon provides two parts:

- `SARibbonApplicationButton`: the button itself;
- `SARibbonApplicationWidget`: a `QFrame` shell that follows the size of the `SARibbonMainWindow` and hides on Esc, with no page, navigation or content.

Vine's main window has a native 3D surface from `createWindowContainer` in the centre. The flyout fix already proved that **a Qt child widget can never cover a native child window**, and that `WA_NativeWindow` plus raise disturbs the render surface (see `../bugs/docking-flyout-under-render.md`).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Use `SARibbonApplicationWidget` directly as a child of the main window | The least code; Esc and resize come for free | The document area is covered by the native 3D surface; either accept a missing piece or hide and rebuild the render surface while it is open |
| B. A top-level `Qt::Tool | FramelessWindowHint` window (parent = the main window) whose geometry follows the main window's client area (chosen) | Not covered by the native surface; the same route as the flyout fix; no global topmost | The window has to synchronise move/resize/minimize/restore, activation and Esc itself |
| C. A `WA_NativeWindow` child window plus raise | Looks like correct overlap | The flyout measurement showed it disturbs the render surface, leaving the central area invisible afterwards; do not go this way again |
| D. Hide the `RenderControl` container while open and show it again on close | The child-widget option becomes workable | Hiding and rebuilding the surface costs a flicker and a session; the Backstage should not touch the render lifetime |

## Decision

Chose B:

- the `Backstage`'s native widget is a **top-level tool window whose parent is the main window** (not a child of it); `Qt::Tool`'s transient relation keeps it above the main window without pressing down other applications the way `WindowStaysOnTopHint` would;
- SARibbon provides only the application button and its click signal; the Backstage implements geometry following, Esc and the back button itself;
- the page and item model comes from appfw, and items reuse `RibbonAction` (the command name is the identity).

## Consequences

- Good:
  - the Backstage is never covered by the 3D or native child windows;
  - the render surface lifetime is not touched;
  - SARibbon is wrapped only thinly, and the implicit parent-child semantics of its Backstage shell are not relied on.
- Bad / cost:
  - the window geometry and activation are maintained here;
  - it covers the client area and not the system title bar (a small visual difference from the real Office Backstage);
  - multi-monitor or fractional scaling can show a 1 px rounding difference from the platform.
- Trigger (when to reopen):
  - the system title bar must be covered and the page fully integrated with the main window (then change the upstream `SARibbonApplicationWidget` instead of stacking another layer);
  - several windows are needed, each with its own Backstage.

## Evidence

- The flyout fix: `.ai/bugs/docking-flyout-under-render.md` (a native child window is always on top; a top-level tool window is the workable direction).
- Cases: `GuiTest.Backstage_ApplicationButtonOpensThePage`, `Backstage_PagesAndNavigation`,
  `Backstage_PageItemsAreCommandBound`, `Backstage_FollowsMainWindowSize`, `Backstage_ClosesOnEscapeOrClose`,
  `Backstage_IsOwnedByMainWindow`.
- Code: `src/fw/appfw/src/gui/Backstage.cpp` (top-level window plus event filter),
  `src/fw/appfw/src/gui/RibbonBar.cpp` (events and the menu/Backstage either-or).
