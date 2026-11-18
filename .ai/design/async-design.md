# the async module (`src/base/async`)

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the lifetime, combinators, cancellation and blocking drive of a header-only coroutine runtime.
> **Sources**: `src/base/async/sdk/vine/async/**`, `tests/test_async`.
> **Backlog**: `../backlog/async.md`

## 1. Overview

### 1.1 Problem and background

`vn::async` is a header-only coroutine runtime (`vn::Async` is an INTERFACE target depending only on `vn::Global` + `vn::Core`),
28 headers and about 7.5k lines. It provides tasks, combinators, synchronisation primitives and schedulers, while writing the
lifetime windows such as “one completion wakes several waiters” down as rules that cannot be violated.

### 1.2 Goals

- `Task<T>` (lazy, single consumer), `SharedTask<T>` (copyable, result cached), `DetachedTask` (eager, self-destroying), `Scope`.
- Combinators: `whenAll` / `whenAny` / `withTimeout` / `retry` / `transform` / `andThen` / `finally`.
- Synchronisation primitives: `AsyncEvent`, `AsyncLatch`, `AsyncMutex`, `AsyncConditionVariable`, `AsyncSemaphore`,
  `AsyncReaderWriterLock`、`AsyncQueue`、`TaskCompletionSource`、`sleepFor`、`yield`。
- Scheduling: `InlineScheduler`, `resumeOn`, `scheduleOn`, `ThreadPoolScheduler`.

### 1.3 Non-goals

- It owns no thread pool (`vn::ThreadPool` lives in base/core), no cancellation token (`vn::CancellationToken` =
  `std::stop_token`) and no Qt event-loop scheduling (`appfw::async::Scheduler`).
- No implicit global pump; the caller passes the pump to the blocking drive.
- `Task<T&>` and a variadic `whenAll` that mixes void are not made core shapes.

### 1.4 One-paragraph conclusion

A coroutine frame's lifetime has one iron rule: **when one completion wakes several waiters, take one waiter out of the queue per round under the lock and resume it outside the lock**;
every other design decision (symmetric transfer, the two blocking gates, the explicit cancellation policy) serves that rule.

## 2. Design Points

### 2.1 The lifetime contract

- A suspended coroutine **cannot** be destroyed by another thread while the object it awaits is completing it.
- **Abandoning a wait is supported**: before destroying a waiting coroutine, the awaiter unregisters itself in its destructor; the library guarantees that “a destroyed waiter is never resumed”.
- Every awaiter carries its own unregister logic (`AsyncEvent::Awaiter::queued_`, `CvWaiter::queued_`,
  `AsyncMutex::LockAwaiter::waiter_`、`TcsAwaiter::handle_`、`SharedTaskAwaiter::handle_`、
  `SleepAwaiter::scheduled_` and so on).

### 2.2 One completion, several waiters: one out of the queue per round under the lock, resume outside

A resumed waiter may synchronously destroy its siblings (when the `whenAny` winner completes it destroys the still-suspended losers), and it may register a new waiter.
Therefore:

- no waiter pointer / handle may be held across `resume()`;
- you cannot “swap the whole list out and then resume one by one” — after the swap a sibling can no longer unregister itself, and the loop would resume a freed frame
  (the `SharedTask` defect was exactly this);
- the boundary is decided by a **generation number** (`AsyncEvent::set_epoch_`), not by a “tail pointer” — a destroyed waiter can be the boundary node.

Obeying this: `AsyncEvent::set`, `AsyncConditionVariable::notify_one/notify_all`, `AsyncSemaphore::release`,
`AsyncReaderWriterLock::resume`、`TaskCompletionSource::resumeWaitersOneByOne`、
`TimerService::run`、`SharedTask::runShared`。

