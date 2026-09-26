#include <vine/async/Task.hpp>
#include <vine/async/AsyncConditionVariable.hpp>
#include <vine/async/AsyncEvent.hpp>
#include <vine/async/AsyncLatch.hpp>
#include <vine/async/AsyncMutex.hpp>
#include <vine/async/AsyncQueue.hpp>
#include <vine/async/AsyncReaderWriterLock.hpp>
#include <vine/async/AsyncSemaphore.hpp>
#include <vine/async/Concepts.hpp>
#include <vine/async/Cancellation.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/async/Finally.hpp>
#include <vine/async/Generator.hpp>
#include <vine/async/Retry.hpp>
#include <vine/async/RunToCompletion.hpp>
#include <vine/async/Scheduler.hpp>
#include <vine/async/Scope.hpp>
#include <vine/async/SharedTask.hpp>
#include <vine/async/Sleep.hpp>
#include <vine/async/TaskCombinators.hpp>
#include <vine/async/TaskCompletionSource.hpp>
#include <vine/async/ThreadPoolScheduler.hpp>
#include <vine/async/When.hpp>
#include <vine/async/WithTimeout.hpp>
#include <vine/async/Yield.hpp>

#include <vine/CancellationToken.hpp>
#include <vine/ThreadPool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#endif

using namespace vn;

