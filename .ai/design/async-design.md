# the async module (`src/base/async`)

> **Status**: current
> **Last verified**: 2026-11-27
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

A coroutine frame's lifetime has one iron rule: **when one completion wakes several waiters, take one waiter out of the queue per round under the
lock, resume it outside the lock, and stop at the generation the call started with**; every other design decision (symmetric transfer,
the two blocking gates, the explicit cancellation policy) serves that rule. Two consequences run through the whole module: a borrowed
handle is registered and withdrawn by its owner (never merely written into shared state), and a capability that cannot do what it was
asked reports that instead of dropping the request.

## 2. Design Points

### 2.1 The lifetime contract

- A suspended coroutine **cannot** be destroyed by another thread while the object it awaits is completing it.
- **Abandoning a wait is supported**: before destroying a waiting coroutine, the awaiter unregisters itself in its destructor; the library guarantees that “a destroyed waiter is never resumed”.
- Every awaiter carries its own unregister logic (`AsyncEvent::Awaiter::queued_`, `CvWaiter::queued_`,
  `AsyncMutex::LockAwaiter::waiter_`、`TcsAwaiter::handle_`、`SharedTaskAwaiter::handle_`、
  `SleepAwaiter::scheduled_` and so on).
- The duty covers a slot that is not a list. A composition has exactly one waiter, so its park slot (`detail::WhenSignal`) is a borrowed
  handle in shared state, and `~WhenPark` withdraws it (§2.6). Without that, a posted cancellation wake arriving after the driver was
  destroyed resumes a freed frame - the one path “no waiter pointer across a resume” cannot reach, because the waiter is found through
  the state rather than through a list.

### 2.2 One completion, several waiters: one out of the queue per round under the lock, resume outside

A resumed waiter may synchronously destroy its siblings (when the `whenAny` winner completes it destroys the still-suspended losers), and it may register a new waiter.
Therefore:

- no waiter pointer / handle may be held across `resume()`;
- you cannot “swap the whole list out and then resume one by one” — after the swap a sibling can no longer unregister itself, and the loop would resume a freed frame
  (the `SharedTask` defect was exactly this);
- the boundary is decided by a **generation number** (`AsyncEvent::set_epoch_`, `CvState::notify_epoch_`), not by a “tail pointer” — a
  destroyed waiter can be the boundary node — and not by “the list is empty”.
- “Emptiness” is the boundary of a *drain*, not of a notify. A waiter resumed inside the loop can re-check its predicate, find it still
  false, and register again; ending on emptiness then releases that waiter too, so a predicate loop hands the notifier an endless stream
  of the same waiter and `notify_all` never returns. A generation bound releases exactly the waiters that were queued when the call started.

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
- The hand-off has a second half. The driver can be destroyed while parked - a `Destroy` parent, a `Scope` teardown - and a posted
  cancellation wake can still be queued behind it. The park slot is therefore *registered*, not merely written: `~WhenPark` calls
  `WhenSignal::unpark`, so a later `wake()` finds nothing and reports `noop_coroutine()` instead of handing back a freed frame.
  `unpark` clears the slot only when it still holds the handle being withdrawn, so it can never cancel a later park.

Serves G1 (one wake-up concept with one owner), G10 (the cost is one mutex acquisition per completion) and G11 (the invariant is asserted at the point where a violation becomes observable).

### 2.7 A hop that cannot be taken is reported, not dropped

- `ThreadPool::tryEnqueue` answers whether the pool accepted the callable; a stopped pool answers `false` and never runs it.
- `ThreadPoolScheduler::ScheduleAwaiter::await_suspend` turns that `false` into `std::logic_error`. The awaiter's contract is “resume on a
  worker of this pool”, and there is no honest fallback: resuming inline silently moves the coroutine onto a thread the caller did not ask
  for, and returning without scheduling parks it forever. A throw is the module's existing answer to a programming error (awaiting an empty
  `Task`, `SharedTask::result()` before completion).
- `ThreadPool::enqueue` refuses the same pool state at the call, with `std::logic_error`: a caller holding a `future` therefore
  holds a task that will run, and nothing has to be decoded from a `broken_promise` later.
- Both forms are templates over the callable and funnel into one private `pushTask(std::function<void()>)`, so the queue's type
  erasure happens once and the pair differs only in what it answers with.

Serves G4 (the failure mode is in the signature and the doc), G6 (the `try*` pair means here what `setResult`/`trySetResult` and
`push`/`tryPush` mean), G10 (one type-erased entry point, one allocation policy) and G11 (a teardown race produces an exception at
a known point, not a hang).

