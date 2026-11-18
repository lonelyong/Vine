# Bug: a task added to a stopped `Scope` still receives the stopped token

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `Scope.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

After the current batch of a `Scope` is stopped, a new batch begun by a later `add()` still receives a stop token that has already been asked to stop. A child reading `currentStopToken()` when it starts finds the token stopped, even though the new batch was never asked to stop.

## Root cause

`Scope::add()` resets the completion event and the exception state when `pending == 0` and treats that as the start of a new batch, but it does not rebuild `source_`. The stopped state `requestStop()` left on `source_` is persistent, so the new children are still injected with a stopped token.

## Impact and verification

- Reproduction: add and complete a batch of tasks, call `requestStop()`, then add more tasks and read `currentStopToken()`; the second batch observes `stop_requested() == true`.
- A standalone minimal probe reproduced it, and the existing `test_async` cases do not cover reusing the scope after a stop.
- The "new batch" therefore resets the completion state but not the cancellation state; a caller cannot tell from the current behaviour of `add()` whether the new batch is still affected by the old stop request.

## Fix direction

State the contract between `requestStop()` and batch reuse: if a new batch is meant to run independently, give that batch a new stop source; if stopping is irreversible for the whole lifetime of the `Scope`, refuse `add()` after a stop explicitly instead of quietly starting an already-cancelled child. Add a regression test covering the token state of an `add()` after a stop.

## Fix

- `Scope::add()` performs `source_ = std::stop_source{}` together with resetting `done` / `first_exception` when `pending == 0` opens a new batch. A stop request now applies only to the batch it was raised in, which is what the `requestStop()` documentation says.

## Verification

- `ScopeTest.ABatchAfterRequestStopGetsAFreshUnrequestedToken`: the new batch still receives the scope's token (`stop_possible()`) while `stop_requested()` is false.
- The existing `ChildrenRunInTheScopesEnvironment...`, `JoinStopAndWait...` and `ADetachedScope...` cases still pass.