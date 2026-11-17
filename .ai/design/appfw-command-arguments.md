# appfw's command arguments

> **Status**: current
> **Last verified**: 2026-11-03
> **Scope**: tokenising one line of text, the argument carrier, the path they travel, the replayable form recorded in history and events, and the empty-line rule; how a command interprets its arguments is not this page.
> **Sources**: `CommandArguments.hpp`, `CommandManager.cpp`, `tests/test_appfw/CommandArgumentsTest.cpp`.
> **Backlog**: `.ai/backlog/appfw-command-arguments.md` (declared parameters, help / completion, asking for a missing argument, object arguments, an upper length bound).

## 1. Overview

### 1.1 Problem and background

The name entries (console, ribbon, shortcut, macro) carry no caller data of their own, while the instance entry carries it in fields; so every command that needs input either asks the user itself or leaves the host to split the words — and then the quoting rules, the error format and the history grow one copy each.
History records names only, so `open a` and `open b` are two records nothing can tell apart.

### 1.2 Goals

- One line of text is tokenised by one set of rules (quotes, backslashes) shared by the console, the startup arguments, macro replay and the tests.
- Arguments travel with the execution frame, are immutable while it runs, and an empty argument list is an empty container.
- A command interprets its own arguments; the framework does not declare, validate or set them.
- History and events record the canonical, replayable form.

### 1.3 Non-goals

- No declared parameters (type / default / required), help, completion or **generated** prompts for a missing argument — see the backlog; a command asking through `UserIO` itself is allowed (F5).
- No interpretation of `--`, `-l10` or `--name=value`: the tokeniser only splits words, and the meaning belongs to the command.
- No object references (id / name), units or dimensions.

### 1.4 One-paragraph conclusion

**The framework tokenises once and hands one set of tokens along with the execution; the command interprets them; history records the canonical text that tokenises back into the same tokens.**

## 2. Design Points

2.1 **Tokenising belongs to the framework, meaning belongs to the command**: quoting and escaping are global conventions, and a copy per command would inevitably fork; but "what `-l 10` means" is the command's grammar, which the framework does not guess.
The tokeniser does not interpret `--`; it is an ordinary token.

2.2 **The carrier holds tokens only**: `CommandArguments` has value semantics and is immutable, and an empty argument list is an empty container rather than nullptr; it carries no name, no pointer and no type information, and does not keep the original line (`toString()` can already reconstruct it).

2.3 **Passing happens once, at dispatch**: the text entry tokenises and gives the first token to name resolution, copying the rest into the execution frame; all three name entries accept arguments, the instance entry does not (its fields are its arguments).

2.4 **A record is a replay**: `toString()` and `tokenizeCommandLine()` are inverses of each other, so one history entry or one event reconstructs the call.

2.5 **The command line wins; only what is missing is asked for**: a command uses the tokens it was given as values and asks through `UserIO` only for what is absent (`test_createbox` is the model): `test_createbox 10 20 30` interrupts nobody, `test_createbox` asks item by item, and a half-written line asks only for the rest.
A token that is present but wrong is a **caller error** (`Failed` plus usage) and never degrades into a prompt — that would hide the mistake.
This is the shape of a **command**, not framework behaviour: once declared parameters exist, the prompt can be generated from the declaration at dispatch (backlog T3).

2.6 **A variant is an explicit discriminator, not a token count**: different ways of writing the same result (length / width / height, or two opposite corners) are chosen by a mode token: `size` / `corners`, plus the one unambiguous shorthand that remains (three plain numbers = size).
The discriminator is a token, so it enters history and replay; both modes at once are refused outright.
A different result object or a different algorithm is not a variant, it is two commands (G1).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Tokenising text | `tokenizeCommandLine()`: whitespace separated, double quotes group, backslashes follow the Windows C runtime rules | landed |
| F2 | Passing arguments | the three name entries take `CommandArguments`; `context.arguments()` is read-only | landed |
| F3 | History and events | `args_record` and `executing` / `executed` carry the same canonical arguments | landed |
| F4 | The text entry and the empty line | `executeCommandLineAsync()` is the only tokenising point; an empty line is refused before dispatch | landed |
| F5 | Interactive fallback | a complete command line ⇒ run directly; a missing item ⇒ `UserIO` asks; a wrong one ⇒ `Failed` (`test_createbox`) | landed (command side) |
| F6 | Argument variants | an explicit mode token selects `size` / `corners`; mutually exclusive, missing items asked for, coordinates in the same syntax as `getPoint3dAsync` | landed (command side) |

### 3.1.1 F1 Tokenising text

- **Behaviour**: `tokenizeCommandLine(line)` returns the token list; `""` is one empty token; an unterminated quote runs to the end of the line.
- **Constraints** (C1): it only splits words and never interprets option semantics; a backslash escapes only immediately before a quote, so `C:\dir\file` is kept as it is.
- **Requirements** (R1): the quoting rules match the process `argv` (Windows C runtime); every text source shares one implementation.
- **Acceptance** (T1): `CommandArgumentsTest.*`.
- **Caveat** (N1): the dialect is part of the contract; changing it changes what every macro and script means.

