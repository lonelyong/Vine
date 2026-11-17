# Command arguments backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

The parameter **carrying mechanism** is in place (see `../design/appfw-command-arguments.md`): one line of text split
into tokens, delivered with the execution frame to `CommandExecutionContext::arguments()`, and a canonical form the
history and the event log can replay. What follows is the part that is not done.

## 1. Actionable Now (No Trigger)

(none)

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Declarative parameters | `Command::parameters()` returns `CommandParameter{name, short_name, kind, position, required, default, description}`; registration checks uniqueness, and dispatch parses by the declaration, fills defaults and reports unknown/duplicate/missing parameters. Types come from the declaration, not from the tokens | three or more commands each hand-writing the same `-x value` parsing, or the framework having to generate usage/completion | `../design/appfw-command-arguments.md` §9 |
| T2 | Help and completion | `help <command>` and `--help` generate the parameter table from the declaration; the console completes parameter names from the second word on | after T1 lands (with no declaration there is nothing to complete) | ibid. |
| T3 | Asking for a missing parameter | the framework asks, using `description`, when a required parameter is missing; the programmatic entry just answers `Failed` plus the usage. Today a command asks through `UserIO` itself (`test_createbox` already follows "run straight through when everything is given, ask only for what is missing") | after T1 lands | `../design/appfw-command-arguments.md` F5 |
| T4 | Object parameters | a parameter declared with `object_kind` travels as a stable id, is resolved at dispatch and re-resolved by the command as needed; `document`/`view` fold into `CommandTarget` | a real command that names a document, view or plugin by name/number | `.ai/decisions/ADR-2026-10-24-command-target.md` |
| T5 | Parameter length bounds | bounds on one `args_record` and on the total tokens of one call, so an over-long input cannot hold on to the history | when memory has to be defended beyond the entry-count cap (N6 grows from "bounded count" to "bounded bytes") | `../design/appfw-command-manager.md` F5 |
| T6 | Parameter sets (variants) | the declaration supports `parameter set`: a discriminant plus one parameter table per set; dispatch validates and asks per set, and help/completion group by set | a second command needs several spellings, or the `test_createbox` pattern parsing would have to be copied | `../design/appfw-command-arguments.md` F6 |
