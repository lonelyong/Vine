# ADR-2026-10-24: the target has exactly one road - no window-level "current 3D area" query

- Status: Accepted (partly supersedes the second half of decision 4 in `ADR-2026-10-24-command-target.md`)
- Date: 2026-10-24
- Related: `../design/appfw-render-view.md` (section 9.2) · `../design/appfw-command-manager.md` (section 2.0) ·
  tests `DocumentViewTest.NoActiveDocumentMeansNoCommandTarget`,
  `DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast`,
  `ModelViewerTest.TheCommandTargetTravelsWithTheExecution`

## Background

Decision 4 of `ADR-2026-10-24-command-target.md` said `MainWindow::primaryRenderControl()` would be **demoted to a derived query** (current view -> active pane -> control) instead of remaining a publication slot the plugins write into. By step 3 of the implementation that compromise no longer held:

1. after the two real consumers were migrated (`ShadingModeCommand`, `TestRenderLiveCommand`), there were **zero callers** in `src/`;
2. it was a **second implementation** of the same traversal as `gui::resolveCommandTarget()` (central area -> document host -> current view -> control), differing only in whether the answer belongs to the window or to this execution;
3. and "the answer belongs to the window" is exactly the half that had been rejected: when a window shows a second 3D area, a window-level single answer is necessarily wrong for one of them, while an execution-level target naturally answers for each.

The same period had a naming problem: the accessor answering "which 3D area does this view show" was first called `activeRenderControl()`, which uses the same word at a different layer than `activate()` / `deactivate()` (**on screen / off screen**) in the same class - and "active" is already the document layer's word in this repository (`DocumentManager::current()`).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep `primaryRenderControl()` as a derived query | Non-command code (panels, status bar) writes three lines fewer | Two roads to one question: a command can bypass the target and ask the window, and the window-level answer is wrong as soon as a second 3D area exists; there is no caller today |
| B. Keep it but mark it `[[deprecated]]` and keep one ABI symbol | Formally forward compatible | ABI 9 is not released, so no "published plugin" needs compatibility; a symbol kept only for compatibility is pure debt |
| **C. Delete it entirely and keep only the execution target (chosen)** | One road per question; "no active document = no target" becomes the only, derivable answer | Non-command code has to resolve for itself (`gui::resolveCommandTarget(window)`); with several windows the resolution input is still incomplete |
| D. Keep the accessor called `activeRenderControl()` | The name says "which of several panes" | A third meaning of "active" in one class; both plugins already have a `renderControl() const` with the same signature, so the extra one is a duplicate |
| E. Name the accessor `renderControl() const` (chosen) | The same name and meaning as the getter both plugins already have, so the extra accessor disappears together with its name; "active" stays with the document layer | The name no longer hints at "which of several panes"; the contract text says it instead |

## Decision

1. **No window-level query**: both `MainWindow::setPrimaryRenderControl()` and `primaryRenderControl()` are deleted.
   "Which 3D area" has exactly two exits: **resolution at dispatch** (`gui::resolveCommandTarget()`, whose result travels with the execution) and **the view's own answer** (`DocumentView::renderControl()`).
2. **The view's accessor is `renderControl() const`**, returning null by default (a 2D view has no 3D area); overriding it only states "which one" and introduces no state. With several panes the view answers "the one the user last worked in" itself.
3. **"No active document = no target"** is the single rule: with no current document, with the central area not being the document host, or with a view that has no 3D area, the resolution result is an empty target, and `CommandFlags::NeedsDocument` / `NeedsView` refuse uniformly on that basis.

## Consequences

- Good: a command cannot "bypass the target and ask the window" - that road does not exist; `renderControl()` merges with the getter both plugins already had, and the vtable gains no new concept; deleting it makes the ABI surface smaller (`MainWindow` has one method fewer).
- Bad / cost: code outside commands has to say for whom it resolves (`gui::resolveCommandTarget(window)`). A shorter road exists - `GuiApplication::centralDocumentHost()->currentView()->renderControl()`, three lines to "the current area", needing neither window nor cast; a command should not take it, because it is the **runtime "current"**: once a command `co_await`s (progress, IO, background work) the user can switch tabs (or click into another MDI pane) while it runs; and there can only be one "current" - when two areas are visible in one window at the same time it cannot say which one issued the command. The whole premise of the target is "the answer is for one dispatch, not for the whole window".
- Known gaps: none. The resolution input is "the host's current view", identical to the tab semantics (one window shows one view at a time, see `ADR-2026-10-24-tabs-not-mdi.md`); several 3D areas inside one view are answered by the view itself (`DocumentView::renderControl()`).
- Trigger (when to reopen): the first **non-command** consumer appears (for example a status bar showing "current 3D area info"), or a need to see two different documents side by side in one window (which means doing the multi-page host and the initiating-control-first resolution together).

## Evidence

- Code: `src/fw/appfw/sdk/vine/appfw/gui/CommandTarget.hpp` (the resolution and its single cast);
  `src/fw/appfw/sdk/vine/appfw/gui/DocumentView.hpp` (the contract of `renderControl() const`);
  `src/fw/appfw/src/CommandManager.cpp` (the refusal before the takeover and the serial gate).
- Tests: `DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast` (the target carries the view, the view answers the pane), `ModelViewerTest.TheCommandTargetTravelsWithTheExecution` (the target reaches the command body through a real dispatch, and a missing target is refused before the body).
- Gates: `python3 scripts/check_ai_docs.py --strict` and `python3 scripts/check_design_docs.py --strict` both report 0.
