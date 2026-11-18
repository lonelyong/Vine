# Ribbon identity, roles and placement

> **Status**: current
> **Last verified**: 2026-10-28
> **Scope**: the identity (id), semantic role, placement, terminal zone and order resolution of ribbon tabs and groups; the display state of command items is in `appfw-command-binding.md`; user-defined layout and contextual tabs are not covered.
> **Sources**: `src/fw/appfw/sdk/vine/appfw/gui/RibbonPlacement.hpp`, `RibbonBar.hpp`, `RibbonTab.hpp`, `RibbonGroup.hpp`; the cases in `tests/test_gui/test_gui.cpp`.
> **Backlog**: `../backlog/appfw-ribbon.md`

## 1. Overview

### 1.1 Problem and background

A tab's identity is a **title string** and its order is **append**: `RibbonBar::addTab()` can only put a tab at the end, and `RibbonTab` / `RibbonGroup` have no id.
So a plugin cannot say "I go before Help", cannot reliably find "Help" to add something into it, and every reference breaks when the title changes (translation, rename) — app_shell builds "plugins / help" in `load()` and model_viewer appends "model" in `postLoad()`, which is exactly how "model" ends up after "help".

### 1.2 Goals

- A tab has a **stable id** (the key for references, dedup and persistence); the title is for display only.
- The framework defines a **vocabulary of common roles** (`RibbonTabRole`, extensible the Qt `UserRole` way), so a plugin can anchor **independently of the host**.
- Tabs and groups support `before` / `after` anchors and `append`; the framework resolves the order, so a plugin computes no index.
- A tab such as "help" uses the `Terminal` policy to stay **last forever**; any insertion is clamped in front of the terminal zone.
- A plugin can find a standard tab by role or id and add groups into it.
- The existing `addTab(tab)` / `addGroup(group)` calls keep their meaning and break no plugin ABI.

### 1.3 Non-goals

- No user customisation (hide / drag / save a layout) — there is no consumer (G8), see the backlog.
- No contextual tabs (shown or hidden by a condition).
- No XML or manifest declaration; a plugin is in-process C++ and the code API is enough (G8).
- No forced second id for a button or action: its identity is the command name (see F6).

### 1.4 One-paragraph conclusion

**Identity is an id, semantics are a role, order is a placement; the framework resolves the order from the declarations, and the terminal policy is an invariant the resolver guarantees rather than a convention.**

## 2. Design Points

### 2.1 Identity, role and command are three separate things (G1)

- What: `id` answers "which one", `role` answers "what is this position for", `command` answers "what happens on click".
- Why: one concept, one definition. Taking the display title as the identity fails on translation or a rename, and giving a button an id of its own would make a third identity beside the command name.
- Boundary: a group has an id only (scoped to its tab); an item has a command name only (an optional id is reserved for a control without a command and is not implemented).

### 2.2 The order is resolved; the physical order is only a mirror (G1, G11)

- What: `RibbonBar` keeps the registrations (tab + placement + registration order), recomputes the logical order on every structural change and then aligns SARibbon's category order with `moveCategory()`.
- Why: `tabAt(i)`, `currentIndex()` and SARibbon's physical order must have one truth; `insertCategoryPage(index)` plus a private vector drifts easily.
- Boundary: the current tab is restored after a reorder; nothing outside SARibbon touches the category order directly.

### 2.3 An anchor resolves only against registered nodes (G3, G8, G11)

- What: the registration sequence (`next_seq`) fixes the resolution direction: a new item can only anchor to a tab or group that is **already registered**, and falls back when it cannot find one.
- Why: one-directional resolution is acyclic by construction and needs no dependency graph or cycle detection; the host builds its standard tabs first and plugins register later, which is exactly this direction.
- Cost: anchoring to a tab of a plugin that loads later falls back (`PlacedByFallback` plus a log line). That is deliberate: the order must not depend on unknown information beyond the plugin scan order.

### 2.4 The terminal zone is an invariant, not a convention (G4)

- What: tabs with `RibbonTabPolicy::Terminal` form a suffix; every insertion of a non-terminal tab is `min(index, firstTerminal)`.
- Why: a request such as `after(Help)` has to fail **explainably**: the tab still appears (no UI is lost), the status is `ClampedBeforeTerminal`, and the log says so.
- Boundary: `Terminal` is declared explicitly by the host in its catalogue, **never inferred from role == Help** — an implicit convention is exactly what is being removed.

### 2.5 A failure has a status and a log line (G4, G11)

