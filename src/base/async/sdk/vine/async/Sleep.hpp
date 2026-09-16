#pragma once

#include "async_global.hpp"

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include <vine/CancellationToken.hpp>

#include "Cancellation.hpp"
#include "Task.hpp"

V_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief One pending wake-up in the shared timer service.
 *
 * Owned by the awaiter; the service keeps only a weak reference, so a timer
 * can never outlive the frame waiting on it. installed/settled/aborted are
 * guarded by the service mutex: `installed` publishes the node into the
 * deadline queue, `aborted` records a token cancellation that arrived while
 * arming, and `settled` is the claim deciding who may resume the continuation.
 */
struct TimerNode
{
    std::chrono::steady_clock::time_point deadline{};
    std::coroutine_handle<>               continuation{};

    /// true once this node is in the deadline queue.
    bool installed{ false };

    /// true once claimed (resumed) or abandoned; never resumed after that.
    bool settled{ false };

    /// true when the token was cancelled before the node was installed.
    bool aborted{ false };

    /// true while the node sits in the service's early-wake queue.
    bool queued{ false };
};

/**
 * @brief Process-wide timer thread shared by every sleep.
 *
 * One thread serves all pending sleeps instead of one thread per sleep, it
 * waits exactly until the earliest deadline (no polling slices), and a token
 * cancellation wakes its waiter at once - on the timer thread, never in the
 * cancelling thread's stack. The service owns no coroutine state: nodes are
 * weak references, so a node whose awaiter is gone is simply dropped when its
 * deadline is reached.
 *
 * Lifetime: the instance is intentionally never destroyed. It owns a
 * process-wide thread, and any static-destruction order would either join that
 * thread (blocking exit) or leave a pending waiter with a destroyed mutex. The
 * running worker keeps the object reachable, so it is not a reported leak.
 */
class TimerService
{
  public:
    /**
     * @brief Returns the process-wide service, starting its thread on first use.
     *
     * @return The service; never null and never destroyed.
     */
    [[nodiscard]]
    static TimerService& instance() noexcept;

    /**
     * @brief Publishes a node into the deadline queue.
     *
     * @param node Node to arm; its deadline and continuation must be set.
     * @return true when the wait is live, false when the token was cancelled
     * while arming and the caller must not suspend.
     */
    bool install(const std::shared_ptr<TimerNode>& node);

    /**
     * @brief Asks the timer thread to resume a wait early (token cancelled).
     *
     * Only queues the request: the resume happens on the timer thread, never in
     * the caller's stack. Resuming in the cancelling thread's stack would run
     * the whole continuation inside request_stop() - re-entrant into whatever
     * the canceller holds - and it also changes the hand-off timing observable
     * to other threads (measured: it made a thread-handshake test in test_gui
     * fail 6/10 runs where the thread-based implementation failed 0/10).
     *
     * @param node Node whose token was cancelled.
     */
    void requestAbort(const std::shared_ptr<TimerNode>& node) noexcept;

    /**
     * @brief Abandons a wait without resuming it (its frame is going away).
     *
     * @param node Node to abandon; ignored when it was already claimed.
     */
    void cancel(const std::shared_ptr<TimerNode>& node) noexcept;

  private:
    TimerService() noexcept = default;

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    /// Timer thread body: wait for the earliest deadline, then wake one waiter.
    void run() noexcept;

    /// Starts the worker thread on first use; the caller must hold mutex_.
    void ensureWorkerLocked();

    std::mutex              mutex_;
    std::condition_variable cv_;

    /// Pending deadlines, earliest first; entries whose owner is gone expire here.
    std::multimap<std::chrono::steady_clock::time_point, std::weak_ptr<TimerNode>> timers_;

    /// Waits whose token was cancelled, to be resumed by the timer thread.
    std::deque<std::weak_ptr<TimerNode>> early_;

    std::thread worker_;
};

inline TimerService& TimerService::instance() noexcept
{
    // Intentionally leaked; see the class comment.
    static TimerService* const service = new TimerService();
    return *service;
}

inline void TimerService::ensureWorkerLocked()
{
    if (!worker_.joinable())
    {
        worker_ = std::thread([this] { run(); });
    }
}

inline bool TimerService::install(const std::shared_ptr<TimerNode>& node)
{
    std::lock_guard lock(mutex_);
    if (node->aborted)
    {
        return false; // Cancelled while arming: never schedule a dead wait.
    }
    node->installed = true;
    timers_.emplace(node->deadline, node);
    ensureWorkerLocked();
    // The new deadline may be earlier than the one the worker waits for.
    cv_.notify_all();
    return true;
}

inline void TimerService::requestAbort(const std::shared_ptr<TimerNode>& node) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (node->settled || node->queued)
        {
            return;
        }
        node->aborted = true;
        if (!node->installed)
        {
            // install() sees `aborted` and refuses to suspend; nothing to resume.
            return;
        }
        node->queued = true;
        early_.push_back(node);
    }
    // Wake the worker: it may be waiting for a deadline far beyond this one.
    cv_.notify_all();
}

inline void TimerService::cancel(const std::shared_ptr<TimerNode>& node) noexcept
{
    std::lock_guard lock(mutex_);
    node->settled = true; // Its entry expires at the deadline, without a resume.
}