### 2.8 `try*` reports what it did

- `AsyncQueue::tryPop` returns `std::optional<T>`: `nullopt` means “nothing was taken”, and a value means exactly one item left the queue.
- It therefore never reports a consumed-but-undeliverable element as “empty”, which the old `bool tryPop(T& out)` did: a throwing move
  became `false`, indistinguishable from an empty queue.
- Returning the value instead of assigning into a caller-provided sink narrows the requirement to what `pop()` already needs - `T`
  move-constructible - and drops the move-assignment and default-construction the out-parameter form implied.
- A throwing move still propagates; the item has left the queue by then, and `pop()` reports the same thing as the task's error. Nothing
  can make a throwing move safe, so the rule is only that the failure is never disguised as “there was nothing there”.

Serves G4 (return value, requirement and failure mode agree) and G6 (the `try*` form of a suspending operation means the same thing on
both sides of the queue).

### 2.9 A single-pass generator has one position

- `Generator` is an input range: the position is the coroutine, so `begin()` may be called more than once. It resumes the body only on the
  first call (`started_`); later calls report the current position, which is the input-range rule (the iterators from one position compare
  equal and are invalidated by advancing).
- An exhausted generator yields `end()`. Its frame is parked at `final_suspend` and is `done()`, and resuming a done coroutine is undefined
  behaviour, so a second `begin()` after the range ran out reports an empty range instead of crashing.
- `Generator<T>` is constrained by `ProducedValue`, not `StorableValue`: a generator produces values, while `StorableValue` admits `void`
  for `Task`/`SharedTask`. `Generator<void>` is therefore rejected at the template boundary instead of failing inside `std::optional<void>`.

Serves G4 (the position rule is stated and enforced) and G12 (the constraint names what the concept is).

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

- **Behaviour**: event/latch/mutex/cv/semaphore/rwlock/queue/TCS/sleep/yield. `AsyncQueue::tryPop()` answers with the item or `nullopt`.
- **Constraints** (C3): one completion waking several waiters must follow §2.2; a destructor unregisters itself and is `noexcept`;
  `tryPop` reports a taken item as a value and a stranded one as an exception, never as `nullopt` (§2.8).
- **Requirements** (R3): no resume under a lock; no resume on the canceller's stack; a cross-thread wake goes through `detail::deferWake()`;
  `AsyncQueue`'s `push`/`pop` suspend themselves before handing off (`WakePeerAndContinue`), while `tryPush`/`tryPop`/`close`
  only post a wake, so a wake may be spurious and a waiter must re-check its condition.
- **Acceptance** (T3): each primitive's cases in `test_async`, plus ASan/TSan.
- **Caveats** (N3): `AsyncConditionVariable`'s sticky flag is a deliberate deviation; do not “fix” it back into a lost wake.

### 3.1.4 F4 scheduling and the generator

- **Behaviour**: `InlineScheduler` / `resumeOn` / `scheduleOn` / `ThreadPoolScheduler`; `Generator<T>` is a synchronous single-pass generator.
- **Constraints** (C4): `Generator` is not awaitable and its `T` satisfies `ProducedValue`; `ThreadPoolScheduler`'s `run` corresponds to
  C#'s `Task.Run`; a pool that refuses a hop is reported as `std::logic_error` (§2.7).
- **Requirements** (R4): a scheduler offers only `schedule()`/`resumeOn` and does not decide the thread policy for the caller - including
  when the pool refuses: the fallback is never “run it here”.
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
| C7 | a capability that cannot do what it was asked reports it: a refused hop throws (§2.7), a stranded element is an exception and never `nullopt` (§2.8). |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | every awaiter carries its own unregister logic and its destructor is noexcept; a destroyed waiter is never resumed. |
| R2 | return handle wherever symmetric transfer is possible, never an inline resume. |
| R3 | an application's blocking gate uses `runToCompletion` + `deliverPostedCalls()`. |
| R4 | an injected cancellation token is visible only at the layer that was combined. |
| R5 | a `Scope` destructor does not wait; without `detach()` it asks the children to stop, and an explicit `join()` is what waits. |
| R6 | no implicit dispatch on `Task`, no global pump. |
| R7 | an RAII guard's constructor and move constructor are `noexcept` exactly when the callback's move is (`Finally`). |
| R8 | a reference returned by `result()` / `co_await` stays valid while some handle to the same computation is alive; it does not outlive the last `SharedTask` copy. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | a coroutine frame is never destroyed by another thread between swapping the waiter list out on the completing thread and resuming it one by one. |
| N2 | `AsyncEvent` decides its boundary with a generation number, not a tail pointer. |
| N3 | `AsyncConditionVariable`'s sticky flag keeps a notify that arrived with no waiter. |
| N4 | `TimerService` deliberately never destructs (a process-wide singleton plus a worker thread). |
| N5 | `SharedTask` takes its waiters out one at a time and never holds a handle across a resume. |
| N6 | `notify_all` releases the generation queued when the call started; a waiter that registers during the loop waits for the next notify. |
| N7 | a parked composition driver withdraws its handle in `~WhenPark`; a later `wake()` reports `noop_coroutine()`. |

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

