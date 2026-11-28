#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include <vine/ThreadPool.hpp>

#include "Concepts.hpp"
#include "Task.hpp"
#include "TaskCompletionSource.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Scheduler that resumes coroutines on a vn::ThreadPool.
 *
 * co_await scheduler.schedule() (or resumeOn(scheduler)) hops the coroutine
 * onto a worker thread of the bound pool. The pool must outlive every
 * coroutine scheduled on it.
 *
 * A stopped pool cannot honour a hop, and the pool's std::logic_error reaches the awaiting coroutine
 * instead of a silent choice: resuming inline would move the coroutine onto a thread the caller did not
 * ask for, and returning without scheduling would park it forever.
 */
class ThreadPoolScheduler {
  public:
    explicit ThreadPoolScheduler(vn::ThreadPool& pool) noexcept
      : pool_(&pool)
    {}

    ThreadPoolScheduler(const ThreadPoolScheduler&)            = delete;
    ThreadPoolScheduler& operator=(const ThreadPoolScheduler&) = delete;

    /**
     * @brief Awaiter that schedules the continuation onto a pool worker.
     *
     * @details
     * - The queued resume is a **claim**, not a bare handle: a worker takes the handle out of the
     *   claim under its lock, so a hop abandoned while it was still queued resumes nothing.
     *   `~ScheduleAwaiter` withdraws it, which is what keeps a `Destroy` composition that lost its
     *   race from leaving a resume of a freed frame in the pool's queue.
     * - One window is left, and it is the framework contract of `async_global.hpp`: a frame is not
     *   destroyed while the thread that took its handle is resuming it.
     */
    class ScheduleAwaiter {
      public:
        explicit ScheduleAwaiter(vn::ThreadPool& pool) noexcept
          : pool_(pool)
        {}

        ScheduleAwaiter(const ScheduleAwaiter&)            = delete;
        ScheduleAwaiter& operator=(const ScheduleAwaiter&) = delete;

        /**
         * @brief Withdraws a hop no worker has taken yet, so it never resumes a destroyed frame.
         *
         * @details
         * - Runs when the awaiting coroutine is destroyed before its queued resume ran: a `Destroy`
         *   composition that lost the race, a cancelled scope, a shutting-down caller.
         * - The handle is cleared under the claim's lock, so a worker that arrives later finds
         *   nothing and resumes nothing. A worker that already took the handle is inside the
         *   resume, which is the one case `async_global.hpp` keeps out of this destructor's reach.
         */
        ~ScheduleAwaiter()
        {
            if (claim_) {
                std::lock_guard<std::mutex> lock(claim_->mutex);
                claim_->abandoned = true;
                claim_->waiter    = {};
            }
        }

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return false;
        }

        /**
         * @brief Queues the continuation on the pool; the pool's refusal propagates.
         *
         * @param h Coroutine to resume on a worker thread.
         * @throws std::logic_error when the pool is stopped; the coroutine is not suspended.
         */
        void await_suspend(std::coroutine_handle<> h)
        {
            auto claim     = std::make_shared<Claim>();
            claim->waiter  = h;
            // The refusal is the pool's to report (ThreadPool::enqueue throws std::logic_error) and it propagates here:
            // the awaiter's promise is the thread it lands on, so resuming elsewhere or never resuming are both worse.
            (void)pool_.enqueue([claim]() {
                std::coroutine_handle<> resume{};
                {
                    std::lock_guard<std::mutex> lock(claim->mutex);
                    if (!claim->abandoned) {
                        resume = std::exchange(claim->waiter, {});
                    }
                }
                if (resume) {
                    resume.resume(); // Never hold the claim's lock across the resume.
                }
            });
            claim_ = std::move(claim);
        }

        /// Drops the claim: the hop landed, so nothing is left to withdraw.
        void await_resume() noexcept
        {
            claim_.reset();
        }

      private:
        /// One queued resume: who to resume, and whether the awaiter took it back.
        struct Claim {
            std::mutex              mutex;
            std::coroutine_handle<> waiter{};
            bool                    abandoned{ false };
        };

