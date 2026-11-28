# async review 1: what `Destroy` assumes, who owns a completion, and which thread the continuation runs on

> **Status**: draft, non-authoritative - a working record. §2 is what the tree states, §3 is the findings, §4 is how the
> measurements were taken, §5 is the peer material, §6 proposes an order, §7 lists the verification commands. This round
> changed no file outside this page and its index entry; every finding is a decision left to the owner.
> **Appended 2026-11-28**: §8 is an independent re-run on gcc 16 / Linux - every memory-safety finding reproduces on the
> same source lines, and six evidence items are corrected there (the suite's result, Q2's cleared list, Q1's option 2,
> Q3's in-tree exposure, four citations, and the design page's duplicated IDs). §2-§7 are unchanged.
> **Last verified**: 2026-11-28 for §8, 2026-11-27 for §2-§7; tree `bc8d270a` on `dev`, whose `src/base/async` is
> byte-identical to `8d96fbb2`.
> **Scope**: all 28 headers of `src/base/async/sdk/vine/async/` read line by line, against the contracts
> `.ai/design/async-design.md` states. Three questions: **does each contract hold on every path it is offered to** (the
> compositions' `Destroy` policy against a child on another thread), **who keeps a primitive alive while it resumes its
> waiters**, and **where does a continuation run after a wake**. `tests/test_async/AsyncTest.cpp` was read by its test
> index (183 cases, all green on the toolchain §4 used - §8 V1 records what this tree reports instead) and not line by
> line; the probes in §4 are what the suite does not contain.
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

## 8. Verification round (2026-11-28, gcc 16 on Linux)

**What this round is.** §2-§7 are unchanged. This chapter is an independent re-run of their evidence on a second
toolchain, plus a check of every `file:line` they cite: what reproduced (§8.1), and the six items whose evidence does not
hold as written (§8.2).

**Tree and scope.** `bc8d270a` on `dev`; `src/base/async`, `tests/test_async` and `async-design.md` are byte-identical to
`8d96fbb2` (`git diff --stat 8d96fbb2..HEAD -- src/base/async tests/test_async .ai/design/async-design.md` is empty), so
the findings were re-tested against exactly the code the page measured.

**Toolchain.** gcc 16.0.1 (`Ubuntu 16-20260322-1ubuntu1`), `-std=c++20 -g -O1 -fsanitize=address`, probes outside the
tree linked against `src/base/core/src/{Exception,String,ThreadPool}.cpp` (§8.3). This is a second, unrelated
implementation of the coroutine lowering: the page's §4 used MSVC 19.52, ASan dynamic runtime.

### 8.1 Reproduced

| Finding | Result here | The line the sanitizer report lands on |
|---|---|---|
| Q1 (a) | heap-use-after-free, **READ** of size 1, pool worker | write into the freed frame at `Task.hpp:71` |
| Q1 (b) | heap-use-after-free, READ of size 8, pool worker | `coroutine_handle::resume()` from the queued closure, `ThreadPoolScheduler.hpp:62` |
| Q2 | heap-use-after-free, READ of size 8, same thread | `TaskCompletionSource.hpp:160`, after the resume at `:167` |
| Q3 | the six-line table printed identically | `TimerService::run` resumes between `lock.unlock()` and `lock.lock()` |
| Q4 | the same two lines | `When.hpp:476` vs `When.hpp:1157` |
| Q5 | cancelled after **206 ms** with `attempts = 3, delay = 100 ms` | two swallowed cancellations and two delays |
| Q6 (b) | compile error: `could not convert ... microseconds ... to ... milliseconds` | `Sleep.hpp:457`, `WithTimeout.hpp:80` |

Q1 (a), the default `withTimeout(runOn(pool, f), d)`, 400 ms callable against a 50 ms timeout:

```
ERROR: AddressSanitizer: heap-use-after-free ... READ of size 1 at ... thread T1
    #3 vn::async::detail::TaskPromiseReturn<int>::return_value(int)     Task.hpp:71
    #4 vn::async::runOn                                                 ThreadPoolScheduler.hpp:120
    #6 ...ScheduleAwaiter::await_suspend(...)::lambda                    ThreadPoolScheduler.hpp:62
freed by thread T2 here:                                               (T2 = the timer thread)
    #4 vn::async::Task<int>::Awaiter::destroy()                          Task.hpp:680
    #5 vn::async::Task<int>::Awaiter::~Awaiter()                         Task.hpp:598
    #6 vn::async::detail::composeAnyChild<int>                           When.hpp:894
    #9 vn::async::detail::WhenAnyChild<int>::~WhenAnyChild()             When.hpp:858
   #15 vn::async::whenAny                                                When.hpp:1195
   #20 vn::async::detail::TimerService::run()                            Sleep.hpp:323
```

Same frame, same free site, same workers as the page's MSVC trace. The one difference is the access kind: the page
recorded a **WRITE** of size 1 (MSVC's `optional::emplace` writes the engaged flag), libstdc++ faults first on the
**READ** of that flag in `_Optional_payload_base::_M_reset`. Neither reading weakens the other - a freed frame is written
or read either way.

Q1 (b), the loser destroyed with its hop still queued:

```
ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8 ... thread T1
    #0 std::coroutine_handle<void>::resume() const
    #1 ...ScheduleAwaiter::await_suspend(...)::lambda                    ThreadPoolScheduler.hpp:62
freed by thread T0 here:
    #4 vn::async::Task<void>::Awaiter::destroy()                         Task.hpp:680
    #5 vn::async::Task<void>::Awaiter::~Awaiter()                        Task.hpp:598
    #6 vn::async::detail::composeChild                                   When.hpp:453
    #9 vn::async::detail::WhenChild::~WhenChild()                        When.hpp:420
```

Q2, the waiter deletes its source inside the resume. Every detail of the page's claim holds, and the report adds one:
the freed block is the **16-byte `TaskCompletionSource` object itself** (the `impl_` member the reference parameter
refers to), not the shared state - which is exactly "a reference into the object it is completing".

Q3 printed the page's six lines verbatim, with the queue's push/pop replaced by an `AsyncQueue<int>` producer and
consumer:

```
inside sleepFor, before the sleep         main thread
inside sleepFor, after the sleep          TIMER THREAD
before whenAny                            main thread
after whenAny was cancelled               TIMER THREAD
after queue->pop() returned               TIMER THREAD
after queue->push() returned              TIMER THREAD
```

### 8.2 Six corrections

**V1 - the suite is 182 of 183 here, and §7's command is not this tree's.**

```
$ cd build/bin && ./test_async --gtest_brief=1
[  FAILED  ] AsyncDefectRegressionTest.YieldLoopStackIsConstant (21 ms)
    Expected: (depth) < (64 * 1024), actual: 319936 vs 65536
[==========] 183 tests from 30 test suites ran. (3413 ms total)
[  PASSED  ] 182 tests.
```

The failing case is a stack-depth assertion, unrelated to every finding here, and the module's own memory note already
records it (same numbers) as pre-existing. So the finding does not change - but "183 cases, all green on this tree" is not
reproducible, and §7 states it as `./build/bin/Debug/test_async.exe` inside a ```bash``` block, a Windows path this tree
does not have (there is no `Debug/` under `build/bin`). Replace the command with the one above and the claim with "182 of
183, the failure being the recorded stack-depth case". The value of the page is that its evidence is repeatable; a command
a reader cannot run costs it that.

**V2 - Q2's cleared list applies the wrong criterion to two primitives.**

Q2 judges an operation by whether it touches **the object it was called on** after a resume, and on that criterion it
names `AsyncEvent::set()` (`AsyncEvent.hpp:201-224`), `notify_all()` (`AsyncConditionVariable.hpp:324-347`) and
`AsyncLatch::countDown()` (`AsyncLatch.hpp:79-81`). It then clears the semaphore and the reader-writer lock with "they
read their waiter before the resume only" - which is the *other* rule (§2.2: no waiter pointer held across a resume).
Both shapes exist, and these two have the second one too:

- `AsyncSemaphore::release()` resumes at `AsyncSemaphore.hpp:174` and the `for (;;)` then re-locks `mutex_` and reads
  `head_` / `count_` (members) on the next iteration.
- `AsyncReaderWriterLock::resume()` resumes at `AsyncReaderWriterLock.hpp:453`, and the loop re-locks `mutex_` at `:433`
  and reads `readers_head_` at `:434`.

`AsyncMutex::unlock()` really is fine, but for a third reason: its `h.resume()` (`AsyncMutex.hpp:306`) is the last
statement of the function, so nothing of the mutex is touched afterwards. The honest list is therefore: `AsyncEvent::set`,
`AsyncSemaphore::release` and `AsyncReaderWriterLock::resume` touch **the object** again after a resume;
`notify_all()` touches its `state_` member; `countDown()` touches the shared `LatchState` whose last owner may be the
waiter whose frame dies inside that very `set()`. The only guard in all five is the "must outlive every coroutine
awaiting it" sentence. What makes `TaskCompletionSource` the finding and these five the contract is that
the source **promises the opposite** (`TaskCompletionSource.hpp:23-27`: the state lives behind a `shared_ptr` so a task
keeps it alive after the source is destroyed), so destroying it during a resume is a shape its own design invites. For the
latch and the CV the fix is the local `shared_ptr` copy Q2 already states; for event, semaphore and rwlock the state IS
the object, so no copy exists and the contract sentence is all there is - which is the same conclusion Q2 reaches for the
event, and should say for all three.

**V3 - Q2 does not cite the sentence the probe violates, and the fix has a second half.**

`TaskCompletionSource.hpp:194-196` states: "Waiters are resumed on the thread that completes the source, and the source
must outlive them." The probe destroys the source *from inside the resume*, i.e. the source does not outlive its waiter -
a stated contract violation, and the page's probe reads as if nothing said so. The finding survives it for the reason in
V2 (lines 23-27 promise the capability the sentence forbids, and the loop's own comment at `:146-153` handles the same
hazard one step further out), but the fix is two halves, not one: take the state by value **and** rewrite that sentence,
either as "the source must not be destroyed from inside a resumed waiter" (the honest reading today) or - better, once the
loop holds its own reference - delete the clause, because the shared state then makes it unnecessary.

**V4 - Q1's option 2 closes (b) but not (a) as written.**

At the moment (a) frees the frame, the coroutine is inside `std::invoke` in `runOn`'s body: the `ScheduleAwaiter` was
destroyed at its `await_resume` long before, so a flag it sets in `~ScheduleAwaiter` cannot be consulted by anyone. What
does work is a keepalive that spans the whole `h.resume()` in the worker's closure - that closure is on the worker's stack
for the entire body, so the frame cannot be freed under it - and the frame freed by that closure when `h.resume()`
returns. `Task::Awaiter::destroy()` on such a frame must then mean "abandon", which needs the promise to know the frame
is pool-owned; that is the real cost of option 2 and belongs in the statement of it. The hop (b) needs only the
`abandoned` flag.

**V5 - Q3's exposure is in-tree already, through `sleepFor`.**

The page measured Q3 through `whenAny` / `withTimeout` / `runOn` / `scheduleOn` and found no caller outside the module.
That is true of those four and false of the primitive underneath:

```
$ grep -rn "sleepFor(" src --include=*.hpp --include=*.cpp | grep -v src/base/async
src/fw/appfw/src/CommandManager.cpp:766              co_await vn::async::sleepFor(kDrainPollInterval);
src/fw/appfw/src/ConsoleProgressReporter.cpp:69      co_await vn::async::sleepFor(delay);
src/plugins/test_plugin/src/commands/TestNested{Progress,Child}Command.cpp
```

So the rule the page asks for is owed by code that exists today, and the concrete failure to name is
`Task::result()` (or any wait on something the timer thread must deliver) inside a continuation that follows a
`sleepFor`: the timer thread is the thread running that continuation, so the wait blocks every timer in the process,
including its own. `CommandManager::waitChainsDrained` is safe only because its continuation is a short loop over a
`std::vector`; nothing states that, which is precisely the sentence Q3 proposes.

**V6 - four citations, and the design page's IDs are not unique.**

- `Scope.hpp:191` - `++state_->pending;` is at `:190`; `:196` (`runScopeChild`) is right.
- `Sleep.hpp:239-243` is cited for "a cancellation" of a sleep, but that is `deferWake`, the composition/scope wake. A
  cancelled `SleepAwaiter` goes `AbortRequest` -> `requestAbort` -> the `early_` queue, and its resume is
  `Sleep.hpp:279-294`. Same effect (the timer thread), wrong site.
- `Scope::join`'s `CancelPolicy::Destroy` destroys no child: it ends the wait and leaves them running (`Scope.hpp:238-241`).
  Grouping it with `whenAny` / `withTimeout`, whose `Destroy` really frees frames, overstates the policy's reach. That the
  four defaults exist is right (`C2`); the finding's reach is the two compositions.
- `When.hpp:1191` is the `rethrow_exception`; the children are destroyed when the driver's frame goes, which the gcc
  report attributes to `:1195`. Same act, and a reader comparing the two traces should not read the difference as one.
- The design page uses the same ID set twice with different meanings, so `§2`'s "(§2.6, N5)" and `§3`'s "§3.1.4 R4" are
  ambiguous in the page they point at: `R4` is the scheduler rule in `§3.1.4` and "an injected cancellation token is
  visible only at the layer that was combined" in `§5.2`; `N5` is the `stop_callback` rule in `§3.1.5` and the `SharedTask`
  rule in `§5.3`; `N1` is "a `Scope` destructor does not wait" in `§3.1.1` and the swap-then-resume invariant in `§5.3`;
  `T4` is the scheduler cases in `§3.1.4` and the pumping drive in `§8`. Qualify every citation with its section. Whether
  the design page should enumerate the per-feature set at all is its own decision; recording it here is so the next round
  does not read `R4` as one rule.

### 8.3 What does not change

- The order in §6 stands. Q1 keeps first place, and V4 only widens its option 2.
- Q2 keeps second place, with V2 adding the semaphore and the reader-writer lock to the same bucket and V3 adding the
  contract sentence to the fix.
- Q3 keeps its sentence-first order, with V5 supplying the two appfw call sites that make the sentence due now rather
  than "when a host appears".
- No backlog entry is added by this round either; §6 remains a proposal to the owner.

### 8.4 Commands this round ran

```bash
# the tree the findings were re-tested against
git rev-parse --short HEAD && git diff --stat 8d96fbb2..HEAD -- src/base/async tests/test_async .ai/design/async-design.md

# build one probe (ASan, outside the tree)
g++ -std=c++20 -g -O1 -fsanitize=address -fno-omit-frame-pointer -DVN_CORE_LIB -DVN_ROOT_NS=vn -pthread \
    -I src/base/global/sdk -I src/base/core/sdk -I src/base/async/sdk \
    q1a_timeout_pool.cpp src/base/core/src/{Exception,String,ThreadPool}.cpp -o q1a && ./q1a

# the suite on this tree (V1)
cd build/bin && ./test_async --gtest_brief=1

# V2: the two loops that re-lock their own mutex after a resume
grep -n "resume()" src/base/async/sdk/vine/async/AsyncSemaphore.hpp src/base/async/sdk/vine/async/AsyncReaderWriterLock.hpp
awk 'NR>=430 && NR<=455' src/base/async/sdk/vine/async/AsyncReaderWriterLock.hpp

# V5: the in-tree users of the primitive Q3 is about
grep -rn "sleepFor(" src --include=*.hpp --include=*.cpp | grep -v src/base/async
```

## 9. What the redesign round landed (2026-11-28)

The owner asked for the redesign rather than the patch list. What is in the tree now, and where the rule lives:

| Finding | State | Where the rule is stated | Red before |
|---|---|---|---|
| Q1 (a) | **closed** - `runOn` / `run` queue the callable and park on a `TaskCompletionSource`; the frame a composition frees is never the one the worker executes | `async-design.md` 2.10, `ThreadPoolScheduler.hpp` | ASan heap-use-after-free on every run |
| Q1 (b) | **closed** - a hop is a claim its awaiter withdraws | 2.10, `ScheduleAwaiter` | ASan heap-use-after-free from the queued closure |
| Q1 (the rest) | **stated** - a child running its own code is not suspended, so `Destroy`'s precondition is written where the policy is chosen | `Cancellation.hpp`, caveat 6.8 | not reproducible without a user child |
| Q2 | **closed** - the completing loop holds the state by value; latch and condition variable hold theirs for the call; `AsyncEvent` states its stricter contract | 2.2, `TaskCompletionSource.hpp`, `AsyncLatch.hpp`, `AsyncConditionVariable.hpp` | ASan heap-use-after-free at `:160` |
| Q4 | **closed** - both spellings throw for an empty list | 2.7 | void list completed silently |
| Q5 | **closed** - `retry` takes a token, forwards it to the delay, and never retries a cancellation | 3.1.2 R2, `Retry.hpp` | measured 206 ms of pointless work |
| Q6 (c) | **closed** - a spawn that never made a child gives its pending tick back | `Scope.hpp` | by reading |
| Q6 (d) | **closed by deletion** - registration failures are the language's to carry, `tryRegisterWaiter` / `failure_` / the header are gone | 2.2 | n/a (a false premise) |
| Q3 | **stated, not redesigned** - `sleepFor` now says the continuation runs on the timer thread; a wake pool is `D13` | `Sleep.hpp`, `async-design.md` D13 | measured |
| Q6 (a), (b) | **backlog** - a generator of move-only values, and a duration parameter that takes any period | `../backlog/async.md` A3 / A4 | compile error for `500us` |

Gates: `test_async` 188 of 189 (the case §8 V1 names), `test_appfw` 65 passed with its three translation cases red for the
released `lrelease`, which is the packaging gap the build log reports and not this module; ASan over the seven new cases is
clean; `check_ai_docs --strict`, `check_design_docs`, `check_cross_refs`, `check_api_style`, `check_public_docs`,
`check_include_hygiene` and `check_doc_symbols` all green.
