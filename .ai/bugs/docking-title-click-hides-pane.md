# Bug: clicking a docked panel's title bar makes the panel invisible (its space stays, a window resize brings it back)

- **Date**: 2026-10-28
- **Module**: third_party/DockingPanes (DockingPaneGeometry / DockingPaneManager / DockingPaneTitleWidget)
- **Status**: Fixed

## Symptom

- A single click (no drag) on any docked panel's title bar made the panel content - the whole panel - invisible, while the
  dock area still reserved its space; resizing the main window once brought the panel back where it was.
- Trigger: certain on a **secondary screen**, or when the window is larger than the screen; not visible while the window
  lies entirely inside the primary screen at `(0,0)`.

## Root cause

Two layers on top of each other, both needed:

1. The "released without hitting a dock target" branch of `DockingPaneManager::floatingPaneEndMove()` tested only the
   **type** (`qobject_cast<DockingPaneContainer*>`) and not the **state**: on a title-bar click the pane is still `Docked`,
   yet it still called `DockingPaneGeometry::keepTitleBarReachable()`. A docked pane is a **child widget** inside a
   `QSplitter` and its `frameGeometry()` is in parent-local coordinates, but that function clamps with a **global screen
   rectangle** and then calls `pane->move()`. When `screen()->availableGeometry()`'s `left/top` is not 0 (secondary
   screen), the local coordinates are clamped as if they were screen coordinates, the pane is moved out of its splitter
   cell - invisible, with the cell's space still reserved - and the next layout pass (a window resize) puts it back.
2. A pure click ran the whole drag-and-drop flow as well: the press had already called `floatingPaneStartMove()`, and on
   release `updateFloatingPane()` showed and then hid the dock indicator cluster (a flicker) and could even re-dock the pane
   when the release happened to land on a sticker.

Measured (the case enlarges the window beyond the screen, which is equivalent to a secondary screen or negative
coordinates): the right panel's geometry was changed from `x=1186` to `x=775` (= `available.right() - 24`).

## Fix

- `DockingPaneGeometry::keepTitleBarReachable()` applies to **windows** (floating panes) only: `!pane->isWindow()` returns
  `false` immediately, and the documentation states why a child widget must not be moved (local versus screen coordinates).
- `DockingPaneManager::floatingPaneEndMove()` applies the rule only when `state() == DockingPaneBase::Floating`.
- `DockingPaneTitleWidget` records whether a move happened after the press; **a pure click no longer emits
  `titleBarEndMove`** and therefore does not scan for a drop target. The drag hand-off `takeGrab()` (tab drag-out, flyout
  drag-out to floating) sets the flag, so real drags behave as before.

## Files involved

- `third_party/DockingPanes/inc/DockingPaneGeometry.h`
- `third_party/DockingPanes/inc/DockingPaneTitleWidget.h`
- `third_party/DockingPanes/src/DockingPaneTitleWidget.cpp`
- `third_party/DockingPanes/src/DockingPaneManager.cpp`
- `third_party/DockingPanes/BUGS.md` (section 13, BUG-21)

## Verification

- `GuiTest.DockPanel_TitleBarClickDoesNotChangeLayout`: with the main window enlarged beyond the screen, clicking the title
  bars of the left, right and top panels in turn asserts that parent, geometry, visibility and dock area are unchanged (the
  right panel's geometry assertion failed before the fix).
- `DockGeometryTest.KeepTitleBarReachableDoesNotMoveChildWidgets`: a child widget must not be moved even when its
  coordinates lie off screen.
- `test_gui` 278 cases (277 pass plus one pre-existing skip).

## Lessons

A screen rectangle can only be compared with **window** coordinates; any geometry rule that "can apply to any pane" must
first ask whether the pane is a window or a child widget in a layout. The combination of `move()`, `mapToGlobal()` and
`frameGeometry()` is the easiest place to mix the two coordinate systems up.
