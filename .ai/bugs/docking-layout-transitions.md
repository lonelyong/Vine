# Bug: a set of docked-layout transition defects (floating, docking, tab groups) (Historical record)

- **Date**: 2026-09-11
- **Module**: third_party/DockingPanes + src/fw/appfw (DockPanel / DockPanelManager)
- **Status**: Fixed (the drag-and-drop hit paths have no automated case, see "Verification")

## Symptom

1. A floating pane could be dragged down to a few pixels and could no longer be grabbed; a fast fling could throw the
   window off screen, after which it could not be dragged back.
2. When a tab was dragged out of a tab group, the pane stopped following the mouse while the button was still held, and
   the drop indicator no longer appeared.
3. For a floating window dragged out of a tab strip, the cursor's position relative to the title bar varied with the tab
   index, so the window jumped out from under the cursor when dragging.
4. After a panel was dragged to the other side, `setFloating(false)` / `restore()` docked it back on the side it had at
   **creation** time.
5. After `removeDockPanel()` the process occasionally crashed or read a released wrapper object.

## Root cause

- **Geometry rules written per path**: four paths created floating panes (title bar, tab strip, flyout, public API), and
  none of them constrained size or position.
- **Drag ownership was never handed over**: a tab drag-out is started by `DockingPaneTabbedContainer`, which holds the
  mouse grab, while dragging out re-arranges and may destroy the tab group, so the grab was lost mid-drag; the old code
  corrected the position with `move()`, which hid the symptom.
- **Drop offsets built from magic numbers**: `floatPane(QPoint(-deltaPos.x(), -10))` used the local X inside the tab label
  as the offset and hard-coded the title-bar height as `10` (it is really `6 + fontMetrics().height()`).
- **Two sources of truth**: `DockPanel` cached the dock area in the `_vn_dockarea` property (written only by
  `addDockPanel()`), while `dockArea()` had long since become a live query, so a stale cache docked the panel on the wrong
  side.
- **Double ownership**: the wrapper object and the `DockingPaneContainer` both considered themselves owners, and
  `userData()` was still used by the library's close callback after the wrapper object had been destroyed.

## Fix

Library side (`third_party/DockingPanes`, see section 5 of its `BUGS.md`):

| Item | Fix |
|---|---|
| Size and bounds | New `inc/DockingPaneGeometry.h`: a 120x80 floating floor and at least 24 px of the title bar kept inside the available screen; applied uniformly at `floatPane(QRect)`, `dockPane()` and `floatingPaneEndMove()` |
| Drag hand-off | On a tab drag-out `m_draggedPane->continueDrag(grabGlobal)` hands the drag to the pane's title bar (`takeGrab()`); the container clears its own drag state and calls `releaseMouse()` only while it still holds the grab |
| Drop offset | `floatPane(QPoint(0, 0))` (the argument is an offset) plus the new `titleHeight()`, positioned at `grabGlobal - QPoint(0, titleHeight() / 2)` |
| Empty tab group | Null-check before reading the tab name and clamp the index with `qBound`; an empty group calls `hide()` and `deletePane(this)` |
| Exception in an event handler | `default:` no longer throws; it logs `qWarning()` and skips docking, and calls `dockPane()` only when a target was hit (`m_targetPosition >= 0`) |
| Hover test | `DockAutoHideButton::onTimerElapsed` uses `underMouse()` instead of `QCursor::pos()` (Wayland returns (0,0)) |
| Auto-hide flyout geometry (**reverted**) | Restored on 2026-09-11 at the user's request: `DockingPaneFlyoutWidget.{h,cpp}` match upstream again (`setPositionAndSize()` is still "move, then resize, plus the 5px/9px fit"), `openFlyout`'s host is still `m_thisWidget` and `keepTitleBarReachable()` keeps two parameters. Conclusion: the position itself stays; if it is ever handled, the direction is to make the flyout a window the window manager does not manage (X11 override-redirect) instead of changing the fit baseline |

Framework side (`src/fw/appfw`):

| Item | Fix |
|---|---|
| Area of record | Before leaving the dock tree (`setFloating(true)` / `pin()` / `collapse()`) `dockPositionOf()` records the live position; docking back restores it through `dockPositionFor(rememberedDockArea(...))` |
| Ownership | `removeDockPanel()`: after `closePane()` decide from `state() != Hidden` whether a close callback really vetoed it; on success `setUserData(nullptr)`, `setOwnsImpl(false)`, `deletePane()` and finally `delete panel` |
| Type safety | `static_cast<QWidget*>(impl())` became `qobject_cast<QWidget*>()` (an impl that is not a QWidget is no longer treated as one) |
| Interface semantics | `setContent()` now states that the pane owns the content it shows (the old client is `deleteLater()`ed by the pane); Doxygen added to the state methods |

## Verification

- `tests/test_gui/test_gui.cpp` gained five cases: `DockPanel_FloatRoundTripKeepsArea`, `DockPanel_TabDetachCollapsesGroup`,
  `DockPanelManager_RemoveDockPanel`, `DockGeometryTest.FloatingMinimum` and `DockGeometryTest.TitleBarStaysReachable`.
- Plus `DockPanel_PinnedFlyoutIsAnchoredToDockArea`: after auto-hiding each of the four edges and clicking the edge button,
  the case asserts that the flyout's edge coordinates agree with the dock area's geometry, including the case where the
  content's minimum size exceeds the size the flyout asked for.
- All 158 `test_gui` cases pass; `test_core` / `test_vsg` / `test_async` / `test_progress` / `test_math` / `test_logging`
  pass as well.
- ASan: `VN_ASAN_FILTER='GuiTest.DockPanel*:GuiTest.DockPanelManager*:DockGeometryTest.*' scripts/asan_check.sh` -> PASS.
- A strict warning scan (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`) adds no warning on the
  changed lines.

## Left undone / deliberately out of scope

- **The drag-and-drop hit paths of tab drag-out and drag-in, and flyout drag-out**: they need a real mouse grab and a
  cooperating window manager, so there is no automated case; this round verified the code paths by reading them (the title
  bar emits global coordinates and the call sites of `floatingPaneStartMove` / `floatingPaneEndMove` are unchanged).
- **The flyout being placed by the window manager when it maps**: a headless environment cannot cover it. The 2026-09-11
  round (a correction after mapping plus a reference frame changed to the whole dock widget) was reverted completely;
  `GuiTest.DockPanel_PinnedFlyoutIsAnchoredToDockArea` still pins the library's original fit (5 px inset, the far edge
  gives up 9 px for the edge strip).
- A floating pane still uses `Qt::ToolTip | Qt::FramelessWindowHint` (switching to `Qt::Tool` adds a taskbar entry and
  window-manager decoration, which the whole drag and indicator machinery would have to support).
- The 1 px jitter under fractional scaling (a Qt rounding property, outside this library's scope).
- `DockingPaneFlyoutWidget::updateCursor()` and the `QCursor::pos()` inside `DockingPaneGlowWidget` (there is no event
  object to rely on, and the host already forces xcb).
- Left in the library: the hit order of tab children depends on list order (`updateFloatingPane`), and `DockFeatures`
  lacks the Floatable / Movable feature bits (see section 3 of `third_party/DockingPanes/BUGS.md`).
