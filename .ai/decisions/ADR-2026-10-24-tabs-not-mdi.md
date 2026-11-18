# ADR-2026-10-24: one window shows one view at a time (tabs, no MDI and no split view)

- Status: Accepted (**still the current conclusion**; supplemented 2026-10-28: a document may float into **another top-level window**, which is floating, not splitting)
- Date: 2026-10-24
- Related: `../design/appfw-document-model.md` (section 1.3) · `../design/appfw-render-view.md` (section 9.2) ·
  `../decisions/ADR-2026-10-24-command-target.md`, `../decisions/ADR-2026-10-24-command-target-no-window-query.md` ·
  `../decisions/ADR-2026-10-28-document-tabs-and-floating.md`

> **Supplement (2026-10-28)**: the multi-document tab strip has landed (`CentralDocumentHost`), and a document can float into its own top-level window.
> The decision of this ADR is unchanged: **each window still shows one view at a time**; a floating window is another window, not a split of this one;
> "documents do not enter the dock tree" is in `ADR-2026-10-28-document-tabs-and-floating.md`.

## Background

Once the command target was the view, one question about "which 3D area counts" was still open: **may several views be visible in one window at the same time**.
The previous ADR recorded this as a known gap, `backlog/appfw-gui.md` V1 (MDI / split view: a multi-page host plus initiating-control-first resolution), with the trigger "an MDI / split-view requirement appears".

The requirement itself never appeared. The document model has said from day one that **the framework does not define the number of areas, their labels, the layout, or single versus multiple documents** (`appfw-document-model.md` section 1.3): how many areas there are and how they are arranged is the application's business. So this was not "the framework lacks a capability" but "should the default host become multi-page now".

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. MDI / split view: several views visible in one window | Comparing two models side by side is natural | The default host becomes multi-page; resolution must add "initiating control first" (walk the parent chain to the `DocumentView`), otherwise a command edits the wrong pane; focus, layout, dragging and closing all need new rules |
| B. Tabs only (chosen) | Matches today's `CentralDocumentHost` (one view shown at a time); resolution only needs "the host's current view", not the initiator | Two documents cannot be seen side by side in one window |
| C. Tabs plus panes inside a view (part of the chosen combination) | The real need "two viewpoints side by side in one area" is expressed by the **view itself**: the view holds several 3D areas and answers "which one the user last worked in" | A view writes its own layout and focus switching (not the framework's job, and pinned by a case: `DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast`) |

## Decision

1. **The default host is tabbed**: one view on screen at a time. No MDI and no split view.
2. **Several 3D areas belong to the view**: a view may hold several areas side by side and answers itself which one a command should act on (`DocumentView::renderControl()`). That is how "two viewpoints side by side" lands, and the framework builds no pane registry.
3. **Resolution no longer needs "initiating control first"**: `gui::resolveCommandTarget(window)` reading "the host's current view" is the complete answer.
4. Memo (not work for now): if an application builds its own host with several visible views, command resolution must add "initiating control first"; the shape of that day is recorded in the history of the deleted V1 entry (git).

## Consequences

- Good: the default host stays simple; resolution has one road and is identical to the tab semantics; the "side by side" need is not rejected, only moved to the view layer, where a test already exists.
- Bad / cost: two **different** documents cannot be seen side by side in one window (side by side means several panes inside one view).
- Trigger (when to reopen): a real need to see two different documents side by side in one window - then do both things together: a multi-page host and initiating-control-first resolution, since either alone has no observable effect.

## Evidence

- Today: `CentralDocumentHost` shows one view at a time (`d->shown`); `DocumentViewTest` pins the single-page semantics and "one document, one view".
- The pane route: `DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast` (the target carries the view, the view answers the pane).
- The document model already said so: the first item of section 1.3 in `appfw-document-model.md` (the framework does not define the number of areas, labels or layout).
