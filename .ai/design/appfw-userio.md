# appfw's UserIO

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the `UserIO` contract, the console and visual backends, file reading and the input protocol.
> **Sources**: `UserIO.hpp`, `ConsoleUserIO.hpp`, `VisualUserIO.hpp`, `tests/test_gui`.
> **Backlog**: none (open items are in `../backlog/`)

## 1. Overview

### 1.1 Problem and background

A command resumes on any thread, yet it has to write to the console, ask the user a question and read a file or directory path.
Output written straight into a `QWidget` crosses threads; a blocking read performed on the waiter's thread stalls shutdown; and two concurrent reads overwrite each other's prompt and result.

### 1.2 Goals

- Every `UserIO` entry may be called from any thread; the implementation does the marshalling.
- One interactive wait at a time, and a cancellation that really releases the wait.
- The headless and GUI backends share the parsing and validation, while their interaction models may differ.
- Headless mode reports progress too (a `LongRunning` command no longer runs silently).

### 1.3 Non-goals

- No drag and drop, no Tab completion of paths, no auto-submit.
- `ConsoleUserIO`'s read does not become a non-blocking `poll`.
- No TTY detection in `ConsoleUserIO::clear()`.

### 1.4 One-paragraph conclusion

`UserIO` gathers "output / ask / read a value / read a path" behind an implementation: callable from any thread, with the implementation doing the marshalling; one interactive slot plus a cancellable, wakeable wait; the GUI uses `ConsolePanel`, the headless backend a reader thread plus a line buffer.

## 2. Design Points

### 2.1 The two backends

| Class | Created by | Output | Input |
| --- | --- | --- | --- |
| `UserIO` (abstract base) | — | `putString` / `clear` | the four `getXxxAsync` plus the four file / directory reads |
| `ConsoleUserIO` | `Application::createUserIO()`, the default without a GUI | `std::cout` (under a mutex) | a reader thread plus a line buffer |
| `VisualUserIO` (a public SDK class with a private PImpl) | `GuiApplication::createUserIO()` | `ConsolePanel` (marshalled) | `lineEntered` / `escapePressed` plus an `AsyncEvent` |

Owner: `ApplicationData::user_io` (a `unique_ptr`, torn down before the dispatcher); `Application::shutdown()` calls `cancelPendingInput()` before it drains the command chains.

### 2.2 Threads

- Every entry may be called from any thread; `VisualUserIO` marshals through `MainThreadDispatcher`, `ConsoleUserIO` takes a stdout mutex.
- Marshalling also needs liveness protection: `VisualUserIO` guards the panel with a `QPointer<QWidget>`, and with the panel gone the call is a no-op.
- `cancelPendingInput()` may be called from any thread, including after the event loop has stopped; it only flips an atomic flag and `set()`s an event, and never touches the UI.

### 2.3 Interaction

- One interactive wait at a time; a second `getXxxAsync` returns `nullopt` immediately plus a warning.
- The slot is released by `ReadScope`: on success, on cancellation, and when the coroutine frame is destroyed early.
- Start, answer and cancellation are one serialized transition (the read mutex in `VisualUserIO`, the state lock in `ConsoleUserIO`), so a cancel that races with an answer or
  with a starting read can neither lose the wakeup nor discard the answer: the answer slot is the result - claiming the slot, emptying it and arming the wakeup happen together,
  a cancel only wakes the read (which hands back whatever the slot holds), and an answer whose read is gone, or whose slot a different read already owns, is dropped.
- An implementation's destructor marks the I/O closed and then cancels: a read that the cancellation resumes cannot park again, and the wait state is never destroyed with a
  queued waiter (see `../bugs/appfw-userio-destructor-pending-read.md`).
- `ConsoleUserIO` reads `std::cin` on a background thread; a cancellation sets shared state and wakes the waiter, and an unconsumed line is left for the next read.
- The input line has two modes, and only the caller knows which one applies: `beginInput()` starts a prompt and `endInput()` returns the line to command entry. A line submitted inside that interval is an answer, so it is not command history, opens no completion and is never rewritten into a command name; the accent underline marks the mode, which is why a text read has one too.

### 2.4 Reading a value

- `getIntAsync()` yields an `int`; parsing goes through `UserIO::parseInt()` (`String::toInt()` wraps silently and must not be used on user input).
- A `toDouble` result must be `std::isfinite`.
- The prompt text is read and written on the application thread only; the write happens inside the marshalled "show the prompt" call, and the result field is written by the application thread and read by the waiter after `done_`.

### 2.5 Output and completion

