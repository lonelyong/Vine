# Bug: the `noexcept` paths of `AsyncQueue` terminate on an allocation or element-move failure

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `AsyncQueue.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

If allocation fails while `AsyncQueue` creates its shared state or enqueues, or if an element's move constructor throws, the exception can cross a `noexcept` boundary and terminate the process instead of being reported as a failed operation.

## Root cause

- The `AsyncQueue` constructor is declared `noexcept`, but its members are initialised with `std::make_shared`, and an allocation failure throws `std::bad_alloc`.
- The `QueuePushAwaiter` constructor is declared `noexcept`, but storing `T` by value can throw from its move constructor.
- `await_suspend()` of a bounded push is declared `noexcept` and calls `enqueueLocked()` from inside it; the queue's `deque::push_back` allocation or `T`'s move constructor can throw.

## Impact and verification

- This is a code review finding based on the exception specifications and the call paths; it was not verified by allocation-failure injection or by a runtime throwing-move probe.
- The public constraints on `T` do not require a nothrow move constructor, and the constructors do not say that an allocation failure terminates.
- Enqueueing from `await_resume()` on the ordinary non-suspending path has no such `noexcept` boundary, but it can still terminate when the awaiter's construction fails.

## Fix direction

Make the queue's failure contract uniform: remove the `noexcept` that does not hold, or turn the enqueue that can fail into an error channel the awaiter reports normally; and state whether `T` must be nothrow-movable. Add tests or compile-time constraints for the allocation-failure and throwing-move paths.

## Fix

- The `AsyncQueue` constructor loses `noexcept`: an allocation failure in `make_shared` now throws to the caller.
- The `QueuePushAwaiter` constructor loses `noexcept`: `T`'s move constructor may throw.
- `QueuePushAwaiter::await_suspend` must keep `noexcept`: a failed enqueue (a throwing move or a failed deque allocation) is captured into `failure_` and rethrown by `await_resume()` on the coroutine's own stack, the same convention as `detail::tryRegisterWaiter`.
- `T` is not required to be nothrow-movable; a throwing move is reported as the task's error.

## Verification

- `AsyncQueueTest.AThrowingMoveFailsThePushInsteadOfTerminating`: each move of `T` is made to throw in turn, and every one must reach the caller as the task's error (before the fix it was `std::terminate`).
- The full `test_async` suite passes in Debug and under ASan.
