# appfw's CommandManager

> **Status**: current
> **Last verified**: 2026-11-03
> **Scope**: command registration, the execution chain, history, disabling, the result status and carrying arguments; the dialog and the input / output are in `appfw-userio.md`.
> **Sources**: `CommandManager.hpp`, `Command.hpp`, `CommandArguments.hpp`, `tests/test_appfw/CommandArgumentsTest.cpp`.
> **Backlog**: none (open items are in `../backlog/`)

## 1. Overview

### 1.1 Problem and background

A command has four entries (async / sync × by name / by instance), and on top of that it needs nested sub-commands, cancellation, exclusion, history, disabling and "this command does not apply right now".
Each of those turns into a requirement on the same object: a synchronous entry must not block the UI thread, exclusion must stop every live chain rather than the foreground one, history must not keep a pointer to a command that is already destroyed, and `Failed` must not be confused with `NotApplicable`.

### 1.2 Goals

- One command chain = one stack + one cancellation source; chains are independent of each other.
- The top-level entries contain every exception, so none reaches the UI or a `DetachedTask`.
- An exclusive command either takes over or is refused outright; it never runs alongside an older chain.
- History keeps value snapshots only; disabling is a flag in the registry, not a removal.
- `Failed` (an error) and `NotApplicable` (no object right now) stay apart.

### 1.3 Non-goals

- No run-time hot unload of a command; disabling is only a flag.
- No synchronous `executeCommandAndWait()` inside a command (it would deadlock).
- No filtering of the ribbon by document type in the framework.
- No `Scheduler` class extraction; the scheduling logic stays in `Impl`.

### 1.4 One-paragraph conclusion

`CommandManager` runs one top-level execution per chain: nested inside a chain, independent between chains; the top-level entries turn every exception into `Failed`; exclusion stops every live chain before taking over; history keeps values; disabling is a flag; `NotApplicable` is not an error.

## 2. Design Points

