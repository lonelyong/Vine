# progress backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

Empty: every item below waits for the event named in its trigger column.

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Consumer-less scope accessors and the inert default constructor | `name` / `parent` / `indicator` / `localPos` / `localLength` / `globalLength` and `ProgressScope()` have no caller in `src/` or `tests/`; `localPos()` of an inert scope reports its length, which reads as "complete". Delete them, or give `localPos()` a defined answer for the inert case, when the surface is next opened. | the next deliberate change to `ProgressScope`'s public surface, or the first consumer that needs a nested local position (a stage breadcrumb or a per-stage bar) | `../design/base-progress.md` 4.3, 9.2 |
| T2 | Stale-stage detection | A scope that survives `ProgressIndicator::start()` reports its remainder into the new run - measured as the new run reaching `0.8` before any item. The contract is documented, not enforced: add a debug assertion (a generation the scope captures) or a `detach()` call. | the first caller that restarts an indicator while a stage may still be alive; `StartupProgress` destroys its stage first today | `../design/base-progress.md` 6.2 |
| T3 | `max <= 0` policy | `ProgressScope(range, name, 0)` clamps the local length to `kMinLength` and completes instantly, so a computed zero total reports 100 % before any work. Decide between an assertion and an explicit "inactive stage" outcome. | the first caller that passes a computed total which can be zero | `../design/base-progress.md` 6.5 |
| T4 | Non-throwing position callback | `complete()` runs the position callback from `~ProgressRange` and `~ProgressScope`, so a throwing callback terminates; `Signal::trigger` does not catch either. Enforce it in the type (a callable the type can check) or at the install site instead of by documentation. | the first callback that can throw, or the next change to `setPositionCallback` | `../design/base-progress.md` 6.6 |
| T5 | appfw plugin-ABI coupling | `ProgressHost` embeds `ProgressIndicator` by value, so a layout change here silently changes appfw's plugin ABI - `VN_APPFW_PLUGIN_ABI_VERSION` compares a declared number, not a layout. Bump it in the same commit that changes this module's layout. | the next batch that changes the layout of `ProgressIndicator` or `ProgressScope` | `../design/base-progress.md` 9.3 |
