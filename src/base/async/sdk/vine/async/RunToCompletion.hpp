#pragma once

#include "async_global.hpp"

#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>

#include "Task.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Blocks the calling thread until the task completes, calling pump() while it waits.
 *
 * The blocking drive for a task that cannot finish on its own: it may suspend on
 * something that only moves if this thread keeps another mechanism running - an
 * event loop that has to deliver a queued call, a foreign queue, a device that
 * is driven by polling. Task::result(), the member spelling of the same block, is
 * the same drive without that: it only waits, so such a task never completes and
 * the call deadlocks. That failure mode has a familiar shape outside C++ (taking
 * `.Result` on a task whose continuation needs the UI thread), and so does the
 * fix: run the other mechanism while waiting.
 *
 * The task is started here, on the calling thread, and runs up to its first
 * suspension - the same start the other blocking drives give, so work still begins where the caller
 * needs it to begin. Every slice between two waits runs pump() once; the wait
 * itself ends as soon as the task completes, so the slice is an upper bound,
 * not a delay.
 *
 * What pump() runs is the caller's business, and it is the only thing that
 * happens here: whatever the pump does not run (timers, paint, input) does not
 * happen. It should be quick and non-blocking. On a thread that owns a loop the
 * task needs, one must pump that loop's queue rather than the loop itself - a
 * pump that turns the loop re-entrantly can run unrelated work at a point the
 * caller is not prepared for.
 *
 * @tparam T    Result type of the task.
 * @tparam Pump Callable invoked while waiting; takes no arguments.
 * @param task  Task to run to completion; must be non-empty.
 * @param pump  Callable to run between waits, so the task can make progress.
 * @return The task's result.
 */
template<typename T, typename Pump>
T runToCompletion(Task<T>&& task, Pump pump)
{
    detail::WaitEvent event;

    auto syncTask = detail::makeWaitTask(std::move(task));
    syncTask.handle_.promise().event = &event;

    syncTask.handle_.resume();
    while (!event.waitFor(std::chrono::microseconds(200)))
    {
        pump();
    }

    auto& promise = syncTask.handle_.promise();
    if (promise.exception)
    {
        std::rethrow_exception(std::move(promise.exception));
    }

    if constexpr (!std::is_void_v<T>)
    {
        return std::move(promise.value).value();
    }
}

VN_ASYNC_NS_END