- `putString` writes one line, `clear` clears the screen; `ConsoleUserIO::clear()` emits the ANSI sequence unconditionally (redirected output collects control characters; recorded and left as is).
- The `VisualUserIO` completion list is a snapshot: it is refreshed when the console is bound and follows command-set changes by subscribing to `CommandManager::commandsChanged`.
- The refresh belongs to the application thread, because it reads the bound panel and asks the plugin manager for display names; `commandsChanged` may fire anywhere, so the handler posts (carrying a liveness token) instead of computing on the firing thread.
- Rebinding a panel detaches the old handler first; binding the same panel twice does not run a line twice.

### 2.6 Headless progress

`ConsoleUserIO` owns a `ConsoleProgressReporter` (`start()`ed by its constructor): one line per update, in the localized form "[progress] 42% <phase name>", with a closing line when it ends; the prefix and the fixed lines are UI strings from the `appfw` catalogue, the rest is unchanged.
Nothing is written after `stop()`.
Throttling: `show_delay{500ms}` / `interval{200ms}` (the minimum gap between lines) / `step_percent{5}`; it subscribes to `ProgressHost::changed()` and wakes up zero times while idle, and `poll()` serves self-driven loops and tests.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Output | `putString` / `clear` callable from any thread, marshalled by the implementation | landed |
| F2 | Asking / reading a value | `getString/Int/Double/BoolAsync`; a single slot | landed |
| F3 | File / directory reads | four separate interfaces; `;` separates several paths; a browse button | landed |
| F4 | Completion | the snapshot is refreshed when the command set changes | landed |
| F5 | Headless progress | `ConsoleProgressReporter`, one line per update | landed |
| F6 | Command history | Up/Down recall the lines that ran as commands; an answer is not one | landed |

### 3.1.1 F1 Output

- **Behaviour**: `putString` writes one line, `clear` clears the screen.
- **Constraints** (C1): callable from any thread; `VisualUserIO` marshals to the application thread, `ConsoleUserIO` takes a stdout mutex.
- **Requirements** (R1): with the panel already destroyed, output is a no-op.
- **Acceptance** (T1): `UserIOTest.OutputFromAWorkerThreadIsMarshalledToTheApplicationThread`.
- **Caveat** (N1): `ConsoleUserIO::clear()` emits the ANSI sequence unconditionally.

### 3.1.2 F2 Asking / reading a value

- **Behaviour**: the four `getXxxAsync`; one wait at a time.
- **Constraints** (C2): a second read returns `nullopt` immediately plus a warning; the slot is released by `ReadScope`.
- **Requirements** (R2): `getIntAsync()` only yields values in the `int` range and never wraps; `toDouble` must be finite.
- **Acceptance** (T2): `SecondReadIsRefusedWhileOneIsPending`, `IntReadKeepsItsValueAndRepromptsOnOverflow`.
- **Caveat** (N2): the prompt text is read and written on the application thread only.

### 3.1.3 F3 File / directory reads

- **Behaviour**: four separate interfaces (file / folder × single / multiple); several paths go on one line separated by `;`; the GUI offers a browse button.
- **Constraints** (C3): the options travel with the request (`FileReadOptions` / `DirReadOptions`); the validation is shared in the base class.
- **Requirements** (R3): the headless side does not re-prompt; a bad path returns `nullopt`.
- **Acceptance** (T3): `ConsoleUserIOReadTest.*` and the GUI browse end-to-end cases.
- **Caveat** (N3): the negated `require_existing` reads as "save as", and is not named `must_exist`.

### 3.1.4 F4 Completion

- **Behaviour**: `VisualUserIO` refreshes the completion when it binds a console and subscribes to `commandsChanged`.
- **Constraints** (C4): the completion list is a snapshot; the per-key path filters the local snapshot only and never touches the registry lock.
- **Requirements** (R4): rebinding a panel detaches the old handler first.
- **Acceptance** (T4): `CommandsChangedReportsRegistryAndAliasEdits`, `RebindingTheConsoleDoesNotRunALineTwice`.
- **Caveat** (N4): the O(N) registry copy happens in `refreshCompletion()` only.

### 3.1.5 F5 Headless progress

- **Behaviour**: `ConsoleUserIO`'s constructor attaches a `ConsoleProgressReporter`; one line per update plus one closing line.
- **Constraints** (C5): nothing is written after `stop()`; the consumer is the last member and is destroyed before `output_mutex_`.
- **Requirements** (R5): zero wake-ups while idle; `poll()` does not need a preceding `start()`.
- **Acceptance** (T5): `ConsoleProgressReporterTest.*`.
- **Caveat** (N5): the virtual `putString` must not be called during destruction; the sink goes through the non-virtual `writeLine`.

