#pragma once

#include "async_global.hpp"

#include <functional>
#include <type_traits>

#include <vine/ThreadPool.hpp>

#include "Concepts.hpp"
#include "Task.hpp"

V_ASYNC_NS_BEGIN

/**
 * @brief Scheduler that resumes coroutines on a vine::ThreadPool.
 *
 * co_await scheduler.schedule() (or resumeOn(scheduler)) hops the coroutine
 * onto a worker thread of the bound pool. The pool must outlive every
 * coroutine scheduled on it.
 */
class ThreadPoolScheduler
{
  public:
    explicit ThreadPoolScheduler(vine::ThreadPool& pool) noexcept : pool_(&pool) {}

    ThreadPoolScheduler(const ThreadPoolScheduler&) = delete;
    ThreadPoolScheduler& operator=(const ThreadPoolScheduler&) = delete;

    /**
     * @brief Awaiter that schedules the continuation onto a pool worker.
     */
    class ScheduleAwaiter
    {
      public:
        explicit ScheduleAwaiter(vine::ThreadPool& pool) noexcept : pool_(pool) {}

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> h)
        {
            pool_.enqueue([h]() mutable { h.resume(); });
        }

        void await_resume() const noexcept {}

      private:
        vine::ThreadPool& pool_;
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
    vine::ThreadPool* pool_{ nullptr };
};

/**
 * @brief Runs a callable on a thread pool; the C# Task.Run equivalent.
 *
 * The returned task, when awaited, hops to a pool worker thread, invokes the
 * callable there, and yields its result (or void). Arguments are captured by
 * value into the task frame. Exceptions thrown by the callable propagate out
 * of the returned task.
 *
 * @tparam F Callable type.
 * @tparam Args Argument types.
 * @param pool Thread pool to run on.
 * @param f Callable to invoke on a worker thread.
 * @param args Arguments passed to f (by value).
 * @return A task that completes with the callable's result.
 */
template<typename F, typename... Args>
[[nodiscard]]
Task<std::invoke_result_t<F, Args...>> runOn(vine::ThreadPool& pool, F f, Args... args)
{
    // Hop to a pool worker before invoking the callable.
    co_await ThreadPoolScheduler{ pool }.schedule();

    if constexpr (std::is_void_v<std::invoke_result_t<F, Args...>>)
    {
        std::invoke(f, args...);
        co_return;
    }
    else
    {
        co_return std::invoke(f, args...);
    }
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
template<typename F, typename... Args>
[[nodiscard]]
Task<std::invoke_result_t<F, Args...>> run(F f, Args... args)
{
    co_return co_await runOn(vine::ThreadPool::defaultPool(), std::move(f), std::move(args)...);
}

V_ASYNC_NS_END
