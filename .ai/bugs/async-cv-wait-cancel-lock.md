# Bug: destroying a parked `AsyncConditionVariable::wait` unlocks a RAII guard twice

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `AsyncConditionVariable.hpp`, `AsyncMutex.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

A coroutine that holds an `AsyncLockGuard` and calls `AsyncConditionVariable::wait(mutex)` is destroyed before the notification by a combinator such as `whenAny`; the guard's destructor then calls `unlock()` on a mutex it has already unlocked, and Debug builds trip the `locked_` assertion.

## Root cause

`wait()` calls `mutex.unlock()` directly before suspending and re-acquires the mutex after the notification, but it cannot update the `AsyncLockGuard` the caller still holds. If the coroutine frame is destroyed during the wait, the guard still believes it owns the lock and releases it again. The "give up the wait" path the CV allows therefore disagrees with the ownership state of the RAII lock.

## Impact and verification

- Standalone reproduction: acquire a guard with `co_await lockAsync(mutex)`, then `co_await cv.wait(mutex)`; use a short timer task as the `whenAny` winner that destroys the CV waiter before the notification.
- A Debug probe trips `assert(locked_)` inside `AsyncMutex::unlock()`.
- The existing `WaiterDestroyedWhileWaiting` case tests the manual `lock()` / `unlock()` form and keeps no RAII guard across the cancellation, so it never covered this ownership mismatch.

## Fix direction

Make the release, wait and re-acquisition of the CV wait agree with the guard's ownership state; or state that abandoning a CV wait while holding a RAII guard is unsupported, and make the interface prevent or detect that use. Add a regression test that destroys the waiter through `lockAsync` + `whenAny`.

## Fix

- New `AsyncConditionVariable::wait(AsyncLockGuard&)`: the guard is cleared for the duration of the wait, and the same mutex is re-locked and re-armed into it before returning. When the coroutine is destroyed while waiting, the guard is empty, so its destructor does not `unlock()` a second time.
- New `AsyncLockGuard::mutex()`, so `wait()` can remember the mutex before clearing the guard.
- `wait(AsyncMutex&)` stays for callers that lock and unlock by hand, and its documentation says not to use it while a guard is held.

## Verification

- `AsyncConditionVariableTest.GuardWaitReacquiresAndReleasesOnce`: after the wait the guard holds the lock again and the destructor releases it exactly once.
- `AsyncConditionVariableTest.AbandonedGuardWaitDoesNotUnlockTwice`: after `whenAny` destroys the waiter the mutex can still be locked, and Debug builds no longer trip `assert(locked_)` in `AsyncMutex::unlock()`.
- The full `test_async` suite passes in Debug and under ASan.
