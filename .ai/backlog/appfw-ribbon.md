# appfw Ribbon backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

(none)

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Explicit `order` / side-by-side ordering | ordering on one anchor by business order instead of registration order | two plugins anchor at the same place and need a stable business order (not the load order) | `../design/appfw-ribbon.md` §9.2 |
| T2 | an id for a control without a command | a button or action keeps the command name as its identity (F6, settled); what is missing is an explicit id for a control that has no command, plus an id on `RibbonGroup::addControl` | user-defined layouts land, or one group holds two command-less controls that must be told apart | `../design/appfw-ribbon.md` F6 |
| T3 | Persisting a user-defined layout | hiding/moving a tab/group/item by id and saving it | the product asks for a "user-customisable ribbon" | `../design/appfw-ribbon.md` F7 |
| T4 | Contextual tabs | showing/hiding a tab by a condition | a real feature appears that shows only for a selected kind of object | `../design/appfw-ribbon.md` F8 |
| T5 | Declarative contribution (a manifest) | a plugin declares tab/group in a manifest and the host parses it | a plugin distribution route appears beyond out-of-process/third-party binaries | `../design/appfw-ribbon.md` D5 |
| T6 | group-level role | a group can be anchored semantically too | a standard tab holds several extensible semantic groups and the id cannot express them | `../design/appfw-ribbon.md` §2.7 |
| T8 | Recent files / MRU | the Backstage "Recent" page and the file entry | the product asks for a recently-opened list and a persistence location already exists | `../design/appfw-backstage.md` F8 |
