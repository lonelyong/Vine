# appfw's document model

> **Status**: current
> **Last verified**: 2026-10-28
> **Scope**: document identity and lifetime, opening and saving, type metadata, the view layer and the central client area.
> **Sources**: `Document.hpp`, `DocumentManager.hpp`, `DocumentView.hpp`, `tests/test_appfw` / `tests/test_gui`.
> **Backlog**: none (open items are in `../backlog/`)

## 1. Overview

### 1.1 Problem and background

A `RenderControl` gives one widget = one native surface = one `RenderEngine` = one `SceneView`; the shell owns the widget and the session, and a content plugin fills in the scene.
That is **view content**, not a document: it has no identity and no lifetime, and nothing to close, save or reopen — so an app that needs those invents them again.

### 1.2 Goals

- The framework defines **document identity and lifetime**: `Document` + `DocumentManager`.
- Document types are registered by plugins; opening routes by the payload's type; saving writes back to the document's source.
- The view layer is instantiated per document; how the central area is laid out is the host's decision.
- Multiple documents: one tab per open document (the bar is hidden when there is only one); a view may float into a top-level window of its own and can only dock back into the central area.

### 1.3 Non-goals

- Not defining how many areas exist, the tabs, the layout, single vs multiple documents, or the set of display kinds.
- `core` does not learn about `Vfs` or IO; `Document::save()` takes no arguments.
- No `closeAll()`, no `dirtyChanged`, no per-plugin owner.
- Multiple windows (one top-level window per document) are outside this design.
- The default host **shows one** view at a time (tab style); several views side by side (MDI / split) are not done, and several 3D areas side by side are expressed by **the view's own panes** (see `../decisions/ADR-2026-10-24-tabs-not-mdi.md`).
  **Floating** is another top-level window (each window still shows one view), and **a document never enters the docking tree** (see `../decisions/ADR-2026-10-28-document-tabs-and-floating.md`).

### 1.4 One-paragraph conclusion

The framework defines only "a document" (identity + lifetime) and the registration of "document types"; opening = `open(payload)` routed by the payload's C++ type, the location is `Vfs + virtual path`, saving writes back through `DocumentSource`; how many areas exist, how they are laid out and what is displayed belongs to the concrete app.

## 2. Design Points

### 2.1 Three different axes

| Axis | Owned by |
| --- | --- |
| surface + device + when to attach (the session) | the shell (one session per window, deciding "where it is drawn") |
| camera + content scene + manipulators (view state) | the view the document type provides |
| identity + lifetime + data (the document) | the framework holds the lifetime, the plugin provides the type |

### 2.2 The framework's surface area

| Layer | Content |
| --- | --- |
| core SDK (no Qt) | `Document` + `DocumentManager` (type registration, `create`/`open`/`close`/`current`, three events) |
| gui SDK | `DocumentView` + `DocumentViewRegistry` + `CentralDocumentHost` |
| the concrete app (a shell plugin) | decides single vs multiple documents, whether to use the default host, how the areas are laid out |
| a plugin (a document type) | registers a typeId + a view factory and decides how it is displayed |

One criterion: an app that only opens table documents should not pay for 3D; a multi-document app should not need N devices; a single-document app should not write tab-bar code.

### 2.3 Ownership

- `DocumentManager` is one per process, hung on `Application::documentManager()`, created in `initialize()` before plugins load.
- The manager owns the documents; `current` is one of them or nullptr.
- The view layer lives in `GuiApplication` only; a view is instantiated per document and `content()` is what goes into the central area.
- The central area is a single slot, and `setCentralWidget()` reparents (Qt recreates the native window) ⇒ put a container in it, install it once, and switch pages inside it with a `QStackedWidget`.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Document semantics | `Document` is pure data + lifetime; `DocumentManager` holds the set, the current one and the events | landed |
| F2 | Type registration | `registerType(type_id, display_name, factory)`; a duplicate is refused | landed |
| F3 | Opening | `open(payload)` routes by the payload's C++ type | landed |
| F4 | Saving | `save()` writes back through `DocumentSource` with no arguments; the base class refuses by default | landed |
| F5 | The view layer | `DocumentView` + the registry + the central host (tab bar, floating windows) | landed |
| F6 | Type metadata / panel | `DocumentTypeInfo` + `DocumentManagerDialog` | landed |

### 3.1.1 F1 Document semantics