`AsyncQueue` goes further: its waker may itself be the object that gets destroyed (a producer woken by `pop()` can win a `whenAny`,
and the following `Destroy` destroys the queue coroutine). Posting the wake to the timer thread is not enough — the waker still runs on its own thread,
and `Destroy` would destroy a frame that is running. So the queue's handoff is **suspend yourself first** (`detail::WakePeerAndContinue`):
an action on the timer thread wakes the peer first and resumes itself afterwards; when the peer wins the combinator, what gets destroyed is the already suspended queue frame.
The woken side re-checks its condition on waking and registers again when it does not hold, so a wake is allowed to be “spurious”.
The cost is that `pop()` / `push()` return only after the peer has run; `tryPush` / `tryPop` / `close` cannot suspend themselves
and can only post a wake.

### 2.3 Symmetric transfer

The `Task` awaiter, `TaskFinalAwaiter`, `WhenChild::FinalAwaiter`, `WhenAnyChild::FinalAwaiter` and
`YieldAwaiter::await_suspend` all **return handle** instead of `h.resume()`: an inline resume leaves one more frame on the machine stack for every await level
  (measured 336 B per `yield` at -O0, so 200k of them overflow the stack). A new awaiter first asks “can this return handle instead?”.
In a composition child the returned handle is the driver's, which is also what leaves the child **suspended** until the driver has run - the lifetime
  requirement §2.6 is about, not just the stack one.

### 2.4 Blocking for the result: two gates

- `Task::result()`: drives the body to completion on the calling thread (lazy as before); **it blocks, it does not dispatch**. The counterpart of C#'s `task.Result`,
  safe only when “nobody on that thread needs to turn the loop” (tools, tests, headless).
- `runToCompletion(task, pump)`: the same drive, calling `pump()` once per slice (200 µs by default). appfw's gate passes
  `MainThreadDispatcher::deliverPostedCalls()`; async installs no “global pump”.
- Pinned by tests: the pump is called exactly once when the task suspends; a task that never suspends must not pump at all; `result()` may be read once,
  and a second read throws `std::logic_error`.

### 2.5 Differences from other coroutine libraries

| Topic | This module | Why |
| --- | --- | --- |
| `Task<T&>` | not supported; `StorableValue` refuses to wrap it at the template boundary | the result is cached in an `std::optional`; supporting a reference would change three places for little gain. |
| the `Awaitable` concept | additionally checks that `await_ready` converts to bool and that `await_suspend` is only `void`/`bool`/a handle value | only a concept that looks up “like co_await” can accept this module's own awaitables. |
| `Generator`'s ranges contract | adds `iterator_concept` + postfix `++` + `operator->` | C++20 ranges only requires postfix `++`, and without it `weakly_incrementable` fails. |
| the cancellation model | an explicit `CancellationToken` + `CancelPolicy` (`StopAndWait` / `Destroy`) | both camps — P2300's environment token and C#'s explicit token — are served; the default follows the wider use. |
| a notify with no waiter | `AsyncConditionVariable` keeps it with a sticky flag | “release the AsyncMutex + register a wait” cannot be atomic, and dropping it deadlocks; `folly::coro::Baton::post` on an unawaited baton also wakes the next waiter, so the deviation has a mainstream precedent. |
| a composition waking its driver | the child hands the driver's handle back and returns it (§2.6) | the driver destroys the children when it finishes, and only a *suspended* coroutine may be destroyed - during `await_suspend` it is not ([expr.await]); `coroutine_handle::destroy` requires a suspended coroutine ([coroutine.handle.destroy]). |

### 2.6 A composition hands its driver back instead of waking it

- A composition has exactly one waiter - its own driver - so its wake-up is a **single-slot hand-off** (`detail::WhenSignal`), not an event with a waiter list.
- A child that finishes offers its `final_suspend::await_suspend` the driver's handle and **returns it** (symmetric transfer); it never resumes the driver there.
  Why: a driver that completes is destroyed together with its children, and a coroutine may only be destroyed while it is **suspended** - during `await_suspend` it is not, so an inline resume destroys a frame that is still executing.
  That is undefined behaviour, and no sanitizer reports it, because nothing writes to the frame after the return.
- `WhenSignal::park` / `wake` therefore keep two states rather than a flag to re-arm: a completion arriving while no driver is parked sets `pending_`, and the next park consumes it without suspending.
  `drainChildren` is then a plain `while (remaining != 0) co_await park`, with no window between clearing a flag and waiting in which a completion could be lost.