        vn::ThreadPool& pool_;

        /// The claim of the hop in flight; empty before `await_suspend` and after the resume.
        std::shared_ptr<Claim> claim_;
    };

    /**
     * @brief Returns an awaiter that resumes on a pool worker thread.
     *
     * @return An awaiter that schedules the continuation onto the pool.
     */
    [[nodiscard]]
    ScheduleAwaiter schedule() noexcept
    {
        return ScheduleAwaiter{ *pool_ };
    }

  private:
    vn::ThreadPool* pool_{ nullptr };
};

/**
 * @brief Runs a callable on a thread pool; the C# Task.Run equivalent.
 *
 * @details
 * - The callable runs on a worker and the returned task completes from it: the awaiting coroutine is
 *   parked on the pool's completion for the whole call, so a composition that owns this task owns a
 *   **suspended** frame even while the callable runs. That is what makes `CancelPolicy::Destroy`
 *   bounded here: abandoning the task frees the frame and drops the value, and the callable finishes
 *   on its own thread (see `async-design.md` 2.10).
 * - The work is queued when the task first runs, so a lazy task that is never awaited enqueues
 *   nothing.
 * - Arguments are moved into the queued work, so a move-only argument (std::unique_ptr, for
 *   example) works as long as the callable takes it by value.
 * - Exceptions thrown by the callable, and the pool's refusal to take the work, both surface as this
 *   task's error.
 *
 * @tparam F Callable type.
 * @tparam Args Argument types.
 * @param pool Thread pool to run on.
 * @param f Callable to invoke on a worker thread.
 * @param args Arguments passed to f (by value).
 * @return A task that completes with the callable's result.
 */
template <typename F, typename... Args>
[[nodiscard]]
Task<std::invoke_result_t<F, Args...>> runOn(vn::ThreadPool& pool, F f, Args... args)
{
    using Result = std::invoke_result_t<F, Args...>;

    auto source = std::make_shared<TaskCompletionSource<Result>>();
    // The callable and its arguments travel as one heap block so the queued closure is a copyable,
    // const-callable lambda: ThreadPool::enqueue wraps what it is given in std::bind, whose call
    // operator may be const, and a mutable capture would not survive that wrapper.
    auto work = std::make_shared<std::tuple<F, Args...>>(std::move(f), std::move(args)...);

    // A refusal (a stopped pool) is thrown on the coroutine's own stack, so it becomes this task's
    // error exactly as it did when the hop reported it (design 2.7).
    (void)pool.enqueue([source, work]() {
        try {
            if constexpr (std::is_void_v<Result>) {
                std::apply([](F& callable, Args&... arguments) { std::invoke(std::move(callable), std::move(arguments)...); }, *work);
                source->setResult();
            }
            else {
                Result value = std::apply([](F& callable, Args&... arguments) -> Result { return std::invoke(std::move(callable), std::move(arguments)...); },
                                          *work);
                source->setResult(std::move(value));
            }
        }
        catch (...) {
            source->setException(std::current_exception());
        }
    });

    co_return co_await source->task();
}

/**
 * @brief Runs a callable on the default thread pool; Task.Run equivalent.
 *
 * Same semantics as runOn() but uses ThreadPool::defaultPool(), so no pool
 * argument is needed. The default pool is process-wide and must outlive every
 * task scheduled on it; call ThreadPool::defaultPool() or configure it before
 * dispatching if it has not been set up.
 *
 * @tparam F Callable type.
 * @tparam Args Argument types.
 * @param f Callable to invoke on a default-pool worker.
 * @param args Arguments passed to f (by value).
 * @return A task that completes with the callable's result.
 */
template <typename F, typename... Args>
[[nodiscard]]
Task<std::invoke_result_t<F, Args...>> run(F f, Args... args)
{
    co_return co_await runOn(vn::ThreadPool::defaultPool(), std::move(f), std::move(args)...);
}

VN_ASYNC_NS_END