namespace {

async::Task<int> answer()
{
    co_return 42;
}

async::Task<void> noop()
{
    co_return;
}

async::Task<int> addOne(int x)
{
    co_return x + 1;
}

async::Task<int> nested()
{
    int a = co_await answer();
    int b = co_await addOne(a);
    co_return b;
}

async::Task<int> failing()
{
    throw std::runtime_error("boom");
    co_return 1;
}

async::Task<int> lazyProbe(bool& ran)
{
    ran = true;
    co_return 7;
}

async::Task<int> delayed(int value, std::chrono::milliseconds delay)
{
    co_await async::sleepFor(delay);
    co_return value;
}

// ---------------------------------------------------------------------------
// The helpers below are plain coroutine functions (NOT coroutine lambdas).
// They are needed because both clang 22 and gcc 16 (experimental) miscompile
// coroutine lambdas that capture variables (loop induction variables, or
// references stored in the enclosing closure), which otherwise crashes or
// misbehaves in these tests. Plain lambdas (no co_await/co_return) are fine.
// ---------------------------------------------------------------------------

async::Task<void> never()
{
    co_await std::suspend_always{};
    co_return;
}

async::Task<void> failTask()
{
    throw std::runtime_error("boom");
    co_return;
}

async::Task<int> failInt()
{
    throw std::runtime_error("boom");
    co_return 0;
}

async::Task<int> one()
{
    co_return 1;
}

async::Task<void> neverFlag(bool& ran)
{
    co_await std::suspend_always{};
    ran = true;
    co_return;
}

async::Task<int> ranOne(bool& ran)
{
    ran = true;
    co_return 1;
}

async::Task<void> ranOneVoid(bool& ran)
{
    ran = true;
    co_return;
}

async::Task<std::unique_ptr<int>> makeUniquePtr(int v)
{
    co_return std::make_unique<int>(v);
}

async::Generator<int> gen123()
{
    co_yield 1;
    co_yield 2;
    co_yield 3;
}

async::Generator<int> genThrow()
{
    co_yield 1;
    throw std::runtime_error("boom");
    co_yield 2;
}

async::DetachedTask setEvent(async::AsyncEvent& event)
{
    event.set();
    co_return;
}

async::Task<int> awaitEventInt(async::AsyncEvent& event, int value)
{
    co_await event;
    co_return value;
}

async::Task<void> awaitEvent(async::AsyncEvent& event)
{
    co_await event;
    co_return;
}

async::Task<void> awaitEventFlag(async::AsyncEvent& event, bool& ran)
{
    co_await event;
    ran = true;
    co_return;
}

async::Task<void> eventWaitCount(async::AsyncEvent& event,
                                 std::atomic<int>& registered,
                                 std::atomic<int>& woken)
{
    ++registered;
    co_await event;
    ++woken;
}

async::Task<void> awaitEventTwice(async::AsyncEvent& event, int& resumes)
{
    co_await event;
    ++resumes;
    co_await event;
    ++resumes;
    co_return;
}

async::Task<void> eventReArmOnce(async::AsyncEvent& event,
                                 std::atomic<bool>& registered,
                                 std::atomic<int>& woken,
                                 std::atomic<bool>& rearmed)
{
    registered.store(true); // About to register the gen-1 waiter.
    co_await event;         // Generation 1.
    woken.fetch_add(1);
    event.reset();          // Re-arm for the next read (VisualUserIO pattern).
    rearmed.store(true);
    co_await event;         // Generation 2.
    woken.fetch_add(1);
}

async::Task<int> scheduleInline(async::InlineScheduler& scheduler)
{
    co_await scheduler.schedule();
    co_return 1;
}

async::Task<void> markDone(int& done)
{
    ++done;
    co_return;
}

async::Task<void> reportIfReleased(async::AsyncEvent& release, std::atomic<bool>& reported)
{
    co_await release;
    reported.store(true);
    co_return;
}

async::Task<void> lockGuardRelease(async::AsyncMutex& mutex)
{
    auto guard = co_await async::lockAsync(mutex);
    (void)guard;
    co_return;
}

async::Task<void> fifoWorker(async::AsyncMutex& mutex, std::vector<int>& order, int tag)
{
    co_await mutex.lock();
    order.push_back(tag);
    mutex.unlock();
}

async::Task<void> mutexLockUnlock(async::AsyncMutex& mutex)
{
    co_await mutex.lock();
    mutex.unlock();
}

async::Task<void> mutexIncrement(async::AsyncMutex& mutex, int& counter)
{
    co_await mutex.lock();
    ++counter;
    mutex.unlock();
}

async::DetachedTask mutexHolder(async::AsyncMutex& mutex,
                                async::AsyncEvent& held,
                                async::AsyncEvent& release,
                                std::atomic<bool>& done)
{
    co_await mutex.lock();
    held.set();
    co_await release;
    mutex.unlock();
    done.store(true);
}

async::DetachedTask semAcquireSet(async::AsyncSemaphore& sem, bool& acquired)
{
    co_await sem.acquire();
    acquired = true;
}

async::Task<void> resumeOnPoolTask(async::ThreadPoolScheduler& scheduler, bool& ran)
{
    co_await async::resumeOn(scheduler);
    ran = true;
    co_return;
}

// Compile-time contract of Concepts.hpp: Awaitable mirrors the co_await lookup
// (awaiter itself, or operator co_await), and it rejects an awaiter whose
// await_suspend return type is none of void / bool / coroutine_handle.
static_assert(async::Awaitable<async::YieldAwaiter>);
static_assert(async::Awaitable<async::Task<int>>);
static_assert(async::Awaitable<async::SharedTask<int>>);
static_assert(async::Awaitable<async::AsyncEvent>);
static_assert(async::Awaitable<async::AsyncEvent::Awaiter>);
static_assert(!async::Awaitable<int>);
static_assert(!async::Awaitable<async::Task<int>&>); // Task is rvalue-awaitable only.

struct IllegalAwaiter
{
    bool await_ready() const noexcept { return true; }
    int  await_suspend(std::coroutine_handle<>) const noexcept { return 0; }
    void await_resume() const noexcept {}
};

static_assert(!async::Awaitable<IllegalAwaiter>);

/// await_suspend must not return a reference to a handle: the language allows
/// only void, bool or a coroutine_handle value (clang rejects this form, g++
/// currently accepts it, so rejecting it keeps the concept portable).
struct ReferenceReturningAwaiter
{
    bool                     await_ready() const noexcept { return true; }
    std::coroutine_handle<>& await_suspend(std::coroutine_handle<>) const noexcept;
    void                     await_resume() const noexcept {}
};

static_assert(!async::Awaitable<ReferenceReturningAwaiter>);

/// await_ready only has to convert contextually to bool, exactly as co_await
/// requires, so a returning-int awaiter is legal (measured on clang and g++).
struct ContextualBoolAwaiter
{
    int  await_ready() const noexcept { return 1; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

static_assert(async::Awaitable<ContextualBoolAwaiter>);

/// Scheduler whose schedule() returns a Task instead of a bare awaiter.
struct TaskReturningScheduler
{
    async::Task<void> schedule() { co_return; }
};

static_assert(async::Schedulable<async::InlineScheduler>);
static_assert(async::Schedulable<async::ThreadPoolScheduler>);
static_assert(async::Schedulable<TaskReturningScheduler>);

// StorableValue keeps unrepresentable results out of Task/SharedTask/Generator:
// a reference result is rejected at the template boundary instead of failing
// deep inside std::optional (naming Task<int&> itself is a hard error, so the
// class-template constraint is pinned through a declaration-only helper).
static_assert(async::StorableValue<int>);
static_assert(async::StorableValue<void>);
static_assert(!async::StorableValue<int&>);
static_assert(!async::StorableValue<int[3]>);

template<typename T>
async::Task<T> taskOfType(); // Declared only: substitution, no coroutine created.

/// Whether Task<T> is a valid specialization at all (checked as a concept, so
/// the constraint failure stays a substitution failure).
template<typename T>
concept TaskAcceptsResult = requires { taskOfType<T>(); };

static_assert(TaskAcceptsResult<int>);
static_assert(TaskAcceptsResult<void>);
static_assert(!TaskAcceptsResult<int&>);
static_assert(!TaskAcceptsResult<int[3]>);

async::Task<int> scheduleViaTaskReturningScheduler(TaskReturningScheduler& scheduler)
{
    co_await async::resumeOn(scheduler);
    co_return 7;
}

async::Task<void> scopeFlag(bool& flag)
{
    flag = true;
    co_return;
}

async::Task<void> scopeAwaitEvent(async::AsyncEvent& event)
{
    co_await event;
    co_return;
}

async::Task<void> sleepCancel(CancellationToken token, bool& cancelled)
{
    try
    {
        co_await async::sleepFor(std::chrono::milliseconds(100), token);
    }
    catch (const async::TaskCancelledException&)
    {
        cancelled = true;
    }
}

async::DetachedTask queuePopDetached(async::AsyncQueue<int>& queue, int& got)
{
    got = co_await queue.pop();
}

async::DetachedTask queuePushDetached(async::AsyncQueue<int>& queue, int value, bool& pushed)
{
    co_await queue.push(value);
    pushed = true;
}

async::Task<void> latchWaitFlag(async::AsyncLatch& latch, bool& released)
{
    co_await latch.wait();
    released = true;
    co_return;
}

async::Task<void> rwBasic(async::AsyncReaderWriterLock& lock, int& value)
{
    {
        auto guard = co_await lock.writerLock();
        (void)guard;
        value = 5;
    }
    {
        auto guard = co_await lock.readerLock();
        (void)guard;
        EXPECT_EQ(value, 5);
    }
    co_return;
}

async::Task<void> rwReaderConcurrent(async::AsyncReaderWriterLock& lock,
                                     std::atomic<int>& active,
                                     std::atomic<int>& max_active)
{
    auto guard = co_await lock.readerLock();
    (void)guard;
    int now = ++active;
    int cur = max_active.load();
    while (cur < now && !max_active.compare_exchange_weak(cur, now))
    {
    }
    co_await async::sleepFor(std::chrono::milliseconds(20));
    --active;
}

async::DetachedTask rwReaderHold(async::AsyncReaderWriterLock& lock,
                                 async::AsyncEvent& r1_held,
                                 async::AsyncEvent& w1_done)
{
    auto guard = co_await lock.readerLock();
    (void)guard;
    r1_held.set();
    co_await w1_done;
}

async::DetachedTask rwWriterThen(async::AsyncReaderWriterLock& lock,
                                 async::AsyncEvent& w1_done,
                                 std::atomic<bool>& ran)
{
    auto guard = co_await lock.writerLock();
    (void)guard;
    ran.store(true);
    w1_done.set();
}

async::DetachedTask rwReaderThen(async::AsyncReaderWriterLock& lock, std::atomic<bool>& ran)
{
    auto guard = co_await lock.readerLock();
    (void)guard;
    ran.store(true);
}

async::DetachedTask rwWriterHolder(async::AsyncReaderWriterLock& lock,
                                   async::AsyncEvent& held,
                                   async::AsyncEvent& release,
                                   std::atomic<bool>& done)
{
    auto guard = co_await lock.writerLock();
    (void)guard;
    held.set();
    co_await release;
    done.store(true);
}

async::DetachedTask rwWriterHoldRelease(async::AsyncReaderWriterLock& lock,
                                        async::AsyncEvent& held,
                                        async::AsyncEvent& release)
{
    auto guard = co_await lock.writerLock();
    (void)guard;
    held.set();
    co_await release;
}

async::Task<void> rwWriterLockUnlock(async::AsyncReaderWriterLock& lock)
{
    auto guard = co_await lock.writerLock();
    (void)guard;
    co_return;
}

async::DetachedTask cvWaiterDetached(async::AsyncMutex& mutex,
                                     async::AsyncConditionVariable& cv,
                                     bool& woken)
{
    co_await mutex.lock();
    co_await cv.wait(mutex);
    woken = true;
    mutex.unlock();
}

async::Task<void> cvWaiterCount(async::AsyncMutex& mutex,
                                async::AsyncConditionVariable& cv,
                                std::atomic<int>& registered,
                                std::atomic<int>& woken)
{
    co_await mutex.lock();
    ++registered;
    co_await cv.wait(mutex);
    ++woken;
    mutex.unlock();
}

async::Task<void> cvWait(async::AsyncMutex& mutex, async::AsyncConditionVariable& cv)
{
    co_await mutex.lock();
    co_await cv.wait(mutex);
    mutex.unlock();
}

async::Task<void> cvWaitBool(async::AsyncMutex& mutex,
                             async::AsyncConditionVariable& cv,
                             bool& woken)
{
    co_await mutex.lock();
    co_await cv.wait(mutex);
    woken = true;
    mutex.unlock();
}

async::Task<void> cvWaitOnce(async::AsyncMutex& mutex,
                             async::AsyncConditionVariable& cv,
                             std::atomic<int>& completed)
{
    co_await mutex.lock();
    co_await cv.wait(mutex);
    mutex.unlock();
    ++completed;
}

async::Task<void> cvWaitFlag(async::AsyncMutex& mutex,
                             async::AsyncConditionVariable& cv,
                             std::atomic<bool>& woken)
{
    co_await mutex.lock();
    co_await cv.wait(mutex);
    woken.store(true);
    mutex.unlock();
}

async::Task<void> cvWaitReacquire(async::AsyncMutex& mutex,
                                  async::AsyncConditionVariable& cv,
                                  std::atomic<bool>& ok)
{
    co_await mutex.lock();
    co_await cv.wait(mutex);
    mutex.unlock();
    ok.store(true);
}

async::Task<int> finallyRunsOnExit(bool& cleaned)
{
    auto guard = async::makeFinally([&cleaned] { cleaned = true; });
    (void)guard;
    co_return 42;
}

async::Task<int> finallyRunsOnException(bool& cleaned)
{
    auto guard = async::makeFinally([&cleaned] { cleaned = true; });
    (void)guard;
    throw std::runtime_error("boom");
    co_return 0;
}

async::Task<void> finallyDismiss(bool& cleaned)
{
    auto guard = async::makeFinally([&cleaned] { cleaned = true; });
    guard.dismiss();
    co_return;
}

async::Task<void> yieldRan(bool& ran)
{
    co_await async::yield();
    ran = true;
    co_return;
}

async::Task<int> sharedAwaitInt(async::SharedTask<int>& st)
{
    co_return co_await st;
}

async::Task<void> sharedAwaitVoid(async::SharedTask<void>& st)
{
    co_await st;
    co_return;
}

async::Task<int> sharedSource(int& runs)
{
    ++runs;
    co_return 42;
}

async::Task<int> sharedSourceSlow(std::atomic<int>& runs)
{
    ++runs;
    co_await async::sleepFor(std::chrono::milliseconds(30));
    co_return 7;
}

async::Task<int> sharedSourceEvent(async::AsyncEvent& release, std::atomic<bool>& started)
{
    started.store(true);
    co_await release;
    co_return 42;
}

async::Task<void> sharedSourceVoid(bool& ran)
{
    ran = true;
    co_return;
}

async::Task<int> sharedSourceRelease(async::AsyncEvent& release)
{
    co_await release;
    co_return 42;
}

async::Task<void> sharedAwaitCheck(async::SharedTask<int>& st, std::atomic<bool>& got)
{
    int v = co_await st;
    got.store(v == 42);
    co_return;
}

async::Task<int> retryAttempts(int& attempts, int failBelow)
{
    ++attempts;
    if (attempts < failBelow)
    {
        throw std::runtime_error("transient");
    }
    co_return 42;
}

async::Task<int> retryFail(int& attempts)
{
    ++attempts;
    throw std::runtime_error("boom");
    co_return 0;
}

async::Task<void> eventLoser(async::AsyncEvent& event,
                             std::atomic<int>& registered,
                             std::atomic<int>& woken)
{
    ++registered;
    co_await event;
    ++woken;
}

async::Task<void> eventMakeAny(async::AsyncEvent& event,
                               std::atomic<int>& registered,
                               std::atomic<int>& woken)
{
    std::vector<async::AnyTask> tasks;
    for (int i = 0; i < 2; ++i)
    {
        tasks.push_back(async::discard(eventLoser(event, registered, woken)));
    }
    co_await async::whenAny(std::move(tasks));
}

async::Task<void> semLoser(async::AsyncSemaphore& sem,
                           std::atomic<int>& registered,
                           std::atomic<int>& acquired)
{
    ++registered;
    co_await sem.acquire();
    ++acquired;
}

async::Task<void> semMakeAny(async::AsyncSemaphore& sem,
                             std::atomic<int>& registered,
                             std::atomic<int>& acquired)
{
    std::vector<async::AnyTask> tasks;
    for (int i = 0; i < 2; ++i)
    {
        tasks.push_back(async::discard(semLoser(sem, registered, acquired)));
    }
    co_await async::whenAny(std::move(tasks));
}

async::Task<void> queueLoser(async::AsyncQueue<int>& queue,
                             std::atomic<int>& registered,
                             std::atomic<int>& popped)
{
    ++registered;
    try
    {
        co_await queue.pop();
    }
    catch (const std::runtime_error&)
    {
    }
    ++popped;
}

async::Task<void> queueMakeAny(async::AsyncQueue<int>& queue,
                               std::atomic<int>& registered,
                               std::atomic<int>& popped)
{
    std::vector<async::AnyTask> tasks;
    for (int i = 0; i < 2; ++i)
    {
        tasks.push_back(async::discard(queueLoser(queue, registered, popped)));
    }
    co_await async::whenAny(std::move(tasks));
}

async::Task<void> rwReaderLoser(async::AsyncReaderWriterLock& lock,
                                std::atomic<int>& registered,
                                std::atomic<int>& acquired)
{
    ++registered;
    auto guard = co_await lock.readerLock();
    (void)guard;
    ++acquired;
}

async::Task<void> rwReaderMakeAny(async::AsyncReaderWriterLock& lock,
                                  std::atomic<int>& registered,
                                  std::atomic<int>& acquired)
{
    std::vector<async::AnyTask> tasks;
    for (int i = 0; i < 2; ++i)
    {
        tasks.push_back(async::discard(rwReaderLoser(lock, registered, acquired)));
    }
    co_await async::whenAny(std::move(tasks));
}

async::Task<void> tcsLoser(async::TaskCompletionSource<int>& tcs,
                           std::atomic<int>& registered,
                           std::atomic<int>& completed)
{
    ++registered;
    int v = co_await tcs.task();
    (void)v;
    ++completed;
}

async::Task<void> tcsMakeAny(async::TaskCompletionSource<int>& tcs,
                             std::atomic<int>& registered,
                             std::atomic<int>& completed)
{
    std::vector<async::AnyTask> tasks;
    for (int i = 0; i < 2; ++i)
    {
        tasks.push_back(async::discard(tcsLoser(tcs, registered, completed)));
    }
    co_await async::whenAny(std::move(tasks));
}

async::Task<void> cvLoser(async::AsyncMutex& mutex,
                          async::AsyncConditionVariable& cv,
                          std::atomic<int>& registered,
                          std::atomic<int>& woken)
{
    co_await mutex.lock();
    ++registered;
    co_await cv.wait(mutex);
    ++woken;
    mutex.unlock();
}

async::Task<void> cvMakeAny(async::AsyncMutex& mutex,
                            async::AsyncConditionVariable& cv,
                            std::atomic<int>& registered,
                            std::atomic<int>& woken)
{
    std::vector<async::AnyTask> tasks;
    for (int i = 0; i < 2; ++i)
    {
        tasks.push_back(async::discard(cvLoser(mutex, cv, registered, woken)));
    }
    co_await async::whenAny(std::move(tasks));
}

/// Set by scopeChildThatReports() once its released child ran: proof that a cancelled join left it alive.
std::atomic<bool> s_scopeChildFinished{ false };

/**
 * @brief Runs a whenAll that never finishes and records where its cancellation is observed.
 *
 * The probe is eager, so it is parked on the composition when the caller asks for stop; the
 * cancellation therefore has to arrive from another thread, and the thread id records which
 * one resumed it. cancelled_seen is published last, with release, so a reader that waits for
 * it can read wake_thread without a race.
 */
async::DetachedTask cancellationProbe(std::vector<async::AnyTask> tasks,
                                      CancellationToken token,
                                      std::atomic<bool>& cancelled_seen,
                                      std::thread::id& wake_thread)
{
    try
    {
        co_await async::whenAll(std::move(tasks), token);
    }
    catch (const async::TaskCancelledException&)
    {
        wake_thread = std::this_thread::get_id();
        cancelled_seen.store(true, std::memory_order_release);
    }
}

/** A scope child that reports itself once released, so a cancelled join can be shown to leave it running. */
async::Task<void> scopeChildThatReports(async::AsyncEvent& release, std::atomic<bool>& finished)
{
    co_await release;
    finished.store(true, std::memory_order_release);
}

/**
 * @brief Joins a scope whose child is still running, recording where the cancellation is seen.
 */
async::DetachedTask scopeJoinProbe(async::AsyncEvent& release,
                                   CancellationToken token,
                                   std::atomic<bool>& cancelled_seen,
                                   std::thread::id& wake_thread)
{
    async::Scope scope;
    scope.add(scopeChildThatReports(release, s_scopeChildFinished));
    try
    {
        co_await scope.join(token);
    }
    catch (const async::TaskCancelledException&)
    {
        wake_thread = std::this_thread::get_id();
        cancelled_seen.store(true, std::memory_order_release);
    }
}

/**
 * @brief Awaitable that parks the coroutine until someone else runs the parked handle.
 *
 * The shape of a step that belongs to another thread's loop: instead of resuming itself, the coroutine hands its handle
 * over, so it only continues once the owner of that loop runs it - which is what the pump of runToCompletion() is for.
 */
struct PostedResume
{
    std::coroutine_handle<>* slot;

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const noexcept { *slot = handle; }
    void await_resume() const noexcept {}
};

/// Task whose only step is posted for another loop to run: on its own it can never finish.
async::Task<int> postedStepProbe(std::coroutine_handle<>* slot, bool* reached)
{
    co_await PostedResume{ slot };
    *reached = true;
    co_return 5;
}

/// Same, but the posted step throws, so the failure happens inside the pump as well.
async::Task<void> postedFailureProbe(std::coroutine_handle<>* slot)
{
    co_await PostedResume{ slot };
    throw std::runtime_error("posted step failed");
}

/// Task that finishes on whatever thread sleepFor() resumes it on, so the pump runs in the meantime.
async::Task<int> sleepThenAnswer(std::chrono::milliseconds delay)
{
    co_await async::sleepFor(delay);
    co_return 42;
}

/**
 * @brief Runs fn on a helper thread and reports whether it finished inside the timeout.
 *
 * A drive that does not pump blocks forever, so the tests below must not join a call that has no way to finish: this
 * waits a bounded time and leaves the stuck thread behind. The caller reads what fn() produced only when this returned
 * true, and the atomic flag (or the join) makes those writes visible.
 *
 * @tparam Fn Callable to run.
 * @param fn Callable to run on the helper thread.
 * @param timeout How long to wait for it.
 * @return true when fn() finished in time, false when it is still blocked.
 */
template<typename Fn>
bool runBoundedly(Fn fn, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    std::atomic<bool> finished{ false };
    std::thread       worker([&] {
        fn();
        finished.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!finished.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!finished.load())
    {
        worker.detach();
        return false;
    }
    worker.join();
    return true;
}

} // namespace

TEST(TaskTest, ResultReturnsValue)
{
    EXPECT_EQ(answer().result(), 42);
    EXPECT_EQ(nested().result(), 43);
}

TEST(TaskTest, ResultVoid)
{
    noop().result();
    SUCCEED();
}

TEST(TaskTest, RunToCompletionRunsThePumpWhileTheTaskWaits)
{
    std::coroutine_handle<> posted{};
    bool                    reached = false;
    int                     pumps   = 0;
    int                     result  = -1;

    const bool finished = runBoundedly([&] {
        result = async::runToCompletion(postedStepProbe(&posted, &reached), [&] {
            ++pumps;
            if (posted != nullptr)
            {
                const std::coroutine_handle<> handle = posted;
                posted = {};
                handle.resume();  // the loop the task is waiting for, driven from the pump
            }
        });
    });

    ASSERT_TRUE(finished) << "runToCompletion() never returned: the step it was waiting for was never delivered";
    EXPECT_EQ(result, 5);
    EXPECT_TRUE(reached);
    // One slice is enough: the step lands in the first pump call, which is what releases the wait.
    EXPECT_EQ(pumps, 1);
}

TEST(TaskTest, RunToCompletionPumpsUntilTheTaskFinishesElsewhere)
{
    int pumps = 0;

    EXPECT_EQ(async::runToCompletion(sleepThenAnswer(std::chrono::milliseconds(20)), [&] { ++pumps; }), 42);
    EXPECT_GT(pumps, 0);
}

TEST(TaskTest, RunToCompletionDoesNotPumpWhenTheTaskNeverSuspends)
{
    int pumps = 0;

    // 一次都不挂起的任务：body 在 resume() 里就跑完、final awaiter 直接 set()，
    // 所以第一个 waitFor 立即返回，pump 一次都不该发生（等于零开销）。
    EXPECT_EQ(async::runToCompletion(answer(), [&] { ++pumps; }), 42);
    EXPECT_EQ(pumps, 0);
}

TEST(TaskTest, RunToCompletionThrowsWhenThePostedStepThrows)
{
    std::coroutine_handle<> posted{};
    bool                    resumed = false;
    std::string             what;

    const bool finished = runBoundedly([&] {
        try
        {
            async::runToCompletion(postedFailureProbe(&posted), [&] {
                if (posted != nullptr)
                {
                    const std::coroutine_handle<> handle = posted;
                    posted = {};
                    resumed = true;
                    handle.resume();
                }
            });
        }
        catch (const std::runtime_error& e)
        {
            what = e.what();
        }
    });

    ASSERT_TRUE(finished) << "runToCompletion() never returned: the step it was waiting for was never delivered";
    EXPECT_TRUE(resumed);
    EXPECT_EQ(what, "posted step failed");
}

TEST(TaskTest, LazySemantics)
{
    bool ran = false;
    auto task = lazyProbe(ran);
    EXPECT_FALSE(ran);
    EXPECT_EQ(std::move(task).result(), 7);
    EXPECT_TRUE(ran);
}

TEST(TaskTest, ExceptionPropagates)
{
    EXPECT_THROW(failing().result(), std::runtime_error);
}

TEST(TaskTest, MoveSemantics)
{
    auto task = answer();
    auto moved = std::move(task);
    EXPECT_FALSE(static_cast<bool>(task));
    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_EQ(std::move(moved).result(), 42);
}

TEST(TaskTest, AwaitEmptyTaskThrows)
{
    async::Task<int> empty;
    EXPECT_THROW(std::move(empty).result(), std::logic_error);
}

TEST(TaskTest, AwaitEmptyVoidTaskThrows)
{
    async::Task<void> empty;
    EXPECT_THROW(std::move(empty).result(), std::logic_error);
}

TEST(TaskTest, ResultCanBeReadOnlyOnce)
{
    // Task 是单消费者：读一次就把帧的所有权移走了，再读必须当场报错而不是挂死。
    auto task = answer();
    EXPECT_EQ(task.result(), 42);
    EXPECT_FALSE(static_cast<bool>(task));
    EXPECT_THROW(task.result(), std::logic_error);
}

TEST(DetachedTaskTest, RunsEagerly)
{
    async::AsyncEvent event;
    setEvent(event);
    EXPECT_TRUE(event.isSet());
}

TEST(AsyncEventTest, SetResumesWaiter)
{
    async::AsyncEvent event;

    auto task = awaitEventInt(event, 99);

    std::thread setter([&event] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        event.set();
    });

    EXPECT_EQ(std::move(task).result(), 99);
    setter.join();
}

TEST(AsyncEventTest, WaiterDestroyedWhileQueued)
{
    async::AsyncEvent event;

    // A waiter queued on the event, then destroyed by whenAny while queued;
    // its awaiter must unregister so set() never resumes a dead frame.
    std::vector<async::AnyTask> race;
    race.push_back(async::discard(awaitEvent(event)));
    race.push_back(noop());
    async::whenAny(std::move(race)).result();

    // Setting the event must not resume the destroyed waiter.
    bool ran = false;
    auto task = awaitEventFlag(event, ran);
    event.set();
    std::move(task).result();
    EXPECT_TRUE(ran);
}

TEST(AsyncEventTest, InitiallySetResumesImmediately)
{
    async::AsyncEvent event(true);
    EXPECT_TRUE(event.isSet());
    auto task = awaitEventInt(event, 99);
    EXPECT_EQ(std::move(task).result(), 99);
}

TEST(AsyncEventTest, SetBeforeAwaitResumesImmediately)
{
    async::AsyncEvent event;
    event.set();
    auto task = awaitEventInt(event, 7);
    EXPECT_EQ(std::move(task).result(), 7);
}

TEST(AsyncEventTest, MultipleWaitersAllWoken)
{
    async::AsyncEvent event;
    std::atomic<int> registered{ 0 };
    std::atomic<int> woken{ 0 };

    std::vector<async::Task<void>> tasks;
    for (int i = 0; i < 5; ++i)
    {
        tasks.push_back(eventWaitCount(event, registered, woken));
    }
    auto all = async::whenAll(std::move(tasks));

    std::thread runner([&all, &registered] {
        std::move(all).result();
    });
    while (registered.load() < 5)
    {
        std::this_thread::yield();
    }
    event.set(); // Wakes all registered waiters.
    runner.join();
    EXPECT_EQ(woken.load(), 5);
}

TEST(AsyncEventTest, ResetThenAwaitSuspends)
{
    async::AsyncEvent event;
    event.set();
    event.reset();
    EXPECT_FALSE(event.isSet());

    bool ran = false;
    auto task = awaitEventFlag(event, ran);
    std::thread setter([&event] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        event.set();
    });
    std::move(task).result();
    setter.join();
    EXPECT_TRUE(ran);
}

TEST(AsyncEventTest, ReAwaitAfterSetResumesImmediately)
{
    async::AsyncEvent event;
    int resumes = 0;

    auto task = awaitEventTwice(event, resumes);

    std::thread setter([&event] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        event.set();
    });
    std::move(task).result();
    setter.join();
    EXPECT_EQ(resumes, 2);
}