- **Behaviour**: `Document` has `typeId/title/isDirty/canClose` + `source()` / `save()`; `DocumentManager` has `create`/`open`/`close`/`current`, `findByTitleOrTypeId()` (title first, returning every match) and the three events.
- **Constraints** (C1): `create` does not touch `current`; `setCurrent` accepts only a document this manager owns; no event when the value does not change.
- **Requirements** (R1): `close(doc)` asks `canClose()` first; a refusal returns false and leaves the set and `current` alone.
- **Acceptance** (T1): `tests/test_appfw/DocumentTest.cpp`.
- **Caveat** (N1): a closing document leaves the set before the event fires; the object is still alive during the event; `currentChanged(null)` comes before `closed`.

### 3.1.2 F2 Type registration

- **Behaviour**: `registerType(type_id, display_name, factory)`; one type per id.
- **Constraints** (C2): a duplicate is refused; an empty id or an empty factory is refused.
- **Requirements** (R2): a factory whose document reports a `typeId()` that differs from the registration ⇒ nullptr plus a warning.
- **Acceptance** (T2): the registration / duplicate-registration cases in `DocumentTest`.
- **Caveat** (N2): registration must happen in the plugin's `load()`; the manager exists before plugins load.

### 3.1.3 F3 Opening

- **Behaviour**: `open(payload)` takes a `raw_ptr<const Object>` only; the key is `payload->getType()`; the opener is found by the payload's type.
- **Constraints** (C3): the payload derives from `Object` and has its own `desc()`; the framework does not interpret, copy or hold the payload object.
- **Requirements** (R3): most specific first → `refine` score → `priority` → registration order; with nobody accepting, nullptr plus a warning.
- **Acceptance** (T3): `AnOpenerBuildsADocumentFromAPayload…`, `TheFrameworkDoesNotCopyAPayload`.
- **Caveat** (N3): a temporary payload needs a local variable first so its address can be taken; `open()` only considers the openers registered when it starts.

### 3.1.4 F4 Saving

- **Behaviour**: `Document::save()` takes no arguments and writes back through its own `DocumentSource`; the base class refuses by default.
- **Constraints** (C4): `DocumentSource` is a value type `{scheme, address}` and is not derived from; "no source" is an empty value.
- **Requirements** (R4): `core` knows nothing about `Vfs`; save-as belongs to the type's own interface.
- **Acceptance** (T4): the save / source cases in `DocumentTest`.
- **Caveat** (N4): do not throw information away with a bool on failure; a type's interface may use `io::Result` / `IoError`.

### 3.1.5 F5 The view layer

- **Behaviour**: `gui::DocumentView` (a thin `Control` wrapper + `document()` + `content()` + `activate()` / `deactivate()`); `DocumentViewRegistry` (type_id → factory, one per type); `gui::CentralDocumentHost`.
  The host gives each open document one tab (order = the order of opening or dragging, and the bar is hidden when there is only one); `setFloating()` moves the page widget into a top-level window of its own, and `setFloating(..., false)` takes it back.
  **The three places that show something have one source**: `DocumentManager::current()` ⇒ the selected tab / the central content / the view that is `activate()`d, all derived in one place, `syncToCurrentDocument()`. The central area is either the current document's page or an explanation page (no current document / the current document is in its own window / the type has no view), and it never shows a non-current document's page. The tab's context menu offers float / dock / close, and all three are only requests to the manager.
  After the current document closes, the host requests the successor tab in `closed` (a non-empty `QTabBar` cannot express "nothing selected", so this is the only way to keep the two in step).
- **Constraints** (C5, C7): a view is instantiated per document; `content()` is what goes into the central area; 2D and 3D both return themselves; a document never enters the docking tree of DockingPanes.
- **Requirements** (R5, R7): the host owns the view objects and the floating windows, the container owns the page widgets; the tab selection and `DocumentManager`'s current are decided by the manager alone (both sides are "requests", and it selects back).
- **Acceptance** (T5): `tests/test_gui/DocumentViewTest.cpp` (16 cases, including tabs / floating / activation / invariants / a refused close / the successor after a close / theme following).
- **Caveat** (N5, N6, N7, N8): the central area is a single slot and `setCentralWidget()` reparents; put a container in it, install it once and switch pages inside; the central area never shows a non-current document.

### 3.1.6 F6 Type metadata / panel

- **Behaviour**: `DocumentTypeInfo` (owner / icon / can_create / payload_types / source_schemes) + `gui::DocumentManagerDialog` + the `app_shell` commands: `show_document_types` (a read-only panel), `switch_document` (switch by title or type id, refusing an ambiguity) and `close_document` (close the dispatch target through `close()`, reporting NotApplicable when it is refused).
- **Constraints** (C6): a type has no enable / disable switch; the owner comes from `Application::registrationOwner()`.
- **Requirements** (R6): the panel shows "open only / creatable" truthfully and pretends nothing.
- **Acceptance** (T6): `DocumentManagerDialogTest`.
- **Caveat** (N6): with no panel registry, a plugin can only build and mount a panel itself.

