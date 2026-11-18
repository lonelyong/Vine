# Bug: an auto-hide flyout is covered by the central render area (Historical record)

- **Date**: 2026-09-02
- **Module**: third_party/DockingPanes (DockingPaneFlyoutWidget)
- **Status**: Fixed

## Symptom

After a panel is pinned to auto-hide, clicking the edge button expands it temporarily, and the part of the flyout that
overlaps the central vsg render area is covered by the render area (it looks like a z-order problem).

## Root cause

The central render area (`RenderControl`) is a native `QWindow` embedded with `QWidget::createWindowContainer` (the Vulkan
surface is a child HWND). On Windows a **native child window always paints above its parent's own drawing**. The
`DockingPaneFlyoutWidget` was an ordinary child widget with no window handle, and `raise()` has no effect on native
siblings, so it was covered as soon as it overlapped the render area.

## Fix (the final design: a top-level tool window)

1. The flyout became a **top-level frameless tool window** with
   `setWindowFlags(Qt::Tool | Qt::FramelessWindowHint)` - it floats above the main window (and all of its native children)
   by construction and does not disturb the render surface at all.
2. The geometry `setPositionAndSize()` computes changed from parent-local coordinates to **screen coordinates**
   (`parentWidget()->rect()` through `mapToGlobal`), otherwise a top-level window lands in the wrong place.
3. `openFlyout()` (DockingPaneContainer / DockingPaneTabbedContainer) calls `raise()` after `show()`.

## Detours not to take again

- `Qt::WA_NativeWindow` was tried (making the flyout a native child window and raising it): the side effects were large -
  the central render area became invisible after the flyout was closed or restored (the native child disturbed the render
  surface and the sibling hierarchy). Conclusion: do not stack a native child window as a sibling of a
  `createWindowContainer` render surface.

## Verification

- After pinning, clicking the auto-hide button shows the flyout above the render area;
- the central render area still displays correctly after the flyout is closed or restored;
- the flyout is anchored at the right edge and can be dragged and resized.