TEST(AsyncEventTest, SetDoesNotReleaseNextGeneration)
{
    // Regression: the pop-one resume loop must release only the waiters queued
    // at set()-time. A coroutine resumed by set() that synchronously re-arms
    // the event (reset + await, the VisualUserIO sequential-read pattern)
    // registers a next-generation waiter; the in-flight set() must NOT wake
    // it — it waits for the following set().
    async::AsyncEvent event;
    std::atomic<bool> registered{ false };
    std::atomic<int>  woken{ 0 };
    std::atomic<bool> rearmed{ false };

    auto task = eventReArmOnce(event, registered, woken, rearmed);
    std::thread runner([&] { std::move(task).result(); });
    while (!registered.load())
    {
        std::this_thread::yield();
    }
    // Let the runner finish enqueueing the gen-1 waiter and suspend.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    event.set(); // 1st set(): resumes gen-1 only.
    EXPECT_EQ(woken.load(), 1);
    EXPECT_TRUE(rearmed.load());

    // The gen-2 waiter (registered on the same set() stack) must stay parked.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(woken.load(), 1);

    event.set(); // 2nd set(): resumes gen-2.
    runner.join();
    EXPECT_EQ(woken.load(), 2);
}

TEST(AsyncEventTest, ConcurrentSetResetStress)
{
    async::AsyncEvent event;
    std::atomic<bool> stop{ false };
    std::atomic<int> completed{ 0 };

    std::thread flipper([&] {
        while (!stop.load())
        {
            event.set();
            event.reset();
        }
    });

    // Repeatedly await while another thread flips set/reset; must never hang.
    for (int i = 0; i < 100; ++i)
    {
        auto task = awaitEvent(event);
        std::move(task).result();
        ++completed;
    }
    stop.store(true);
    flipper.join();
    EXPECT_EQ(completed.load(), 100);
}

TEST(SchedulerTest, InlineSchedulerRunsInline)
{
    async::InlineScheduler scheduler;
    auto task = scheduleInline(scheduler);
    EXPECT_EQ(std::move(task).result(), 1);
}

TEST(SchedulerTest, ScheduleOnRunsTask)
{
    async::InlineScheduler scheduler;
    auto task = async::scheduleOn(scheduler, answer());
    EXPECT_EQ(std::move(task).result(), 42);
}

TEST(SchedulerTest, ScheduleMayReturnATask)
{
    // Invariant: Awaitable unwraps operator co_await, so a scheduler whose
    // schedule() returns a Task - not a bare awaiter - is Schedulable as well.
    // Before the concept was fixed, Awaitable<Task<void>> was false, so this
    // scheduler could not be passed to resumeOn()/scheduleOn() at all.
    TaskReturningScheduler scheduler;
    EXPECT_EQ(scheduleViaTaskReturningScheduler(scheduler).result(), 7);
}

TEST(CancellationTest, TokenAliases)
{
    vn::CancellationSource source;
    vn::CancellationToken token = source.get_token();
    EXPECT_FALSE(token.stop_requested());
    source.request_stop();
    EXPECT_TRUE(token.stop_requested());
}

TEST(TaskTest, DiscardErasesResultType)
{
    std::vector<async::AnyTask> pending;
    pending.push_back(async::discard(answer()));   // Task<int> -> Task<void>
    pending.push_back(async::discard(noop()));     // Task<void> -> Task<void>
    pending.push_back(async::discard(addOne(1)));  // Task<int> -> Task<void>

    for (auto& t : pending)
    {
        std::move(t).result();
    }
    SUCCEED();
}

TEST(TaskTest, DiscardPropagatesException)
{
    auto erased = async::discard(failing());
    EXPECT_THROW(std::move(erased).result(), std::runtime_error);
}

TEST(TaskTest, WhenAllAwaitsAll)
{
    int done = 0;

    std::vector<async::AnyTask> tasks;
    tasks.push_back(async::discard(markDone(done)));
    tasks.push_back(async::discard(markDone(done)));
    tasks.push_back(async::discard(markDone(done)));

    async::whenAll(std::move(tasks)).result();
    EXPECT_EQ(done, 3);
}

TEST(TaskTest, WhenAllPropagatesFirstException)
{
    std::vector<async::AnyTask> tasks;
    tasks.push_back(async::discard(failTask()));
    tasks.push_back(noop());

    EXPECT_THROW(async::whenAll(std::move(tasks)).result(), std::runtime_error);
}

TEST(TaskTest, WhenAllVariadicAcceptsVoidTasks)
{
    // Invariant: the variadic form composes void tasks directly, without
    // discard() and without a hand-built container. Before the fix the
    // constraint excluded void results, so only the container form compiled.
    int done = 0;
    async::whenAll(markDone(done), markDone(done), markDone(done)).result();
    EXPECT_EQ(done, 3);

    async::whenAll(noop()).result(); // A single void task needs no container either.
}

TEST(TaskTest, WhenAllVariadicVoidPropagatesFailure)
{
    EXPECT_THROW(async::whenAll(failTask(), noop()).result(), std::runtime_error);
}

TEST(TaskTest, WhenAnyVariadicAcceptsVoidTasks)
{
    // Invariant: the first void task to finish completes the composition and the
    // still-parked siblings are destroyed with it, so a sibling that never gets
    // its event never reports. Before the fix only the container form compiled.
    async::AsyncEvent never_set;
    std::atomic<bool> reported{ false };

    async::whenAny(noop(), reportIfReleased(never_set, reported)).result();
    EXPECT_FALSE(reported.load());
}

TEST(TaskTest, WhenAnyCompletesOnFirst)
{
    bool ran = false;

    std::vector<async::AnyTask> tasks;
    tasks.push_back(async::discard(neverFlag(ran)));
    tasks.push_back(noop());

    async::whenAny(std::move(tasks)).result();
    EXPECT_FALSE(ran);   // the never-completing task is destroyed with the composition
}

TEST(TaskTest, WithCancellationThrowsWhenCancelled)
{
    vn::CancellationSource source;
    source.request_stop();

    auto task    = one();
    auto wrapped = async::withCancellation(source.get_token(), std::move(task));
    EXPECT_THROW(std::move(wrapped).result(), async::TaskCancelledException);
}