- What: `RibbonPlaceResult{status,index}` returns `Placed / PlacedByFallback / ClampedBeforeTerminal / DuplicateId / AlreadyAdded / InvalidAnchor`.
- Why: Blender's string categories and Eclipse's locationURI both land silently in the wrong place; being locatable has to be part of the API.
- Boundary: the log records one fact once, and nothing throws (building a UI must not crash because a layout request failed).

### 2.6 The mechanism belongs to the framework, the catalogue to the host (G2)

- What: appfw provides the id / role / placement / policy and the resolver; the ids, titles, roles and policies of tabs such as "plugins / help" are declared by app_shell.
- Why: appfw is the host framework and must not know which tabs an application has; a plugin depends on appfw only and points at a standard slot by role, never at app_shell's headers.
- Boundary: `RibbonTabRole` is the framework's vocabulary (stable values, only ever added to); a concrete tab's id lives in the host's namespace.

### 2.7 Extension points only for the layer that has a consumer (G8)

- What: v1 has a tab-level role plus tab- and group-level id / placement; `order`, item ids, user customisation and contextual tabs are not in the code.
- Why: a field with no consumer becomes a contract to maintain, and the registration order already settles today's ties (the plugin load order is deterministic).
- Boundary: a new need reopens the topic on the triggers in §9.2 and nothing is reserved in advance.
  The display state of a command item is a different subject, see `appfw-command-binding.md`.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Tab identity and policy | the three attributes id, role and policy; set before the add | landed |
| F2 | Tab placement | append / before / after (by id or role); terminal-zone clamping | landed |
| F3 | Tab lookup | find a tab by id or by role | landed |
| F4 | Group identity and placement | group id + before / after / append; adding a group to a standard tab | landed |
| F5 | Legacy interface compatibility | `addTab(tab)` / `addGroup(g)` still append | landed |
| F6 | Item identity | a button's or action's identity is the command name; no new id | landed (contract) |
| F7 | Persisting a user-defined layout | hide / move / save by id | not done, see backlog |
| F8 | Contextual tabs | shown or hidden by a condition | not done, see backlog |

### 3.1.1 F1 Tab identity and policy

- **Behaviour**: `RibbonTab::setId/setRole/setPolicy`; an empty id means "append only, never an anchor target"; `RibbonTabRole::None` means no semantic role.
- **Constraints** (C1, C2, C4): the attributes are set before the add, and a change afterwards does not affect this resolution; role values enter the ABI and are only ever added to.
- **Requirements** (R1): a `Terminal` tab is in the terminal zone.
- **Acceptance** (T1): `RibbonBar_RolesAndPlacement`.
- **Caveat** (N1): a second registration with the same id is refused.

### 3.1.2 F2 Tab placement and the terminal zone

- **Behaviour**: `addTab(tab, placement)`; `Append` puts the tab at the end of the non-terminal zone; `Before/After` find the anchor by id or by role; a request past the terminal zone is clamped back.
- **Constraints** (C3): the anchor must be a registered tab; otherwise it falls back and returns `PlacedByFallback`.
- **Requirements** (R2, R3): every non-terminal tab is in front of the terminal zone; a failure returns a status and is logged.
- **Acceptance** (T2, T3, T4): `RibbonBar_TerminalTabStaysLast`, `RibbonBar_UnknownAnchorFallsBack`, `RibbonBar_AfterHelpIsClamped`.
- **Caveat** (P1): if the host forgets to mark "help" `Terminal`, there is no terminal zone.

### 3.1.3 F3 Tab lookup

- **Behaviour**: `findTab(id)` looks up exactly; `findTab(role)` returns the first tab with that role in logical order; `role == None` and an empty id always return `nullptr`.
- **Constraints** (C4): an id is unique within the bar.
- **Requirements** (R4): lookup follows the logical order and ignores SARibbon's physical index.
- **Acceptance** (T5): `RibbonBar_FindByRoleAndId`.

### 3.1.4 F4 Group identity and placement

- **Behaviour**: `RibbonGroup::setId`; `RibbonTab::addGroup(g, placement)`, `findGroup(id)`; the anchor is a group id (a role anchor returns `InvalidAnchor` at group level).
- **Constraints** (C5): a group id is unique within its tab; an empty id may repeat.
- **Requirements** (R5): a plugin can contribute to a standard tab with `findTab(RibbonTabRole::Help)->addGroup(...)`.
- **Acceptance** (T6): `RibbonTab_GroupPlacementAndContribution`.
- **Caveat** (P2): do not pass a role to a group-level placement.

### 3.1.5 F5 Legacy interface compatibility

