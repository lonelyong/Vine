# Bug: several value tasks from one `TaskCompletionSource` move the same result

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `TaskCompletionSource.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

When several `task()`s are taken from the same `TaskCompletionSource<T>` and awaited, only the first consumer gets the full result; later consumers read an object that has already been moved from. If several completed tasks read it from different threads at the same time, the result object can also be a data race.

## Root cause

`TcsState<T>` stores a single `std::optional<T>`, and every `TcsAwaiter::await_resume()` performs `std::move(*state_->value)`. Yet the public contract allows several independent tasks to be taken from one source and says they complete together, and the `setResult` documentation says the result is delivered to every waiting task; the implementation moves it as if for a single consumer.

## Impact and verification

- A standalone probe creates two `task()`s from one `TaskCompletionSource<std::string>` and reads them one after the other after setting the result to `"payload"`; the output is `payload|`, the second result empty.
- The existing `MultipleWaiters` case uses `TaskCompletionSource<int>` (moving an `int` is copying, which happens to hide the defect) and the void case is called `VoidSource`; neither checks whether several consumers read a complete value.
- No TSan run has covered concurrent `await_resume()` on tasks that are already ready; the current lock-free implementation of sharing one moved result carries that risk.

## Fix direction

Decide the result contract first: several consumers sharing one read-only value, a copy per consumer for copyable types, or a value-bearing source defined as single-consumer. Then make the state type, `await_resume`, the API documentation and the move-only constraint agree, and add sequential and concurrent tests with several value tasks.

## Fix

- `TcsAwaiter::await_resume()` returns `T`: a copy-constructible `T` is copied per consumer, so several tasks (concurrent awaits included) all read the complete value; a move-only `T` keeps the single-consumer move, and that limitation is stated in the class comment, in `task()` and in `setResult()`.
- The comment on `TcsState<T>` says the same: copied to every waiter, a move-only value can be taken once.

## Verification

- `TaskCompletionSourceTest.EveryValueTaskReceivesItsOwnCopy`: both `std::string` tasks read `"payload"`.
- The existing `MultipleWaiters`, `VoidSource` and concurrent `setResult` cases still pass.