### 6.6 A composition driver destroyed while parked

- How it fails: the driver's frame is destroyed (a `Destroy` parent, a `Scope` teardown) while its handle sits in `WhenSignal::parked_`.
  A cancellation wake already posted to the timer thread keeps the *state* alive, so it runs, claims the stale handle and resumes a freed
  frame. Neither ASan nor the debug asserts see it, because the state that holds the handle is not part of the frame.
- How to detect it: `WhenLifetimeTest.ADestroyedParkWithdrawsItsHandle` parks a live probe frame, destroys the awaiter, and requires
  `wake()` to report `noop_coroutine()`. Removing `WhenSignal::unpark` from `~WhenPark` turns it red.
- How to prevent it: `~WhenPark` withdraws the handle (§2.1, §2.6); the park slot is a registration, not a write into shared state.

### 6.7 The element a throwing move strands

- How it fails: `detail::popOne` move-constructs the front element out of the deque. A throwing move leaves the element either still in the
  queue but partially moved from (the throw happens before `pop_front`) or destroyed with the returned `optional` (it happens on the way out).
- How to detect it: a type that throws on the Nth move (`ThrowingMove` in `tests/test_async`), with the counter swept across the moves.
- How to prevent it: nothing makes a throwing move safe. The rule is only that the failure is *reported* - as the task's error from
  `pop()`, as a propagated exception from `tryPop()` - and never disguised as “the queue was empty” (§2.8).

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
| D9 | keeping `bool tryPop(T& out)` | G4: it requires move-assignment the API never states, and reports a stranded element as “nothing there”. `std::optional<T> tryPop()` carries the same information with fewer requirements and no lie (§2.8). | — |
| D10 | resuming inline when the pool refuses a hop | G4: it would silently drop the awaiter's only promise, the thread it lands on. A throw keeps the promise and turns a teardown race into a reported error (§2.7). | — |
| D11 | rejecting `co_await` on an rvalue `SharedTask` (`operator co_await() const&& = delete`) | it would forbid the safe forms (`auto v = co_await sharedTask(t);` copies the value) to prevent the unsafe one (`const T& v = co_await sharedTask(t);`). G8: a lifetime requirement (R8) is the narrower cost. | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | the normal / abandoned / destroyed paths of the task types | the Task/SharedTask/Scope cases in `test_async` |
| T2 | combinator cancellation and timeout | `TaskTest.WhenAllCancellation`, `WhenAllStopAndWaitIsHeldByAChildThatIgnoresTheToken` |
| T3 | no lost wake, no UAF, in the primitives | `test_async` + ASan/TSan |
| T4 | the blocking drive pumps exactly once, or not at all | the `result()`/`runToCompletion` cases in `test_async` |
| T5 | a mutation counter-check: reverting to the old spelling must turn it red | `git checkout -- <header>` → rebuild → run the target cases |
| T6 | a destroyed park leaves no handle behind | `WhenLifetimeTest.ADestroyedParkWithdrawsItsHandle` |
| T7 | `notify_all` releases one generation only, and returns | `AsyncConditionVariableTest.NotifyAllReleasesOnlyTheGenerationItWasCalledWith` |
| T8 | a refused hop is an exception, not a hang | `ThreadPoolSchedulerTest.ASchedulingRequestOnAStoppedPoolThrows`, `ThreadPoolTest.TryEnqueueReportsAStoppedPool`, `ThreadPoolTest.EnqueueOnAStoppedPoolThrows` |
| T9 | the generator's position rule and its constraint | `GeneratorTest.YieldsSequence`, `GeneratorTest.SecondBeginReportsTheCurrentPosition`, the `ProducedValue` `static_assert`s |

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