- The rule is checked where it can still be observed: `~WhenChild` / `~WhenAnyChild` assert that the child's final `await_suspend` is not on the stack (`async-design.md` §6.4).
- Cancellation still posts its wake-up (`detail::deferWhenWake`): a `std::stop_callback` may not resume the driver either, for the same same-thread-destruction reason.

Serves G1 (one wake-up concept with one owner), G10 (the cost is one mutex acquisition per completion) and G11 (the invariant is asserted at the point where a violation becomes observable).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | task types | `Task` / `SharedTask` / `DetachedTask` / `Scope` | landed |
| F2 | combinators | `whenAll` / `whenAny` / `withTimeout` / `retry` / `transform` / `andThen` / `finally` | landed |
| F3 | synchronisation primitives | event/latch/mutex/cv/semaphore/rwlock/queue/TCS/sleep/yield | landed |
| F4 | scheduling and generator | `InlineScheduler` / `resumeOn` / `scheduleOn` / `ThreadPoolScheduler` / `Generator` | landed |
| F5 | cancellation policy | `CancelPolicy::StopAndWait` / `Destroy` | landed |
| F6 | blocking drive | `Task::result()` / `runToCompletion(task, pump)` | landed |

### 3.1.1 F1 task types

- **Behaviour**: `Task<T>` is lazy and single-consumer; `SharedTask<T>` is copyable with a cached result; `DetachedTask` is eager and self-destroying;
  `Scope` owns dynamic child tasks.
- **Constraints** (C1): a `Task` must be driven by `co_await` or `result()`; a `SharedTask` result is cached once only.
- **Requirements** (R1): `Task<T&>` is not supported; return a pointer / `std::reference_wrapper` when a reference is wanted.
- **Acceptance** (T1): the Task/SharedTask/Scope cases in `test_async`.
- **Caveats** (N1): a `Scope` destructor does not wait; without `detach()` it only asks the children to stop, and a `co_await join()` is still needed to confirm they finished.

### 3.1.2 F2 combinators

- **Behaviour**: `whenAll` waits for all, `whenAny` for the first, `withTimeout` cancels on timeout, and `retry`/`transform`/`andThen`/`finally`
  compose results.
- **Constraints** (C2): `whenAll` defaults to `StopAndWait`; `whenAny` / `withTimeout` / `Scope::join` default to `Destroy`.
- **Requirements** (R2): a child that does not read the environment token holds `StopAndWait` up; pick `Destroy` explicitly when “never hang” matters.
- **Acceptance** (T2): `TaskTest.WhenAllStopAndWaitIsHeldByAChildThatIgnoresTheToken` and others.
- **Caveats** (N2): `Destroy` does not “request, then destroy”; a request only fires its own `stop_callback` when the frame is destroyed a few microseconds later.

### 3.1.3 F3 synchronisation primitives

- **Behaviour**: event/latch/mutex/cv/semaphore/rwlock/queue/TCS/sleep/yield.
- **Constraints** (C3): one completion waking several waiters must follow §2.2; a destructor unregisters itself and is `noexcept`.
- **Requirements** (R3): no resume under a lock; no resume on the canceller's stack; a cross-thread wake goes through `detail::deferWake()`;
  `AsyncQueue`'s `push`/`pop` suspend themselves before handing off (`WakePeerAndContinue`), while `tryPush`/`tryPop`/`close`
  only post a wake, so a wake may be spurious and a waiter must re-check its condition.
- **Acceptance** (T3): each primitive's cases in `test_async`, plus ASan/TSan.
- **Caveats** (N3): `AsyncConditionVariable`'s sticky flag is a deliberate deviation; do not “fix” it back into a lost wake.

### 3.1.4 F4 scheduling and the generator

- **Behaviour**: `InlineScheduler` / `resumeOn` / `scheduleOn` / `ThreadPoolScheduler`; `Generator<T>` is a synchronous single-pass generator.
- **Constraints** (C4): `Generator` is not awaitable; `ThreadPoolScheduler`'s `run` corresponds to C#'s `Task.Run`.
- **Requirements** (R4): a scheduler offers only `schedule()`/`resumeOn` and does not decide the thread policy for the caller.
- **Acceptance** (T4): the Scheduler/Generator cases in `test_async`.
- **Caveats** (N4): the window between `sleepFor`'s timer-thread resume and the final check is the caller's contract.