inline void TimerService::run() noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        // 1. Cancelled waits wake first, whatever their deadline: they were
        //    queued by requestAbort() rather than by the clock.
        if (!early_.empty())
        {
            auto node = early_.front().lock();
            early_.pop_front();

            std::coroutine_handle<> continuation{};
            if (node && !node->settled)
            {
                node->settled = true;
                continuation  = node->continuation;
            }
            if (continuation)
            {
                lock.unlock();
                continuation.resume(); // Never hold the mutex across a resume.
                lock.lock();
            }
            continue;
        }

        if (timers_.empty())
        {
            cv_.wait(lock); // Nothing pending: no polling, no wake-ups.
            continue;
        }

        if (timers_.begin()->first > std::chrono::steady_clock::now())
        {
            // Sleep exactly until the earliest deadline; install() and
            // requestAbort() notify when the queue changes.
            cv_.wait_until(lock, timers_.begin()->first);
            continue;
        }

        // The earliest deadline is due (or its owner is gone). Pop one node per
        // lock and resume it outside, so a resumed coroutine that abandons a
        // sibling can claim that sibling before this loop reaches it.
        auto it   = timers_.begin();
        auto node = it->second.lock();
        timers_.erase(it);

        std::coroutine_handle<> continuation{};
        if (node && !node->settled)
        {
            node->settled = true;
            continuation  = node->continuation;
        }

        if (continuation)
        {
            lock.unlock();
            continuation.resume(); // Never hold the mutex across a resume.
            lock.lock();
        }
    }
}

/**
 * @brief Awaiter that resumes a coroutine after a duration, or on cancellation.
 *
 * The wait is one deadline in the shared TimerService: no thread per sleep, no
 * polling, and a cancelled token wakes the coroutine at once (on the timer
 * thread). await_resume then throws TaskCancelledException. Destroying the
 * awaiting frame before the deadline abandons the wait, so the timer thread
 * never resumes a destroyed frame.
 */
class SleepAwaiter
{
  public:
    SleepAwaiter(std::chrono::milliseconds duration, CancellationToken token)
      : node_(std::make_shared<TimerNode>()), duration_(duration), token_(std::move(token))
    {}
    SleepAwaiter(const SleepAwaiter&) = delete;
    SleepAwaiter& operator=(const SleepAwaiter&) = delete;

    /**
     * @brief Abandons the wait when the awaiting frame is destroyed.
     *
     * The token registration is dropped first, so no callback can be in flight
     * (or fire) once the node is claimed; the timer thread either observes the
     * claim while the node is still queued and never resumes it, or it already
     * passed the resume itself, which is the framework contract in
     * async_global.hpp (no concurrent destroy during a resume).
     */
    ~SleepAwaiter()
    {
        abort_.reset();
        if (scheduled_)
        {
            TimerService::instance().cancel(node_);
        }
    }

    /**
     * @brief Returns whether the sleep completes without suspending.
     *
     * @return true for zero/negative durations or an already-cancelled token.
     */
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return duration_.count() <= 0 || token_.stop_requested();
    }

    /**
     * @brief Arms the shared timer and suspends.
     *
     * @param h Coroutine to resume at the deadline (or on cancellation).
     * @return true to suspend; false when the token was cancelled while arming,
     * in which case the wait was never scheduled and await_resume throws.
     */
    bool await_suspend(std::coroutine_handle<> h)
    {
        node_->deadline     = std::chrono::steady_clock::now() + duration_;
        node_->continuation = h;

        if (token_.stop_possible())
        {
            // May invoke the callback synchronously when the token is already
            // stopped. requestAbort() then only marks the node, so nothing is
            // resumed while we are still inside await_suspend.
            abort_.emplace(token_, AbortRequest{ node_ });
        }

        // Publish last: install() hands the node to the timer thread, and the
        // destructor (which may run on that thread, at the deadline) reads this
        // flag, so it has to be written before the node becomes visible.
        scheduled_ = true;
        if (!TimerService::instance().install(node_))
        {
            scheduled_ = false; // Never published, so there is nothing to cancel.
            return false;       // Cancelled while arming: no wait, await_resume throws.
        }
        return true;
    }

    /**
     * @brief Throws when the sleep was cut short by cancellation.
     */
    void await_resume()
    {
        if (token_.stop_requested())
        {
            throw TaskCancelledException{};
        }
    }

  private:
    /// Stop-callback body: asks the service to wake this wait early.
    struct AbortRequest
    {
        std::shared_ptr<TimerNode> node;

        void operator()() const noexcept { TimerService::instance().requestAbort(node); }
    };

    /// This wait's identity in the timer service; the awaiter owns it.
    std::shared_ptr<TimerNode> node_;

    std::chrono::milliseconds duration_;
    CancellationToken         token_;

    /// Token registration; declared after node_ so it dies before it.
    std::optional<std::stop_callback<AbortRequest>> abort_;

    /// true once the node is in the deadline queue, i.e. only then is cancel() needed.
    bool scheduled_{ false };
};

} // namespace detail

/**
 * @brief Suspends the coroutine for the given duration.
 *
 * The wake-up is a deadline in the process-wide timer service, so a sleep
 * costs one timer entry rather than one OS thread, and one wake-up rather than
 * one per polling slice. Cancelling the token resumes the coroutine
 * immediately and await_resume throws TaskCancelledException; destroying the
 * awaiting task before the deadline abandons the wait safely.
 *
 * @param duration Sleep duration; zero or negative returns immediately.
 * @param token Optional cancellation token.
 * @return A task that completes after the duration.
 */
[[nodiscard]]
inline Task<void> sleepFor(std::chrono::milliseconds duration, CancellationToken token = {})
{
    co_await detail::SleepAwaiter{ duration, std::move(token) };
}

V_ASYNC_NS_END
