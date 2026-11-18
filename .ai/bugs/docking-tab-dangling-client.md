# Bug: dragging the console into a tab group crashes, or the console disappears (dangling `clientWidget`) (Historical record)

- **Date**: 2026-09-02
- **Module**: third_party/DockingPanes (DockingPaneContainer)
- **Status**: Fixed

## Symptom

Dragging the bottom console panel onto the left "Project" or the right "Properties" panel to form a tabbed group:

- The original build crashed inside `DockingPaneTabbedContainer::addPane` at `QStackedWidget::addWidget(client)`
  (`QObject::parent` read a dangling pointer, `DockingPaneTabbedContainer.cpp:170` / `DockingPaneManager.cpp:767`).
- With a `QPointer` fallback it no longer crashed, but the console panel disappeared entirely (`addPane` received a null
  client and skipped it silently, so the console never joined the tab).
- Grouping tabs with a right-click and then a left-click did not crash (the left and right panels have no content, so that
  path was not reached).

## Root cause

`DockingPaneManager::createPane(id, title, widget, ...)` constructs the `DockingPaneContainer` from the widget the caller
passed, and **`m_clientWidget` is assigned that widget once, in the constructor only**. The host (Vine's
`DockPanelManager::addDockPanel`) first builds a temporary `placeholder` as the client for a panel that has content and
then swaps in the real content with `setClientWidget(real content)` - but
**`DockingPaneContainer::setClientWidget()` only touches the layout and never updates the `m_clientWidget` member**. So:

1. `m_clientWidget` kept pointing at the original `placeholder`;
2. the `placeholder` was then deleted with `deleteLater()`, so `clientWidget()` returned a dangling pointer;
3. grouping a tab dereferenced it in `addPane` and crashed (the `QPointer` version nulls automatically instead, so the
   console vanished silently - hiding the defect rather than fixing it).

The left and right panels never replaced their content, so there the placeholder was the real content and was never
deleted, and the path was not reached. The console is the only panel that is created with content (`ConsolePanel`), so the
crash was certain.

## Fix

`DockingPaneContainer::setClientWidget()` (`DockingPaneContainer.cpp`) now synchronises the member with the widget it really
puts into the layout, after clearing the layout:

```cpp
m_clientWidget = widget;   // after the empty-layout cleanup, before the null early-return
```

`clientWidget()` therefore always answers with the content actually shown; no stale reference survives the placeholder's
deletion, and `addPane` / `openFlyout` (the auto-hide preview) both get the correct content.

## Verification

- Dragging the console onto the left or right panel to form a tab group: no crash, the console appears as a tab with its
  content.
- Grouping tabs with a right-click and then a left-click still works.
- The auto-hide (flyout) preview, which could previously show the placeholder or a dangling pointer, is fixed as well.

## Lessons

A pane container's client can be replaced after construction; any member that caches a "content widget" pointer in the
constructor must be updated by the setter that replaces the content, or it leaves a dangling pointer. When chasing a
related problem, look for members that are assigned once in the constructor while a "replace content" setter exists
(`focusProxy` in DockingPanes is the same shape and was handled in the same pass).