### 3.1.5 F5 cancellation policy

- **Behaviour**: an external cancel triggers `CancelPolicy`; `StopAndWait` calls `request_stop()` first and then waits for the last child to finish;
  `Destroy` destroys directly.
- **Constraints** (C5): cancellation is a **request** that the operation reports; `Destroy` does not “request, then destroy”.
- **Requirements** (R5): an injected token is visible only at the layer that was combined; a wrapper that does not forward it cuts the channel.
- **Acceptance** (T5): `TaskTest.WhenAllCancellation`, `ScopeTest.JoinCancellation…`.
- **Caveats** (N5): no direct resume inside `std::stop_callback`; everything goes through `TimerService::postWake()` + `deferWake()`.

### 3.1.6 F6 blocking drive

- **Behaviour**: `Task::result()` drives synchronously; `runToCompletion(task, pump)` calls the pump per slice.
- **Constraints** (C6): `result()` blocks only and does not dispatch; `runToCompletion` takes the pump from the caller.
- **Requirements** (R6): blocking on an application thread must use `runToCompletion` + `deliverPostedCalls()`, never `result()`.
- **Acceptance** (T6): the `result()`/`runToCompletion` cases in `test_async`; appfw's synchronous plugin gate.
- **Caveats** (N6): `Task::result()` may be read once; a second read throws `std::logic_error`.

## 4. Public Interface

### 4.1 Headers and modules

| Interface / header | Contract | Notes |
| --- | --- | --- |
| `Task.hpp` / `SharedTask.hpp` / `DetachedTask.hpp` / `Scope.hpp` | task types | see F1 |
| `When.hpp` / `WithTimeout.hpp` / `Retry.hpp` / `TaskCombinators.hpp` / `Finally.hpp` | combinators | see F2 |
| `AsyncEvent.hpp` / `AsyncLatch.hpp` / `AsyncMutex.hpp` / `AsyncConditionVariable.hpp` / `AsyncSemaphore.hpp` / `AsyncReaderWriterLock.hpp` / `AsyncQueue.hpp` / `TaskCompletionSource.hpp` | synchronisation primitives | see F3 |
| `Scheduler.hpp` / `ThreadPoolScheduler.hpp` / `Generator.hpp` / `Concepts.hpp` | scheduling / generator / concepts | see F4 |
| `StopToken.hpp` / `CancelPolicy` | cancellation | see F5 |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | a `Task` must be driven; a `SharedTask` result is cached once; `Task<T&>` is not supported. |
| C2 | `whenAll` defaults to `StopAndWait`; `whenAny`/`withTimeout`/`Scope::join` default to `Destroy`. |
| C3 | one completion waking several waiters: one out of the queue per round under the lock, resume outside. |
| C4 | no resume under a lock; no resume on the canceller's stack; a cross-thread wake goes through `deferWake()`. |
| C5 | cancellation is a request; `Destroy` does not “request, then destroy”. |
| C6 | `result()` blocks without dispatching; `runToCompletion` takes the pump from the caller. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | every awaiter carries its own unregister logic and its destructor is noexcept; a destroyed waiter is never resumed. |
| R2 | return handle wherever symmetric transfer is possible, never an inline resume. |
| R3 | an application's blocking gate uses `runToCompletion` + `deliverPostedCalls()`. |
| R4 | an injected cancellation token is visible only at the layer that was combined. |
| R5 | a `Scope` destructor does not wait; without `detach()` it asks the children to stop, and an explicit `join()` is what waits. |
| R6 | no implicit dispatch on `Task`, no global pump. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | a coroutine frame is never destroyed by another thread between swapping the waiter list out on the completing thread and resuming it one by one. |
| N2 | `AsyncEvent` decides its boundary with a generation number, not a tail pointer. |
| N3 | `AsyncConditionVariable`'s sticky flag keeps a notify that arrived with no waiter. |
| N4 | `TimerService` deliberately never destructs (a process-wide singleton plus a worker thread). |
| N5 | `SharedTask` takes its waiters out one at a time and never holds a handle across a resume. |