TEST(TaskTest, WithCancellationRunsTask)
{
    auto task    = answer();
    auto wrapped = async::withCancellation(vn::CancellationToken{}, std::move(task));
    EXPECT_EQ(std::move(wrapped).result(), 42);
}

TEST(TaskTest, WhenAllAlreadyCancelled)
{
    vn::CancellationSource source;
    source.request_stop();

    std::vector<async::AnyTask> tasks;
    tasks.push_back(async::discard(never()));

    EXPECT_THROW(
        async::whenAll(std::move(tasks), source.get_token()).result(),
        async::TaskCancelledException);
}

TEST(TaskTest, WhenAllCancellation)
{
    // Cancellation is observed on the timer thread, not inside request_stop(): the wake-up is
    // posted there so the composition never unwinds inside the canceller's stop_callback
    // (destroying a stop_callback from inside its own callback is the same-thread case the
    // standard leaves undefined). This is also what std::stop_token promises: request_stop()
    // returns once the callbacks have run, not once the cancelled work has finished.
    vn::CancellationSource source;

    std::vector<async::AnyTask> tasks;
    tasks.push_back(async::discard(never()));

    std::atomic<bool> cancelled_seen{ false };
    std::thread::id   wake_thread{};
    cancellationProbe(std::move(tasks), source.get_token(), cancelled_seen, wake_thread);

    std::thread::id canceller_thread{};
    std::thread     canceller([&] {
        canceller_thread = std::this_thread::get_id();
        source.request_stop();
    });
    canceller.join();

    for (auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
         !cancelled_seen.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(cancelled_seen.load());
    EXPECT_NE(wake_thread, canceller_thread);
}

TEST(GeneratorTest, YieldsSequence)
{
    auto gen = gen123();

    std::vector<int> values;
    for (int v : gen)
    {
        values.push_back(v);
    }
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 2);
    EXPECT_EQ(values[2], 3);
}

TEST(GeneratorTest, ExceptionPropagates)
{
    auto gen = genThrow();

    auto it = gen.begin();
    EXPECT_EQ(*it, 1);
    EXPECT_THROW(++it, std::runtime_error);
}

TEST(GeneratorTest, SatisfiesTheCxx20InputRangeContract)
{
    // Invariant: Generator is a std::ranges input_range, so ranges algorithms
    // and views accept it. Before the fix the iterator had no postfix increment,
    // which is enough for weakly_incrementable to fail and with it input_range.
    static_assert(std::ranges::input_range<async::Generator<int>>);
    static_assert(std::input_iterator<async::Generator<int>::iterator>);

    auto counted = gen123();
    EXPECT_EQ(std::ranges::distance(counted), 3);

    auto taken = gen123();
    int sum   = 0;
    for (int v : std::views::take(taken, 2))
    {
        sum += v;
    }
    EXPECT_EQ(sum, 3);

    auto arrowed = gen123();
    EXPECT_EQ(arrowed.begin().operator->(), std::addressof(*arrowed.begin()));
}

TEST(AsyncMutexTest, LockUnlock)
{
    async::AsyncMutex mutex;
    EXPECT_TRUE(mutex.try_lock());
    EXPECT_FALSE(mutex.try_lock());
    mutex.unlock();
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(AsyncMutexTest, LockGuardReleases)
{
    async::AsyncMutex mutex;
    auto task = lockGuardRelease(mutex);
    std::move(task).result();
    EXPECT_TRUE(mutex.try_lock());   // released when the guard was destroyed
    mutex.unlock();
}

TEST(AsyncMutexTest, FifoOrder)
{
    async::AsyncMutex mutex;
    std::vector<int> order;

    EXPECT_TRUE(mutex.try_lock()); // Hold the lock so later tasks queue.

    async::Scope scope;
    for (int i = 1; i <= 4; ++i)
    {
        scope.add(fifoWorker(mutex, order, i));
    }

    // All four are now queued; releasing kicks off FIFO handoff.
    mutex.unlock();
    EXPECT_EQ(order, (std::vector<int>{ 1, 2, 3, 4 }));

    scope.join().result();
}

TEST(AsyncMutexTest, WaiterDestroyedWhileQueued)
{
    async::AsyncMutex mutex;
    async::AsyncEvent held;
    async::AsyncEvent release;
    std::atomic<bool> holder_done{ false };

    mutexHolder(mutex, held, release, holder_done);
    awaitEvent(held).result();

    // A waiter queued on the mutex, destroyed by whenAny while queued; its
    // awaiter must unregister so unlock never resumes a dead frame.
    std::vector<async::AnyTask> race;
    race.push_back(async::discard(mutexLockUnlock(mutex)));
    race.push_back(noop());
    async::whenAny(std::move(race)).result();

    release.set();
    for (int i = 0; i < 1000 && !holder_done.load(); ++i)
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE(holder_done.load());
}

TEST(AsyncMutexTest, ContentionStress)
{
    async::AsyncMutex mutex;
    constexpr int kThreads = 8;
    constexpr int kIterations = 2000;
    int counter = 0;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIterations; ++i)
            {
                auto task = mutexIncrement(mutex, counter);
                std::move(task).result();
            }
        });
    }
    for (auto& t : threads)
    {
        t.join();
    }
    EXPECT_EQ(counter, kThreads * kIterations);
}

TEST(AsyncSemaphoreTest, AcquireRelease)
{
    async::AsyncSemaphore sem(1);
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());
    sem.release();
    EXPECT_TRUE(sem.try_acquire());
    sem.release();
}

TEST(AsyncSemaphoreTest, WaiterResumedOnRelease)
{
    async::AsyncSemaphore sem(0);
    bool acquired = false;
    semAcquireSet(sem, acquired);

    sem.release();
    EXPECT_TRUE(acquired);
}

TEST(ThreadPoolSchedulerTest, ResumeOnPool)
{
    vn::ThreadPool pool(2);
    async::ThreadPoolScheduler scheduler(pool);
    bool ran = false;

    auto task = resumeOnPoolTask(scheduler, ran);

    std::move(task).result();
    EXPECT_TRUE(ran);
}

TEST(ThreadPoolSchedulerTest, RunOnPool)
{
    vn::ThreadPool pool(2);
    auto task = async::runOn(pool, [](int a, int b) { return a + b; }, 20, 22);
    EXPECT_EQ(std::move(task).result(), 42);
}

TEST(ThreadPoolSchedulerTest, RunOnPoolVoid)
{
    vn::ThreadPool pool(2);
    bool ran = false;
    auto task = async::runOn(pool, [&ran] { ran = true; });
    std::move(task).result();
    EXPECT_TRUE(ran);
}

TEST(ThreadPoolSchedulerTest, RunUsesDefaultPool)
{
    auto task = async::run([](int a, int b) { return a + b; }, 20, 22);
    EXPECT_EQ(std::move(task).result(), 42);
}

TEST(ThreadPoolSchedulerTest, RunVoidUsesDefaultPool)
{
    bool ran = false;
    auto task = async::run([&ran] { ran = true; });
    std::move(task).result();
    EXPECT_TRUE(ran);
}

TEST(TaskCompletionSourceTest, CompleteWithValue)
{
    async::TaskCompletionSource<int> tcs;
    auto task = tcs.task();
    tcs.setResult(42);
    EXPECT_EQ(std::move(task).result(), 42);
}

TEST(TaskCompletionSourceTest, CompleteBeforeAwait)
{
    async::TaskCompletionSource<int> tcs;
    tcs.setResult(42);
    EXPECT_EQ(tcs.task().result(), 42);
}

TEST(TaskCompletionSourceTest, CompleteWithException)
{
    async::TaskCompletionSource<int> tcs;
    tcs.setException(std::runtime_error("boom"));
    EXPECT_THROW(tcs.task().result(), std::runtime_error);
}

TEST(TaskCompletionSourceTest, VoidSource)
{
    async::TaskCompletionSource<void> tcs;
    auto task = tcs.task();
    tcs.setResult();
    std::move(task).result();
    SUCCEED();
}

TEST(TaskCompletionSourceTest, MultipleWaiters)
{
    async::TaskCompletionSource<int> tcs;
    auto t1 = tcs.task();
    auto t2 = tcs.task();
    tcs.setResult(7);
    EXPECT_EQ(std::move(t1).result(), 7);
    EXPECT_EQ(std::move(t2).result(), 7);
}

TEST(TaskCompletionSourceTest, TrySetTwiceReturnsFalse)
{
    async::TaskCompletionSource<int> tcs;
    EXPECT_TRUE(tcs.trySetResult(1));
    EXPECT_FALSE(tcs.trySetResult(2));
}

TEST(TaskCompletionSourceTest, SetTwiceThrows)
{
    async::TaskCompletionSource<int> tcs;
    tcs.setResult(1);
    EXPECT_THROW(tcs.setResult(2), std::logic_error);
}

TEST(TaskCompletionSourceTest, SetFromAnotherThread)
{
    async::TaskCompletionSource<int> tcs;
    auto task = tcs.task();
    std::thread setter([&tcs] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        tcs.setResult(99);
    });
    EXPECT_EQ(std::move(task).result(), 99);
    setter.join();
}

TEST(ScopeTest, AddAndJoin)
{
    async::Scope scope;
    bool a = false;
    bool b = false;
    scope.add(noop());
    scope.add(scopeFlag(a));
    scope.add(scopeFlag(b));
    scope.join().result();
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
}

TEST(ScopeTest, JoinWaitsForSlowChild)
{
    async::Scope scope;
    async::AsyncEvent slow;
    scope.add(scopeAwaitEvent(slow));

    std::thread setter([&slow] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        slow.set();
    });
    scope.join().result();
    setter.join();
}

TEST(ScopeTest, JoinRethrowsChildFailure)
{
    async::Scope scope;
    scope.add(failTask());
    EXPECT_THROW(scope.join().result(), std::runtime_error);
}

TEST(ScopeTest, JoinCancellationWakesOffTheCancellersStackAndLeavesTheChildRunning)
{
    // Cancelling a join() only stops the wait, and the child keeps running (documented). The
    // wake-up itself must come from the timer thread: resuming join() inside request_stop()
    // would destroy its stop_callback while that callback is still on the canceller's stack.
    vn::CancellationSource source;
    async::AsyncEvent      release;
    s_scopeChildFinished.store(false, std::memory_order_release);

    std::atomic<bool> cancelled_seen{ false };
    std::thread::id   wake_thread{};
    scopeJoinProbe(release, source.get_token(), cancelled_seen, wake_thread);

    std::thread::id canceller_thread{};
    std::thread     canceller([&] {
        canceller_thread = std::this_thread::get_id();
        source.request_stop();
    });
    canceller.join();

    for (auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
         !cancelled_seen.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(cancelled_seen.load());
    EXPECT_NE(wake_thread, canceller_thread);

    // The cancelled join did not destroy the child: releasing it still runs its body.
    release.set();
    for (auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
         !s_scopeChildFinished.load(std::memory_order_acquire)
         && std::chrono::steady_clock::now() < deadline;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(s_scopeChildFinished.load());
}

TEST(ScopeTest, RunsChildrenOnPool)
{
    async::Scope scope;
    vn::ThreadPool pool(2);
    std::atomic<int> counter{ 0 };
    for (int i = 0; i < 4; ++i)
    {
        scope.add(async::runOn(pool, [&counter] { return ++counter; }));
    }
    scope.join().result();
    EXPECT_EQ(counter.load(), 4);
}

TEST(TypedWhenAllTest, TupleResult)
{
    auto task = async::whenAll(answer(), addOne(10));
    auto [a, b] = std::move(task).result();
    EXPECT_EQ(a, 42);
    EXPECT_EQ(b, 11);
}

TEST(TypedWhenAllTest, SingleTask)
{
    auto task = async::whenAll(answer());
    EXPECT_EQ(std::move(task).result(), std::make_tuple(42));
}

TEST(TypedWhenAllTest, VectorResult)
{
    std::vector<async::Task<int>> tasks;
    tasks.push_back(answer());
    tasks.push_back(addOne(41));
    auto result = async::whenAll(std::move(tasks)).result();
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], 42);
    EXPECT_EQ(result[1], 42);
}

TEST(TypedWhenAllTest, VectorRethrowsFirstException)
{
    std::vector<async::Task<int>> tasks;
    tasks.push_back(answer());
    tasks.push_back(failing());
    EXPECT_THROW(async::whenAll(std::move(tasks)).result(), std::runtime_error);
}

