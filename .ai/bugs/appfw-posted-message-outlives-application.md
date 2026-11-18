# CommandManager: a posted message kept a borrowed `UserIO*` past the application

- **Status**: Fixed (2026-11-18); the design page states the rule.
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `src/CommandManager.cpp` (`reportToUser`)

## Symptom and impact

A command that finished on a worker thread handed its failure message to the application thread through `postToMainThread`, capturing the `UserIO*` it looked up at the call site. The `UserIO` is a `unique_ptr` owned by `ApplicationData`, so a message still in the queue when the application is destroyed would have been written into freed memory.

Nothing made it happen in practice: `Application::shutdown()` stops the event loop before it destroys the I/O, so the queued call is dropped. The code therefore depended on the teardown order of two other objects - the kind of guarantee that everyone repeats and nobody tests.

## Root cause

The posted call carried a borrowed pointer instead of asking for the live one. The rest of the codebase already answers this question another way: the application's own registry is cleared before its data is released, so asking `Application::current()` inside the queued call is both cheaper and correct.

## Fix

The queue now carries the message only; the call asks `Application::current()` and then `userIO()` when it runs. A message delivered after the application is gone reaches nobody, and one delivered while a *new* application exists reaches that one - the only two sensible outcomes for a fire-and-forget report.

## Verification

- The path is covered by the existing detached-command cases (`CommandManager_DetachedNotApplicableIsExplained...` and the console's own `NotApplicableTypedInTheConsoleIsAHintNotAnError`), which exercise the marshal; a shutdown-order test would need to stop the loop mid-queue, which is what `Application::shutdown()` already does.

## Design constraints

The finding touches **G10** (the lifetime of a posted call was assumed from a teardown order instead of stated) and **G11** (the dependency was invisible at the call site).
