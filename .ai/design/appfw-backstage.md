# Backstage and the application button

> **Status**: current
> **Last verified**: 2026-10-29
> **Scope**: the application button's click event, the menu / Backstage either-or, the Backstage top-level overlay window and its page navigation; not the content of individual pages such as recent files or import / export.
> **Sources**: `src/fw/appfw/sdk/vine/appfw/gui/Backstage.hpp`, `RibbonBar.hpp`, `MainWindow.hpp`; the cases in `tests/test_gui/test_gui.cpp`.
> **Backlog**: `../backlog/appfw-ribbon.md`

## 1. Overview

### 1.1 Problem and background

The application button in the ribbon's top-left corner ("File") is a flat `QMenu` with no click event, no page model and no whole-window entry like the Office Backstage.
SARibbon's `SARibbonApplicationWidget` is only a `QFrame` shell that follows its parent window and hides on Esc — no pages, no navigation, no content — and as a **child widget** it is covered by the central native 3D window (`.ai/bugs/docking-flyout-under-render.md`).

### 1.2 Goals

- Expose the application button's click as an event (`RibbonBar::applicationButtonClicked()`) so a host or plugin can react to it.
- Provide a Backstage component: pages (id / title / icon), left navigation, command items, open / close / Esc, and geometry that follows the main window.
- The application button chooses between the menu and the Backstage; the two never pop up at once.
- Reuse the existing commands: a Backstage item *is* a `RibbonAction` (the command name is its identity), so no second command vocabulary is introduced.
- The new component leaves the existing ABI alone: only new classes and new methods, with the event held in `Impl`.

### 1.3 Non-goals

- No page content such as recent files (MRU), import / export or accounts (there is no storage and no requirement; see §9.2).
- No multi-section page layout (a page has one body: a command column or one widget, see F7).
- No change to the upstream `SARibbonApplicationWidget` code.

### 1.4 One-paragraph conclusion

**The application button is the trigger, the Backstage is a separate component; the overlay has to be a top-level Tool window, and pages and items reuse the id and command vocabulary.**

## 2. Design Points

### 2.1 The application button only triggers; the Backstage is a separate component (G1)

- What: `RibbonBar` exposes the click and owns the menu / Backstage mode switch; `Backstage` owns the overlay window and the pages.
- Why: button input, window stacking and the page model are three responsibilities; folding them into one class repeats the "one class knows everything" problem.
- Boundary: `MainWindow` owns the Backstage's lifetime; `RibbonBar` only borrows the pointer.

### 2.2 The overlay is a top-level Tool window, not a child widget (G10)

