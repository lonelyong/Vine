# async notes (`src/base/async`)

> **Known flake (observed 2026-10-13, unfixed)**: `test_async` **sometimes hangs** - in one full CTest
> run it ran for **over 7 minutes, accumulated 0.2 s of CPU across 20 threads** (the process was alive
> but stopped progressing), while another run the same day passed. Neither the hang nor the pass produces
> a new log line, so the suspicion is a deadlock on the waiter/waker path (the boundary of the three
> iron rules). Trigger = the next time it blocks the gate; finding it needs a stack trace, so do not
> re-derive it as a new problem.

> Detailed design, the differences from cppcoro/P2300 and the known risks: `.ai/design/async-design.md`.

> **Known red (observed 2026-10-19, unfixed)**: `ctest`'s `test_async` **fails reliably** on this machine
> at `AsyncDefectRegressionTest.YieldLoopStackIsConstant` (`depth 319936` vs the expected `< 65536`; it
> fails the same way when run alone, so it is not a flake). The comment says "buggy build ~1.7 MB" while
> the measured 312 KB still exceeds the threshold ⇒ suspected compiler/build-type-dependent frame size.
> Unrelated to graphics, but it keeps a full `ctest` at 23/24 - look here first when async is next
> touched. Next step (P2) design and go/no-go: `.ai/backlog/async.md`.
> This file holds only "what to remember the moment you start working".

## Structure

- Pure header-only; `vn::Async` is an INTERFACE target (depending only on `vn::Global` + `vn::Core`).
- 26 headers; all under `sdk/vine/async/`, classes in `namespace vn::async`.
- Tasks: `Task<T>` (lazy, single consumer), `SharedTask<T>` (copyable, caches the result),
  `DetachedTask` (eager, self-destroying), `Scope`.
- Combinators: `whenAll` / `whenAny` (vector and variadic, including all-void variadic), `withTimeout`,
  `retry`, `transform`, `andThen`.
- Primitives: `AsyncEvent`, `AsyncLatch`, `AsyncMutex` (+`AsyncLockGuard`), `AsyncConditionVariable`,
  `AsyncSemaphore`, `AsyncReaderWriterLock`, `AsyncQueue`, `TaskCompletionSource`, `sleepFor` (a shared
  timer service), `yield`.
- Concepts: `Awaitable` (expands `operator co_await` the way co_await does), `Schedulable`,
  `StorableValue`.

## The three iron rules

1. **One completion wakes N waiters**: dequeue one under the lock per round and resume outside it; never
   "swap the whole set out and resume them one by one" (that is where `SharedTask`'s 2026-09-17 UAF came
   from). The generation counter (`AsyncEvent::set_epoch_`) is the boundary, not a tail pointer.
2. **When a symmetric transfer is possible, `return handle`** rather than `h.resume()` (otherwise every
   await level adds a frame and a `yield` loop overflows the stack).
3. **Never hold a lock across a resume, and never resume on the canceller's stack** (the latter is why
   `Sleep.hpp::requestAbort` has its `early_` queue; the fourth `test_gui` full regression found this).

## Contracts and boundaries

- A suspended coroutine **must not** be destroyed concurrently with the thread that is completing it
  (`async_global.hpp`); **abandoning the wait** is supported (the awaiter deregisters itself).
- Cancellation is an explicit `CancellationToken` (= `std::stop_token`); on cancellation/timeout a
  combinator **destroys** its unfinished children outright, rather than P2300's "request stop, then wait
  for it to finish".
- `AsyncConditionVariable` keeps a notification for a zero wait with a **sticky flag** (a deliberate
  deviation from `std::condition_variable`).
- `Task` / `SharedTask` / `Generator` value types are constrained by `StorableValue`: **no references,
  arrays or function types** (use a pointer or `reference_wrapper`).
- Variadic `whenAll` / `whenAny` support only "all void" or "all non-void"; a mixed set goes through
  `vector<AnyTask>` + `discard()`.
- `AsyncMutex::try_lock` / `AsyncSemaphore::try_acquire` use snake_case (matching std) and are a
  **deliberate exception** to the repository's camelCase rule.
- **Blocking for a result has exactly two doors** (2026-09-26): `Task::result()` = pure blocking (C#'s
  `task.Result`, with the same deadlock trap: if the body's next step needs the calling thread to keep
  pumping, it locks itself up) / `runToCompletion(task, pump)` = the same drive with a `pump()` every
  200 µs (the pump comes from the caller; async installs **no global pump**). `SyncWait.hpp` and `syncWait()` are deleted. <!-- drift-ok -->

## Duplicate implementations (resolved)

- `vn::appfw::async::sleep()` was **deleted** (2026-09-17): it duplicated `vn::async::sleepFor()` and had
  no callers left; two `test_asyncqt` cases were removed with it. `appfw::async::Scheduler` (Qt event
  loop scheduling) is not a duplicate and stays.

## Traps

- **The thread that creates a coroutine frame must outlive the coroutine**: create a `DetachedTask` on a
  thread that will exit, let it suspend and resume it from another thread ⇒ the stack segment holding
  the frame disappears with that thread and the resume is a SIGSEGV (measured 2026-09-17: in `gdb` the
  crash site's `this` pointed into an exited thread's stack). To resume across threads, keep the creating
  thread in an event loop or in `Task::result()`: `test_asyncqt`'s
  `MainThreadDispatcherResumesOnApplicationThread` is the pattern - the application thread creates, a
  timer thread resumes first (asserting "definitely not on the application thread"), then it hops back.

## Commands

- Build and run: `cmake --build build --target test_async && ./build/bin/test_async` (140 cases today).
- The TSan/ASan recipes, and the trick that makes a stale-resume defect deterministic (padding inside the
  frame plus settling after `Task::result()`), are in the design doc §7.
