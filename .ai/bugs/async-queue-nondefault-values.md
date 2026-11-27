# Bug: `AsyncQueue::pop` implicitly requires a default-constructible, assignable element

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `AsyncQueue.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

`AsyncQueue<T>::pop()` fails to compile for a legitimate non-default-constructible, movable type, even though the queue only has to move one enqueued element out.

## Root cause

`detail::popOne()` declares `T value;` and move-assigns the front element into it, which additionally requires `T` to be default-constructible and move-assignable. The template and API documentation of `AsyncQueue` never states those constraints, and the other queue operations pass values by move construction and move enqueue.

## Impact and verification

- A compile probe instantiates `AsyncQueue<T>::pop()` with a type whose default constructor is deleted and which provides a move constructor and move assignment; it fails at the default construction in `popOne()`.
- That type has the movable semantics a queue element needs, yet the pop API cannot be used with it.

## Fix direction

Construct the return value from the front element, so the unnecessary default construction and assignment requirement disappears; add compile-time and runtime tests for the element types that are supported, including one that is not default-constructible but is move-constructible.

## Fix

- `detail::popOne()` and `QueuePopAwaiter::await_resume()` use `std::optional<T>` plus `emplace(std::move(front))`: only move construction of `T` is required.
- The contract of `pop()` is narrowed to "`T` is move-constructible"; `tryPop(T&)` still needs move assignment because the value is written into the caller's sink, and the class comment now says so.- **Correction (2026-11-27)**: the `tryPop(T&)` sentence above is history. `tryPop()` now returns
  `std::optional<T>` instead of assigning into an out-parameter, so it carries the same "move-constructible only"
  requirement as `pop()`, and a thrown element move propagates instead of being reported as "the queue was empty".
  See `.ai/design/async-design.md` §2.8.
## Verification

- `AsyncQueueTest.PopSupportsAMoveOnlyTypeWithoutDefaultConstruction`: a type with both the default constructor and move assignment deleted can still `push` and `pop`.
