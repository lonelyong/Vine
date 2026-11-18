# UserIO: destroying the I/O with a read parked on it destroys the completion event under the waiter

- **Status**: Fixed (2026-11-18); a regression case covers the visual backend.
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `gui/VisualUserIO.cpp` (`~VisualUserIO`, `beginRead`, `endRead`, `cancelInteraction`), `ConsoleUserIO.cpp` (`~ConsoleUserIO`, `beginRead`)

## Symptom and impact

Both backends kept the waiting read in a coroutine frame parked on an `AsyncEvent` (`done`, `StdinReader::line_ready`) owned by the I/O object, and neither destructor cancelled it.
Destroying the I/O while a command was still waiting therefore destroyed the event with a queued waiter: debug builds tripped `~AsyncEvent`'s assertion (`head_ == nullptr && tail_ == nullptr`), and release builds left the frame pointing at freed state, to be resumed into nothing or destroyed against a dead mutex.

The only protection was the call order in `Application::shutdown()` (cancel the pending input, then drain the chains). Any host that disposed of its `UserIO` directly - a test, a tool, a host that never ran `run()` - hit it.

## Root cause

The I/O object owned the wait state but not the obligation to release it. The same functions also let a cancellation race with a starting read (claiming the slot, arming the event and deciding the outcome were three separate steps), and a cancellation that arrived after the answer was recorded could discard what the user had typed.

## Fix

1. Both destructors now mark the I/O closed first, then `cancelPendingInput()`. A read that the cancellation resumes cannot start another one - `beginRead()` refuses a closed I/O - so no waiter can be parked when the event dies.
2. The start / answer / cancel transition is serialized (read mutex in `VisualUserIO`, the existing state lock in `ConsoleUserIO`), so a cancel cannot clear the wakeup a read is about to park on.
3. `VisualUserIO` records an explicit `answered` flag: an answer that arrived first wins, and a cancel only ends a read that is still unanswered.
4. Both destructors log when a read is nevertheless still pending: draining the command stays the host's job, and an unclean teardown should be visible instead of silent.

## Verification

- `UserIOTest.DestroyingTheIoWithAPendingReadReleasesIt` starts a read on a worker thread, waits until the prompt is on screen, destroys the I/O and requires the read to end with `nullopt`.
- `test_gui` (the other `UserIOTest` / `ConsoleUserIOReadTest` cases) still pass.

## Remaining gap

There is no case that destroys a `ConsoleUserIO` while its read is parked: the fixture's rendezvous would have to poll a stream other threads write to. The contract is documented on `UserIO`, and the shared code path is covered by the visual backend.

## Design constraints

The finding touches **G4** (the destruction contract is stated instead of implied by call order) and **G10** (lifetime of the wait state and the coroutine frames parked on it).
