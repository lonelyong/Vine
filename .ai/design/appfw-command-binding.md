# The display state of a command item

> **Status**: current
> **Last verified**: 2026-10-28
> **Scope**: how a command item (a button, a drop-down or application-menu entry, a Backstage item) derives its enabled / tooltip / checked state from `CommandManager` and from this dispatch's target; not the identity and placement of ribbon tabs and groups (see `appfw-ribbon.md`).
> **Sources**: `src/fw/appfw/sdk/vine/appfw/CommandManager.hpp`, `gui/RibbonButton.hpp`, `gui/RibbonAction.hpp`, `src/fw/appfw/src/gui/CommandBinding.hpp`; the cases in `tests/test_gui`.
> **Backlog**: `../backlog/appfw-ribbon.md`

## 1. Overview

### 1.1 Problem and background

A command can be executed by name, but "can this button be clicked now, what does the tooltip say, is this the active mode" is a question about the command, not about the button — and when each host or plugin answers it for itself, the answers drift: a command the user disabled stays clickable, a `NeedsDocument` / `NeedsView` command is refused only after the click, a `CommandInfo::description` nobody shows as a tooltip, and five shading buttons with nothing to say which one is current.
Writing `NotApplicable` into the disable preference is worse still: it turns a temporary context into a permanent setting.

### 1.2 Goals

- One rule: the item that called `setCommand(name)` is a **view** of the command, and its display state is derived in one place.
- Every kind of command item is covered: ribbon buttons, drop-down items, application-menu items, Backstage items, quick-access items.
- A target requirement (`NeedsDocument` / `NeedsView`) is judged against **this dispatch's target**, and a missing target only greys the item out without touching the preference.
- The tooltip follows the command's description; the checked state comes from an optional provider, with no "radio group" model introduced.

### 1.3 Non-goals

- Not writing `CommandManager::setCommandEnabled()`: that is the user's persisted preference, not the current context.
- No global busy grey-out during a `LongRunning` command (no consumer, and it would flicker).
- No multi-window target (there is one window today: both refresh and dispatch use `MainWindow::current()`).
- No id for a control without a command, no contextual tabs and no user-defined layout (see `../backlog/appfw-ribbon.md`).

### 1.4 One-paragraph conclusion

**A command item only displays the command's state: registration, preference and target requirement are answered by `CommandManager`, the description is the tooltip, the checked state is answered by a provider; and the rule has exactly one implementation, `CommandBinding`.**

## 2. Design Points

### 2.1 An item is a view, not a second owner of the state (G1, G5, G7)

- What: the item keeps the command name (its identity) and recomputes every other state from the command manager.
- Why: if "can it be clicked" also had a cached copy in the item, an unregistration, a disable or a target change would all be missed.
- Boundary: the command name is still the identity; the item's own `setEnabled` / `setTooltip` / `setChecked` only affect an item that has no command bound.

### 2.2 The rule has exactly one implementation (G2, G6)

- What: `RibbonButton` and `RibbonAction` each hold an internal `detail::CommandBinding` (`src/gui/CommandBinding.hpp`) that subscribes, computes and writes back. They only supply the callback that puts the state onto the native control.
- Why: if buttons and menu / Backstage items each wrote their own copy, a rule such as `NeedsDocument` would sooner or later be updated in one place only.
- Boundary: it is a GUI-layer internal type and adds no public API; the command layer (`CommandManager`) knows nothing about widgets.

### 2.3 What is derived is the display state, not the persisted preference (G1, G4)

- What: `enabled = isCommandEnabled(name)`, minus the case where `NeedsDocument` / `NeedsView` is not satisfied by the current target.
- Why: the disable preference belongs to the user (it is written into the configuration); a missing target belongs to the context, and mixing the two turns "cannot be clicked for now" into "permanently disabled".
- Boundary: a refresh only calls `QWidget/QAction::setEnabled` and never writes back to the manager; `NotApplicable` is not a disable either.

### 2.4 Refreshes are driven by manager events, marshalled when they cross a thread (G11)

