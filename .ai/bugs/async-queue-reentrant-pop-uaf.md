# Bug: `AsyncQueue::pop` can be destroyed re-entrantly after it wakes the producer (use-after-free)

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `AsyncQueue.hpp`, `When.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

When a bounded queue is full, a producer waits in `push()`; another task `pop()`s from the queue and the waiting producer is resumed synchronously. If the producer wins in `whenAny`, `whenAny` destroys the task frame that is still executing `pop()`, which then keeps accessing a freed coroutine frame.

## Root cause

`detail::popOne()` removes the element and resumes the waiting producer before returning the value. That resume can re-enter through `whenAny`'s completion path and destroy the current `AsyncQueue::pop()` coroutine frame; the code following `co_return co_await` in `pop()` then runs inside a destroyed frame. Resuming any waiter before the consuming operation has returned therefore lets the current coroutine on the call stack be destroyed indirectly.

## Impact and verification

- Minimal reproduction: a queue of capacity 1 is filled with one element; `whenAny` waits on both a `push()` blocked by the full queue and a `pop()` that runs after a delay.
- A standalone probe under Clang 23 + ASan reproduces the heap-use-after-free, with the read in `AsyncQueue::pop()` and the free on the path where `whenAny` destroys the still-running pop task.
- The 158 cases of `test_async` passed at the time, but none covered "pop wakes the producer, the producer wins the composition, and the pop task is destroyed at the same moment".

## Fix direction

Do not resume the producer before `pop()` has finished its own state transition and returned its result; design a wake ordering that cannot destroy the coroutine of an operation that is still executing, and add the `whenAny` + bounded-queue regression test described above together with ASan verification.

## Fix

- Hand-off rule: an operation that gives away a slot or an element **suspends itself first** (`detail::WakePeerAndContinue`), and then an action on the timer thread wakes the peer before resuming the operation. When the peer wins `whenAny`, `Destroy` destroys a **suspended** coroutine frame (and equally suspended ancestors), which is a legal destruction point.
- Merely posting the wake to the timer thread is not enough: the operation is still executing on its own thread, and the peer's `Destroy` would destroy a running frame (re-verified 2026-11-02: the worker thread runs `pop()`, the producer is woken by the timer, ASan reports `heap-use-after-free`).
- A waiter re-checks the condition when it wakes and re-registers if the condition does not hold (`QueuePopAwaiter` / `QueuePushAwaiter` can be awaited repeatedly).
- `WakePeerAndContinue`'s destructor deregisters its own handle: when the operation is destroyed first, the action only wakes the peer and does not resume the dead frame. The remaining `tryPush` / `tryPop` / `close`, which cannot suspend themselves, can only post the wake, and the header says so.

## Verification

- `AsyncQueueTest.PopWakingAProducerDoesNotDestroyTheExecutingPop`: a `push` on a full queue of capacity 1 and a delayed `pop` race in `whenAny` (50 rounds); ASan is clean, while before the fix the same scenario (worker thread plus an `AsyncEvent` waking the popper) reported `heap-use-after-free` with the read on the popper's resume.
- `AsyncQueueTest.PopHandsAFreedSlotOverBeforeItReturns` / `PushHandsTheItemOverBeforeItReturns`: the hand-off is ordered - by the time `pop()` / `push()` returns, the peer it woke has already run.
- The full `test_async` suite passes in Debug and under ASan.
- Semantic change (written into the header): `pop()` / `push()` return only after the peer has run; if the caller is the loser in a `Destroy` composition it is destroyed at that suspension point (the value `pop()` had already taken is discarded with it).