## 6. Caveats and Traps

### 6.1 A synchronous resume inside `std::stop_callback`

- How it fails: the coroutine frame holding the callback is destroyed while the callback runs, and `~stop_callback` hits the standard's undefined case (measured: abort).
- How to detect it: a direct `done.set()` in `whenRace`/`whenAllImpl`/`whenAny`/`Scope::join` turns the case red.
- How to prevent it: everything goes through `TimerService::postWake()` + `detail::deferWake()`, and the timer thread does the resume.

### 6.2 `TimerService`'s OOM exposure

- How it fails: `postWake`'s `deque::push_back` fails inside a `noexcept` callback.
- How to detect it: inject an allocation failure; it is the same spot as `requestAbort()`'s `early_.push_back`.
- How to prevent it: switch to an intrusive queue when it must be zero-allocation; currently owed to the backlog.

### 6.3 A `Scope` destructor does not wait

- How it fails: a child that ignores the stop token keeps running after the `Scope` is destroyed; the destructor does not wait for it.
- How to detect it: check `pendingChildren()`, or `co_await join()` explicitly on every path that needs the completion guarantee.
- How to prevent it: `join()` explicitly before leaving the scope; `detach()` says the destructor does not request a stop.

### 6.4 Returning to an inline wake-up in a composition child

- How it fails: a child that resumes its driver from its own `final_suspend::await_suspend` destroys its own frame.
  The driver completes, completing destroys the composition frame, and that destroys every child - including the one whose `await_suspend` has not returned.
  The standard only considers a coroutine suspended once `await_suspend` has returned, so the resume is undefined behaviour; current compilers do not fault and ASan does not report it, because the freed frame is not written to afterwards.
  This is what the module did before §2.6, where the wake-up was `AsyncEvent::set()` and `set()` resumes inline.
- How to detect it: `~WhenChild` / `~WhenAnyChild` assert that the child's final `await_suspend` is not on the stack - the flag is set around the wake-up, so the destructor sees it exactly when the old ordering is back.
  `tests/test_async`'s `WhenLifetimeTest` cases drive the only two ways a child can wake a **parked** driver: a `whenAll` whose last child finishes in the timer, and a `StopAndWait` `whenAny` that wakes twice.
  Putting `parked.resume()` back into `WhenSignal::wake` turns both red.
- How to prevent it: §2.6 - hand the handle back and let the compiler suspend the child first.

### 6.5 A granted reader destroyed before resuming would leave a queued writer stranded

- How it fails: `AsyncReaderWriterLock::takeWaitersLocked` grants every queued reader at once (`readers_ += waiting_readers_`) and lets
  `resume()` wake them one at a time. A writer that arrives after the grant queues, because `tryAcquireWriter` sees `readers_ > 0`.
  `dequeueReader` subtracts a granted reader from `readers_` when its awaiter is destroyed; should the counter reach zero that way, nothing
  calls `takeWaitersLocked()`, and the queued writer is never woken.
- How to detect it: the state `readers_ == 0 && writers_head_ != nullptr` with no wake in flight.
- How to prevent it: `dequeueReader` would have to hand the lock on when the counter it touches reaches zero, exactly as `unlockRead()` does.
  Not done, and **not reachable today**: the granted readers are popped and resumed by the same loop that granted them, so a granted reader can
  only be destroyed from inside another granted reader's resume - which always leaves that other reader alive to unlock, and so to hand the
  lock on. Recorded as an invariant for whoever next edits `dequeueReader`.

## 7. Rejected Options