- What: subscribe to `commandsChanged` (registration / preference / description), `executed` (the provider's answer may have changed) and `DocumentManager::currentChanged` (the target changed).
- Why: the state has no polling point; missing one of those subscriptions is an "occasionally stale" bug.
- Boundary: `executed` may fire on a worker thread (a command resuming on a timer or an IO thread), so the refresh goes back to the application thread through `MainThreadDispatcher` before touching a control; `setCommand()` evaluates once immediately so the first paint is not wrong.

### 2.5 The tooltip belongs to the binding until the caller takes over (G4)

- What: the binding writes the command description and remembers the text it wrote; if the control's current tooltip differs from that, the caller has taken over and the binding stops overwriting it.
- Why: the description is the command's documentation and should appear by default, but an item may need a more specific hint; the two must not silently overwrite each other.
- Boundary: when the description becomes empty and the current text is still the binding's, the tooltip is cleared.

### 2.6 The checked state comes from a provider; no "radio group" model (G8)

- What: `setCheckedProvider(std::function<bool()>)` answers "is this mode on right now"; with a provider the item becomes checkable and is re-evaluated on every refresh.
- Why: the mutual exclusion is a business fact (the current shading mode), not a general ribbon concept; the five buttons' providers read the same target, so exactly one of them is true.
- Boundary: a provider that throws is logged as a warning and the previous state is kept; with no provider the checked state belongs entirely to the caller.

### 2.7 An item with no command subscribes to nothing and changes nothing (G8)

- What: after `setCommand("")`, or when no command was ever set, the binding holds no subscription and writes no enabled / tooltip / checked state.
- Why: a listener with no consumer is noise, and a button the caller manages itself must not be "helpfully" reset by the framework.
- Boundary: binding a command again restores the subscriptions.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Enabled state follows the command | registration, user preference and target requirement decide together | landed |
| F2 | Target requirements grey the item out | a `NeedsDocument` / `NeedsView` command with no target only greys the item | landed |
| F3 | The description as the tooltip | the caller may take over | landed |
| F4 | The checked-state provider | re-evaluated on every refresh | landed |
| F5 | Every item kind covered | buttons, drop-down items, application menus, Backstage | landed |

### 3.1.1 F1 The enabled state follows the command

- **Behaviour**: `setCommand(name)` evaluates immediately and recomputes on `commandsChanged` / `currentChanged`.
- **Constraints** (C1): unregistered and user-disabled both show as not executable; the state is displayed only and no preference is written.
- **Requirements** (R1): after a command is registered again or enabled, the item recovers by itself.
- **Acceptance** (T1): `GuiTest.RibbonItemsFollowTheCommandState`.
- **Caveat** (N1): an item built while its command is not yet registered starts greyed out and recovers when the registration event arrives.

### 3.1.2 F2 Target requirements grey the item out

- **Behaviour**: `CommandFlags` come from `CommandManager::commandFlags()`; the target comes from `resolveCommandTarget(MainWindow::current())`.
- **Constraints** (C2): only the item's clickability is lowered; the command's state is unchanged and nothing is written to the configuration.
- **Requirements** (R2): `NeedsDocument` looks at `target.document`, `NeedsView` at `target.view` (a view counts only when it has a 3D area).
- **Acceptance** (T2): `GuiTest.RibbonItemsFollowTheResolvedTarget`.
- **Caveat** (N2): the current document with no view resolves to an empty target (`resolveCommandTarget` takes the document from the view).

### 3.1.3 F3 The description as the tooltip

- **Behaviour**: `commandDescription(name)` is written into the tooltip and follows a change of description.
- **Constraints** (C3): different text set by the caller wins, and from then on the binding does not overwrite it.
- **Requirements** (R3): an empty description clears the tooltip the binding wrote instead of leaving the old text.
- **Acceptance** (T3): the tooltip assertions of `GuiTest.RibbonItemsFollowTheCommandState`; `GuiTest.RibbonMenusShowCommandTooltips` pins down that a menu really shows it.
- **Caveat** (N3): a menu does not show a `QAction` tooltip by default, see P4.

### 3.1.4 F4 The checked-state provider

- **Behaviour**: `setCheckedProvider(fn)`; every refresh calls `fn()`, and when it has a value the item is made checkable and its checked state written.
- **Constraints** (C4): the provider belongs to the bound command and has no effect without one; an exception is a warning only.
- **Requirements** (R4): the state is re-evaluated after the command executes (`executed`), so "this mode was just clicked" shows as checked immediately.
- **Acceptance** (T4): `GuiTest.RibbonItemsFollowTheCommandState`; `ModelViewerTest.TheShadingModeCommandsDriveTheCurrentModelView` pins down that the provider reads the very 3D area the command acts on.
- **Caveat** (N4): the provider reads the request (`RenderControl::viewerOptions().mode`) rather than the chain, so there is an answer even before the surface is attached.

### 3.1.5 F5 Every item kind covered

- **Behaviour**: ribbon buttons, button drop-down items, application-menu items, Backstage items and quick-access items are all `RibbonButton` / `RibbonAction`.
- **Constraints** (C5): no new item type is introduced; one command may appear in several items, each refreshing itself.
- **Requirements** (R5): a Backstage item keeps no state of its own (`QToolButton::setDefaultAction` follows the action).
- **Acceptance** (T5): `GuiTest.Backstage_PageItemsAreCommandBound` plus a review of `RibbonAction`'s binding.
- **Caveat** (N5): two items for one command hold two subscriptions; the count is small and the price buys independence.

## 4. Public Interface

### 4.1 CommandManager

| Interface | Contract | Note |
|---|---|---|
| `commandFlags(name)` | the flags cached at registration; an alias resolves the same way | `nullopt` when unregistered |
| `commandDescription(name)` | the description cached at registration | `nullopt` when unregistered |
| `isCommandEnabled(name)` | whether it can be executed | false for both disabled and unregistered |

### 4.2 RibbonButton / RibbonAction

| Interface | Contract | Note |
|---|---|---|
| `setCommand(name)` / `command()` | the command name is the identity; once bound the state is derived | an empty name unbinds |
| `setCheckedProvider(fn)` | the optional checked-state answer | with no provider the checked state is the caller's |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- C1: only the item's display state is written, never `setCommandEnabled()`.
- C2: a target requirement is resolved on the application thread; `executed` from another thread is marshalled first.
- C3: the tooltip is only updated where the binding itself wrote the text.
- C4: a provider only takes effect with a named command; its exception is not propagated.
- C5: no new item type and no second identity.

### 5.2 Requirements (R#)

- R1: after a registration or enable change the item becomes clickable again by itself.
- R2: a missing target greys the item out without rewriting the command.
- R3: a change of description reaches the tooltip; once the caller takes over, its text is kept.
- R4: the checked state is re-evaluated after the command executes.
- R5: the rule for every command item has one implementation, `CommandBinding`.

### 5.3 Invariants (N#)

- N1: an item stores the command name only and caches no enabled / description / checked state.
- N2: an unbound item holds no subscription.
- N3: one refresh reads the command registry at most three times (enabled / flags / description) and instantiates no command.
- N4: the refresh happens on the application thread.

## 6. Caveats and Traps

### 6.1 P1 Calling setEnabled() on a bound item

- How it fails: the caller believes it disabled something, and the next refresh restores the derived value.
- How to detect it: in `RibbonItemsFollowTheCommandState`, enabled follows the command manager only.
- How to prevent it: for a lasting disable use `CommandManager::setCommandEnabled()`; the header says so.

### 6.2 P2 The command is not registered yet when the UI is built

- How it fails: the button starts greyed out and is mistaken for broken.
- How to detect it: `commandsChanged` restores it once the registration arrives, and the registration is visible in the log.
- How to prevent it: the host registers commands before it builds the items (which is the order app_shell and model_viewer use).

### 6.3 P3 A view registered later than its document

- How it fails: the current document's view registers after `currentChanged`, so the target is never resolved again.
- How to detect it: it needs a change event from the view registry, which does not exist today.
- How to prevent it: a plugin registers its views in `load()`, before any document opens; that is the only order supported today.

### 6.4 P4 A menu does not show tooltips

- How it fails: the description exists, and hovering a drop-down or application-menu item shows nothing.
- How to detect it: the menu needs `setToolTipsVisible(true)` (the framework turns it on wherever it creates a menu).
- How to prevent it: `GuiTest.RibbonMenusShowCommandTooltips`; a new place that creates a menu must use the same setting.

### 6.5 P5 A provider throws

- How it fails: the refresh is interrupted and the button stops half-way.
- How to detect it: the log contains "checked-state provider threw".
- How to prevent it: a provider reads state and does no IO; after an exception the item keeps its previous checked state.

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | Every host or plugin subscribing and refreshing its own items | N implementations of one rule inevitably drift (G5, G6, G7) | §2.2 of this page |
| D2 | `setCommandEnabled()` to express "not applicable right now" | It writes a temporary context into a persisted preference (G1, G4) | `appfw-command-manager.md` N3 |
| D3 | A global "currently checked item" table or a radio-group model | The exclusion is a business fact and a general mechanism has no consumer (G8) | §2.6 of this page |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | enabled follows registration and preference | `GuiTest.RibbonItemsFollowTheCommandState` |
| T2 | a target requirement drives the grey-out | `GuiTest.RibbonItemsFollowTheResolvedTarget` |
| T3 | the description becomes the tooltip, the caller wins | `GuiTest.RibbonItemsFollowTheCommandState` |
| T4 | the provider and the command act on the same area | `GuiTest.RibbonItemsFollowTheCommandState`, `ModelViewerTest.TheShadingModeCommandsDriveTheCurrentModelView` |
| T5 | a single read of flags and description, through aliases | `GuiTest.CommandManager_FlagsAndDescriptionResolveThroughAliases` |
| T6 | a menu shows its items' tooltips | `GuiTest.RibbonMenusShowCommandTooltips` |
| T7 | a command that finished on a worker thread is refreshed through the dispatcher | `GuiTest.RibbonItemStateFromAWorkerThreadIsMarshalled` |

## 9. Status and Evolution

### 9.1 Current status

F1–F5 are in place; `test_gui` is green; model_viewer's five shading buttons use a provider.

### 9.2 Known gaps / backlog

- Multiple windows: refresh and dispatch both ask `MainWindow::current()`; with a second window the target resolves per window (see `../backlog/appfw-ribbon.md`).
- A grey-out during a `LongRunning` command, contextual tabs and an id for a control without a command have no consumer; they reopen on the triggers in `../backlog/`.
- A change in the view registry does not re-resolve the target (P3); the registry gains an event when that is needed.

### 9.3 Compatibility / migration

- `CommandManager::commandFlags/commandDescription` and `setCheckedProvider` are non-virtual additions, so the plugin ABI is unchanged.
- The click semantics of `setCommand()` are unchanged; once bound, enabled / tooltip / checked become derived display state.

## 10. References

### 10.1 Code

- `src/fw/appfw/src/gui/CommandBinding.hpp` (the single implementation of the rule)
- `src/fw/appfw/src/gui/RibbonButton.cpp`, `RibbonAction.cpp` (writing back to the native control)
- `src/plugins/model_viewer/src/commands/ShadingModeCommands.hpp` (the `isCurrentShadingMode` provider)

### 10.2 Documents and ADRs

- `appfw-command-manager.md` (the single-entry reads of §4.1, N3)
- `appfw-ribbon.md` (item identity and placement)
- `appfw-backstage.md` (an item is a command action)

### 10.3 External material

- Qt's `QAction` / `QToolButton::setDefaultAction`: an action carries enabled / checkable / checked / toolTip.
- The Office ribbon: enabling at command level and iconographic state.
