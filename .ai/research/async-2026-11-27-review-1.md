# async review 1: what `Destroy` assumes, who owns a completion, and which thread the continuation runs on

> **Status**: draft, non-authoritative - a working record. §2 is what the tree states, §3 is the findings, §4 is how the
> measurements were taken, §5 is the peer material, §6 proposes an order, §7 lists the verification commands. This round
> changed no file outside this page and its index entry; every finding is a decision left to the owner.
> **Last verified**: 2026-11-27, tree `8d96fbb2` on `dev`.
> **Scope**: all 28 headers of `src/base/async/sdk/vine/async/` read line by line, against the contracts
> `.ai/design/async-design.md` states. Three questions: **does each contract hold on every path it is offered to** (the
> compositions' `Destroy` policy against a child on another thread), **who keeps a primitive alive while it resumes its
> waiters**, and **where does a continuation run after a wake**. `tests/test_async/AsyncTest.cpp` was read by its test
> index (183 cases, all green on this tree) and not line by line; the probes in §4 are what the suite does not contain.
> **Sources**: `src/base/async/sdk/vine/async/{Task,When,WithTimeout,Sleep,AsyncQueue,TaskCompletionSource,AsyncLatch,
> AsyncEvent,AsyncConditionVariable,ThreadPoolScheduler,Scheduler,Retry,Generator,Scope,SharedTask}.hpp`,
> `src/base/core/sdk/vine/ThreadPool.hpp`, `src/base/core/src/ThreadPool.cpp`, `.ai/design/async-design.md`,
> `.ai/memory/async.md`, `.ai/backlog/async.md`.
> **Backlog**: `../backlog/async.md` - this round adds no entry; §6 lists what it would add, for approval.
> **Numbering**: `Q#` is this page's own set. The module's `F#` / `C#` / `R#` / `N#` / `D#` / `T#` / `G#` remain the design
> page's.
> **Rule of evidence**: every finding names a `file:line`, a test, or a report produced by a command quoted beside it.
> No peer source was read this round, so nothing below is a statement about another library.

## 1. Why this round is needed

The module's lifetime rule has one boundary it states everywhere - a suspended coroutine is never destroyed while it is
being completed - and eight recorded defects were closed against it. This round asks the question that rule takes for
granted: **is the thing being destroyed suspended at all, and is the handle it published withdrawn?** Two paths say no.
A third question is cheaper but bites the first user who meets it: after `co_await sleepFor(16ms)`, what thread is the
next line on?

## 2. What the tree states today

- `CancelPolicy::Destroy`: "the unfinished children are destroyed as soon as the composition completes ... The child must
  be safe to destroy at any suspension point" (`Cancellation.hpp:34-37`). `whenAny`, `withTimeout` and `Scope::join`
  default to it (`When.hpp:616`, `When.hpp:1152`, `WithTimeout.hpp:80`, `WithTimeout.hpp:100`, `Scope.hpp:216`).
- Every awaiter carries its own unregister logic and "a destroyed waiter is never resumed" (`async-design.md` R1, §2.1).
  Read in the tree: `SleepAwaiter::~SleepAwaiter` claims its node (`Sleep.hpp:358-364` -> `TimerService::cancel`,
  `Sleep.hpp:216-220`); `~WakePeerAndContinue` clears its registration (`AsyncQueue.hpp:89-93`); the list awaiters erase
  themselves under the owner's lock.
- The queue's hand-off exists because "`Destroy` would destroy a frame that is running" (`async-design.md` §2.2): an
  operation that wakes a peer suspends first.
- A primitive "must outlive every coroutine awaiting it" (`AsyncEvent.hpp:26`, `AsyncConditionVariable.hpp:186`). A
  `TaskCompletionSource`'s state "lives behind a shared_ptr so a task obtained from task() keeps the state alive even
  after the source itself is destroyed" (`TaskCompletionSource.hpp:23-27`).
- "A scheduler offers only `schedule()` / `resumeOn` and does not decide the thread policy for the caller"
  (`async-design.md` §3.1.4 R4). A cancellation wake "is posted to the timer thread" (§2.6, N5).

## 3. Findings

### Q1 (composition x thread pool / high): `CancelPolicy::Destroy` destroys a child that is not suspended on a withdrawable awaiter

**(a) The default `withTimeout` over pool work is a use-after-free whenever the timeout fires.** `withTimeout(runOn(pool, f), d)`
races `f` against a timer under `Destroy` (`WithTimeout.hpp:80-86`). When the timer wins, the driver resumes on the timer
thread, rethrows at `When.hpp:1191`, and unwinding its frame destroys every child (`When.hpp:1171`): `composeAnyChild`
(`When.hpp:894`) destroys the `runOn` task, `Task::Awaiter::destroy` (`Task.hpp:680`) destroys a frame that is at that
moment executing `std::invoke` on a worker. Measured - ASan, 400 ms callable, 50 ms timeout:

```
ERROR: AddressSanitizer: heap-use-after-free ... WRITE of size 1 ... thread T1          (T1 = the pool worker)
    #0 vn::async::detail::TaskPromiseReturn<int>::return_value          Task.hpp:71
    #1 vn::async::runOn$_ResumeCoro$1                                   ThreadPoolScheduler.hpp:120
freed by thread T2 here:                                                (T2 = the timer thread)
    #2 vn::async::Task<int>::Awaiter::destroy                            Task.hpp:680
    #4 vn::async::detail::composeAnyChild$_ResumeCoro$1<int>             When.hpp:894
    #6 std::vector<...WhenAnyChild<int>...>::~vector                     (unwinding whenAny's frame)
   #26 vn::async::whenAny$_ResumeCoro$1<int>                             When.hpp:1191
   #27 vn::async::detail::TimerService::run                              Sleep.hpp:323
```

The worker comes back from `f` and writes its result into a freed frame. This is the documented failure mode - "Destroying,
through its handle, a coroutine that is not suspended is undefined behavior" (cppreference, Coroutines) - and the
`in_await_suspend` assert (`When.hpp:419`, `When.hpp:857`) cannot see it, because the child is not in `await_suspend`;
it is in the callable.

**(b) A pool hop is the one awaiter that publishes its handle and cannot withdraw it.** `ScheduleAwaiter::await_suspend`
captures the handle raw into the pool's queue - `pool_.enqueue([h]() mutable { h.resume(); })`
(`ThreadPoolScheduler.hpp:62`) - and has no destructor. A loser destroyed while its hop is still queued (a saturated
pool: `whenAny(scheduleOn(pool, ...), quick())`) leaves a closure that resumes a freed frame. Measured - one worker,
held by a gate task; the loser's frame is freed by `whenAny`, then the gate opens:

```
ERROR: AddressSanitizer: access-violation on unknown address 0x000000000370 ... pc points to the zero page ... T1
    #1 std::_Binder<...,`vn::async::ThreadPoolScheduler::ScheduleAwaiter::await_suspend'::`2'::<lambda_1> >::operator()<>
    #8 `vn::ThreadPool::start'::`7'::<lambda_1>::operator()      ThreadPool.cpp:68
```

**(c) The same window exists on the timer thread (by reading).** A posted queue wake resumes the popper on the timer
thread (`AsyncQueue.hpp:197`), and the popper runs there until its next suspension point. A sibling that completes on
another thread inside that run destroys a running frame. §2.2's hand-off rule protects the operation that *handed off*;
nothing protects the peer it wakes.

**Why neither policy makes `withTimeout(runOn(...))` a bound.** `StopAndWait` asks the child to stop through its
environment token; a plain callable cannot read one, so the composition waits for the callable to return (`When.hpp:1183-1185`).
`Destroy` frees the frame under the callable (a). `whenAll` is safe only because its default is `StopAndWait` and it never
finishes early under `Destroy` (`When.hpp:335-340`).

**In-tree exposure.** No caller outside the module reaches `whenAny` / `withTimeout` / `runOn` / `scheduleOn` (§7, last
command). `WithTimeoutTest` races a sleeper and a task that never completes; the seven `ThreadPoolSchedulerTest` cases
(`AsyncTest.cpp:1638-1672`, `:4070-4088`) compose nothing. So this is latent, but `withTimeout(run(...), d)` is the
spelling a host reaches for first.

**Options, for the owner.**
1. State the precondition where it is offered: `ThreadPoolScheduler.hpp`, `withTimeout`, and the `Destroy` paragraph in
   `Cancellation.hpp` - "Destroy bounds only a child that suspends on this module's withdrawable awaiters; a child on a
   pool is bounded by nothing."
2. Make a pool-side coroutine self-owning, the way `TimerNode` and `WakePeerAndContinue::HandOff` already are: the queued
   closure holds a keepalive and an `abandoned` flag; `~ScheduleAwaiter` sets the flag instead of letting the frame go, and
   the worker destroys the frame when the body returns. `Destroy` then means "forget" for a pool child, (a) and (b) both
   close, and the hop stops being the exception to R1.
3. A debug assert in the spirit of `in_await_suspend`: a `running_` flag set by the worker closure and checked by
   `~Task::Awaiter`, so (a) is red in a debug build the moment a test composes `runOn` with `withTimeout`.

### Q2 (completion ownership / high-medium): the completing side of a `TaskCompletionSource` loops on a reference to the source's own member

`resumeWaitersOneByOne(const std::shared_ptr<...>& state)` (`TaskCompletionSource.hpp:155`) is called with the member
`impl_` from eight sites (`:233`, `:253`, `:275`, `:310`, `:388`, `:406`, `:428`, `:463`). After each `h.resume()`
(`:167`) the loop takes `state->mutex` again (`:160-161`). A waiter that destroys its source from inside that resume - the
session that owns the source closes when the wait ends - leaves the loop with a dangling reference. Measured - ASan, one
waiter that does `delete &src` after `co_await src->task()`:

```
ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8 ... thread T0
    #0 vn::async::detail::resumeWaitersOneByOne<int>     TaskCompletionSource.hpp:161
    #1 vn::async::TaskCompletionSource<int>::setResult   TaskCompletionSource.hpp:233
freed by thread T0 here:
    #2 consume$_ResumeCoro$1                              (the waiter, inside the resume)
    #3 std::coroutine_handle<void>::resume
    #4 vn::async::detail::resumeWaitersOneByOne<int>      TaskCompletionSource.hpp:167
```

Same report with two waiters: the first resumed deletes the source, and the loop dereferences the member before it can
reach the second waiter. The header protects the consuming side by design (`:23-27`) and leaves the completing side on a
reference into the object it is completing.

**The fix is one token**: take `std::shared_ptr<TaskCompletionSourceState<T>> state` by value; the callers already hold
the member and pass a copy. Cost: one ref-count bump per completion.

**Siblings of the same shape, held today only by "the primitive must outlive every coroutine awaiting it":**
`AsyncEvent::set()` touches `head_` after each resume (`AsyncEvent.hpp:201-224`); `AsyncConditionVariable::notify_all()`
reads its `state_` member after each resume (`AsyncConditionVariable.hpp:324-347`); `AsyncLatch::countDown()` calls
`done.set()` on a `LatchState` whose last owner may be the waiter whose frame dies inside that very `set()`
(`AsyncLatch.hpp:79-81`). For the two shared_ptr states (latch, CV) the fix is a local copy before the loop; for the event
it is the contract sentence, which should then say "until `set()` returns". `AsyncMutex::unlock()` (`AsyncMutex.hpp:303-307`)
and the semaphore's and the reader-writer lock's loops read their waiter before the resume only - they are fine.

### Q3 (execution context / medium): the timer thread runs the caller's continuation, and every other timer waits for it

Measured - the thread each continuation runs on, main blocked in `result()`:

```
inside sleepFor, before the sleep              main thread
inside sleepFor, after the sleep               TIMER THREAD
before whenAny                                 main thread
after whenAny was cancelled                    TIMER THREAD
after queue->pop() returned                    TIMER THREAD
after queue->push() returned                   TIMER THREAD
```

Three mechanisms put user code on the timer thread: the deadline resume (`Sleep.hpp:311-325`), every posted wake -
a cancellation (`When.hpp:219-223`, `Sleep.hpp:239-243`), a queue hand-off (`AsyncQueue.hpp:144-169`) - and the early
wake (`Sleep.hpp:279-294`). Each `resume()` there runs between `lock.unlock()` and `lock.lock()`, on one thread
(`Sleep.hpp:179`). Consequences:

- **Thread affinity changes silently.** Code after `co_await sleepFor(...)` is on another thread until its next
  suspension. The design page's §3.1.4 R4 says a scheduler does not decide the thread policy for the caller; the queue and the
  cancellation path do, and `sleepFor` is the first primitive a host meets. A coroutine that started on an application
  thread and touches thread-affine state after a sleep now does so off that thread.
- **One long continuation delays every timer in the process.** A 200 ms continuation after a `pop()` postpones every sleep
  deadline, every `withTimeout`, every cancellation wake and every queue hand-off by up to its length. A continuation that
  blocks on anything the timer thread has to deliver deadlocks all of them.
- **What is stated today.** `TimerService` says a cancellation wakes its waiter "on the timer thread" (`Sleep.hpp:76-77`)
  and the queue says a timer-thread action "wake[s] the peer and resume[s] it" (`AsyncQueue.hpp:574-575`). Neither says
  the code *after* the `co_await` runs there. `AsyncTest.cpp` records a thread id in six places (`:868`, `:891`, `:3254` the thread a wake
  ran on; `:1433`, `:1796`, `:3327` the thread a cancel ran on); none asserts the thread a continuation runs on, and none
  measures one sleeper's delay caused by another coroutine's continuation.

**Options, for the owner.** (1) Write the rule where it bites - `sleepFor`, the queue's hand-off paragraph, and a new N#
  in the design page: "the continuation runs on the waking thread; use `scheduleOn` to go back; keep it short". (2) Keep
  user code off the timer thread: posted wakes run on a small wake pool and the timer thread only moves deadlines - a
  design change, and the single-thread ordering the queue's `wakeAll` relies on (`AsyncQueue.hpp:229-233`) would have to
  be re-established. (1) is the sentence; (2) is the decision.

### Q4 (API consistency / medium-low): `whenAny` on an empty list has two answers

`whenDriver` returns at once when `count == 0` for both modes (`When.hpp:475-478`), so `whenAny(std::vector<AnyTask>{})`
completes as a success with no task; the typed overload throws `std::invalid_argument("async::whenAny: empty task list")`
(`When.hpp:1157-1158`). Measured:

```
void   whenAny(std::vector<AnyTask>{}):    returned normally
typed  whenAny(std::vector<Task<int>>{}): threw async::whenAny: empty task list
```

"First of none" completing successfully is the silent success §2.7 refuses for a refused hop, and it is two spellings for
one question (G1). `TypedWhenAnyTest.EmptyListThrows` (`AsyncTest.cpp:2746-2750`) pins the typed answer; no case pins the
void one. One line in `whenDriver` - refuse `WhenMode::Any` with an empty container - makes both agree; which answer is
right (throw, or never complete) is the owner's.

### Q5 (`retry` / medium-low): the loop retries cancellation, and nothing can stop the loop

`retry` takes no token (`Retry.hpp:30`) and rethrows only on the last attempt (`Retry.hpp:51-55`). An attempt that ends in
`TaskCancelledException` - a `sleepFor(d, token)` or a `withCancellation` inside the factory's task - is swallowed; the loop
then sleeps `delay` without a token and builds the next attempt on the same cancelled token. `attempts - 1` pointless
attempts and `attempts - 1` delays later, the cancellation surfaces. A parent under `StopAndWait` waits all of it out,
because the loop's own sleeps read no token either. Two spellings fix it: a `token` parameter forwarded to both sleeps and
checked before each attempt, or an immediate rethrow for `TaskCancelledException`.

### Q6 (small, by reading)

| # | Where | What |
|---|---|---|
| a | `Generator.hpp:104`, `:146-155` | a generator yields `const T&` only, so a generator of move-only values cannot hand ownership out; `std::generator` yields `T&&` for such `T` |
| b | `Sleep.hpp:457`, `WithTimeout.hpp:80` | the duration parameter is `std::chrono::milliseconds`, so `sleepFor(500us)` does not convert; a `class Rep, class Period` parameter costs nothing |
| c | `Scope.hpp:191`, `:196` | `spawn()` increments `pending` before it creates the child; if the child's frame allocation throws, `join()` waits for a child that never existed |
| d | `WaiterList.hpp:16-21` | the comment says a registration failure "happens inside await_suspend, which has to be noexcept"; the language defines the case ("the exception is caught, the coroutine is resumed, and the exception is immediately re-thrown", cppreference, Coroutines), and `SleepAwaiter::await_suspend` (`Sleep.hpp:384`) is not `noexcept`. The convention is right; the necessity is overstated |

### Checked and cleared (so the next round does not repeat them)

`AsyncEvent::set()`'s generation loop and `notify_all()`'s boundary hold as written; `AsyncMutex::unlock()` keeps `locked_`
true across a hand-off, so a newcomer cannot barge ahead of a queued waiter; the semaphore's permit accounting stays exact
across a barging `try_acquire()`; `WhenBaton::park` / `wake` / `unpark` has no re-arm window and a destroyed park leaves
no handle; `Task::result()` on an empty task throws `std::logic_error` rather than blocking; design §6.5 stays unreachable
under the stated contract (the only way to destroy a granted reader from another thread is the contract violation itself).
`Task<const int>` compiles on MSVC 19.52 (`StorableValue` admits it, `std::optional` accepts it) - not a gap.

## 4. How the measurements were taken

Standalone probes outside the tree, compiled against the headers as they stand:

```
cl /nologo /std:c++20 /EHsc /utf-8 /O1 /MD /DNDEBUG /DVN_CORE_LIB /DVN_ROOT_NS=vn /fsanitize=address /Zi
   /I src/base/global/sdk /I src/base/core/sdk /I src/base/async/sdk
   <probe>.cpp src/base/core/src/Exception.cpp src/base/core/src/String.cpp src/base/core/src/ThreadPool.cpp
```

MSVC 19.52.37025 (Visual Studio 18), ASan dynamic runtime. `vcvars64.bat` was not used on this machine - it needs
`reg.exe`, which the session's sandbox refuses - so `INCLUDE` / `LIB` name the tool and SDK (10.0.26100.0) directories
directly. The bodies that matter, reduced:

```cpp
// Q1 (a): withTimeout over pool work; the timeout fires while the callable runs.
vn::ThreadPool pool(1);
auto heavy = []() -> int { std::this_thread::sleep_for(400ms); return 42; };
withTimeout(runOn(pool, heavy), 50ms).result();                    // ASan: heap-use-after-free, WRITE, worker thread
```

```cpp
// Q1 (b): a loser whose hop is queued behind a busy worker.
auto gate = std::make_shared<std::promise<void>>();
auto released = gate->get_future().share();
(void)pool.enqueue([released] { released.wait(); });                // the only worker is busy
std::vector<AnyTask> race{ discard(hopper()), discard(winner()) }; // hopper = co_await schedule(); winner = co_return 1
whenAny(std::move(race)).result();                                  // the loser is freed with its hop still queued
gate->set_value();                                                  // the worker resumes the freed frame: access-violation
```

```cpp
// Q2: a waiter that destroys its source inside the resume.
Task<void> consume(TaskCompletionSource<int>* src) {
    (void)co_await src->task();
    delete src;                                                     // ASan: heap-use-after-free at TaskCompletionSource.hpp:161
}
auto* src = new TaskCompletionSource<int>();
Scope scope; scope.spawn(consume(src)); src->setResult(7);
```

```cpp
// Q3: which thread the continuation is on.
Task<void> afterSleep() {
    report(std::this_thread::get_id());           // main thread
    co_await sleepFor(20ms);
    report(std::this_thread::get_id());           // TIMER THREAD
}
// likewise after whenAny cancelled through a stop_source, after queue->pop() woken by a push, after queue->push().
```

## 5. Peer material not to copy

No peer source was read this round, so this page makes no claim about tokio, folly, asio or P2300 beyond what
`.ai/design/async-design.md` §2.5 and §10.3 already state. For Q1 the relevant difference is structural rather than a
quotation: a runtime that destroys a dropped future can do so because nothing else is ever running it; a coroutine
resumed by a thread pool is.

## 6. Proposed order (for the owner to approve; not automatically added to the backlog)

| Order | Finding | Smallest change | Acceptance |
|---|---|---|---|
| 1 | Q1 | option 1 now (the sentences), option 2 as the real fix | `WithTimeoutTest.TimeoutWhileTheCallableRunsOnTheWorkerIsNotAFree` and `ThreadPoolSchedulerTest.AnAbandonedHopIsNeverResumed` under ASan |
| 2 | Q2 | pass the state by value; a local copy in the latch and the CV | `TaskCompletionSourceTest.AWaiterThatDestroysTheSourceInsideTheResume` under ASan |
| 3 | Q4 | one check in `whenDriver` | `WhenAnyTest.AnEmptyListHasOneAnswerForBothOverloads` |
| 4 | Q5 | a token on `retry`, or rethrow the cancellation | `RetryTest.CancellationIsNotRetried` |
| 5 | Q3 | the sentence in three headers and one N#; (2) is a separate decision | a case that asserts the continuation's thread, and one that measures the delay |
| 6 | Q6 | per row | per row |

## 7. Verification commands

```bash
# the suite on this tree
./build/bin/Debug/test_async.exe                      # 183 tests from 30 suites, PASSED, 23.0 s

# Q1: the only awaiter that publishes a handle it cannot withdraw, and the policy's stated precondition
grep -n "pool_.enqueue" src/base/async/sdk/vine/async/ThreadPoolScheduler.hpp
grep -n -A3 "Destroy: the unfinished children" src/base/async/sdk/vine/async/Cancellation.hpp

# Q2: every site that binds the member to the loop's reference parameter
grep -n "resumeWaitersOneByOne(impl_)" src/base/async/sdk/vine/async/TaskCompletionSource.hpp

# Q3: the resumes that run on the timer thread
grep -n "\.resume()" src/base/async/sdk/vine/async/Sleep.hpp src/base/async/sdk/vine/async/AsyncQueue.hpp

# Q4: the two answers
grep -n -B1 -A2 "count == 0" src/base/async/sdk/vine/async/When.hpp

# Q1 exposure: no caller of the four entry points outside the module
grep -rn -E "whenAny\(|withTimeout\(|scheduleOn\(|runOn\(" src --include=*.hpp --include=*.cpp | grep -v "src/base/async"
```