### 3.1.2 F2 Passing arguments

- **Behaviour**: `executeCommandAsync/AndWait/Detached(name, args, target)` puts the tokens into the execution frame, and the command reads them through `CommandExecutionContext::arguments()`.
- **Constraints** (C2): value semantics, immutable while the execution runs; arguments are never written into the command object; the instance entry takes no args.
- **Requirements** (R2): an empty argument list is not nullptr; the command may read it on any thread it resumes on.
- **Acceptance** (T2): `CommandArgumentsManagerTest.ArgumentsReachTheCommandThroughTheNameEntry`, `InstanceEntryCarriesNoArguments`.
- **Caveat** (N2): a command that wants fields binds them into a local structure at the top of `execute()`; the framework does not do it for it.

### 3.1.3 F3 History and events

- **Behaviour**: `CommandHistoryEntry::args_record` and the two event parameters carry `args.toString()`.
- **Constraints** (C3): what is recorded is the canonical text, not the user's original input; execution, events and history agree.
- **Requirements** (R3): `tokenizeCommandLine(args_record)` yields the tokens the command saw.
- **Acceptance** (T3): `HistoryRecordsTheReplayableInvocation`, `EventsCarryTheSameArgumentsAsTheExecution`.
- **Caveat** (N3): argument length is bounded only by the number of history entries today, see backlog T5.

### 3.1.4 F4 The text entry and the empty line

- **Behaviour**: `executeCommandLineAsync(line)` tokenises and takes the first token as the name; with no token it returns `Failed`.
- **Constraints** (C4): this is the only place that calls the tokeniser; an empty line creates no instance, enters no history and fires no event.
- **Requirements** (R4): the console ignores an empty line before calling; every other host gets an explicit `Failed`.
- **Acceptance** (T4): `EmptyLineIsRefusedBeforeAnythingRuns`.
- **Caveat** (N4): console completion covers command names only and offers nothing once a separator appears.

### 3.1.5 F5 Interactive fallback

- **Behaviour**: a command with positional arguments consumes `arguments()` first and only `co_await io->getDoubleAsync(...)` for what is missing; when everything is given it asks nothing.
- **Constraints** (C5): a token that exists but cannot be converted ⇒ `Failed` plus usage, never a second prompt; surplus tokens are refused the same way.
- **Requirements** (R5): the prompts and their order are the command's (today); semantic validation (length, width and height must be positive) stays in the command.
- **Acceptance** (T5): a manual console check: `test_createbox 10 20 30` asks nothing, `test_createbox` asks item by item, `test_createbox 10` asks for two, `test_createbox 10 abc 30` ⇒ `Failed`.
- **Caveat** (N5): this exists so a command with arguments works well in the console and in a macro; generated prompts wait for declared parameters (backlog T3), so do not grow a second argument syntax in the framework first.

### 3.1.6 F6 Argument variants

- **Behaviour**: `test_createbox size <length> <width> <height>` and `test_createbox corners <x,y,z> <x,y,z>`; the shorthand `test_createbox <length> <width> <height>` = size. Both end up with the same box.
- **Constraints** (C6): exactly one unambiguous shorthand; the discriminator occupies the first token; a second mode keyword ⇒ `Failed`; each mode fixes its token count and types.
- **Requirements** (R6): the coordinate syntax matches `UserIO::getPoint3dAsync` (exactly three finite numbers as `x,y,z`); only missing items are asked for, and with no token at all the mode is asked first; both modes converge on one result.
- **Acceptance** (T6): manual console: `test_createbox 10 20 30`, `size 10 20 30`, `corners 0,0,0 10,20,30`, `test_createbox` (asks the mode first); errors: `corner ...` (unknown mode), `size 1 2` (missing → asked), `corners 0,0,0` (second point missing → asked), `corners 0,0,0 0,10,20` (collinear axis ⇒ `Failed`), `size 1 2 corners 0,0,0 1,1,1` (both modes ⇒ `Failed`).
- **Caveat** (N6): the discriminator enters `args_record`, so a replay cannot drift into the other mode; a shorthand replay stays deterministic. Once declarations exist this becomes a parameter set (backlog T6).

## 4. Public Interface

### 4.1 `CommandArguments` / tokenising

| Interface | Contract |
| --- | --- |
| `tokenizeCommandLine(const String&)` | pure function, tokenises; one dialect |
| `CommandArguments::tokens()/size()/empty()/operator[]` | a read-only view of the tokens |
| `CommandArguments::toString()` | canonical text; the inverse of the tokeniser |

### 4.2 Entries

| Interface | Note |
| --- | --- |
| `executeCommandLineAsync(line, target)` | the text entry, the only tokenising point |
| `executeCommandAsync(name, args, target)` / `executeCommandAndWait(...)` / `executeDetached(...)` | the name entries, carrying arguments |
| `executeCommandAsync(Command*, target)` | the instance entry, taking no arguments |
| `CommandExecutionContext::arguments()` | this execution's tokens, read-only |