- **Behaviour**: `addTab(RibbonTab*)` is equivalent to `addTab(tab, RibbonPlacement::append())`; `addGroup(RibbonGroup*)` is equivalent to appending.
- **Constraints** (C6): no existing signature or return type changes; the new capability arrives only as overloads and new methods.
- **Requirements** (R6): app_shell, model_viewer and the tests compile unchanged and keep their append semantics.
- **Acceptance** (T7): `RibbonBar_LegacyAddStillAppends` plus the existing ribbon cases.

### 3.1.6 F6 Item identity

- **Behaviour**: `RibbonButton::command()` / `RibbonAction::command()` is the item's identity; a control without a command has no placement API yet.
- **Constraints** (C7): no separate id or role is invented for an item (G1, G8).
- **Requirements** (R7): when customisation is built, it keys on the command name, and only a control without a command gets an explicit id.
- **Acceptance** (T8): a design review plus the backlog trigger.
- **Caveat** (P3): the same command may appear in two groups (the command is the action, the button is a view).

## 4. Public Interface

### 4.1 RibbonPlacement (`RibbonPlacement.hpp`)

| Interface | Contract | Note |
|---|---|---|
| `RibbonPlacement::append()` | append at the end of the non-terminal zone | the full signature is in the header |
| `before/after(id)` | anchor to a sibling id in the same scope | falls back when not found |
| `before/after(role)` | tab level only | returns `InvalidAnchor` at group level |
| `RibbonPlaceResult` | `status` + the final logical index | `index == -1` means it was not placed |

### 4.2 RibbonBar

| Interface | Contract | Note |
|---|---|---|
| `addTab(tab)` | the legacy append entry | the original signature is kept |
| `addTab(tab, placement)` | resolves the placement and reorders | returns the status and the index |
| `findTab(id)` / `findTab(role)` | lookup in logical order | an empty id / None returns nullptr |
| `numTabs()/tabAt(i)` | logical order | agrees with the physical order (N2) |

### 4.3 RibbonTab

| Interface | Contract | Note |
|---|---|---|
| `setId/id` | the stable identity | set before the add |
| `setRole/role` | the semantic role | the framework vocabulary + UserRole |
| `setPolicy/policy` | `Terminal` and friends | used by the host's catalogue |
| `addGroup(g)` | the legacy append entry | the original signature is kept |
| `addGroup(g, placement)` | group-level placement | the anchor is an id only |
| `findGroup(id)` | group lookup | an empty id returns nullptr |

### 4.4 RibbonGroup

| Interface | Contract | Note |
|---|---|---|
| `setId/id` | unique within the tab | an empty id may repeat |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- C1: id / role / policy are set before `addTab`; the resolver reads the values as they were when the tab joined.
- C2: the numeric values of `RibbonTabRole` are part of the plugin ABI: only added to, with gaps left.
- C3: an anchor resolves only against an already registered (earlier in registration order) sibling.
- C4: a tab id is unique within the bar; ids are ASCII and never translated.
- C5: a group id is unique within its tab; a group-level anchor is an id only.
- C6: the existing public signatures and return types are unchanged.
- C7: an item's identity is the command name.

### 5.2 Requirements (R#)

- R1: a `Terminal` tab is always in the terminal zone.
- R2: no non-terminal tab may appear after the terminal zone.
- R3: a failure returns a status and is logged; nothing throws and nothing is silent.
- R4: after a reorder the previous current tab is kept (when it is still there).
- R5: a plugin can find a standard tab by role and contribute a group to it.
- R6: existing call sites behave as before.
- R7: an item gets no second identity.

### 5.3 Invariants (N#)

- N1: `RibbonBar`'s entry list *is* the logical order; SARibbon's category order agrees with it after every structural change.
- N2: the terminal tabs form a suffix of the logical order; a terminal tab's own relative placement is ignored and it is ordered by registration sequence within the terminal zone.
- N3: one tab pointer is registered once and one group pointer is registered once.
- N4: an empty id takes no part in uniqueness or anchor matching.

## 6. Caveats and Traps

### 6.1 Forgetting to declare the terminal policy
- How it fails: `after(Help)` really lands after help.
- How to detect it: the `RibbonBar_TerminalTabStaysLast` case; check for `setPolicy(Terminal)` in the catalogue.
- How to prevent it: a catalogue template in the host, and a code review of the policies.

### 6.2 Changing id or role after the add
- How it fails: `findTab` does not find it, and later anchor resolution uses the new value while earlier entries hold the old one.
- How to detect it: the lookup returns nullptr, and `PlacedByFallback` appears in the log.
- How to prevent it: the documentation requires setting the attributes before the add; v1 adds no "changed" notification inside the wrapper (G8).