### 3.1.6 F6 Command history

- **Behaviour**: Up/Down navigate the lines submitted as commands; an empty line is dropped; an answer to a read is never recorded, and the completion popup is off while the line answers.
- **Constraints** (C7): the panel decides membership from the mode it was given (`beginInput` / `endInput`), never from what the text looks like.
- **Requirements** (R6): the completion refresh runs on the application thread whatever thread changed the command set.
- **Acceptance** (T8): `UserIOTest.AnAnswerIsNotACommandLine`, `UserIOTest.CompletionRefreshFromAWorkerThreadReachesThePanel`.
- **Caveat** (N6): the answering mode is ended by the read, not by the submission: a prompt that is never ended leaves history and completion off.

## 4. Public Interface

### 4.1 `UserIO`

| Interface | Contract | Note |
| --- | --- | --- |
| `putString` / `clear` | callable from any thread | the implementation marshals |
| `getStringAsync` / `getIntAsync` / `getDoubleAsync` / `getBoolAsync` | a single slot | range-checked for `int` |
| `getFileAsync` / `getFilesAsync` / `getDirAsync` / `getDirsAsync` | file / directory reads | `;` separates several paths |
| `cancelPendingInput()` | callable from any thread | only flips an atomic flag and `set()`s |
| `FileReadOptions` / `DirReadOptions` | options | `extensions` / `require_existing` / `suggested_path` |

### 4.2 The backends

| Interface | Contract | Note |
| --- | --- | --- |
| `ConsoleUserIO` | headless; a reader thread plus a line buffer | a stdout mutex |
| `VisualUserIO` | GUI; a public class with a private PImpl | `setConsolePanel` binds it |
| `ConsoleProgressReporter` | headless progress | subscribes to `ProgressHost::changed()` |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | Callable from any thread; the implementation marshals. |
| C2 | At most one interactive wait at a time; the slot is released by `ReadScope`. |
| C3 | The file / directory options travel with the request; the validation is shared in the base class. |
| C4 | The completion list is a snapshot; the per-key path never touches the registry lock. |
| C5 | Nothing is written after `stop()`; the consumer is destroyed before `output_mutex_`. |
| C6 | An implementation's destructor releases the read that is waiting and refuses later ones; the wait state is never destroyed with a waiter queued. |
| C7 | The panel records a line as command history only while the line is not answering a read; only `beginInput` / `endInput` set that mode. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | With the panel destroyed, output is a no-op. |
| R2 | `getIntAsync` yields only values in the `int` range; `toDouble` is finite. |
| R3 | The headless side does not re-prompt; a bad path returns `nullopt`. |
| R4 | Rebinding a panel detaches the old handler first. |
| R5 | Progress wakes up zero times while idle; `poll()` needs no preceding `start()`. |
| R6 | The completion refresh runs on the application thread, whichever thread changed the command set. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | Once `cancelPendingInput()` has returned, the pending read has been released: it reports the answer when one had already arrived, `nullopt` otherwise. |
| N2 | A read may start on any thread; the prompt bookkeeping and the panel writes happen on the application thread only. |
| N3 | Completion agrees with `CommandManager::commandInfos()`. |
| N4 | While a foreground progress exists, the headless output carries a progress line. |
| N5 | A cancellation never discards an answer that arrived first. |
| N6 | Up/Down only recall commands: a submitted answer never becomes one. |

## 6. Caveats and Traps

### 6.1 The panel's lifetime after marshalling

- How it fails: a posted callback sits on the event loop while the panel is destroyed → a write into freed memory.
- How to detect it: output left undelivered at the end of a test crashes the next `deliverPostedCalls()`.
- How to prevent it: guard with `QPointer<QWidget>`; with the panel gone the call is a no-op.

### 6.2 A blocking read and cancellation

- How it fails: `std::getline` blocks on the waiter's thread, `cancelPendingInput()` has no effect, and shutdown hangs.
- How to detect it: the read never returns after a cancellation, and shutdown times out.
- How to prevent it: a reader thread plus a line buffer plus a wakeable `AsyncEvent`; an unconsumed line is left for the next read.
- How it also fails: the I/O is destroyed while a command is still parked on a read, so the completion event dies under a queued waiter (debug: assertion; release: a
  coroutine frame left on freed state).