## 5. Constraints, Requirements and Invariants

| # | Constraint |
| --- | --- |
| C1 | Tokenising is lexical only and interprets no options; a backslash escapes only immediately before a quote. |
| C2 | Arguments have value semantics and are immutable during execution; they are never written into the command object. |
| C3 | History and events record the canonical text. |
| C4 | An empty line is refused before dispatch. |
| C5 | A token that exists but cannot be converted ⇒ `Failed`, no second prompt; surplus tokens are refused. |
| C6 | Exactly one unambiguous shorthand; the discriminator occupies the first token; both modes at once are refused. |

| # | Requirement |
| --- | --- |
| R1 | Every text source shares the tokeniser and the dialect. |
| R2 | An empty argument list is an empty container; readable from any thread. |
| R3 | `toString()` round-trips identically. |
| R4 | The console ignores an empty line; every other host gets `Failed`. |
| R5 | The prompts, their order and the semantic validation are the command's; the framework generates no prompt. |
| R6 | The mode fixes the token count and types; coordinates use the `getPoint3dAsync` syntax; both modes produce one result. |

| # | Invariant |
| --- | --- |
| N1 | One execution has one argument list, destroyed with the frame when it ends. |
| N2 | The name and the tokens travel separately; the name takes no token slot. |
| N3 | A variant's discriminator appears in the call record, or the unambiguous shorthand determines it. |

## 6. Caveats and Traps

### 6.1 Treating arguments as command state

- How it fails: they are written into a member, so a second execution on the same instance reads the previous values, and concurrent executions race.
- How to detect it: a command is default-constructed by its factory and destroyed after it runs; the instance entry may reuse one pointer.
- How to prevent it: read `context.arguments()` only; bind fields at the top of `execute()` when they are wanted.

### 6.2 Every command carrying its own quoting rules

- How it fails: `"C:\Program Files\x"` is one token in one command and three in another.
- How to detect it: a second tokenising implementation is the signal.
- How to prevent it: tokenising may only call `tokenizeCommandLine()`.

### 6.3 Storing the original line instead of the canonical form

- How it fails: a replay depends on how the user happened to write the line, so a change of alias or quoting style cannot reproduce it.
- How to detect it: the string in a history entry differs from `toString()`.
- How to prevent it: record `toString()` only; if the original wording must be shown, keep it as a separate field that is not the replay source.

## 7. Rejected Options

| # | Option | Reason |
| --- | --- | --- |
| D1 | The manager setting values into command members | It needs reflection or dispatch on strings, and adds two new invariants: "an instance is used once" and "a half-initialised object must not run". |
| D2 | A value type per argument kind (a full `any` / variant) | Errors move to run time, and it fights the `ConfigManager` value model; tokens are all that is needed today. |
| D3 | The tokeniser interpreting `--` and short options | Without a declaration there is no answer to "who owns the short name"; tokenise first, add semantics when declarations exist. |
| D4 | Inferring the variant from the token count (`0,0,0 10,10,10` vs `10 20 30`) | With an item missing it cannot say which form was meant, a replay cannot decide, and one optional parameter makes it ambiguous. |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | tokenising: quotes, escapes, paths, an unterminated quote | `CommandArgumentsTest` |
| T2 | the three entries see one set of arguments; the instance entry sees none | `CommandArgumentsManagerTest` |
| T3 | history and events replay | `HistoryRecordsTheReplayableInvocation`, `EventsCarryTheSameArgumentsAsTheExecution` |
| T4 | an empty line runs nothing | `EmptyLineIsRefusedBeforeAnythingRuns` |
| T5 | interactive fallback (command side) | console: `test_createbox 10 20 30` / `test_createbox` / `test_createbox 10` / `test_createbox 10 abc 30` |
| T6 | argument variants (command side) | console: `size …` / `corners …` / the shorthand / unknown mode / missing item / both modes at once |

## 9. Status and Evolution

The mechanism is in place; declared parameters, help / completion, prompts for a missing argument, object arguments and an upper length bound follow the triggers in `.ai/backlog/appfw-command-arguments.md`.
`CommandArguments` is a subset of the eventual carrier: once declarations exist it gains typed access and pre-dispatch validation, while `tokens()` stays.
The interactive fallback (F5) and the argument variants (F6) are **command-side** implementations today (`test_createbox`); when a second command needs several forms they move into a parameter set in the declaration (backlog T6).

## 10. References

- `src/fw/appfw/sdk/vine/appfw/CommandArguments.hpp`, `src/fw/appfw/src/CommandArguments.cpp`
- `src/fw/appfw/sdk/vine/appfw/Command.hpp` (`arguments()`), `src/fw/appfw/src/CommandManager.cpp`
- `tests/test_appfw/CommandArgumentsTest.cpp`
- `appfw-command-manager.md` (chains, history, events)