- What: `Backstage`'s native widget is a top-level window with `Qt::Tool | FramelessWindowHint`, its **parent window set to the main window** (so it stacks above the main window and its native children without being globally topmost), and its geometry follows the main window's client area.
- Why: the central 3D view is a native child created by `createWindowContainer`, and on Windows it is always painted above Qt child widgets; a child-widget overlay would leave a hole in the document area. The fly-out fix already showed that a top-level Tool window is the workable direction.
- Cost and gesture: move / resize / minimize / restore, activation and Esc have to be synchronised by hand; the overlay has no title bar of its own, so dragging its empty space (header, title text, the edge of the navigation rail) drags the **main window** — moving the overlay alone would break the "equals the client area" promise and the next sync would jump it back. While dragging, the overlay follows in the **same event** (it does not wait for the main window's `Move` event), or a fast drag shows the two windows a frame apart.

### 2.3 A page is a flat list of ids; an item reuses a command (G1, G8)

- What: `Backstage` holds a set of `BackstagePage`s in the order they were added; a page id is unique and an empty id takes no part in lookups; a page item is a `RibbonAction`.
- Why: the command name is already the action's identity, so the Backstage does not invent an item id or role; page order has no sorting requirement today, so append is enough (`G8`).
- Boundary: a page id is used for lookup and dedup only; a page's title and icon are set before it is added.

### 2.4 Ownership: the window owns the Backstage, the stack owns the page widgets (G10)

- What: `MainWindow::setBackstage()` takes over the Backstage pointer; `Backstage::addPage()` hands the page widget to the internal `QStackedWidget` and marks the page wrapper as not owning its impl.
- Why: to avoid the wrapper object and the Qt parent both deleting the same widget; it matches the ownership convention of ribbon tabs and groups.
- Boundary: the `RibbonAction`s inside a page are held by the caller; the page only creates the buttons and calls `setDefaultAction()`.

### 2.5 Wrapping only SARibbon's application button (G2, G12)

- What: use `SARibbonApplicationButton` and `SARibbonBar::applicationButtonClicked`; the Backstage's window and page model are our own.
- Why: SARibbon's Backstage shell cannot satisfy the native-surface stacking requirement, and forcing it would drag the upstream widget's parent-window semantics in (it requires a `SARibbonMainWindow*` parent).
- Boundary: `SARibbonApplicationWidget` is not a dependency; its resize and Esc behaviour is implemented by the Backstage itself.

### 2.6 Menu or Backstage, never both (G4)

- What: `RibbonBar::setBackstage(bs)` detaches the application button's `QMenu`; `setBackstage(nullptr)` restores it.
- Why: a button that pops a menu and opens a page at the same time is two contradictory behaviours; the mode has to be explicit.
- Boundary: `clearApplicationMenu()` empties the menu items only; it never deletes a `RibbonAction`.

### 2.7 Appearance: a navigation rail and a content surface (G4, G12, G6)

- What: a 240 px navigation rail (a back button at the top, then the page rows) and a `palette(base)` content surface (a large title, a 1 px separator, an item column capped at 760 px); every colour comes from a `palette(...)` role so it follows the light / dark theme (same as `detail::blendIntoSurface()`). The back arrow is drawn from `palette(window-text)`: the platform's `SP_ArrowBack` is a skeuomorphic bitmap, and a text glyph is not centred inside a bordered stylesheet button.
- Why: the default widgets laid out flat read like an empty security dialog, while Office Backstage and the VS Code settings page both build hierarchy from "a column of navigation + one content surface" and two type sizes; the back arrow means "leave this level", not "this page", so it belongs at the top of the rail; the item column is capped because a row stretched across 4K is no longer a row.
- Boundary (C15): the layout is on an 8 px grid — a navigation row and an item row are both 40 px high, 2 px apart, with 12 px padding and a 20 px icon slot; the title and the item column share a 32 px left indent; the header has 24 / 16 px above and below; the item column sits 24 px from the separator, with a 1 px translucent hairline between rows; hover and press use a neutral translucent grey rather than relying on some palette role happening not to be white; the icon slot belongs to the row layout, and a row without an icon uses a transparent placeholder, otherwise its text shifts left of its neighbours. The item column lives in a scroll area capped at 760 px (a thin vertical scrollbar), so the scrollbar lands at the end of the list rather than at the window edge; a body page is not wrapped in a scroll area — scrolling belongs to the body.

### 2.8 One switch for the entry: the application button's visibility (G1, G8)

- What: `RibbonBar::setApplicationButtonVisible(bool)` is the single control over "is the Backstage / application-menu entry offered"; `Backstage` itself has no enabled state and `open()` works for any holder. The entry's default text is "File" (changeable through `setApplicationText()` / `applicationText()`; the wording belongs to the host).
- Why: the application button is the only entry into both the menu and the Backstage, so giving the Backstage a "disabled" state would be a second representation of the same fact: the host would have to keep two states in step, and `applicationButtonVisible()` would be set false by two different causes, leaving a reader unable to tell "the host does not want it" from "the feature is switched off" (`G1`, one fact one owner).
- Boundary: hiding the entry is a presentation choice, not a permission boundary — a command or shortcut can still call `open()`, and whether to refuse is the host's decision; to remove the Backstage entirely use `MainWindow::setBackstage(nullptr)` (see D6).

### 2.9 Pages and tabs are isomorphic: id is identity, role is the semantic anchor, placement is intent (G1, G6, G9)

- What: `BackstagePageRole` (Home / New / Open / Info / Print / Export / Plugins / Help / Account / Options / Close / UserRole) + `BackstagePlacement` (append / before / after, anchored by id or by role) + `BackstagePlaceResult`, the same shape and meaning as `RibbonTabRole` / `RibbonPlacement` / `RibbonPlaceResult`; `addPage(page)` is still the append form.
- Why: page order used to equal the order of addition (that is, of loading), so a plugin could not say "put mine after Info". Sharing one vocabulary with tabs means a plugin learns it once and uses it in two places, and the resolution rules are the same table too — the terminal zone (`BackstagePagePolicy::Terminal`, used by `Close`) + role order + a single anchor + clamping back into range.
- Cost and boundary: the resolver exists once in `Backstage.cpp` and once in `RibbonBar.cpp` (same decision table, isomorphic cases on both sides, a fork is a bug); it is not extracted into a template because a template instantiated by two enums is harder to read than two isomorphic copies (G11).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Application button click | `RibbonBar::applicationButtonClicked()` | landed |
| F2 | Menu / Backstage mode | `setBackstage()` / `clearApplicationMenu()` / `backstage()` | landed |
| F3 | Backstage pages | `addPage/findPage/currentPage/pageAt/numPages` | landed |
| F4 | Command items | `BackstagePage::addItem(RibbonAction*)` | landed |
| F5 | Open / close / geometry | `open/close/isOpen`, Esc, follows the main window | landed |
| F6 | Lifetime | `MainWindow::setBackstage()` takes ownership | landed |
| F7 | Page body | `setBody(Control*)`; one body only (a command column or one widget) | landed |
| F8 | Recent files / MRU | the recent-files page | not done, see backlog |
| F9 | Page visibility | `setPageVisible/isPageVisible`; the navigation row hides, the index does not change | landed |
| F10 | Item visibility | `setItemVisible/isItemVisible`; the button leaves the layout | landed |
| F12 | Page placement | `BackstagePlacement` / `addPage(page, placement)` / `findPage(role)`; terminal zone and role order | landed |
| F13 | Contributor identity and group removal | `setProvider` / `removePages(provider)`; a plugin withdraws its own pages on unload / reload | landed |
| F14 | Item-column scrolling | the page column scrolls when the items exceed one page (thin scrollbar); a body page is not wrapped in a scroll area | landed |

### 3.1.1 F1–F6 base capabilities (behaviour details are in the headers, constraints in C1–C6 of §5.1)

| # | Behaviour | Acceptance |
|---|---|---|
| F1 | The button's `clicked` is forwarded as `Event<RibbonBar, EventArgs>` (P1: the event and the menu coexist, so a host using the Backstage calls `setBackstage()` first to detach the menu) | T1 `Backstage_ApplicationButtonOpensThePage` |
| F2 | `setBackstage(bs)` detaches the button's menu and subscribes to the event that opens `bs`; `nullptr` disconnects and restores the menu (P2: `clearApplicationMenu()` deletes no action) | T1, T2 |
| F3 | Pages enter the left navigation and the `QStackedWidget` in the order they were added; a non-empty id is unique; the first visible page becomes the current one (P3: a page with an empty id may exist, but `findPage("")` never finds it) | T2 `Backstage_PagesAndNavigation` |
| F4 | `addItem(RibbonAction*)` creates a `QToolButton` and calls `setDefaultAction()`; a click runs the command (P4: an item destroyed before its page leaves the button's action dangling) | T3 `Backstage_PageItemsAreCommandBound` |
| F5 | `open()` synchronises geometry, shows, raises and activates; Esc or the back button calls `close()`; the overlay follows or hides on the main window's move / resize / minimize (P5: a 1 px offset on multi-monitor or fractional scaling is the platform's rounding) | T4 `Backstage_FollowsMainWindowSize`, `Backstage_ClosesOnEscapeOrClose` |
| F6 | `MainWindow::setBackstage()` takes ownership; a replacement first disconnects the ribbon's subscription and then deletes the old object (no crash in the destructor, the pointer is cleared) | T5 `Backstage_IsOwnedByMainWindow` |

### 3.1.2 F9, F10, F12, F13 — page visibility, placement, body and contributors

| # | Behaviour | Acceptance |
|---|---|---|
| F9 | `setPageVisible` hides or restores the navigation row; hiding the current page selects the next visible one, and with all pages hidden there is no current page and `open()` refuses (R7) | T6 `Backstage_PagesCanBeHidden` |
| F10 | `setItemVisible` hides or restores the item button; the index and the action do not change (C9) | T7 `BackstagePage_ItemsCanBeHidden` |
| F12 | `addPage(page, placement)` inserts by placement intent; terminal zone + role order + a single anchor + clamping back into range, with the navigation and the stack mirroring it (C12) | T9 `Backstage_PageRolesAndPlacement`, `Backstage_PagePlacementEdgeCases` |
| F13 | `setProvider` + `removePages(provider)` withdraw a whole group; the following page takes over as current and a removed page may be added again (C13) | T10 `Backstage_PagesAreRemovedByProvider` |
| F7 | `setBody(Control*)` replaces the command column with one widget; a page that already has items refuses a body and a page with a body refuses items, and replacing the body hands the old one back to the caller (C14) | T12 `BackstagePage_BodyReplacesTheCommandColumn` |

## 4. Public Interface

### 4.1 RibbonBar

| Interface | Contract | Note |
|---|---|---|
| `applicationButtonClicked()` | the application button's click event | the full signature is in the header |
| `clearApplicationMenu()` | empties the menu items | deletes no action |
| `setBackstage(Backstage*)` | menu or Backstage, never both | borrows the pointer |
| `backstage()` | the current Backstage | may be nullptr |
| `setApplicationButtonVisible(bool)` | shows or hides the File entry (and with it the application-menu entry) | a presentation choice, not a permission |
| `applicationButtonVisible()` | whether the entry is currently shown | reports the widget's state |

### 4.2 Backstage

| Interface | Contract | Note |
|---|---|---|
| `Backstage(MainWindow*)` | creates the top-level overlay window and follows the main window | takes over no page |
| `addPage(BackstagePage*)` | appends a page | a duplicate non-empty id returns false |
| `addPage(BackstagePage*, const BackstagePlacement&)` | adds a page by placement intent | returns the status and the logical index |
| `findPage(const String&)` / `findPage(BackstagePageRole)` | look up by id / by role | the first match in logical order |
| `removePages(const String& provider)` | withdraws one contributor's pages as a group | returns how many were removed; an empty provider matches nothing |
| `findPage/pageAt/numPages` | page lookup | logical order |
| `setCurrentPage/currentPage` | switches the page | keeps the navigation and the content stack in step |
| `setPageVisible/isPageVisible` | page visibility | hiding does not delete, and the index does not change |
| `open/close/isOpen` | show / hide | Esc or the back button closes; `open()` refuses when no page is visible |

### 4.3 BackstagePage

| Interface | Contract | Note |
|---|---|---|
| `setId/id` | the page's identity | set before the page is added |
| `setTitle/title`, `setIcon/icon` | the navigation title and icon | set before the page is added |
| `setRole/role`, `setPolicy/policy` | the semantic anchor and the terminal zone | set before the page is added; `Close` is marked Terminal |
| `setProvider/provider` | the contributor's identity | set before the page is added; what `removePages` matches |
| `setBody/body` | the page body (one widget) | mutually exclusive with `addItem`; the page holds the widget |
| `addItem(RibbonAction*)` | a command item; one item per action | the caller holds the action |
| `numItems/itemAt` | item lookup | logical order; includes hidden items |
| `setItemVisible/isItemVisible` | item visibility | the action is the identity, and the index does not change |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- C1: the application button's event fires on the UI thread.
- C2: `RibbonBar` does not own the Backstage; `MainWindow` does.
- C3: a page's id, title and icon are set before it is added.
- C4: the Backstage never deletes a page item (a `RibbonAction`).
- C5: the overlay covers the main window's client area, not the system title bar.
- C6: a `MainWindow` has exactly one Backstage.
- C7: hiding a page or an item deletes nothing and changes no index; hiding the current page requires a visible page to take over, and with none there is no current page and `open()` is refused.
- C9: an item is keyed by its action (one item per action); hiding is not disabling the command.
- C10: a page holds only the item buttons created by `addItem()` and no decorative buttons (the cases assert the items with `findChildren<QToolButton*>()`, so one extra button fails them).
- C12: page order comes from the placement resolution, not from the order of addition or loading; a terminal page is always last, and the navigation row and the content stack indices follow the logical order.
- C13: `removePages(provider)` withdraws the matching pages only; a withdrawn page's widget is taken out of the stack and handed back to the page (so it can be added again), and an empty provider matches nothing.
- C14: a page has exactly one body — the command column or the widget installed by `setBody()`; the two never coexist, and replacing the body hands the old one back to its owner.
- C15: the layout is a grid — a navigation row and an item row are both 40 px high, 2 px apart, with a 20 px icon slot; the page title and the item column share a 32 px left indent. Changing one dimension means looking at the other column, or the two columns stop keeping time.

### 5.2 Requirements (R#)

- R1: an application-button click must be observable (the event).
- R2: the menu and the Backstage must not pop up at the same time.
- R3: the Backstage must not be covered by a native 3D surface.
- R4: a non-empty page id is unique; a duplicate addition is refused and logged.
- R5: after closing or hiding, the main window is usable again.
- R6: replacing or destroying the `MainWindow` must leave no dangling subscription.
- R7: a hidden page is still findable and can become current again once restored; a hidden item keeps its index and action, and the same action restores it.

### 5.3 Invariants (N#)

- N1: the Backstage is a top-level window, not a child widget of the main window.
- N2: the navigation row and the content-stack index always agree.
- N3: after the add, the page wrapper does not own its widget (the stack does).
- N4: after the menu items are cleared, the `RibbonAction`s still belong to the caller.
- N5: the navigation-row index and the page index agree (a hidden row still occupies its place); a hidden page cannot be selected by `setCurrentPage()`.
- N7: `pages` (the logical order) is the single truth; the navigation rows and the stack index are only mirrors, and the current page is kept by object, not by row number.

## 6. Caveats and Traps

### 6.1 Using SARibbonApplicationWidget as a child widget
- How it fails: the document area (the native 3D view) covers the Backstage, which looks like "only part of it is drawn".
- How to detect it: the central area covers the overlay after the Backstage opens.
- How to prevent it: use a top-level Tool window; see `../bugs/docking-flyout-under-render.md`.

### 6.2 Deletion order
- How it fails: a page wrapper is destroyed before the Backstage stack, so the widget is deleted twice; or an item's action is deleted first and leaves the button dangling.
- How to detect it: ASan, or a crash when the window closes.
- How to prevent it: `setOwnsImpl(false)` when the page is added; the caller holds the items.

### 6.3 Esc does not work
- How it fails: the navigation list holds focus and swallows the Esc key.
- How to detect it: pressing Esc after opening does not close it.
- How to prevent it: use `QShortcut(QKeySequence::Cancel)` with `WidgetWithChildrenShortcut` instead of relying on `keyPressEvent`.

### 6.4 Geometry out of sync
- How it fails: the Backstage stays where it was after the main window is maximised or moved.
- How to detect it: after maximising, the overlay does not cover everything.
- How to prevent it: an event filter on the main window synchronises `Move/Resize/WindowStateChange/Show/Hide`.

### 6.5 Counting after hiding
- How it fails: a host reads `numPages()` / `numItems()` as "how many are visible" and lays out the hidden ones too.
- How to detect it: the cases assert that the count does not change after hiding and that `itemAt/pageAt` still return the original object.
- How to prevent it: count `isPageVisible()` / `isItemVisible()` yourself when a visible count is what is needed.

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | `SARibbonApplicationWidget` as a child widget of the main window | covered by the native 3D surface (G10) | `../decisions/ADR-2026-10-28-backstage-overlay-window.md` |
| D2 | Hiding / rebuilding `RenderControl` while the Backstage is open | surface-lifetime cost and flicker (G10) | same |
| D3 | A `WA_NativeWindow` child window plus raise | the fly-out already showed that this disturbs the render surface (G10) | same |
| D4 | The Backstage owning the page wrappers | double ownership with the Qt parent, and the risk of a double delete (G10) | same |
| D5 | Arbitrary content layouts per page (v1) | no consumer; items already cover today's needs (G8) | — |
| D6 | Adding `enabled` to `Backstage` (disable instead of remove, with the entry hiding itself) | the entry *is* the application button, and `setApplicationButtonVisible(false)` already states the same fact; two switches would have to be kept in step, and `applicationButtonVisible()` would be set false by two different causes (G8). To keep the pages but drop the entry: hide the button; to remove the Backstage entirely: `setBackstage(nullptr)` | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | the application-button event opens the Backstage | `GuiTest.Backstage_ApplicationButtonOpensThePage` |
| T2 | adding / removing / looking up pages, the current page | `GuiTest.Backstage_PagesAndNavigation` |
| T3 | an item is an action bound to a command | `GuiTest.Backstage_PageItemsAreCommandBound` |
| T4 | follows the main window's size, closes | `GuiTest.Backstage_FollowsMainWindowSize`, `GuiTest.Backstage_ClosesOnEscapeOrClose` |
| T5 | replacing the owner does not crash | `GuiTest.Backstage_IsOwnedByMainWindow` |
| T6 | page hiding / restoring / no visible page | `GuiTest.Backstage_PagesCanBeHidden` |
| T7 | item hiding / restoring | `GuiTest.BackstagePage_ItemsCanBeHidden` |
| T8 | hiding / restoring the File entry reaches the widget | `GuiTest.RibbonBar_ApplicationButtonHidesTheEntry` |
| T9 | page placement: role order / anchor / terminal zone / fallback / clamping / lookup by role | `GuiTest.Backstage_PageRolesAndPlacement`, `GuiTest.Backstage_PagePlacementEdgeCases` |
| T10 | group removal by provider, the next page taking over, a removed page being added again | `GuiTest.Backstage_PagesAreRemovedByProvider` |
| T11 | the standard catalogue: a plugin page anchored by role in front of the terminal help page | `GuiTest.Backstage_StandardRoleCatalog` |
| T12 | page body: exclusive with items, replacing the body returns the old one, null does not count | `GuiTest.BackstagePage_BodyReplacesTheCommandColumn` |
| T13 | layout: navigation rows reserve the icon slot, the item column shares the title's left indent, an item row is 40 px | `GuiTest.Backstage_NavRowsReserveTheIconColumn`, `GuiTest.Backstage_PageRowsAlignWithTheTitle` |
| T15 | items scroll when they exceed one screen; a body page is not wrapped in a scroll area | `GuiTest.Backstage_ManyItemsScroll` |
| T16 | dragging the overlay's background drags the main window (and stops on release) | `GuiTest.Backstage_DraggingItsBackgroundMovesTheMainWindow` |

## 9. Status and Evolution

### 9.1 Current status

F1–F6 are the base layer; F9/F10 add page and item visibility.
`app_shell` demonstrates command items with two pages (plugins, help), both tagged with a role (Plugins / Help, the help page `Terminal`) and a provider (`app_shell`), so a plugin can anchor its own pages in front of them by role.
The first real consumer of F7 is the same shell's About page (`setBody(AboutPanel)`, see `appfw-app-info.md`).
An item is a `RibbonAction`, so the enabled state, the tooltip and the check state follow the command (see `appfw-command-binding.md`) and the page does not write a second copy.

### 9.2 Known gaps / backlog

See `../backlog/appfw-ribbon.md`: recent files / MRU (T8), a declarative manifest (T5).

### 9.3 Compatibility / migration

- `Backstage` / `BackstagePage` are new classes; `RibbonBar` only gains methods and an `Impl` event, so its class layout is unchanged.
- Page placement (F12) is `BackstagePlacement` / `BackstagePlaceResult`, `setRole` / `setPolicy` on a page and `Backstage::addPage(page, placement)` / `findPage(role)`.
  `addPage(page)` remains the append form, and a null page still returns false; only the placement overload throws, as `RibbonTab::addGroup` does.
- Contributor identity (F13) is `setProvider` / `provider` and `removePages(provider)`; `app_shell` tags its two pages with `u8"app_shell"` and removes them in `unload()`.
- `RibbonPlacement::Where` is an alias of `PlacementWhere` (source compatible, the enumerators are unchanged); both placement families share that enum.
- `appendApplicationMenu()` stays; a host that does not call `setBackstage()` behaves as before.
- Migration: turn the `appendApplicationMenu` entries into `BackstagePage::addItem` calls and call `MainWindow::setBackstage()`.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/gui/Backstage.hpp`, `src/fw/appfw/src/gui/Backstage.cpp` (the component itself)
- `src/fw/appfw/src/gui/RibbonBar.cpp` (the event and the mode switch), `src/fw/appfw/src/gui/MainWindow.cpp` (ownership)
- `src/plugins/app_shell/src/AppShellUi.cpp` (page assembly)

### 10.2 Documents and ADRs

- `../decisions/ADR-2026-10-28-backstage-overlay-window.md`
- `../bugs/docking-flyout-under-render.md`

### 10.3 External material

- SARibbon: `SARibbonApplicationButton`, `SARibbonBar::applicationButtonClicked`, `SARibbonApplicationWidget`; Office Backstage: a whole-window file page with navigation on the left.