- How to prevent that: the destructor closes the I/O (a later read answers `nullopt`), cancels the one that is waiting, and logs when a read is still pending - draining the
  command stays the host's job, which is why `Application::shutdown()` cancels before it drains.

### 6.3 `String::toInt()` wraps silently

- How it fails: the user types 1000 and the value becomes -24.
- How to detect it: `static_cast<int8_t>(1000)`; a value beyond `int` wraps.
- How to prevent it: `UserIO::parseInt()` range-checks, and `toDouble` checks for finite.

### 6.4 A prompt that is never ended

- How it fails: a host calls `beginInput()` and never `endInput()`, so every later line is treated as an answer - it is not recorded in history and offers no completion.
- How to detect it: Up stays empty although commands ran, and typing a command prefix shows no popup.
- How to prevent it: end the prompt when the read ends, answered or cancelled - `VisualUserIO` does it in `endRead()`, which runs from the read's RAII scope.

### 6.5 A cancelled read can re-arm the line one turn later

- How it fails: `showPrompt()` is posted from the thread that started the read, while a cancellation issued on the application thread ends the read inline; the queued "show the prompt" call then runs after the end and puts the line back into answering mode for a read nobody waits on, so the next command is neither recorded in history nor completed.
- How to detect it: start a read on a worker thread and cancel it from the application thread before the prompt appears, then type a command and press Up.
- How to prevent it: give the read an identity the prompt state can compare (a generation counter, or posting the end through the same queue as the prompt), so the queued call skips itself when its read already ended. Not done yet: the visible cost is one lost history entry, against a second source of truth next to the answer slot (G8), and the stale call also re-installs a picker, which is the older half of the same ordering question.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | A "current completion entry" getter on `ConsolePanel` | Widening the public API for a test is not worth it. | — |
| D2 | A non-blocking `poll` for `ConsoleUserIO`'s read | High cross-platform cost; a reader thread plus a line buffer already gives cancellability without losing a line. | — |
| D3 | TTY detection in `ConsoleUserIO::clear()` | It needs a platform branch; recorded and left as is. | — |
| D4 | A "storage type" accessor on `UserIO` | `parseInt` is the only parsing logic that needs sharing. | — |
| D5 | Merging the two backends' pending-read state machines | Their waiting mechanisms are structurally different; only about 15 lines of slot protocol are shareable. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | output marshalled across threads | `UserIOTest.OutputFromAWorkerThreadIsMarshalledToTheApplicationThread` |
| T2 | the single slot / the `int` range | `SecondReadIsRefusedWhileOneIsPending`, `IntReadKeepsItsValueAndRepromptsOnOverflow` |
| T3 | headless read / cancellation / parsing | `ConsoleUserIOReadTest.*` (4 cases) |
| T4 | completion refresh / rebinding | `CommandsChangedReportsRegistryAndAliasEdits`, `RebindingTheConsoleDoesNotRunALineTwice` |
| T5 | headless progress | `ConsoleProgressReporterTest.*` (3 cases) |
| T6 | the `String::toInt` range | `test_core String.NumericConversions` |
| T7 | a waiting read is released by the destructor | `UserIOTest.DestroyingTheIoWithAPendingReadReleasesIt` |
| T8 | an answer is not a command line; the refresh lands from any thread | `UserIOTest.AnAnswerIsNotACommandLine`, `UserIOTest.CompletionRefreshFromAWorkerThreadReachesThePanel` |

## 9. Status and Evolution

### 9.1 Current status

The two backends, the single slot, the file / directory reads, the completion refresh and the headless progress are all in place.
The input line distinguishes a command from an answer by the mode its caller sets, so history and completion stay out of a read.

### 9.2 Known gaps / backlog

- Drag and drop, Tab completion of paths and auto-submit are not done.
- With no interactive panel, `getXxxAsync` hangs until shutdown; changing that means first defining in `UserIO` what "there is no interactive surface" counts as.

### 9.3 Compatibility / migration

`getIntAsync` returns an `int` (not an `int8_t`); the four file / directory read virtuals are appended at the end of the vtable, and the ABI is currently 8u.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/UserIO.hpp`
- `src/fw/appfw/src/ConsoleUserIO.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/VisualUserIO.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConsoleProgressReporter.hpp`
- `tests/test_gui/test_gui.cpp`

### 10.2 Documents and ADRs

- `appfw-progress.md`
- `appfw-command-manager.md`
- `../backlog/`

### 10.3 External material

- The contracts of `String::toInt()` / `toDouble`.
- `ConsolePanel`'s `lineEntered` / `escapePressed`.
