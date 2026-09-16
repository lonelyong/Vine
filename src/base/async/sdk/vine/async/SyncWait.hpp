#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <exception>
#include <semaphore>
#include <type_traits>
#include <utility>

#include "Task.hpp"

V_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Blocking handshake between the driven task and syncWait.
 *
 * syncWait drives the task on the calling thread; when the task completes, its
 * final awaiter releases the semaphore so syncWait stops blocking and returns
 * the produced result.
 */
struct SyncWaitEvent
{
    std::binary_semaphore semaphore{ 0 };

    void set() noexcept { semaphore.release(); }

    void wait() noexcept { semaphore.acquire(); }
};

/**
 * @brief Helper coroutine that awaits a task and notifies a waiter on completion.
 */
template<typename T>
class SyncWaitTask
{
  public:
    struct promise_type : detail::TaskPromiseReturn<T>
    {
        [[nodiscard]]
        SyncWaitTask get_return_object() noexcept
        {
            return SyncWaitTask{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            [[nodiscard]]
            bool await_ready() const noexcept
            {
                return false;
            }

            void await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                if (h.promise().event)
                {
                    h.promise().event->set();
                }
            }

            void await_resume() const noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() noexcept { exception = std::current_exception(); }

        std::exception_ptr exception{};
        SyncWaitEvent* event{ nullptr };
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit SyncWaitTask(handle_type h) noexcept : handle_(h) {}

    ~SyncWaitTask()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    SyncWaitTask(const SyncWaitTask&) = delete;
    SyncWaitTask& operator=(const SyncWaitTask&) = delete;
    SyncWaitTask(SyncWaitTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    SyncWaitTask& operator=(SyncWaitTask&&) = delete;

    handle_type handle_;
};

/**
 * @brief Helper coroutine that awaits a task and notifies syncWait.
 *
 * syncWait resumes this coroutine on the calling thread; when the wrapped task
 * has produced its result, this coroutine's final awaiter signals the
 * SyncWaitEvent so syncWait unblocks and returns the value.
 */
template<typename T>
SyncWaitTask<T> makeSyncWaitTask(Task<T>&& task)
{
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

} // namespace detail

/**
 * @brief Blocks the calling thread until the task completes and returns its result.
 *
 * The task is driven to completion by a helper coroutine resumed on the calling
 * thread. Do not call this on a thread whose event loop is required to resume
 * the task, or it will deadlock.
 *
 * @tparam T Result type of the task.
 * @param task Task to run to completion; must be non-empty.
 * @return The task's result.
 */
template<typename T>
T syncWait(Task<T>&& task)
{
    detail::SyncWaitEvent event;

    auto syncTask = detail::makeSyncWaitTask(std::move(task));
    syncTask.handle_.promise().event = &event;

    syncTask.handle_.resume();
    event.wait();

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

V_ASYNC_NS_END