TEST(TypedWhenAnyTest, ReturnsFirstResult)
{
    std::vector<async::Task<int>> tasks;
    tasks.push_back(delayed(1, std::chrono::milliseconds(60)));
    tasks.push_back(delayed(2, std::chrono::milliseconds(10)));
    EXPECT_EQ(async::whenAny(std::move(tasks)).result(), 2);
}

TEST(TypedWhenAnyTest, RethrowsFirstFailure)
{
    std::vector<async::Task<int>> tasks;
    tasks.push_back(failInt());
    tasks.push_back(delayed(2, std::chrono::milliseconds(100)));
    EXPECT_THROW(async::whenAny(std::move(tasks)).result(), std::runtime_error);
}

TEST(TypedWhenAnyTest, VariadicReturnsFirst)
{
    auto task = async::whenAny(delayed(1, std::chrono::milliseconds(60)),
                               delayed(2, std::chrono::milliseconds(10)));
    EXPECT_EQ(std::move(task).result(), 2);
}

TEST(SleepTest, SleepsForDuration)
{
    auto start = std::chrono::steady_clock::now();
    async::sleepFor(std::chrono::milliseconds(50)).result();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_GE(elapsed.count(), 40);
}

TEST(SleepTest, CancellationThrows)
{
    vn::CancellationSource source;
    bool cancelled = false;
    auto runner = sleepCancel(source.get_token(), cancelled);

    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        source.request_stop();
    });
    std::move(runner).result();
    canceller.join();
    EXPECT_TRUE(cancelled);
}

TEST(AsyncQueueTest, PushPop)
{
    async::AsyncQueue<int> queue;
    queue.push(1).result();
    queue.push(2).result();
    EXPECT_EQ(queue.pop().result(), 1);
    EXPECT_EQ(queue.pop().result(), 2);
}

TEST(AsyncQueueTest, PopWaitsForPush)
{
    async::AsyncQueue<int> queue;
    int got = 0;
    queuePopDetached(queue, got);
    queue.push(42).result();
    EXPECT_EQ(got, 42);
}

TEST(AsyncQueueTest, BoundedPushSuspendsWhenFull)
{
    async::AsyncQueue<int> queue(1);
    queue.push(1).result(); // Fills the bounded queue.

    bool pushed = false;
    queuePushDetached(queue, 2, pushed);
    EXPECT_FALSE(pushed);

    EXPECT_EQ(queue.pop().result(), 1); // Frees room.
    EXPECT_TRUE(pushed);
}

TEST(AsyncQueueTest, CloseThrowsOnDrainedPop)
{
    async::AsyncQueue<int> queue;
    queue.push(1).result();
    queue.close();
    EXPECT_EQ(queue.pop().result(), 1); // Drains remaining items.
    EXPECT_THROW(queue.pop().result(), std::runtime_error);
}

TEST(AsyncLatchTest, CountDownReleases)
{
    async::AsyncLatch latch(2);
    EXPECT_FALSE(latch.isReady());
    latch.countDown();
    EXPECT_FALSE(latch.isReady());
    latch.countDown();
    EXPECT_TRUE(latch.isReady());
    latch.wait().result();
}

TEST(AsyncLatchTest, WaitSuspendsUntilReady)
{
    async::AsyncLatch latch(1);
    bool released = false;
    auto task = latchWaitFlag(latch, released);

    std::thread t([&latch] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        latch.countDown();
    });
    std::move(task).result();
    t.join();
    EXPECT_TRUE(released);
}

TEST(AsyncRwLockTest, BasicReadWrite)
{
    async::AsyncReaderWriterLock lock;
    int value = 0;
    auto task = rwBasic(lock, value);
    std::move(task).result();
}

TEST(AsyncRwLockTest, ConcurrentReaders)
{
    async::AsyncReaderWriterLock lock;
    std::atomic<int> active{ 0 };
    std::atomic<int> max_active{ 0 };

    std::vector<async::Task<void>> tasks;
    for (int i = 0; i < 3; ++i)
    {
        tasks.push_back(rwReaderConcurrent(lock, active, max_active));
    }
    async::whenAll(std::move(tasks)).result();
    EXPECT_GE(max_active.load(), 2);
}

TEST(AsyncRwLockTest, WriterPreference)
{
    async::AsyncReaderWriterLock lock;
    async::AsyncEvent r1_held;
    async::AsyncEvent w1_done;
    std::atomic<bool> w1_ran{ false };
    std::atomic<bool> r2_ran{ false };

    // R1 holds a read lock until w1_done.
    rwReaderHold(lock, r1_held, w1_done);
    awaitEvent(r1_held).result();

    // W1 queues behind R1; R2 arriving after must not jump ahead of W1.
    rwWriterThen(lock, w1_done, w1_ran);
    rwReaderThen(lock, r2_ran);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(w1_ran.load());
    EXPECT_FALSE(r2_ran.load()); // Writer is queued: reader must not acquire.

    w1_done.set(); // Release R1: W1 runs first, then R2.
    for (int i = 0; i < 1000 && !r2_ran.load(); ++i)
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE(w1_ran.load());
    EXPECT_TRUE(r2_ran.load());
}

TEST(AsyncRwLockTest, WaiterDestroyedWhileQueued)
{
    async::AsyncReaderWriterLock lock;
    async::AsyncEvent held;
    async::AsyncEvent release;
    std::atomic<bool> holder_done{ false };

    rwWriterHolder(lock, held, release, holder_done);
    awaitEvent(held).result();

    // A waiter queued on the write lock, then destroyed by whenAny while
    // queued; its awaiter must unregister so release never resumes a dead frame.
    std::vector<async::AnyTask> race;
    race.push_back(async::discard(rwWriterLockUnlock(lock)));
    race.push_back(noop());
    async::whenAny(std::move(race)).result();

    release.set();
    for (int i = 0; i < 1000 && !holder_done.load(); ++i)
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE(holder_done.load());
}

TEST(AsyncConditionVariableTest, NotifyResumesWaiter)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    bool woken = false;
    cvWaiterDetached(mutex, cv, woken);

    cv.notify_one();
    for (int i = 0; i < 1000 && !woken; ++i)
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE(woken);
}

TEST(AsyncConditionVariableTest, NotifyBeforeWaitIsConsumed)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;

    cv.notify_one(); // No waiter: preserved as a pending notification.

    bool woken = false;
    auto task = cvWaitBool(mutex, cv, woken);
    std::move(task).result();
    EXPECT_TRUE(woken);
}

TEST(AsyncConditionVariableTest, NotifyAllWakesAll)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    std::atomic<int> registered{ 0 };
    std::atomic<int> woken{ 0 };

    std::vector<async::Task<void>> tasks;
    for (int i = 0; i < 4; ++i)
    {
        tasks.push_back(cvWaiterCount(mutex, cv, registered, woken));
    }
    auto all = async::whenAll(std::move(tasks));
    std::thread runner([&all, &registered] { std::move(all).result(); });
    while (registered.load() < 4)
    {
        std::this_thread::yield();
    }
    cv.notify_all();
    runner.join();
    EXPECT_EQ(woken.load(), 4);
}

TEST(AsyncConditionVariableTest, NotifyOneWakesSingleWaiter)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    std::atomic<int> registered{ 0 };
    std::atomic<int> woken{ 0 };

    std::vector<async::Task<void>> tasks;
    for (int i = 0; i < 3; ++i)
    {
        tasks.push_back(cvWaiterCount(mutex, cv, registered, woken));
    }
    auto all = async::whenAll(std::move(tasks));
    std::thread runner([&all, &registered] { std::move(all).result(); });
    while (registered.load() < 3)
    {
        std::this_thread::yield();
    }

    cv.notify_one(); // Wakes exactly one waiter.
    for (int i = 0; i < 1000 && woken.load() < 1; ++i)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(woken.load(), 1);

    cv.notify_all(); // Release the rest so the test can finish.
    runner.join();
    EXPECT_EQ(woken.load(), 3);
}

TEST(AsyncConditionVariableTest, WaiterDestroyedWhileWaiting)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;

    // A waiter that acquires the mutex and waits on the CV, then is destroyed
    // by whenAny while suspended; its node must unregister so a later notify
    // never resumes a dead frame.
    std::vector<async::AnyTask> race;
    race.push_back(async::discard(cvWait(mutex, cv)));
    race.push_back(noop());
    async::whenAny(std::move(race)).result();

    // Notifying after the waiter is gone must not resume a dead frame.
    cv.notify_all();
    SUCCEED();
}

TEST(AsyncConditionVariableTest, WaitReacquiresMutex)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    std::atomic<bool> ok{ false };

    auto w1 = cvWaitReacquire(mutex, cv, ok);

    std::thread notifier([&cv] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        cv.notify_one();
    });
    std::move(w1).result();
    notifier.join();
    EXPECT_TRUE(ok.load());
}

TEST(AsyncConditionVariableTest, NotifyAllWinnerDestroysLoserIsSafe)
{
    // Regression for a real UAF: when notify_all resumed a whenAny winner, the
    // winner re-acquired the free mutex and completed, and whenAny then
    // synchronously destroyed the still-suspended loser (structured
    // concurrency). A notify_all that detached the whole list up front and
    // held raw waiter pointers across the resume would resume the loser's
    // dangling handle. The implementation must pop and resume one waiter at a
    // time so a destroyed sibling is simply unregistered and never touched.
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    std::atomic<int> registered{ 0 };
    std::atomic<int> woken{ 0 };

    for (int iter = 0; iter < 200; ++iter)
    {
        registered.store(0);
        woken.store(0);
        auto any = cvMakeAny(mutex, cv, registered, woken);
        std::thread runner([&any, &registered] { std::move(any).result(); });
        while (registered.load() < 2)
        {
            std::this_thread::yield();
        }
        // Give both waiters time to finish registering (unlock + enqueue).
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        cv.notify_all();
        runner.join();
        EXPECT_GE(woken.load(), 1);
    }
}