## 4. Public Interface

### 4.1 core

| Interface | Contract | Note |
| --- | --- | --- |
| `Document` | `typeId/title/isDirty/canClose` + `source()` / `save()` | pure data + lifetime |
| `DocumentManager::registerType` | one type per id | a duplicate / empty id / empty factory is refused |
| `DocumentManager::create/open/close/current` | the set and the current document | the events are in §5 |
| `DocumentManager::findByTitleOrTypeId` | look up the open set by title (first) or type id | returns every match; an empty text or no match gives an empty result |
| `DocumentManager::registerOpener<Payload>` | register by payload type | `DocumentOpenerRegistration` |
| `opened/closed/currentChanged` | events | application thread |

### 4.2 gui

| Interface | Contract | Note |
| --- | --- | --- |
| `DocumentView` | `document()` / `content()` / `activate()` / `deactivate()` | instantiated per document |
| `DocumentViewRegistry` | type_id → factory | a duplicate / empty id / empty factory is refused |
| `CentralDocumentHost` | the central page container + the tab bar | one current, three places agreeing; the contract is in the header |
| `documentCount()` / `documentAt(i)` | a projection of the open set (one tab per document) | order = the order after opening or dragging |
| `isFloating(doc)` / `setFloating(doc, bool)` | float into its own top-level window / dock back into the central area | not docking: the window has no dock target |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | `create` does not touch `current`; `setCurrent` accepts only this manager's documents. |
| C2 | A type id is unique; an empty id or an empty factory is refused. |
| C3 | The payload derives from `Object` and has a `desc()`; the framework does not interpret, copy or hold it. |
| C4 | `DocumentSource` is a value type and is not derived from; no source = an empty value. |
| C5 | A view is instantiated per document; `content()` is what goes into the central area. |
| C6 | A type has no enable / disable switch; the owner comes from `registrationOwner()`. |
| C7 | A document never enters the docking tree of DockingPanes: its place is either a central-area tab or its own floating window. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | `close` asks `canClose()` first; a refusal leaves the set and `current` alone. |
| R2 | The `typeId()` a factory returns must match the registration. |
| R3 | Opener selection order: most specific first → refine → priority → registration order. |
| R4 | `core` knows nothing about `Vfs`; `save()` takes no arguments. |
| R5 | The host owns the view objects and the container owns the page widgets; `owns=false`. |
| R6 | The panel shows creatable / open-only truthfully. |
| R7 | The tab selection and `current` are decided by `DocumentManager` alone: both sides are only requests, and it selects the other back (no two truths). |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | A closing document leaves the set before the event fires; the object is still alive during it. |
| N2 | `Vfs` is borrowed and outlives the document; one instance is never touched by two threads at once. |
| N3 | A payload is valid only during `open()`; a document must not keep a pointer to it. |
| N4 | A view is instantiated per document; switching back reuses the same one and only closing destroys it. |
| N5 | The central area is a single slot that reparents; the container is installed once. |
| N6 | A floating window is not a dock pane: it has no dock target and cannot be dragged to an edge or an auto-hide strip; the only way back is the dock button / API / closing the document. |
| N7 | A page moves once between the tab (`QStackedWidget`) and the floating window: two parent windows exist, never both attached, and `takePage()` detaches it first. |
| N8 | The central area always shows the current document's page or the explanation page; the tab selection, `current()` and the central content never disagree. Activating a floating window is only a `setCurrent()` request. After the current document closes, the host requests the successor tab to become current (closing a non-current document leaves `current` alone). |

## 6. Caveats and Traps

### 6.1 Payload lifetime

- How it fails: the document keeps a payload pointer, the payload is a temporary once `open()` returns → use-after-free.
- How to detect it: ASan; a `raw_ptr<const Payload>` member inside a document.
- How to prevent it: the opener copies the pointer or handle it needs into the document or the source; bulk data goes into a payload field and a `shared_ptr` is copied.

### 6.2 Event order when a document closes

- How it fails: `closed` picks the next current document and the following `currentChanged` overwrites it.
- How to detect it: `currentChanged(null)` must come before `closed`.
- How to prevent it: implement the order of §3.1.1; the object is destroyed only after `close()` returns. This order is exactly what lets the host request the successor tab to become current inside `closed`, so the tab bar and the central area never disagree (§3.1.5, N8).