### 6.3 Anchoring to a tab that registers later
- How it fails: `PlacedByFallback`, so the tab lands at the end of the non-terminal zone instead of next to its target.
- How to detect it: the returned status plus the log line.
- How to prevent it: the host builds its standard tabs first; plugins do not anchor to each other unless the load order is stated.

### 6.4 A duplicate id
- How it fails: two tabs fight over one id, and lookup and persistence become ambiguous.
- How to detect it: `DuplicateId`, and the second registration is refused.
- How to prevent it: name ids `<vendor>.<plugin>.<name>`; use an empty id only for a one-off tab.

### 6.5 The physical order changed from outside
- How it fails: `tabAt(i)` disagrees with what is displayed.
- How to detect it: the existing cases assert `tabAt(currentIndex())`.
- How to prevent it: never move or remove at the SARibbon level; go through `RibbonBar`.

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | A global numeric priority or sequence only | It cannot express "before help", and inserting a standard tab renumbers everything (G4, G9) | `../decisions/ADR-2026-10-28-ribbon-roles-and-placement.md` |
| D2 | Id anchors only (the IntelliJ way) | With the id absent in a host the placement silently drifts, and a plugin is not portable across hosts (G4, G11) | same |
| D3 | Role anchors only | It cannot point at one specific custom tab or group (G4) | same |
| D4 | A forced id on every item too | It makes a third identity beside the command name (G1, G8) | same |
| D5 | A declarative manifest or XML | There is no out-of-process plugin consumer (G8) | same |
| D6 | A full dependency graph with cycle detection | One-directional resolution by registration order is enough, and the extra mechanism has no consumer (G8, G11) | same |
| D7 | Automatic termination when `role == Help` | An implicit convention, whereas termination has to be an explicit policy (G4) | same |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | role / id / policy take effect and the order matches the placement | `GuiTest.RibbonBar_RolesAndPlacement` |
| T2 | a terminal tab stays last | `GuiTest.RibbonBar_TerminalTabStaysLast` |
| T3 | an unknown anchor falls back and still lands before the terminal zone | `GuiTest.RibbonBar_UnknownAnchorFallsBack` |
| T4 | `after(Help)` is clamped back and reported | `GuiTest.RibbonBar_AfterHelpIsClamped` |
| T5 | lookup by id and by role | `GuiTest.RibbonBar_FindByRoleAndId` |
| T6 | group placement and contributing to a standard tab | `GuiTest.RibbonTab_GroupPlacementAndContribution` |
| T7 | the legacy append semantics are unchanged | `GuiTest.RibbonBar_LegacyAddStillAppends` plus the existing ribbon cases |
| T8 | an item's identity is its command name | a design review (F6) |

## 9. Status and Evolution

### 9.1 Current status

F1–F6 are in place; the `test_gui` cases cover placement and the terminal zone; `RibbonBar` owns the SARibbon order mirror.
The display state of a command item is in `appfw-command-binding.md`.

### 9.2 Known gaps / backlog

See `../backlog/appfw-ribbon.md`; it is the one list of what is not in the code, and a gap leaves it when it lands.

### 9.3 Compatibility / migration

- `addTab(tab)` / `addGroup(g)` behave as before; every new capability is an overload or a new method.
- The click semantics of `setCommand()` are unchanged (the item state is in `appfw-command-binding.md`).
- No virtual function changes: the new state of `RibbonTab` / `RibbonGroup` / `RibbonBar` lives in `Impl` and changes no class layout.
- Migration advice: give a standard tab an id, role and policy; a plugin tab uses `before(RibbonTabRole::Help)` or `append()`.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/gui/RibbonPlacement.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/Gui.hpp` (`RibbonTabRole`, `RibbonTabPolicy`)
- `src/fw/appfw/src/gui/RibbonBar.cpp` (the resolver and the physical mirror)
- `src/plugins/app_shell/src/AppShellUi.cpp` (the standard catalogue)
- `src/plugins/model_viewer/src/ModelViewerPlugin.cpp` (`before(Help)`, the checked-state provider of the five shading buttons)

### 10.2 Documents and ADRs

- `appfw-command-binding.md` (the display state of a command item)
- `../decisions/ADR-2026-10-28-ribbon-roles-and-placement.md`

### 10.3 External material

- Qt's `Qt::ItemDataRole` / `Qt::UserRole`: the pattern of predefined roles plus a custom range.
- Office's custom UI XML: relative positioning through `insertBeforeMso` / `insertAfterMso`.
- Visual Studio VSCT: stable command ids plus `priority`.
- The IntelliJ Platform: `group-id` + `anchor` + `order`.
