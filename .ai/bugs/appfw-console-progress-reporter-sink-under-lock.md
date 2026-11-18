# ConsoleProgressReporter: the sink is called while the state lock is held

- **Status**: Fixed (2026-11-18).
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `ConsoleProgressReporter.cpp` (`Impl::refresh`, `Impl::write`, `stop`, `poll`), `ConsoleProgressReporter.hpp`

## Symptom and impact

`Impl::refresh()` produced a line and wrote it through the user-supplied `Sink` before its caller released the state mutex; the comment said so explicitly ("the caller holds mutex_").
Consequences:

- A sink that calls back into the reporter (`poll()` or `stop()`) takes the same non-recursive mutex and deadlocks.
- A slow sink - the headless one writes to a terminal - holds the state lock for the whole write, so a progress notification or a `stop()` waits behind a terminal flush.

## Root cause

The write happened under the state lock because the lock doubled as the "nothing is written after `stop()`" barrier, which `ConsoleUserIO` relies on (its sink writes through a `this` that dies right after the reporter is destroyed). The two roles were not separated.

## Fix

1. `refresh()` now returns `Refresh{ line, delay }` and no longer writes.
2. `write(line)` runs outside the state lock, under a dedicated write gate.
3. `stop()` flips the flag and bumps the generation under the state lock, releases it, then takes the write gate. The barrier is unchanged - a write already running finishes before `stop()` returns, and one that starts later sees the flag - while the two locks are never nested, so a callback that re-enters cannot deadlock against the stop wait.
4. The sink contract is documented on `Sink` (`ConsoleProgressReporter.hpp`): it runs outside the state lock and must not call back into the reporter.

## Verification

- `ConsoleProgressReporterTest.*` (throttling, change notification, wakeup after the reporting thread is gone) and `ConsoleUserIOPrintsTheProgressOfAForegroundOperation` pass unchanged; the latter is the case that depends on the stop barrier.

## Design constraints

The finding touches **G4** (a callback's contract stated) and **G10** (a user callback under a lock is a real cost, not a detail).
