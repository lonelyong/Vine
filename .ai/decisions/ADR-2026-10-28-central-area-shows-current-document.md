# ADR-2026-10-28: the central area shows the current document only (a floating current document is replaced by a placeholder page)

- Status: Accepted
- Date: 2026-10-28
- Related: `../design/appfw-document-model.md` (sections 3.1.5, 5.3 N8, 6.6, 7 D7) ·
  `ADR-2026-10-28-document-tabs-and-floating.md`, `ADR-2026-10-24-tabs-not-mdi.md`,
  `ADR-2026-10-24-command-target.md`

## Background

Multiple documents already had a tab strip and "float into its own top-level window". The host used to keep "the last docked page" in the central area while a document floated, so three answers went their own ways:

- the tab strip selected the current document B (which had already floated out);
- the central area still showed A's page;
- `resolveCommandTarget()` took the render control of the current view B.

As soon as the user activated any document window, "which is the current document" gained a fourth candidate answer. Had main-window activation been made to set `current`, the host would have had to listen to a main window it does not own, and the moment of switching would still have a window in which "the central area shows A while current is B".

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the last docked page in the central area | Visually continuous while a document floats out | Two sources of truth; after the main window is activated a command still lands on the floating document |
| B. The central area shows the current document only; a placeholder page when it floats (chosen) | The selected tab, `current`, the central content and the command target always agree; main-window activation needs no hook | While a document floats, the central area becomes a placeholder instead of "keeping an eye on the previous one" |
| C. On main-window activation, make the last document shown in the central area current | Visually continuous and `current` follows | The host must listen to the main window; together with "activating a floating window makes it current" that is two rules, and the moment of switching can still disagree |

## Decision

1. **`DocumentManager::current()` is the only owner**. The host keeps no copy of "the current document"; one place, `syncToCurrentDocument()`, derives the tab selection, the central content and the view passed to `activate()`.
2. **The central area is either the current document's page or a placeholder page**: no current document / the current document is in its own window / its type has no view. The placeholder says which case it is (and names the document when it floats).
3. **Activation is a request**: activating a floating window only calls `setCurrent()`; clicking a tab only calls `setCurrent()`; the API `setCurrent()` makes the tab and the central area follow. The host is not a second owner.
4. **A floating document is still a tab**: it does not enter the dock tree (C7 unchanged), and it docks back in exactly three ways - the window's "Dock" button, the tab's context menu or the API, or closing the document.
5. **Closing the current document makes the host request its successor** (the same position, otherwise the previous one). A non-empty `QTabBar` always has a current tab (`setCurrentIndex(-1)` is a no-op while tabs exist, measured on Qt 6.10), so "nothing selected" cannot be drawn; the model therefore cannot stop at "documents exist but none is current", or the strip's highlight and the central content would necessarily disagree (N8).

## Consequences

- Good: at any moment "selected tab / central content / command target" agree, and a single case can pin it; the main window needs no activation hook; with several windows "whichever window is active owns the current document" needs no extra state; after closing the current document its successor shows immediately, leaving no empty central area.
- Bad / cost: while the current document floats, the central area no longer shows another document (deliberately); a user who wants to see two at once has to leave one of them docked (multiple views are outside this design).
- Trigger (when to look at A/C again): a real need for the central area to show several documents side by side, or for the main window and a floating window to have independent command targets - then "one current per application" itself has to be redesigned.

## Evidence

- `tests/test_gui/DocumentViewTest.cpp`:
  `EveryOpenDocumentGetsATabAndATabSelectionSelectsTheDocument` (a mouse tab switch produces currentChanged),
  `TheCentralAreaOnlyEverShowsTheCurrentDocument` (the invariant),
  `ActivatingAFloatingDocumentsWindowSelectsIt` (activation is current),
  `ADocumentFloatsIntoItsOwnWindowAndDocksBackIntoItsTab` (floating and docking back).
- `syncToCurrentDocument()` in `src/fw/appfw/src/gui/CentralDocumentHost.cpp`.
