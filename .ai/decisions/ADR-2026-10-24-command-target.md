# ADR-2026-10-24: a command's target is the view, not the document and not the render control

- Status: Accepted (design final; the implementation goes in four steps, see "Implementation order")
- Date: 2026-10-24
- Follow-up: the second half of decision 4 (keeping `primaryRenderControl()` as a derived query) and the naming of the "active pane" were superseded by `ADR-2026-10-24-command-target-no-window-query.md`; the other decisions still hold.
- Related: `../design/appfw-render-view.md` · `../design/appfw-command-manager.md` ·
  `../design/appfw-document-model.md` · `../backlog/graphics.md` (T3)

## Background

A command had exactly one way to find "the 3D area it should change": `MainWindow::current()` (a global static holding the most recently active main window) plus `MainWindow::primaryRenderControl()` (a **single slot** that `ModelRenderView` / `DemoView` write on **going on screen and clear on going off screen**).
Nine call sites relied on it, and `ModelRenderView::isActive()` even used `primaryRenderControl() == control_` as an identity test.

It was broken in four ways, and none of them was a wiring problem:

1. **A single slot**: when two views are visible at once (two panes in one view, or two documents side by side) the later one occupies the slot, so the other's command and identity test are simply wrong;
2. **A GUI facade used as identity**: `RenderControl`'s own header says it is "the widget-side wrapper" (the real surface/engine/view/viewer lives in the private `SurfaceWindow`); a facade can have two instances pointing at the same place (popping a page into its own window), so equal pointers no longer mean "the same area";
3. **Granularity stops at the widget**: two panes in one view are two `RenderControl`s, while `primaryRenderControl()` can only name one; with a widget as the target, "which pane" is never answered;
4. **The initiator is lost at dispatch**: `RibbonAction::triggered -> commandManager()->executeDetached(name)` passes only the command name, although the UI knows exactly who triggered it.

The external frameworks answer the same way: Blender passes `context` (including the active area and region) **explicitly into** every operator; Qt Creator answers "the current context" from an `IContext` stack over the widget hierarchy and enables actions by context; VS Code commands carry parameters from the **call site**, and contribution points declare `when` conditions. None of the three has a command go and find the current window itself.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. The target is the **document** (`DocTarget`) | Close to content commands such as open/save/undo | Cannot answer "which place" when one document has two views or two panes; the view knows its document (`DocumentView::document()`), but not the other way round |
| B. The target is the **`RenderControl`** (roughly today's shape) | No new concepts | A GUI facade as identity; resource ownership coupled with presentation; granularity stops at the widget |
| C. The target is the **`SceneView`** (the graphics layer) | Can answer "which one draws" | An appfw command also needs the document; cannot answer the focused pane inside a view |
| D. Keep the global single slot | No change | All four problems stay |
| **E. The target is the `DocumentView`, layers nullable and requirements declared per command (chosen)** | Carries the target at the finest place that can answer "which one"; the document is derived from the view; panes belong to the view, so the framework need not know them | One plugin ABI bump; one more target type |

## Decision

1. **The target is the `DocumentView`**: a command acts on "one page at one place", not on "that content". Graphics-layer objects (`RenderControl` / `SceneView` / `Viewer`) are taken from it; `RenderControl` stays "the widget host of a 3D area" and is **not** a target.
2. **All three layers may be null, and a command declares what it needs**: `CommandTarget{document, view}`; `CommandFlags` gains `NeedsDocument` / `NeedsView`, and the framework **refuses uniformly** at dispatch, saying what is missing. The console, GUI-less tests and batch runs are **legal cases**: document-level commands run as usual, view-level commands get an explicit "a 3D view is required" answer (today every view-level command writes its own empty-chain check).
3. **The target is resolved at dispatch**: the initiating widget (focus, parent chain) comes first; with no initiator -> current document -> its view -> the view's **active pane**; otherwise -> empty. There is one implementation, and a failure is reported once.
4. **`primaryRenderControl()` is demoted to a derived query** (current view -> active pane -> control) and is no longer a publication slot the plugins write into; `isActive()` becomes "am I the command's target" instead of comparing pointers against a global slot.
5. **Multiple panes inside a view are the view's own state**: the framework builds no pane registry and does no layout or focus routing (following `appfw-render-view.md` section 6.4).
6. **One ABI bump**: `CommandExecutionContext` gains a virtual function and `CommandInfo` gains fields, both on the plugin boundary, so `VN_APPFW_PLUGIN_ABI_VERSION` goes 8 -> 9 once, not in several small bumps.

## Consequences

- Plugins no longer call `setPrimaryRenderControl`; the publishing code in `ModelRenderView` / `DemoView` is deleted and `isActive()` is rewritten.
- A view-level command loses one repeated empty-chain check; console and GUI-less behaviour is unified by the framework instead of one message per command.
- The first "one view, two panes" case only needs the view to expose its active pane; the framework does not change.
- Plugins must be rebuilt (ABI 9); a plugin that was not rebuilt is refused by the existing version check instead of running wrongly.

## Implementation order

1. Types and resolution: `CommandTarget` plus the resolution function plus `DocumentView::activeRenderControl()` plus the derived `primaryRenderControl()` plus `NeedsDocument` / `NeedsView` plus the uniform refusal (GUI-less unit tests included). **Behaviour is unchanged**, no call site changes.
2. Dispatch carries the target: ribbon, menu, shortcut and console resolve the initiator; `CommandExecutionContext` exposes `target()` (and the ABI is bumped at the same time).
3. Migrate the call sites: the nine `MainWindow::current()` / `primaryRenderControl()` call sites use the target; the publication slot is deleted.
4. Close-out: documentation and backlog (T3 is deleted), plus the end-to-end case with two panes in one view.

## Verification

- GUI-less: with a document but no view, a `NeedsDocument` command succeeds, a `NeedsView` command returns an explicit refusal, and `CommandInfo` can express "unavailable" in advance.
- With a GUI: one view with two panes, and the command lands in the focused pane; `isActive()` no longer depends on a global slot.
- Regression: the existing document-view cases of `test_gui` (including "two documents each keep their own resources") stay green.
