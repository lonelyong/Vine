# SharedTask resumes a released frame while waking its waiters (use-after-free) (Historical record)

**Date**: 2026-09-17 / **Status**: Fixed / **Files**: `src/base/async/sdk/vine/async/SharedTask.hpp`,
`tests/test_async/AsyncTest.cpp`

## Symptom

When a `SharedTask` completes with more than one waiter and the first waiter to be resumed destroys another waiter **inside
its own resume**, the process crashes in the resume loop of `detail::runShared()`:

- -O1 + ASan: `heap-use-after-free`, the read in `h.resume()` of `runShared`, the free in
  `~WhenChild() -> composeChild()`'s `Task::Awaiter::destroy()`.
- -O2 without a sanitizer: most runs SIGSEGV outright (exit 139).
- Minimal reproduction: one `SharedTask` with two waiters inside `whenAny`, with the source task suspending for 30 ms
  first - the winner of `whenAny` destroys the still-suspended loser when it completes, which is exactly the scenario the
  module documentation describes.

## Root cause

At the end of `runShared()`, the whole waiter list was **swapped out** before the handles were resumed one by one:

```cpp
to_resume.swap(state->waiters);
for (auto h : to_resume) h.resume();
```

A handle that has been swapped out is no longer in the shared list, so a sibling waiter cannot deregister itself when it is
destroyed (`~SharedTaskAwaiter`'s `std::erase` operates on an empty list), and the loop goes on to resume a coroutine frame
that has already been released.

This violated the module's own written rule: **when one completion wakes several waiters, dequeue one per lock and never
hold a handle across a resume** (`AsyncEvent::set`, `TaskCompletionSource::resumeWaitersOneByOne`, `AsyncSemaphore::release`,
`AsyncReaderWriterLock::resume`, `AsyncQueue::close` and `TimerService::run` all follow it).

## Fix

`runShared()` became a loop with the same shape as `resumeWaitersOneByOne`: dequeue one waiter per lock (finishing when the
queue is empty, with `completed` published before the first resume) and resume outside the lock.

## Verification

- New regression case `AsyncDefectRegressionTest.SharedTaskResumesWaitersOneAtATime` (one of 135).
- Mutation proof: after `git checkout -- SharedTask.hpp` and a rebuild the case segfaults **10 times out of 10**; with the
  fix restored it fails **0 times out of 10**.
- Control: the same scenario built on `TaskCompletionSource` / `AsyncEvent` (also one completion and several waiters)
  already passed, which shows the root cause was specific to the batch swap in `runShared`.
- A reusable technique for making this class of defect deterministic: make the waiter's coroutine frame thick enough to get
  its own mmap (256 KiB of padding inside the frame plus an asm barrier against optimisation) and leave a short settle after
  `Task::result()` - the stale resume happens **after** `Task::result()` returns, and a process that exits earlier never
  sees it. Measured: 19/20 hits without the hardening, 20/20 with it.

## Lessons

- "Swap out, then iterate" looks simpler in a concurrent container, but it **throws away the destroyed waiter's chance to
  deregister itself**; in this module that is a hard prohibition.
- The comment on the existing case `SharedTaskTest.WaiterDestroyedBeforeCompletion` states exactly this invariant, but the
  scenario had only one waiter, so the invariant is only pinned by a case with **two or more waiters where the winner
  destroys the loser**.