| # | Option | Why not | ADR |
| --- | --- | --- | --- |
| D1 | `Task<T&>` | the result is cached in an `optional`; supporting a reference would change Task/SharedTask/When, three places, for little gain. | — |
| D2 | a variadic `whenAll` that mixes void | it would drop the void slots from the result tuple, an API shape change, so it was not done. | — |
| D3 | an implicit global pump / `Task` dispatching itself | who gets advanced is the host's policy and async should not guess it. | — |
| D4 | `SyncWait.hpp` / `syncWait()` | a duplicate of `Task::result()`; the mechanism moved into `Task.hpp` and one name remains. <!-- drift-ok --> | — |
| D5 | `appfw::async::sleep()` | a duplicate of `vn::async::sleepFor()` with no caller. | — |
| D6 | turning `try_lock`/`try_acquire` into camelCase | they mirror `std::mutex`/`std::counting_semaphore`, and the rename would be a breaking change. | — |
| D7 | a `set_stopped` cancellation result channel (a three-state `Task` result) | it would make cancellation not-an-error, but `Task<T>`'s result, every combinator, `result()` and the cancellation cases all change, while the layers above already classify `TaskCancelledException` as "cancelled"; the local tightening of `withTimeout` is the change to make instead, and `CancelPolicy` already expresses it. | — |
| D8 | an executor or thread-affinity parameter on `Task` (P2300's scheduler environment, asio's bound executor, folly's executor that propagates through every continuation) | it would make every coroutine carry a context most of them never use, and move the choice from the call site into the library. cppcoro chose the same absence and offers `resume_on` only, which is exactly `resumeOn` / `scheduleOn`; a caller that must land somewhere says so with a `co_await` (G8: no call site needs it today). | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | the normal / abandoned / destroyed paths of the task types | the Task/SharedTask/Scope cases in `test_async` |
| T2 | combinator cancellation and timeout | `TaskTest.WhenAllCancellation`, `WhenAllStopAndWaitIsHeldByAChildThatIgnoresTheToken` |
| T3 | no lost wake, no UAF, in the primitives | `test_async` + ASan/TSan |
| T4 | the blocking drive pumps exactly once, or not at all | the `result()`/`runToCompletion` cases in `test_async` |
| T5 | a mutation counter-check: reverting to the old spelling must turn it red | `git checkout -- <header>` → rebuild → run the target cases |

## 9. Status and Evolution

### 9.1 Current Status

Tasks, combinators, primitives and schedulers are in place; the cancellation policy closes on `StopAndWait`/`Destroy`; the task environment token and
`Scope`'s stop request are in place.

### 9.2 Known Gaps and Backlog

- Frame allocation is not done; the criterion it waits for is in `../backlog/async.md`.
- `6.5` is an invariant note, not a reachable defect: the accounting direction it describes cannot be reached under the framework contract in
  `async_global.hpp`. The composition wake-up ordering it sat next to was a real defect and is fixed; see §2.6 and §6.4.

### 9.3 Compatibility and Migration

There is no `SyncWait.hpp` / `syncWait()`; call sites use `x.result()`; <!-- drift-ok -->
There is no `appfw::async::sleep()`; use `vn::async::sleepFor()`. New code follows the headers in §4.

## 10. References

### 10.1 Code

- `src/base/async/sdk/vine/async/**`
- `src/base/core/sdk/vine/ThreadPool.hpp`
- `tests/test_async`

### 10.2 Documents and ADRs

- `.ai/memory/async.md`
- `../backlog/async.md`
- `../bugs/shared-task-batch-resume-uaf.md`

### 10.3 External Sources

- the corresponding semantics of cppcoro / folly / P2300 (see §2.5).
- the systems C2 was decided against: Trio's nursery and Kotlin's `coroutineScope` (cancel the siblings, then wait for
  all of them), Rust's `tokio::select!` (drop is cancellation, nothing is waited for) against `join!`, C#
  `Task.WhenAll` (a separate `Canceled` state, exceptions aggregated), and P2300 / asio (`set_stopped` plus a
  completion-condition parameter).
- the standard's undefined cases around `std::stop_token` / `std::stop_callback`.
- the standard's coroutine clauses the wake-up rules rest on: `[expr.await]` (a coroutine is suspended only once `await_suspend` returns)
  and `[coroutine.handle.destroy]` (a handle may only be destroyed while the coroutine is suspended).
- `folly::coro::Baton` — the sticky `post()` the condition variable's deviation follows, and the reason it has no lost-wakeup window: the
  baton *is* the mutex, so "release + register" is one operation rather than two.
