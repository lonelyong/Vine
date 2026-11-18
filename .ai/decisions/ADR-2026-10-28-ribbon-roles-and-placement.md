# ADR-2026-10-28: ribbon placement uses role, id and anchors, not a global ordinal

- Status: Accepted
- Date: 2026-10-28
- Related: `../design/appfw-ribbon.md`, `tests/test_gui/test_gui.cpp`

## Background

The ribbon could only append tabs, and a tab's identity was its display title. app_shell created "Plugins/Help" in `load()`, model_viewer appended "Model" in `postLoad()`, and "Model" therefore ended up after "Help". A plugin could not express "before Help", nor reliably find a standard tab to add a group to; and renaming a title (translation, rename) broke every reference.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. A global numeric priority (`order` / `priority`) | The simplest implementation, and sorting is a total order | Cannot express "before Help"; inserting a standard tab means renumbering everything; plugins can only guess relative to each other |
| B. Id anchors only (the IntelliJ shape) | Precise, relative placement | Silently wrong when the host has no such id; strong coupling between plugin and host ids |
| C. Role anchors only | A plugin works across hosts and versions | Cannot point precisely at one custom tab or group |
| D. Role plus id plus placement (this decision) | A semantic default plus precise targeting; a fallback when an anchor is missing; a guaranteed terminal zone | One more vocabulary and one resolver; role values enter the ABI |
| E. A declarative manifest or XML | Out-of-process plugins could declare UI | Only in-process C++ plugins exist today (violates G8); parsing and validation cost a lot |

## Decision

Chose D:

- **id** is the node's identity (a tab is unique within the bar, a group within its tab); the title is display only.
- **role** is a semantic vocabulary the framework defines (`Home/View/Tools/Plugins/Help`, custom roles from `UserRole + n`), used as a host-independent anchor and lookup key; role values only grow and enter the plugin ABI.
- **placement** is `append / before / after`, and the anchor is a sibling id or, at tab level, a role.
- **The terminal policy is declared explicitly**: `RibbonTabPolicy::Terminal` forms a suffix; a non-terminal insert is clamped in front of the first terminal tab, returning `ClampedBeforeTerminal` and logging (no lost UI, nothing silent).
- **An anchor resolves only against already registered siblings** (earlier in registration order); if it is not found, the node falls back to the end of the non-terminal zone and returns `PlacedByFallback`. One-way resolution has no cycles by construction, so no dependency graph or cycle check is needed.
- **v1 introduces no `order` field**: ties use registration order (the plugin load order is deterministic: directory sort plus dependency order).
- **Item identity is the command name**: no second id is imposed on buttons or actions; it is added when there is a consumer for command-less controls or custom layouts.

## Consequences

- Good:
  - A plugin can say "before Help" or "add into Help" without knowing the host's version or language.
  - The order is declarative, and after an anchor is removed its dependents flow back by re-resolution.
  - Failures are locatable: a status plus a log line, so a test can assert "clamped back" or "fell back".
  - The old `addTab(tab)` / `addGroup(g)` semantics are unchanged; no virtual function changes, so the plugin ABI is not bumped.
- Bad / cost:
  - One more role vocabulary and resolver (about 150 lines).
  - Anchoring to a tab registered later falls back - plugins that anchor to each other need an explicit registration order.
  - The host must remember to mark "Help" as `Terminal`, or the terminal zone does not exist (mitigated by the case and the directory template).
  - The order of ties on one anchor depends on plugin load order; business ordering waits for `order` (see the backlog trigger).
- Trigger (when to reopen):
  - two plugins need a stable business order on the same anchor rather than load order;
  - user-defined layouts appear (which need item ids and persistence);
  - out-of-process plugins appear (which need a declarative manifest).

## Evidence

- Cases: `GuiTest.RibbonBar_RolesAndPlacement`, `RibbonBar_TerminalTabStaysLast`,
  `RibbonBar_UnknownAnchorFallsBack`, `RibbonBar_AfterHelpIsClamped`, `RibbonBar_FindByRoleAndId`,
  `RibbonTab_GroupPlacementAndContribution`, `RibbonBar_LegacyAddStillAppends`.
- Code: `src/fw/appfw/src/gui/RibbonBar.cpp` (the resolution and the SARibbon mirror),
  `src/fw/appfw/sdk/vine/appfw/gui/RibbonPlacement.hpp`.
- Gates: `test_gui`, `check_public_docs.py`, `check_include_hygiene.py`, `check_ai_docs.py --strict`, `check_design_docs.py`.