TEST(AsyncConditionVariableTest, ConcurrentNotifyAllStress)
{
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    std::atomic<int> completed{ 0 };
    std::atomic<int> active{ 0 };
    std::atomic<bool> stop{ false };

    // One notifier floods notify_all while several threads run waiters to
    // completion; the notifier keeps going until every in-flight waiter is
    // done, so no waiter is left suspended and the test cannot deadlock.
    std::thread notifier([&] {
        for (;;)
        {
            cv.notify_all();
            if (stop.load() && active.load() == 0)
            {
                break;
            }
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> waiters;
    for (int t = 0; t < 4; ++t)
    {
        waiters.emplace_back([&] {
            while (!stop.load())
            {
                ++active;
                cvWaitOnce(mutex, cv, completed).result();
                --active;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    notifier.join();
    for (auto& w : waiters)
    {
        w.join();
    }
    EXPECT_GT(completed.load(), 0);
}

TEST(AsyncConditionVariableTest, RepeatedWaitNotifyCycles)
{
    // Each cycle: a fresh lazy waiter, a sticky notify issued before it even
    // starts, then the waiter consumes the sticky notification and completes.
    // Exercises re-wait after a wake plus the sticky-flag round trip.
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;
    for (int i = 0; i < 1000; ++i)
    {
        std::atomic<bool> woken{ false };
        auto task = cvWaitFlag(mutex, cv, woken);
        cv.notify_one(); // No waiter yet; preserved as a pending notification.
        std::move(task).result();
        EXPECT_TRUE(woken.load());
    }
}

TEST(AsyncConditionVariableTest, SingleNotifyNoLostWakeup)
{
    // Regression: arming the sticky flag in a separate critical section from
    // the emptiness check lets a waiter slip into the gap — it registers
    // (sees no sticky), then the sticky is set, and it is never resumed. The
    // fixed notify_all arms the sticky atomically with the check, so exactly
    // one notify per wait is never lost. Probabilistic by nature; the bounded
    // spin turns a lost wakeup into a clean failure instead of a hang.
    async::AsyncMutex mutex;
    async::AsyncConditionVariable cv;

    for (int iter = 0; iter < 1000; ++iter)
    {
        std::atomic<bool> woken{ false };

        std::thread waiter([&] {
            cvWaitFlag(mutex, cv, woken).result();
        });

        std::this_thread::yield();
        cv.notify_all(); // Exactly one notification; must not be lost.

        for (int i = 0; i < 100000 && !woken.load(); ++i)
        {
            std::this_thread::yield();
        }
        waiter.join();
        EXPECT_TRUE(woken.load());
    }
}

TEST(TaskCombinatorTest, TransformMapsResult)
{
    auto task = async::transform(answer(), [](int x) { return x * 2; });
    EXPECT_EQ(std::move(task).result(), 84);
}

TEST(TaskCombinatorTest, TransformVoid)
{
    int side = 0;
    auto task = async::transform(noop(), [&side] {
        side = 1;
        return 7;
    });
    EXPECT_EQ(std::move(task).result(), 7);
    EXPECT_EQ(side, 1);
}

TEST(TaskCombinatorTest, AndThenChainsTasks)
{
    auto task = async::andThen(answer(), [](int x) { return addOne(x); });
    EXPECT_EQ(std::move(task).result(), 43);
}

TEST(TaskCombinatorTest, AndThenVoid)
{
    auto task = async::andThen(noop(), []() { return answer(); });
    EXPECT_EQ(std::move(task).result(), 42);
}

TEST(WithTimeoutTest, CompletesInTime)
{
    auto task = async::withTimeout(delayed(42, std::chrono::milliseconds(10)),
                                   std::chrono::milliseconds(500));
    EXPECT_EQ(std::move(task).result(), 42);
}

TEST(WithTimeoutTest, TimesOut)
{
    auto task = async::withTimeout(delayed(1, std::chrono::milliseconds(500)),
                                   std::chrono::milliseconds(30));
    EXPECT_THROW(std::move(task).result(), async::TimeoutException);
}

TEST(WithTimeoutTest, TimesOutVoid)
{
    auto task = async::withTimeout(never(), std::chrono::milliseconds(30));
    EXPECT_THROW(std::move(task).result(), async::TimeoutException);
}

TEST(RetryTest, SucceedsAfterRetries)
{
    int attempts = 0;
    auto task = async::retry([&] { return retryAttempts(attempts, 3); }, 5);
    EXPECT_EQ(std::move(task).result(), 42);
    EXPECT_EQ(attempts, 3);
}

TEST(RetryTest, FailsAfterAttempts)
{
    int attempts = 0;
    auto task = async::retry([&] { return retryFail(attempts); }, 3);
    EXPECT_THROW(std::move(task).result(), std::runtime_error);
    EXPECT_EQ(attempts, 3);
}

TEST(FinallyTest, RunsOnExit)
{
    bool cleaned = false;
    auto task = finallyRunsOnExit(cleaned);
    EXPECT_EQ(std::move(task).result(), 42);
    EXPECT_TRUE(cleaned);
}

TEST(FinallyTest, RunsOnException)
{
    bool cleaned = false;
    auto task = finallyRunsOnException(cleaned);
    EXPECT_THROW(std::move(task).result(), std::runtime_error);
    EXPECT_TRUE(cleaned);
}

TEST(FinallyTest, DismissSkips)
{
    bool cleaned = false;
    auto task = finallyDismiss(cleaned);
    std::move(task).result();
    EXPECT_FALSE(cleaned);
}

TEST(YieldTest, Resumes)
{
    bool ran = false;
    auto task = yieldRan(ran);
    std::move(task).result();
    EXPECT_TRUE(ran);
}

TEST(SharedTaskTest, MultipleAwaitSameResult)
{
    int runs = 0;
    auto st = async::sharedTask(sharedSource(runs));

    int a = sharedAwaitInt(st).result();
    int b = sharedAwaitInt(st).result();
    EXPECT_EQ(a, 42);
    EXPECT_EQ(b, 42);
    EXPECT_EQ(runs, 1);
}

TEST(SharedTaskTest, ResultAfterCompletion)
{
    auto st = async::sharedTask(answer());
    EXPECT_FALSE(st.isReady());
    sharedAwaitInt(st).result();
    EXPECT_TRUE(st.isReady());
    EXPECT_EQ(st.result(), 42);
}

TEST(SharedTaskTest, ExceptionPropagatesToAll)
{
    auto st = async::sharedTask(failing());
    auto t1 = sharedAwaitInt(st);
    auto t2 = sharedAwaitInt(st);
    EXPECT_THROW(std::move(t1).result(), std::runtime_error);
    EXPECT_THROW(std::move(t2).result(), std::runtime_error);
}

TEST(SharedTaskTest, CopyableAndShared)
{
    auto st = async::sharedTask(answer());
    auto st2 = st; // Copy: both point at the same computation.
    int a = sharedAwaitInt(st).result();
    int b = sharedAwaitInt(st2).result();
    EXPECT_EQ(a, 42);
    EXPECT_EQ(b, 42);
}

TEST(SharedTaskTest, VoidSharedTask)
{
    bool ran = false;
    auto st = async::sharedTask(sharedSourceVoid(ran));
    sharedAwaitVoid(st).result();
    EXPECT_TRUE(ran);
}

TEST(SharedTaskTest, ConcurrentAwaitersRunOnce)
{
    std::atomic<int> runs{ 0 };
    auto st = async::sharedTask(sharedSourceSlow(runs));

    std::vector<async::Task<int>> tasks;
    for (int i = 0; i < 4; ++i)
    {
        tasks.push_back(sharedAwaitInt(st));
    }
    auto results = async::whenAll(std::move(tasks)).result();
    for (int r : results)
    {
        EXPECT_EQ(r, 7);
    }
    EXPECT_EQ(runs.load(), 1);
}

TEST(SharedTaskTest, EmptySharedTask)
{
    async::SharedTask<int> st;
    EXPECT_FALSE(static_cast<bool>(st));
    EXPECT_FALSE(st.isReady());
    EXPECT_THROW(sharedAwaitInt(st).result(), std::logic_error);
    EXPECT_THROW([&st]() { return st.result(); }(), std::logic_error);
}

TEST(SharedTaskTest, LazyDoesNotRunUntilFirstAwait)
{
    int runs = 0;
    auto st = async::sharedTask(sharedSource(runs));
    EXPECT_EQ(runs, 0); // Creating the SharedTask does not run the source.

    int v = sharedAwaitInt(st).result();
    EXPECT_EQ(v, 42);
    EXPECT_EQ(runs, 1);
}

TEST(SharedTaskTest, SharedTaskDestroyedWhileRunning)
{
    async::AsyncEvent release;
    std::atomic<bool> source_started{ false };
    std::atomic<bool> got{ false };

    auto st = async::sharedTask(sharedSourceEvent(release, source_started));

    auto t = sharedAwaitCheck(st, got);
    std::thread runner([&] { std::move(t).result(); });

    while (!source_started.load())
    {
        std::this_thread::yield();
    }
    st = async::SharedTask<int>{}; // Drop the handle while the source runs.
    release.set();
    runner.join();
    EXPECT_TRUE(got.load());
}

TEST(SharedTaskTest, WaiterDestroyedBeforeCompletion)
{
    async::AsyncEvent release;
    auto st = async::sharedTask(sharedSourceRelease(release));

    // An abandoned waiter: whenAny destroys it while it is suspended awaiting
    // st; its awaiter must unregister so completion never resumes a dead frame.
    std::vector<async::AnyTask> race;
    race.push_back(async::discard(sharedAwaitInt(st)));
    race.push_back(noop());
    async::whenAny(std::move(race)).result();

    release.set(); // Completing the source must not resume the destroyed waiter.
    int v = sharedAwaitInt(st).result();
    EXPECT_EQ(v, 42);
}

TEST(TaskTest, WhenAllEmptyVectorSucceeds)
{
    std::vector<async::AnyTask> tasks;
    async::whenAll(std::move(tasks)).result();
    SUCCEED();
}

TEST(TaskTest, WhenAllTypedEmptyReturnsEmptyVector)
{
    std::vector<async::Task<int>> tasks;
    auto result = async::whenAll(std::move(tasks)).result();
    EXPECT_TRUE(result.empty());
}

TEST(TypedWhenAnyTest, EmptyListThrows)
{
    std::vector<async::Task<int>> tasks;
    EXPECT_THROW(async::whenAny(std::move(tasks)).result(), std::invalid_argument);
}

TEST(TaskTest, WhenAllCancelledDoesNotStartChildren)
{
    vn::CancellationSource source;
    source.request_stop();

    bool ran = false;
    std::vector<async::Task<int>> tasks;
    tasks.push_back(ranOne(ran));
    EXPECT_THROW(async::whenAll(std::move(tasks), source.get_token()).result(),
                 async::TaskCancelledException);
    EXPECT_FALSE(ran); // Children must not start when already cancelled.
}

TEST(TaskTest, WhenAllVariadicVoidCancelledDoesNotStartChildren)
{
    // Invariant: the void variadic overloads forward their token to the shared
    // driver instead of silently dropping it, so an already-cancelled token
    // still throws before any child runs.
    vn::CancellationSource source;
    source.request_stop();

    bool ran = false;
    EXPECT_THROW(async::whenAll(source.get_token(), ranOneVoid(ran)).result(),
                 async::TaskCancelledException);
    EXPECT_FALSE(ran);

    bool ran_any = false;
    EXPECT_THROW(async::whenAny(source.get_token(), ranOneVoid(ran_any), noop()).result(),
                 async::TaskCancelledException);
    EXPECT_FALSE(ran_any);
}

TEST(TaskTest, MoveOnlyResultComposition)
{
    std::vector<async::Task<std::unique_ptr<int>>> tasks;
    tasks.push_back(makeUniquePtr(42));
    tasks.push_back(makeUniquePtr(7));
    auto result = async::whenAll(std::move(tasks)).result();
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(*result[0], 42);
    EXPECT_EQ(*result[1], 7);
}

TEST(AsyncEventTest, SetWinnerDestroysLoserIsSafe)
{
    // Regression: a batch set() that held raw waiter pointers across a resume
    // would resume a dangling sibling when the resumed waiter is a whenAny
    // winner whose completion destroys the still-suspended loser.
    std::atomic<int> registered{ 0 };
    std::atomic<int> woken{ 0 };

    for (int iter = 0; iter < 200; ++iter)
    {
        async::AsyncEvent event;
        registered.store(0);
        woken.store(0);

        auto any = eventMakeAny(event, registered, woken);
        std::thread runner([&any, &registered] { std::move(any).result(); });
        while (registered.load() < 2)
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        event.set();
        runner.join();
        EXPECT_GE(woken.load(), 1);
    }
}

TEST(AsyncSemaphoreTest, ReleaseWinnerDestroysLoserIsSafe)
{
    // Regression: a batch release() that copied every waiter handle before
    // resuming could resume a dangling handle when the resumed waiter is a
    // whenAny winner whose completion destroys the still-suspended loser.
    async::AsyncSemaphore sem(0);
    std::atomic<int> registered{ 0 };
    std::atomic<int> acquired{ 0 };

    for (int iter = 0; iter < 200; ++iter)
    {
        registered.store(0);
        acquired.store(0);

        auto any = semMakeAny(sem, registered, acquired);
        std::thread runner([&any, &registered] { std::move(any).result(); });
        while (registered.load() < 2)
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        sem.release(2); // Grant both queued acquirers.
        runner.join();
        EXPECT_GE(acquired.load(), 1);
    }
}

TEST(AsyncQueueTest, CloseWinnerDestroysLoserIsSafe)
{
    // Regression: a batch close() that collected every waiter handle before
    // resuming could resume a dangling handle when the resumed popper is a
    // whenAny winner whose completion destroys the still-suspended sibling.
    std::atomic<int> registered{ 0 };
    std::atomic<int> popped{ 0 };

    for (int iter = 0; iter < 200; ++iter)
    {
        async::AsyncQueue<int> queue(0);
        registered.store(0);
        popped.store(0);

        auto any = queueMakeAny(queue, registered, popped);
        std::thread runner([&any, &registered] { std::move(any).result(); });
        while (registered.load() < 2)
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        queue.close();
        runner.join();
        EXPECT_GE(popped.load(), 1);
    }
}

TEST(AsyncRwLockTest, UnlockWriteWinnerDestroysLoserReaderIsSafe)
{
    // Regression: a batch reader wake that held raw reader pointers across a
    // resume would resume a dangling sibling when the resumed reader is a
    // whenAny winner whose completion destroys the still-suspended loser.
    async::AsyncReaderWriterLock lock;
    async::AsyncEvent held;
    async::AsyncEvent release;
    std::atomic<int> registered{ 0 };
    std::atomic<int> acquired{ 0 };

    // Hold the write lock so readers queue.
    rwWriterHoldRelease(lock, held, release);
    awaitEvent(held).result();

    for (int iter = 0; iter < 200; ++iter)
    {
        registered.store(0);
        acquired.store(0);

        auto any = rwReaderMakeAny(lock, registered, acquired);
        std::thread runner([&any, &registered] { std::move(any).result(); });
        while (registered.load() < 2)
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        release.set(); // Writer releases; both queued readers are granted.
        runner.join();
        EXPECT_GE(acquired.load(), 1);
    }
}

TEST(TaskCompletionSourceTest, SetResultWinnerDestroysLoserIsSafe)
{
    // Regression: a completion that swapped every waiter handle out and
    // resumed them all could resume a dangling handle when the resumed waiter
    // is a whenAny winner whose completion destroys the still-suspended loser.
    std::atomic<int> registered{ 0 };
    std::atomic<int> completed{ 0 };

    for (int iter = 0; iter < 200; ++iter)
    {
        async::TaskCompletionSource<int> tcs;
        registered.store(0);
        completed.store(0);

        auto any = tcsMakeAny(tcs, registered, completed);
        std::thread runner([&any, &registered] { std::move(any).result(); });
        while (registered.load() < 2)
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        tcs.setResult(42);
        runner.join();
        EXPECT_GE(completed.load(), 1);
    }
}

// ---------------------------------------------------------------------------
// Regression tests for the async-library defects found on 2026-09-17.
// Each test names the invariant it pins down and the failure mode it guards.
// ---------------------------------------------------------------------------

namespace {

async::Generator<int> genEmpty()
{
    co_return;
}

async::Generator<int> genThrowBeforeYield()
{
    throw std::runtime_error("boom before the first value");
    co_yield 1;
}

async::Task<void> yieldLoop(unsigned iterations)
{
    for (unsigned i = 0; i < iterations; ++i)
    {
        co_await async::yield();
    }
    co_return;
}

#if defined(__GNUC__) || defined(__clang__)
async::Task<void> yieldLoopDepth(unsigned iterations, std::ptrdiff_t& depth)
{
    char* top    = static_cast<char*>(__builtin_frame_address(0));
    char* bottom = top;
    for (unsigned i = 0; i < iterations; ++i)
    {
        bottom = static_cast<char*>(__builtin_frame_address(0));
        co_await async::yield();
    }
    depth = top - bottom;
    co_return;
}
#endif

async::Task<void> eventGen2Waiter(async::AsyncEvent& event,
                                  std::atomic<int>& woken,
                                  std::atomic<int>& is_set)
{
    co_await event;
    is_set.store(event.isSet() ? 1 : 0); // Woken while the event is clear => spurious.
    ++woken;
    co_return;
}

async::Task<void> eventReArmAndFinish(async::AsyncEvent& event,
                                      async::Scope& scope,
                                      std::atomic<int>& registered,
                                      std::atomic<int>& gen2_woken,
                                      std::atomic<int>& gen2_is_set)
{
    ++registered;
    co_await event; // gen-1 waiter, queued first, so it is the first one resumed
    event.reset();  // re-arm while set() is still unwinding (the read-loop pattern)
    scope.add(eventGen2Waiter(event, gen2_woken, gen2_is_set)); // gen-2 waiter, queued now
    co_return; // finishing makes whenAny the winner: the sibling below is destroyed
}

async::Task<void> eventBoundaryWaiter(async::AsyncEvent& event,
                                      std::atomic<int>& registered,
                                      std::atomic<int>& ran)
{
    ++registered;
    co_await event; // gen-1 waiter, queued second => it is the generation boundary
    ++ran;
    co_return;
}

async::Task<void> eventRaceReArmWithBoundarySibling(async::AsyncEvent& event,
                                                    async::Scope& scope,
                                                    std::atomic<int>& registered,
                                                    std::atomic<int>& gen2_woken,
                                                    std::atomic<int>& gen2_is_set,
                                                    std::atomic<int>& boundary_ran)
{
    std::vector<async::AnyTask> race;
    race.push_back(async::discard(eventReArmAndFinish(event, scope, registered, gen2_woken, gen2_is_set)));
    race.push_back(async::discard(eventBoundaryWaiter(event, registered, boundary_ran)));
    co_await async::whenAny(std::move(race));
}

} // namespace

TEST(AsyncDefectRegressionTest, ZeroCountLatchWaitReturnsImmediately)
{
    // Invariant: a latch whose count is already zero is released, so wait() must
    // complete without suspending. Before the fix the done event was armed only
    // by countDown(), so wait() hung forever while isReady() reported true.
    // withTimeout turns that hang into a plain test failure.
    async::AsyncLatch latch(0);
    ASSERT_TRUE(latch.isReady());

    EXPECT_NO_THROW(async::withTimeout(latch.wait(), std::chrono::milliseconds(200)).result());
}

TEST(AsyncDefectRegressionTest, EventKeepsNextGenerationParkedWhenBoundaryIsDestroyed)
{
    // Invariant (documented on AsyncEvent::set): only the waiters queued at the
    // moment set() is called are released; a waiter registered during the resume
    // window waits for the next set().
    //
    // Before the fix the boundary was a pointer to the queue tail, so when the
    // resumed waiter destroyed that tail (a whenAny winner abandoning its
    // sibling) the release loop lost the boundary and went on to wake the
    // next-generation waiter while the event was still clear - a spurious
    // wakeup that breaks waiters which do not re-check their predicate (the
    // sequential read loops in ConsoleUserIO / VisualUserIO).
    async::AsyncEvent event;
    async::Scope scope;
    std::atomic<int> registered{ 0 };
    std::atomic<int> gen2_woken{ 0 };
    std::atomic<int> gen2_is_set{ -1 };
    std::atomic<int> boundary_ran{ 0 };

    std::thread runner([&] {
        eventRaceReArmWithBoundarySibling(
            event, scope, registered, gen2_woken, gen2_is_set, boundary_ran).result();
    });
    while (registered.load() < 2)
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // both gen-1 waiters parked

    event.set(); // set #1 releases the two gen-1 waiters only

    // Precondition, so the test cannot pass vacuously: the boundary waiter was
    // abandoned (not resumed), i.e. the boundary really did disappear while
    // set() was unwinding.
    EXPECT_EQ(boundary_ran.load(), 0);
    // The invariant under test.
    EXPECT_EQ(gen2_woken.load(), 0);
    EXPECT_EQ(gen2_is_set.load(), -1);

    runner.join();

    event.set(); // set #2 releases the gen-2 waiter
    scope.join().result();
    EXPECT_EQ(gen2_woken.load(), 1);
    EXPECT_EQ(gen2_is_set.load(), 1); // ...and only while the event is set
}

TEST(AsyncDefectRegressionTest, FinishedGeneratorReleasesItsFrame)
{
    // Invariant: begin() owns the frame, so a generator that finishes before its
    // first co_yield must free it (empty body, or a throw before the first
    // value). Before the fix begin() dropped the handle without destroying the
    // frame (leak) and rethrew before dropping it, which also left a handle
    // parked at final_suspend. LeakSanitizer is what observes this: run under
    // the ASan gate for the leak half of the evidence.
    {
        auto empty = genEmpty();
        EXPECT_TRUE(empty.begin() == empty.end());
    }
    {
        auto throwing = genThrowBeforeYield();
        // begin() is [[nodiscard]]; the throw is the point here, not the iterator it would return.
        EXPECT_THROW(static_cast<void>(throwing.begin()), std::runtime_error);
    }
}

TEST(AsyncDefectRegressionTest, ThrowingGeneratorInvalidatesIterator)
{
    // Invariant: a generator that throws is exhausted from the caller's point of
    // view, so the increment that rethrows must leave the iterator equal to
    // end(). Before the fix the rethrow ran before coro_ was cleared, so
    // `it != end()` still held and the caller's next increment resumed a
    // coroutine parked at final_suspend (undefined behaviour, a segfault here).
    auto gen = genThrow(); // co_yield 1; throw; co_yield 2;
    auto it  = gen.begin();
    ASSERT_EQ(*it, 1);

    EXPECT_THROW(++it, std::runtime_error);
    EXPECT_TRUE(it == gen.end());
}

TEST(AsyncDefectRegressionTest, YieldLoopStackIsConstant)
{
    // Invariant: co_await yield() must not consume stack per iteration - it is a
    // suspension point, so resuming has to go through symmetric transfer.
    // Before the fix await_suspend called h.resume() inline, keeping one
    // await_suspend plus one resume frame alive per yield: 336 bytes/yield at
    // -O0 (a 100k loop overflows the 8 MiB stack) and 32 bytes/yield at -O2.
#if defined(__GNUC__) || defined(__clang__)
    std::ptrdiff_t depth = 0;
    yieldLoopDepth(5000, depth).result();
    EXPECT_LT(depth, 64 * 1024); // O(1); the buggy build measures ~1.7 MB
#endif
    // Portable backstop that needs no frame-address builtin: this loop dies of
    // stack exhaustion when the awaiter resumes inline.
    yieldLoop(100000).result();
}

namespace {

#if defined(__GNUC__) || defined(__clang__)
/// Opaque barrier: the pointer escapes into inline asm, so the compiler cannot
/// prove the padding unused and has to keep it inside the coroutine frame.
void keepFramePaddingAlive(const void* p, std::size_t n) noexcept
{
    asm volatile("" : : "r"(p), "r"(n) : "memory");
}
#else
/// Best effort elsewhere: touch both ends of the padding.
void keepFramePaddingAlive(const void* p, std::size_t n) noexcept
{
    const volatile unsigned char* bytes = static_cast<const volatile unsigned char*>(p);
    (void)bytes[0];
    (void)bytes[n - 1];
}
#endif

/// Coroutine-frame padding: a frame this large is served by its own mmap'ed
/// chunk, so resuming it after destruction faults on the unmapped page instead
/// of quietly reading freed-but-mapped memory.
struct FramePadding
{
    std::array<unsigned char, 256 * 1024> bytes{};
};

async::Task<int> sharedAwaitCounted(async::SharedTask<int>& st, std::atomic<int>& resumed)
{
    FramePadding padding;
    padding.bytes[0] = 1;
    keepFramePaddingAlive(&padding, sizeof(padding));

    const int v = co_await st;

    keepFramePaddingAlive(&padding, sizeof(padding)); // Must stay live across the await.
    resumed.fetch_add(v == 7 && padding.bytes[0] == 1 ? 1 : 0);
    co_return v;
}

} // namespace

TEST(AsyncDefectRegressionTest, SharedTaskResumesWaitersOneAtATime)
{
    // Invariant: resuming one waiter of a shared completion must never resume a
    // sibling that the resumed waiter destroyed. whenAny makes the first waiter
    // to finish destroy the losers, which is the pattern every other completion
    // source in this module handles by popping one waiter per lock and never
    // holding a handle across a resume. Before the fix runShared() swapped the
    // whole waiter list out first and then resumed each handle in it, so the
    // losing waiter was resumed after its frame had been freed: heap-use-
    // after-free under ASan, and a fault on the frame's own mmap'ed chunk
    // otherwise (measured: 20/20 faults with padded frames and the settle
    // window below, 19/20 without either).
    std::atomic<int> runs{ 0 };
    auto st = async::sharedTask(sharedSourceSlow(runs)); // sleeps 30 ms, then yields 7

    std::atomic<int> resumed{ 0 };
    std::vector<async::AnyTask> race;
    race.reserve(2);
    race.push_back(async::discard(sharedAwaitCounted(st, resumed)));
    race.push_back(async::discard(sharedAwaitCounted(st, resumed)));

    // The 30 ms source keeps both waiters parked simultaneously, so completion
    // resumes two of them: the winner finishes, the loser is destroyed.
    async::whenAny(std::move(race)).result();

    // .result() returns as soon as the winner is published, while the loser is
    // still being resumed (or destroyed) on the timer thread: stay alive long
    // enough for that to happen, or process exit would hide the fault.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(st.isReady());
    EXPECT_EQ(runs.load(), 1);    // The source still ran exactly once.
    EXPECT_EQ(resumed.load(), 1); // Only the winner ran; the loser was destroyed.
    EXPECT_EQ(sharedAwaitInt(st).result(), 7); // Completion stayed usable.
}

// ---------------------------------------------------------------------------
// Sleep timer service: one process-wide thread, exact deadlines, immediate
// cancellation. The measurements these tests pin down are in the class comment
// of detail::TimerService (Sleep.hpp).
// ---------------------------------------------------------------------------

namespace {

#if defined(__linux__)
/// Number of OS threads this process currently has (Linux: /proc/self/task).
long osThreadCount()
{
    long count = 0;
    if (DIR* dir = opendir("/proc/self/task"))
    {
        while (readdir(dir) != nullptr)
        {
            ++count;
        }
        closedir(dir);
    }
    return count - 2; // "." and ".."
}
#endif

async::Task<void> sleepCounted(std::chrono::milliseconds duration, std::atomic<int>& completed)
{
    co_await async::sleepFor(duration);
    ++completed;
    co_return;
}

async::Task<void> sleepFlag(std::chrono::milliseconds duration, std::atomic<bool>& done)
{
    co_await async::sleepFor(duration);
    done.store(true);
    co_return;
}

async::Task<void> cancellableSleepFor(vn::CancellationToken token, std::atomic<bool>& cancelled)
{
    try
    {
        co_await async::sleepFor(std::chrono::milliseconds(500), token);
    }
    catch (const async::TaskCancelledException&)
    {
        cancelled.store(true);
    }
    co_return;
}

async::Task<void> sleepRecordingThread(vn::CancellationToken token,
                                       std::thread::id& wake_thread,
                                       std::atomic<bool>& cancelled)
{
    try
    {
        co_await async::sleepFor(std::chrono::milliseconds(500), token);
    }
    catch (const async::TaskCancelledException&)
    {
        wake_thread = std::this_thread::get_id(); // The thread the resume ran on.
        cancelled.store(true);
    }
    co_return;
}

} // namespace

TEST(SleepTimerTest, ConcurrentSleepsShareOneTimerThread)
{
    // One timer thread serves every pending sleep. The previous implementation
    // started one OS thread per sleep, so this line read "+64" for 64 sleeps.
    async::Scope scope;
#if defined(__linux__)
    const long before = osThreadCount();
#endif
    std::atomic<int> completed{ 0 };
    for (int i = 0; i < 32; ++i)
    {
        scope.add(sleepCounted(std::chrono::milliseconds(100), completed));
    }
#if defined(__linux__)
    const long during = osThreadCount();
    EXPECT_LE(during - before, 2); // The shared worker, plus slack.
#endif
    scope.join().result();
    EXPECT_EQ(completed.load(), 32);
}

TEST(SleepTimerTest, CancellationWakesPromptly)
{
    // 23 ms deliberately avoids the 10 ms grid the old slice loop polled on:
    // it measured ~8.8 ms worst case there, because the token was only noticed
    // at the next slice boundary. The timer service resumes the waiter on the
    // cancelling thread, so the bound below has ~20x margin.
    long long worst = 0;
    for (int i = 0; i < 5; ++i)
    {
        vn::CancellationSource source;
        std::atomic<bool>        cancelled{ false };
        auto                     task = cancellableSleepFor(source.get_token(), cancelled);

        std::chrono::steady_clock::time_point requested{};
        std::thread canceller([&source, &requested] {
            std::this_thread::sleep_for(std::chrono::milliseconds(23));
            requested = std::chrono::steady_clock::now();
            source.request_stop();
        });
        std::move(task).result();
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - requested)
                                 .count();
        canceller.join();

        EXPECT_TRUE(cancelled.load());
        if (latency > worst)
        {
            worst = latency;
        }
    }
    EXPECT_LT(worst, 3000); // Microseconds; the polling implementation: ~8800.
}

TEST(SleepTimerTest, CancellationResumesOnTheTimerThreadNotTheCancellers)
{
    // Resuming the continuation inside request_stop() would run all of the
    // cancelled coroutine's remaining code in the canceller's stack: re-entrant
    // into whatever the canceller holds, and it changes the thread hand-off
    // timing that other threads observe. Measured before this was fixed: 6/10
    // full test_gui runs failed its exclusive-takeover handshake, against 0/10
    // for the thread-based implementation.
    vn::CancellationSource source;
    std::thread::id          wake_thread{};
    std::atomic<bool>        cancelled{ false };
    auto                     task = sleepRecordingThread(source.get_token(), wake_thread, cancelled);

    std::thread::id canceller_thread{};
    std::thread     canceller([&] {
        canceller_thread = std::this_thread::get_id();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        source.request_stop();
    });
    std::move(task).result();
    canceller.join();

    EXPECT_TRUE(cancelled.load());
    EXPECT_NE(wake_thread, canceller_thread);
}

TEST(SleepTimerTest, ShorterDeadlineInsertedLaterStillFiresFirst)
{
    // install() must wake the worker when a new deadline jumps the queue;
    // without that notification the later, shorter sleep would only fire when
    // the earlier long deadline expires. Bound chosen far below the 400 ms long
    // sleep, so this is a real check and not a timing race.
    async::Scope      scope;
    std::atomic<bool> long_done{ false };
    std::atomic<bool> short_done{ false };

    scope.add(sleepFlag(std::chrono::milliseconds(400), long_done));
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // long sleep arms first

    const auto start = std::chrono::steady_clock::now();
    scope.add(sleepFlag(std::chrono::milliseconds(20), short_done));
    while (!short_done.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(1))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();

    EXPECT_TRUE(short_done.load());
    EXPECT_LT(waited, 150); // Should be ~20 ms; a missed notify would be ~380 ms.

    scope.join().result();
    EXPECT_TRUE(long_done.load());
}

TEST(SleepTimerTest, ManyStaggeredDeadlinesAllFireOnce)
{
    // Exercises the worker's "pop one due node per lock, resume outside" loop:
    // 100 deadlines land in one narrow window, so the queue is repeatedly
    // non-empty and several nodes are due at the same wake-up.
    async::Scope      scope;
    std::atomic<int>  completed{ 0 };
    const auto        start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i)
    {
        scope.add(sleepCounted(std::chrono::milliseconds(1 + i % 20), completed));
    }
    scope.join().result();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_EQ(completed.load(), 100);
    EXPECT_LT(elapsed, 400); // ~20 ms of deadlines; serialization would show here.
}

TEST(SleepTimerTest, AbandonedSleepIsDroppedWithoutResuming)
{
    // Destroying a suspended sleep claims its timer node, so the timer thread
    // must drop the entry at its deadline instead of resuming a dead frame.
    // A resume here is a use-after-free, which the ASan gate sees as a crash.
    std::atomic<bool> abandoned_ran{ false };

    std::vector<async::AnyTask> race;
    race.push_back(async::discard(sleepFlag(std::chrono::milliseconds(60), abandoned_ran)));
    race.push_back(async::discard(async::sleepFor(std::chrono::milliseconds(5))));
    async::whenAny(std::move(race)).result(); // the 60 ms sleep is destroyed here

    std::this_thread::sleep_for(std::chrono::milliseconds(120)); // past its deadline
    EXPECT_FALSE(abandoned_ran.load());
}

// ---------------------------------------------------------------------------
// Composition and queue contracts that used to be promises the code could not
// keep: a result that cannot be moved into a composition's state, and a queue
// whose capacity was only checked before the enqueue.
// ---------------------------------------------------------------------------

/**
 * @brief Result type whose move constructor throws on a chosen attempt.
 *
 * Two moves stand between a child's co_return and the composition's state: the promise stores
 * the value (move 1), then the final awaiter moves it into the composition (move 2). Letting
 * either throw is legal - the value type only has to be movable - and both must surface as the
 * task's error. Escaping an await_suspend that is noexcept would terminate the process instead,
 * which is what the tests below catch.
 */
struct ThrowingMove
{
    /// Which move attempt throws; 0 means none.
    inline static int s_throw_on{ 0 };

    /// Move attempts so far; reset by the tests.
    inline static int s_moves{ 0 };

    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove& operator=(ThrowingMove&&) = delete;

    ThrowingMove(ThrowingMove&&)
    {
        if (++s_moves == s_throw_on)
        {
            throw std::runtime_error("throwing move");
        }
    }
};

async::Task<ThrowingMove> throwingMoveTask()
{
    co_return ThrowingMove{};
}

/// Pushes one value and counts the pushes that completed; the count is what a bound is visible in.
async::Task<void> pushAndCount(async::AsyncQueue<int>& queue, int value, std::atomic<int>& completed)
{
    co_await queue.push(value);
    completed.fetch_add(1, std::memory_order_release);
}

/// Waiter container whose registration always fails, so the no-throw path can be exercised.
struct FailingWaiters
{
    void push_back(std::coroutine_handle<>)
    {
        throw std::bad_alloc{};
    }
};

/// Awaiter whose await_suspend result is none of the three forms the language allows.
struct IllegalSuspendResult
{
    bool await_ready() const noexcept { return false; }
    int  await_suspend(std::coroutine_handle<>) const noexcept { return 0; }
    void await_resume() const noexcept {}
};

/// Type that is not awaitable at all.
struct NotAwaitable
{};

// The Awaitable concept accepts what co_await accepts - including the module's own awaitables,
// which are reachable only through operator co_await - and rejects what the compiler would
// refuse. Both sides are pinned here because a concept that says yes to everything is worthless.
static_assert(async::Awaitable<async::Task<int>>);
static_assert(async::Awaitable<async::Task<void>>);
static_assert(async::Awaitable<async::SharedTask<int>>);
static_assert(async::Awaitable<async::AsyncEvent&>);
static_assert(async::Awaitable<async::detail::QueuePopAwaiter<int>>);
static_assert(!async::Awaitable<IllegalSuspendResult>);
static_assert(!async::Awaitable<NotAwaitable>);
static_assert(!async::Awaitable<int>);
static_assert(!async::Awaitable<void>);

TEST(AsyncDetailTest, AThrowingWaiterListIsReportedInsteadOfEscaping)
{
    // The waiter lists are std::vectors, so registering a waiter allocates inside await_suspend,
    // which the awaiters declare noexcept: a thrown bad_alloc there would terminate the process,
    // and letting it escape would leave the coroutine suspended with nothing holding it. The
    // failure is captured and rethrown from await_resume() instead - on the coroutine's own
    // stack, where unhandled_exception() turns it into the task's error.
    FailingWaiters      waiters;
    std::exception_ptr  failure{};

    EXPECT_FALSE(async::detail::tryRegisterWaiter(waiters, std::coroutine_handle<>{}, failure));
    ASSERT_NE(failure, nullptr);
    EXPECT_THROW(std::rethrow_exception(failure), std::bad_alloc);
}

TEST(WhenAnyTest, AValueThatCannotBeMovedBecomesTheCompositionsError)
{
    // A result type only has to be movable, and its move constructor may throw - including on the
    // moves the library performs itself, inside the child's promise and inside the composition's
    // state. Both of those stores sit in noexcept code, so a throw that escaped would terminate
    // the process. Every move of the chain is tried in turn: each has to reach the caller as this
    // task's error. Only runtime_error is caught, so any other outcome - including a terminate -
    // fails the test.
    bool any_threw = false;
    for (int move = 1; move <= 8; ++move)
    {
        ThrowingMove::s_moves    = 0;
        ThrowingMove::s_throw_on = move;

        std::vector<async::Task<ThrowingMove>> tasks;
        tasks.push_back(throwingMoveTask());
        try
        {
            static_cast<void>(async::whenAny(std::move(tasks)).result());
        }
        catch (const std::runtime_error&)
        {
            any_threw = true; // the move failure came out as the task's error
        }
    }
    ThrowingMove::s_throw_on = 0;

    EXPECT_TRUE(any_threw);
}

TEST(AsyncQueueTest, ABoundedPushClaimsItsSlotInsideTheEnqueue)
{
    // The bound is only real when the room is claimed in the same critical section that enqueues.
    // A bounded awaiter therefore may not report "ready": that would enqueue from await_resume(),
    // after the lock had been released, and two pushers could both claim the same free slot. An
    // unbounded queue has no bound to keep, so it stays inline.
    auto bounded    = std::make_shared<async::detail::AsyncQueueState<int>>();
    bounded->capacity = 1;
    async::detail::QueuePushAwaiter<int> bounded_push(bounded, 1);
    EXPECT_FALSE(bounded_push.await_ready());

    auto unbounded = std::make_shared<async::detail::AsyncQueueState<int>>();
    async::detail::QueuePushAwaiter<int> unbounded_push(unbounded, 2);
    EXPECT_TRUE(unbounded_push.await_ready());
}

TEST(AsyncQueueTest, ABoundedQueueNeverHoldsMoreThanItsCapacity)
{
    // capacity == 1 with no consumer: exactly one of four concurrent pushes may complete, and it
    // has to hold for every interleaving. The room is claimed under the same lock that enqueues,
    // so a second pusher cannot slip into a slot that was free when it looked - which is what
    // the check-then-enqueue shape let happen.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        async::AsyncQueue<int> queue(1);
        std::atomic<int>       completed{ 0 };

        std::vector<std::thread> pushers;
        for (int i = 0; i < 4; ++i)
        {
            pushers.emplace_back([&queue, i, &completed] {
                auto push = pushAndCount(queue, i, completed);
                push.result(); // blocks until this push has been enqueued
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int settled = completed.load(std::memory_order_acquire);

        // Free every slot so the parked pushers finish and the threads can join.
        for (int i = 0; i < 4; ++i)
        {
            static_cast<void>(queue.pop().result());
        }
        for (auto& pusher : pushers)
        {
            pusher.join();
        }

        EXPECT_EQ(settled, 1) << "attempt " << attempt << ": a bounded queue of capacity 1 "
                                 "accepted more than one push with no consumer";
    }
}