2.0 **A command declares which layer it needs, and the framework refuses uniformly**: the application, document and view layers may all be absent, so a command declares its premise with `CommandFlags::NeedsDocument` / `NeedsView`, the refusal happens before dispatch (`NotApplicable` plus which layer is missing), and the command body reads `CommandExecutionContext::target()` instead of checking for null itself; the console and a headless test are legal situations.
The target is resolved **at dispatch** (`gui::resolveCommandTarget()`: the current document's view → its document, and a view only enters the target when it has a 3D area) and travels with the execution, see `../decisions/ADR-2026-10-24-command-target.md`.

### 2.1 The four public entries

All of them are **top-level entries** (`nested=false`); only `CommandExecutionContext::executeChild()` runs with `nested=true`.

| Entry | Use | Thread / blocking |
| --- | --- | --- |
| `executeCommandAsync(Command*)` | top-level async | returns a `Task` at once |
| `executeCommandAsync(name)` | top-level async by name | returns a `Task` at once |
| `executeCommandAndWait(...)` | tools / tests | blocks the calling thread |
| `executeDetached(...)` | ribbon / fire-and-forget | waits for nobody and reports nothing |

A command must **not** use the synchronous entry inside itself; it should `co_await executeCommandAsync(...)` or call `executeChild()`.

### 2.2 The chain model

```
CommandManager ── Impl ─┬─ shared_ptr<Chain> foreground
                        ├─ vector<weak_ptr<Chain>> live_chains
                        ├─ bool foreground_busy
                        ├─ bool exclusive_busy
                        ├─ atomic<uint64_t> cancel_generation
                        ├─ deque<CommandHistoryEntry> history
                        ├─ map<String, RegisteredCommand> registry
                        ├─ map<String, String> aliases
                        ├─ mutex (foreground/live/history/snapshot)
                        └─ registry_mutex (registry/aliases, leaf lock)
Chain ── vector<Command*> commands (the stack, innermost at the tail)
      ├─ int runs (zero = this chain is winding down)
      └─ stop_source (stopped, never replaced)
```

- One chain = one stack + one cancellation source; chains cannot clear, pop or cancel each other.
- The cancellation source only ever `request_stop()`s and is never replaced ⇒ a `stop_token` handed out through a `Context` stays valid.
- A chain's `runs` reaches zero only after its own frame has completely wound down (destroy the `ProgressHost`, pop the stack, and only then `leaveChain()`); the entry, the
  stack entry and the two gate claims are released by one RAII object (`ChainGuard`), and the entry is taken **before** that object exists, so the guard can only give back
  what the frame already holds.
- Allocation failure is not modelled on the admission path, or anywhere else in this manager: the only exception a chain allocation, the live-registry push, the stack push
  or the `ProgressHost` constructor can raise is `std::bad_alloc`, and it is not turned into a command outcome. Everything after the entry - including the `changed()`
  handlers the progress host fires - runs inside the guard and is therefore released normally.
- `executeCommandLineAsync()` is not a coroutine for the same reason it cannot be lazy: it splits the line before it hands the tokens over, because a lazy task cannot
  own the caller's reference. A `std::bad_alloc` from the tokeniser therefore leaves the function as an exception instead of becoming a `Failed` result - the same policy
  as above, not a second one, and the result language (`Failed` is for the line or the name, not for the machine) stays honest.

### 2.3 The serialisation gate and exclusion

- Serialisation gate: `foreground_busy || ProgressHost::current() != nullptr`; the check and the claim happen in one critical section, and `ChainGuard`'s destructor releases it.
- Exclusion: a top-level Exclusive command takes a snapshot of the live chains, calls `request_stop()` on **each** of them, and then waits in 5 ms slices for them to drain (bounded at 2 s). On success it bypasses the serialisation gate; on timeout ⇒ `Failed`, and it never runs alongside an older chain.
- `cancelAll()` bumps `cancel_generation`, so a waiting take-over winds down with `Cancelled` at its next polling point.
- Known residue: exclusion is "clear the field at start time", not an entry lock; an ordinary top-level command admitted while it runs can still coexist with it.

### 2.4 Thread safety

- A command's `execute()`, the factory, the snapshot callbacks, the event callbacks and the command's virtual functions all run outside the locks.
- `ProgressHost::current()` is sampled outside the `mutex`.
- `Signal` itself is a thread-safe snapshot (an immutable table plus an atomic publish); subscribing and firing may run concurrently.
- The read-modify-write of the disable preference is not atomic (only the UI thread calls it, and `ConfigManager` locks itself), deliberately without a mutex of its own.

### 2.5 Arguments travel with the execution

The name entry carries no caller data and the instance entry carries it in fields; the difference is the **source**, not two mechanisms: the framework tokenises once and hands the tokens to `context.arguments()` with the execution frame, and the meaning belongs to the command (see `appfw-command-arguments.md`).

### 2.6 Aliases are names, and a name resolves to one command

- An alias is a second spelling of a command name; the map is followed transitively, so an alias may point at another alias.
- `registerAlias()` is the only place that can still explain a mistake, so it refuses the two shapes that would make a name silently unreachable: an alias whose own spelling is a registered command name (dispatch resolves the alias first, so the command would lose its own name), and an alias that closes a cycle, a self-alias being the one-hop case (`resolveName()` stops at the first repeat, so the name resolves to nothing).
- A target that is not registered yet is *not* refused: a plugin may declare its aliases before its commands.
- Every refusal logs why. The alternative is what the rule exists to remove: a registration that "worked" and a command that then does nothing.
- The owner tag these commands are attributed to has **one home, the application** (`Application::registrationOwner()`, under its own lock): the manager reads it when it registers a command and keeps no copy, so a host that sets the tag straight on the application cannot end up with two answers.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Top-level execution | the four entries create a chain, write `foreground` and contain exceptions | landed |
| F2 | Nested sub-commands | `executeChild()` joins the parent chain, depth capped at 64 | landed |
| F3 | Result status | `Success` / `Failed` / `Cancelled` / `NotApplicable` | landed |
| F4 | Cancel / drain | `cancelCurrent` / `cancelAll` / `cancelAllAndWait` | landed |
| F5 | History | value snapshots, capacity 1024, no result payload | landed |
| F6 | Disabling | a registry flag + a persisted preference + both entries gated | landed |
| F7 | The command manager dialog | list / state / disable / enable | landed |

### 3.1.1 F1 Top-level execution

- **Behaviour**: the four entries create a new chain and write `foreground`; `nested=true` uses the parent chain.
- **Constraints** (C1): the top-level entries turn an exception that escapes the command body or the factory into `Failed`; allocation failure is not modelled (§2.2), so a throw from the call itself - copying into the frame, splitting the line - is not a command outcome.
- **Requirements** (R1): a snapshot callback that throws ⇒ the command does not run; a throwing `executing` / `executed` callback is logged only.
- **Acceptance** (T1): the `CommandManager_*` top-level execution cases.
- **Caveat** (N1): an uncaught exception in a `DetachedTask` is `terminate`, so the containment is not optional.

### 3.1.2 F2 Nested sub-commands

- **Behaviour**: `context->executeChild(name)` or `executeChild(unique_ptr<Command>)`; it joins the parent chain.
- **Constraints** (C2): `stackSize() >= maxChainDepth()` refuses and returns `Failed`; the instance overload refuses `nullptr` and a disabled instance.
- **Requirements** (R2): a sub-command enters events and history under the name it reports itself; an unregistered one does not appear in `commandInfos()`.
- **Acceptance** (T2): `CommandManager_ExecuteChildRunsACallerSuppliedInstance` and friends.
- **Caveat** (N2): nesting must go through `executeChild()`; the manager cannot tell "nested" from "a new top level".

### 3.1.3 F3 Result status

- **Behaviour**: `Success` / `Failed` / `Cancelled` / `NotApplicable`.
- **Constraints** (C3): `NotApplicable` is not a failure; `succeeded()` is still `status == Success`.
- **Requirements** (R3): the three user-visible paths stay apart: `logOutcome()` logs at info level, `executeDetached()` passes it on, the console shows a warning.
- **Acceptance** (T3): `CommandManager_DetachedNotApplicableIsExplainedAndNotReportedAsAFailure`, `UserIOTest.NotApplicableTypedInTheConsoleIsAHintNotAnError`.
- **Caveat** (N3): never use `setCommandEnabled()` to express "not applicable" (that is the persisted preference).

### 3.1.4 F4 Cancel / drain

- **Behaviour**: `cancelCurrent()` stops the foreground chain only; `cancelAll()` stops every live chain; `cancelAllAndWait(timeout)` waits for them to drain.
- **Constraints** (C4): a chain's `runs` reaches zero only after the stack is popped and the host destroyed; the wait polls in 5 ms steps instead of using an `AsyncEvent`.
- **Requirements** (R4): `Application::shutdown()` calls `UserIO::cancelPendingInput()` before `cancelAllAndWait()`.
- **Acceptance** (T4): `ExclusiveStopsEveryLiveChain`, `CancelAllAbortsWaitingTakeOver`.
- **Caveat** (N4): a command must not call `cancelAllAndWait()` inside itself; to stop the application use `Application::quit()`.

### 3.1.5 F5 History

- **Behaviour**: when the `deque` reaches `maxHistoryEntries()` (1024) it `pop_front()`s; `historyAt(0)` is the oldest entry kept.
- **Constraints** (C5): history keeps value snapshots only; the `std::any` of `CommandResult::data()` is not stored.
- **Requirements** (R5): a command object may be destroyed as soon as its coroutine returns; history holds no pointer.
- **Acceptance** (T5): the `CommandManager_*History*` cases.
- **Caveat** (N5): only a bounded number of entries makes the byte count bounded with it.

### 3.1.6 F6 Disabling

- **Behaviour**: `setCommandEnabled()` resolves the name first, then writes the preference and flips `RegisteredCommand::enabled`.
- **Constraints** (C6): disabling is a flag, not a removal; a disabled command is still in `names()` / `commandInfos()`.
- **Requirements** (R6): both the by-name and the by-instance entries are gated; `isRegistered()` only tests existence.
- **Acceptance** (T6): `CommandManager_DisableIsAFlagAndIsPersisted`, `CommandManager_DisableBlocksCallerSuppliedInstance`.
- **Caveat** (N6): console completion skips disabled commands; `list_commands` appends `[disabled]` to the row.

### 3.1.7 F7 The command manager dialog

- **Behaviour**: a filter box → the command table (name / state / alias / plugin / group / description) → refresh / disable / close. The table follows `commandsChanged` while the dialog is open, and a rebuild keeps the selected row by command name.
- **Constraints** (C7): a disabled row is greyed and italic, never hidden; the buttons are greyed until something is selected.
- **Requirements** (R7): the dialog offers no "unload command"; disabling is a flag.
- **Acceptance** (T7): `CommandManagerDialogTest.*`.
- **Caveat** (N7): the table style is shared with the plugin manager through `TableStyle.hpp`.

## 4. Public Interface

### 4.1 `CommandManager`

| Interface | Contract | Note |
| --- | --- | --- |
| `executeCommandAsync(Command*)` / `(std::unique_ptr<Command>)` | top-level async | the pointer is borrowed for the whole execution, the `unique_ptr` is owned by the task |
| `executeCommandAsync(name, args, target)` / `executeCommandAndWait(name, args, target)` / `executeDetached(name, args, target)` | the name entries carrying arguments | arguments are only carried |
| `executeCommandLineAsync(line, target)` | the text entry | the only place that calls `tokenizeCommandLine` |
| `executeCommandAndWait(...)` | blocking | tools / tests |
| `executeDetached(...)` | fire-and-forget | a failure goes to `UserIO` |
| `cancelCurrent()` / `cancelAll()` / `cancelAllAndWait(timeout)` | cancellation | chains are independent |
| `registerCommand<T>()` / `unregisterCommand()` | registration | the factory is probed once |
| `setCommandEnabled()` / `isCommandEnabled()` / `disabledCommands()` | disabling | persisted |
| `names()` / `commandInfos()` / `isRegistered()` | enumeration / existence | disabled commands are still listed |
| `commandFlags()` / `commandDescription()` | one entry's flags / description (cached at registration, aliases resolved) | the command UI is in `appfw-command-binding.md` |
| `historyCount()` / `historyAt()` / `clearHistory()` | history | value snapshots |

### 4.2 `Command` / `CommandExecutionContext`

| Interface | Contract | Note |
| --- | --- | --- |
| `execute(CommandExecutionContext*)` | the command body | a coroutine |
| `flags()` / `name()` / `group()` | metadata | called outside the locks |
| `arguments()` | this execution's tokens | immutable, alive with the frame |
| `executeChild(name)` / `executeChild(unique_ptr<Command>)` | nesting | joins the parent chain |
| `stopToken()` / `isCancelled()` | cancellation | stopped, never replaced |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | The top-level entries turn an exception that escapes the command body or the factory into `Failed`; allocation failure is not modelled (§2.2). |
| C2 | Nesting depth ≤64; the instance overload refuses `nullptr` and a disabled instance. |
| C3 | `NotApplicable` is not a failure; `succeeded()` is still `Success`. |
| C4 | A chain's `runs` reaches zero only after the stack is popped and the host destroyed; the wait polls. The entry and every release are one RAII object, constructed after the entry. |
| C5 | History keeps value snapshots only, never a result payload. |
| C6 | Disabling is a flag; both the by-name and the by-instance entries are gated. |
| C7 | The dialog greys a disabled row out and never hides it. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | A snapshot callback that throws ⇒ nothing runs; a throwing event callback is logged only. |
| R2 | A sub-command enters events and history under the name it reports itself. |
| R3 | `NotApplicable` is distinguished in the log, the console and the ribbon. |
| R4 | `Application::shutdown()` cancels pending input before draining the command chains. |
| R5 | A command object may be destroyed once its execution returns; history holds no pointer. |
| R6 | `isRegistered()` tests existence only; executability is `isCommandEnabled()`. |
| R7 | The dialog offers no "unload command". |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | One chain = one stack + one cancellation source; chains are independent. |
| N2 | The cancellation source is stopped, never replaced. |
| N3 | No user callback runs under a lock. |
| N4 | The foreground chain is unique; `cancelCurrent()` acts on it only. |
| N5 | An exclusive command either takes over or is `Failed`; two exclusives never run at once. |
| N6 | History and nesting depth are bounded. |

## 6. Caveats and Traps

### 6.1 Exclusion is not an entry lock

- How it fails: an ordinary top-level command admitted after an exclusive one started runs alongside it.
- How to detect it: `ExclusiveStopsEveryLiveChain` only covers the moment of start; the run-time window is still there.
- How to prevent it: only a "closed door" state removes it, which is a separate API change.

### 6.2 Re-entrancy and cancellation inside a handler

- How it fails: running another command inside `executing` / `executed`, unregistering the command that just ran, adding or removing handlers.
- How to detect it: `Signal`'s snapshot semantics — an addition this round has no effect, a removal is not called again.
- How to prevent it: notifications happen outside the locks; never rely on "added this round takes effect immediately".

### 6.3 Live chains at shutdown

- How it fails: a command frame still points at `Impl` / `CommandManager` when the manager is destroyed → use-after-free.
- How to detect it: `~CommandManager` only warns when it finds a live chain.
- How to prevent it: the host calls `cancelAllAndWait()` before destroying; `Application::shutdown()` already does.

### 6.4 A message posted to an application that is gone

- How it fails: `reportToUser()` hands the message to the application thread when a command finished on a worker; the application - and with it the `UserIO` - can be destroyed before the queued call runs, so a captured `UserIO*` writes into freed memory.
- How to detect it: a command that finishes during shutdown, and a crash in the queued delivery rather than in the command.
- How to prevent it: the queued call carries the message only and asks `Application::current()` itself. That pointer is cleared before `Application`'s data - the `UserIO` included - is released, so a message delivered too late reaches nobody instead of a dead object.

### 6.5 A view that follows `commandsChanged` and also writes to it

- How it fails: the dialog disables a command, which fires `commandsChanged`, the dialog's own handler refreshes the table from it, and every `QTableWidgetItem` the toggling code still holds - the row it read the name from - is deleted under it.
- How to detect it: a crash immediately after the first toggle, in the few lines that build the confirmation message.
- How to prevent it: take what is needed (the name, the label) out of the widgets before calling into the manager, and treat any model call as able to replace the rows.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | A synchronous `executeCommandAndWait()` inside a command | The blocked thread may be the UI or the timer thread, and resuming needs it to pump → deadlock. | — |
| D2 | Exclusion cancelling the foreground chain only | A detached chain pushed out of the foreground keeps running, and the two run concurrently. | — |
| D3 | History storing `Command*` | A command is destroyed as soon as its coroutine returns, so the pointer dangles. | — |
| D4 | `setCommandEnabled()` to express "not applicable" | A user preference is persisted; not-applicable is a temporary fact. | — |
| D5 | The framework carrying `document_types` on a command | It widens the `Command` vtable / ABI, and the command judges more accurately itself. | — |
| D6 | Extracting a `Scheduler` class | The audit found the current shape consistent with the invariants, so the extraction buys nothing. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | top-level execution / exception containment | `CommandManager_*` |
| T2 | nested / instance sub-commands | `CommandManager_ExecuteChildRunsACallerSuppliedInstance` |
| T3 | the three `NotApplicable` paths | `DetachedNotApplicableIsExplained...`, `NotApplicableTypedInTheConsole...` |
| T4 | exclusion / cancellation / drain | `ExclusiveStopsEveryLiveChain`, `CancelAllAbortsWaitingTakeOver` |
| T5 | disabling / persistence / the instance entry | `DisableIsAFlagAndIsPersisted`, `DisableBlocksCallerSuppliedInstance` |
| T6 | the dialog | `CommandManagerDialogTest.*` |
| T8 | aliases / the owning instance entry / the owner tag | `CommandManager_RejectsAnAliasThatShadowsOrLoops`, `CommandManager_OwningInstanceEntryOutlivesTheCaller`, `CommandManager_RegistrationTagLivesOnTheApplication` |

## 9. Status and Evolution

### 9.1 Current status

The four entries, the chain model, exclusion, cancel / drain, history, disabling, `NotApplicable` and the dialog are all in place; the argument carrier is in `appfw-command-arguments.md`.

### 9.2 Known gaps / backlog

- Arguments are tokens only: no declarations, types, defaults or required flags, and an unknown or repeated argument is reported by the command itself; see `.ai/backlog/appfw-command-arguments.md`.
- Exclusion is not an entry lock; an ordinary top-level command admitted while it runs can still coexist with it.
- `CommandExecutionContext::executeChild(name)` carries no arguments: a parent that has to pass tokens constructs the child instance itself (`executeChild(unique_ptr<Command>)`). A name overload appends a vtable slot and has no consumer yet; see `.ai/backlog/appfw-command-arguments.md` (T7).
- The read-modify-write of the disable preference is not atomic (only the UI thread calls it, so there is no mutex); disabling or unregistering a command does not change the registry layout, and a plugin that registers again comes back with its preference.

### 9.3 Compatibility / migration

The migration of `getIntAsync` and the other `UserIO` calls is not this page; `CommandStatus` gained `NotApplicable`, so a caller has to distinguish "error" from "not applicable" by status.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/`: `CommandManager.hpp`, `Command.hpp`
- `src/fw/appfw/src/`: `CommandManager.cpp`, `gui/CommandManagerDialog.cpp`
- `tests/test_gui/test_gui.cpp`

### 10.2 Documents and ADRs

- `appfw-userio.md`, `appfw-eventbus.md`, `appfw-command-arguments.md`, `../backlog/`
- External material: the thread-safe snapshot semantics of `Signal`; the foreground-host semantics of `ProgressHost`.
