# ADR-2026-10-28: document tabs live in the central area, and floating is a top-level window of its own (not part of the dock tree)

- Status: Accepted
- Date: 2026-10-28
- Related: `../design/appfw-document-model.md` (sections 1.2, 3.1.5, 5.3) ·
  `../decisions/ADR-2026-10-24-tabs-not-mdi.md`, `../decisions/ADR-2026-10-24-command-target.md` ·
  `../design/appfw-render-surface.md` (F3/C3 reparenting)

## Background

Multiple documents need three things: **several tabs in the central client area**, **a view that can float into its own window**, and **docking back only into the central area, never into any dock area**.

The vendored DockingPanes in this repository (with local patches) cannot express the first two:

- the central client area is a `DockingPaneClient`, and `setClientWidget()` installs exactly one widget;
- when a floating pane is dragged over the client area, `updateFloatingPane()` **explicitly turns the tab indicator off** (`setTabVisible(false)`);
- the neighbour of `dockPane(dockTab, ...)` must be a docked `DockingPaneContainer` / `DockingPaneTabbedContainer`, and the client area is neither;
- a `DockingPaneTabbedContainer` can only be created by the manager's internal `dockPane(dockTab, ...)`, and `createPane()` always creates a single pane;
- a cursor that loses its pane is docked to one of the four edges by `handleFrameEdgeHit()`, with **no whitelist of allowed targets**.

So "document tabs in the middle, docking back only into the middle" either has to be carried by the host or patched into the third-party code.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. The host owns its own tab strip and its own floating window (chosen) | "Cannot dock elsewhere" is a **structural** guarantee: a document is not a pane at all, so there is no target to hit; the third-party code is untouched | The float and dock-back gestures are written here (about one window class); the library's dock indicators are unavailable |
| B. Patch DockingPanes: make `TabbedContainer` the client area and whitelist the dock targets | Uses the library's tab UI and tab drag-out and drag-in | Three private hit tests change; documents would enter `saveLayout()`; an upstream merge silently kills the patch (failure mode = "it can dock to the left now") |

## Decision

1. **`CentralDocumentHost` owns a `QTabBar` plus a `QStackedWidget`**: one tab per open document (views are still built lazily), and no strip while there is only one.
2. **A document never enters the dock tree**: `setFloating()` creates a top-level `QWidget` owned by this host, not a `DockingPaneContainer`.
3. **Docking back has exactly three ways**: the floating window's "Dock" button, `setFloating(doc, false)`, and closing the document.
4. **`DocumentManager` is the only owner of tab selection and current**: selecting a tab calls `setCurrent()`, and `currentChanged` re-selects the tab.
5. **Closing a tab and closing a floating window both go through `DocumentManager::close()`**, and `canClose()`'s veto still applies.
6. **Focus follows the document**: activating a floating window makes it current, so a ribbon command in the main window lands on "the document last worked in" and the existing resolution of `resolveCommandTarget(window)` needs no change.
7. **A document is not layout**: `DockingPaneManager::saveLayout()` stores tool panels only; which documents are open belongs to the session and is separate.

## Consequences

- Good: one concept (a document is a tab or its own window), with no invisible rules about allowed and forbidden dock positions; DockingPanes stays untouched; a single-document application never sees a tab strip.
- Bad / cost: floating reparents the page widget => the native 3D window is rebuilt once (the framework supports it: swap the surface only, not the session, see `appfw-render-surface.md` F3/C3), which flickers; a floating window has no dock preview and no snapping.
- Trigger (when to look at B again): a real need for documents to share a tabbed layout with tool panels, or for a document to snap to an edge as a dock area - then do both things together: a client area that can be a `dockTab` neighbour, plus a whitelist of dock targets.

## Evidence

- `tests/test_gui/DocumentViewTest.cpp`: `EveryOpenDocumentGetsATabAndATabSelectionSelectsTheDocument`,
  `SwitchingTabsReusesEachViewAndNeverReinstallsTheCentralArea`,
  `ADocumentFloatsIntoItsOwnWindowAndDocksBackIntoItsTab`,
  `ClosingAFloatedDocumentsWindowGoesThroughTheManagerSoAVetoKeepsIt`.
- The third-party facts: `updateFloatingPane()` in `third_party/DockingPanes/src/DockingPaneManager.cpp` (`setTabVisible(false)`), the `dockTab` branch of `dockPane()`, and `handleFrameEdgeHit()`.