### 6.3 `Vfs` lifetime and threads

- How it fails: the `Vfs` is destroyed when the document closes, and the document's `save()` still needs it.
- How to detect it: `Vfs` is borrowed and has no internal lock.
- How to prevent it: the host closes the documents first and destroys the `Vfs` afterwards; one `Vfs` never crosses threads.

### 6.4 Floating = reparenting

- How it fails: assuming that moving a 3D page into a floating window means a new render session (and so calling `init()` again), or the opposite — that the widget can be moved without the native window being recreated.
- How to detect it: after the native window is recreated `RenderControl` goes back to `Pending` and then to `Presenting`; the session / device / pipeline do not change (`appfw-render-surface.md` F3/C3).
- How to prevent it: floating moves the page widget only and leaves the session alone; never create a `RenderControl` for floating.

### 6.5 Two "current"s

- How it fails: the tab bar remembers its own selection while the manager has another current, so one switch desynchronises them (and a command lands on the wrong document).
- How to detect it: `documentAt(strip->currentIndex()) == documents.current()` must hold after every operation.
- How to prevent it: a tab selection only calls `setCurrent()`, and `currentChanged` selects the tab back (`R7`); installing or removing a tab uses `QSignalBlocker` so Qt's "auto-select the first one" does not become a selection.

### 6.6 A floating document is not "still in the central area"

- How it fails: assuming floating only opens an extra window while the central area keeps showing its page — so the selected tab says A, the central area shows B, and the command target may be a third (`resolveCommandTarget()` takes the current view).
- How to detect it: with the current document floating, `shownContent()` is the explanation page and `currentView()` is still that document; after docking back, `shownContent()` is its page again.
- How to prevent it: ask `shownContent()` / `currentView()` for "what is on screen"; never infer it from a tab index or from "the page shown last time".

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | A `path` instead of `Vfs + virtual path` | Network, database and private formats cannot be expressed that way. | — |
| D2 | Deriving `DocumentSource` | Two kind hierarchies, lost value semantics, and the burden pushed onto every document type. | — |
| D3 | A `shared_ptr` for the payload | What is kept alive is the descriptor, not the `Vfs` or buffer it points at, and the framework does not know how long it should live. | — |
| D4 | A per-plugin owner | Add it when a real need appears. | — |
| D5 | A `dirtyChanged` event | A document type raises its own change event and the host subscribes to it. | — |
| D6 | `closeAll()` | Three lines of loop in the host, and the "partial failure" policy belongs to the host. | — |
| D7 | Keeping the last docked page in the central area while floating | It produces two "what is current"s: the tab selection, the central content and the command target each say something different; with an explanation page there is one answer (N8). | `../decisions/ADR-2026-10-28-central-area-shows-current-document.md` |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | document semantics / event order / closing | `tests/test_appfw/DocumentTest.cpp` |
| T2 | type registration / duplicate / empty | same |
| T3 | opener selection / no copy | `AnOpenerBuildsADocumentFromAPayload…`, `TheFrameworkDoesNotCopyAPayload` |
| T4 | saving / source | `DocumentTest` |
| T5 | the view layer / the central host | `tests/test_gui/DocumentViewTest.cpp` (16 cases) |
| T6 | the type metadata panel | `DocumentManagerDialogTest` |

## 9. Status and Evolution

### 9.1 Current status

The core document model, opening and saving, the type metadata, the view layer and the central host are all in place.

### 9.2 Known gaps / backlog

- `MeshLoader::load()` / `loadMerged()` take a path or a stream and have no `Vfs` entry ⇒ a model file addressed as `Vfs` + virtual path cannot be read.
- With no panel registry, a plugin can only build and mount a panel itself.
- Multiple windows are outside this design.

### 9.3 Compatibility / migration

`DocumentSource` is a value type; `Document::save()` takes no arguments; new code uses the interface of §4.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/Document.hpp`
- `src/fw/appfw/sdk/vine/appfw/DocumentManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/DocumentView.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/CentralDocumentHost.hpp`
- `tests/test_appfw/DocumentTest.cpp`, `tests/test_gui/DocumentViewTest.cpp`

### 10.2 Documents and ADRs

- `appfw-render-view.md`
- `appfw-userio.md`
- `../backlog/`

### 10.3 External material

- `src/base/iobase`'s `Vfs` / `Result` / `IoError`.
- The reference implementation in `src/plugins/model_viewer/`.
