# Bug: `withCancellation` does not pass its token to the wrapped task

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `Cancellation.hpp`, `StopToken.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

`withCancellation(token, task)` only checks the token when the wrapper task starts; the wrapped task cannot read that token through `currentStopToken()`, and a stop requested while it runs does not reach it.

## Root cause

The wrapper calls `throwIfCancelled(token)` and then awaits the original task directly, without injecting the token into its promise through `TaskEnvironment::setToken()` or `withStopToken()`. The function comment says the token is "passed down", but the implementation only checks before the start.

## Impact and verification

- A standalone probe wraps a task that reads `currentStopToken()` with a cancellable token; the task observes `stop_possible() == false`.
- The existing `WithCancellation` cases only test refusing to start when already cancelled and completing normally with an empty token; they do not test whether the token reaches the child task or a mid-run cancellation.
- A caller that believes the combinator propagates cancellation therefore leaves the child running unless it opens an explicit token channel of its own.

## Fix direction

State and unify the API semantics: if it is an environment wrapper, it must inject the token before the child starts and define how a mid-run cancellation behaves; if it only checks before the start, rename or re-document it so it does not imply propagation. Add tests for environment visibility and for a stop requested while running.

## Fix

- `withCancellation()` calls `detail::TaskEnvironment::setToken(task, token)` after `throwIfCancelled()`, the same way `withStopToken()` and the combinators inject it, and then awaits the original task.
- The comment now says the token becomes the task's environment and that a mid-run stop is observed only by code that reads the token (cooperative).

## Verification

- `TaskTest.WithCancellationHandsItsTokenToTheTaskEnvironment`: inside the task body, `stop_possible()` of `currentStopToken()` is true.
- `TaskTest.WithCancellationCancelsTheTaskWhileItRuns`: while the task sleeps, an outside `request_stop()` wakes it within 1 s (without the injection it runs the full 2 s deadline).
- The existing `WithCancellationThrowsWhenCancelled` / `WithCancellationRunsTask` cases still pass.
