# ProgressHost: presenters dereference a host pointer sampled from the registry, across threads

- **Status**: Fixed (2026-11-18); the snapshot API replaces the pointer for every observer.
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `ProgressHost.hpp`, `ProgressHost.cpp`, `gui/ProgressPresenter.cpp`, `ConsoleProgressReporter.cpp`

## Symptom and impact

`current()`, `activeHosts()` and `foregroundStack()` return borrowed `ProgressHost*` values taken under the registry lock and used after it is released.
A `LongRunning` command owns its host in a `unique_ptr` inside its coroutine frame and destroys it on whichever thread ended the command, while the status bar draws on the application thread and the headless reporter wakes on the shared timer thread.
The two presenters therefore read `indicator().position()` / `label()` through a pointer that could be freed in between (a data race and a use-after-free), and `ProgressPresenter`'s cancel button dereferenced the pointer even later - on the next click, long after the operation had ended.

The design page argued that re-sampling the registry after `changed()` was enough. It is not: a re-sample yields another borrowed pointer with the same problem.

## Root cause

The registry published identity of a mutable object (a pointer) to observers whose lifetime is not nested in the operation's. Only the operation itself may borrow its host, because its own frame keeps it alive.

## Fix

1. `ProgressHost::Snapshot` is a value: `id`, `position`, `label`, and the shared `std::stop_source`.
2. `currentSnapshot()`, `foregroundSnapshots()` and `activeHostCount()` build snapshots under the registry lock, so the copy cannot race a concurrent destruction.
3. Each host gets a process-unique `id`; a presenter compares ids instead of pointers to tell "a new operation owns the bar" from "the same one reported again".
4. Both presenters draw from snapshots; the cancel button keeps the snapshot's `stop_source`, which stays meaningful after the host is gone.
5. `current()` / `activeHosts()` / `foregroundStack()` stay for the operation side (`current()` inside its own execution) and for tests that assert identity on a live host; their documentation says they are borrowed.

## Verification

- `ProgressHostTest.SnapshotIsAValueThatOutlivesItsHost` keeps a snapshot, destroys the host, and checks that the copy is still readable, that the foreground is empty, and that `cancelSource.request_stop()` still works.
- `ProgressPresenterTest.*`, `ConsoleProgressReporterTest.*` and the `LongRunning` cases in `test_gui` pass unchanged.

## Design constraints

The finding touches **G4** (borrowed versus owned stated by the signature) and **G10** (a pointer cannot carry a lifetime across threads; a value can).
