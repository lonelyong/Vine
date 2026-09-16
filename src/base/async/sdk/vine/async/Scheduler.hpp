#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <type_traits>
#include <utility>

#include "Concepts.hpp"
#include "Task.hpp"

V_ASYNC_NS_BEGIN

/**
 * @brief Resumes the continuation on the given scheduler.
 *
 * Equivalent to co_await scheduler.schedule(); provided for a descriptive name
 * aligned with cppcoro's resume_on.
 *
 * @tparam S Scheduler type.
 * @param s Scheduler deciding the resume context.
 * @return An awaitable that suspends and resumes via the scheduler.
 */
template<Schedulable S>
auto resumeOn(S& s) -> decltype(s.schedule())
{
    return s.schedule();
}

/**
 * @brief Starts a task on the given scheduler.
 *
 * The coroutine first hops to the scheduler's context, then runs the task.
 *
 * @tparam S Scheduler type.
 * @tparam T Result type of the task.
 * @param s Scheduler deciding where the task starts; must outlive the task.
 * @param task Task to run.
 * @return A task that runs task after scheduling onto s.
 */
template<Schedulable S, typename T>
[[nodiscard]]
Task<T> scheduleOn(S& s, Task<T> task)
{
    co_await s.schedule(); // Hop to the scheduler's context before running the task.
    if constexpr (std::is_void_v<T>)
    {
        co_await std::move(task);
        co_return;
    }
    else
    {
        co_return co_await std::move(task);
    }
}

/**
 * @brief Scheduler that resumes a coroutine inline without suspending.
 *
 * schedule() returns an awaiter whose await_ready() is always true, so
 * co_await schedule() never suspends and the coroutine keeps running on the
 * same thread. Useful as the default when no context switch is required.
 */
class InlineScheduler
{
  public:
    /**
     * @brief Awaiter that never suspends.
     *
     * await_ready() returning true makes the coroutine skip suspension and
     * continue inline on the calling thread.
     */
    class ScheduleAwaiter
    {
      public:
        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return true; // Never suspends; continue inline.
        }

        void await_suspend(std::coroutine_handle<>) const noexcept {}

        void await_resume() const noexcept {}
    };

    /**
     * @brief Returns an awaiter that resumes inline (never suspends).
     *
     * @return An awaiter whose await_ready() is true.
     */
    [[nodiscard]]
    ScheduleAwaiter schedule() const noexcept
    {
        return {};
    }
};

V_ASYNC_NS_END
