#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "Task.hpp"
#include "WaiterList.hpp"

VN_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Shared completion state of a TaskCompletionSource.
 *
 * Lives behind a shared_ptr so a task obtained from task() keeps the state
 * alive even after the source itself is destroyed. The value is moved out by
 * the first awaiter; with a move-only T await the returned task only once.
 */
template<typename T>
struct TcsState
{
    std::mutex mutex;
    bool completed{ false };
    std::optional<T> value{};
    std::exception_ptr exception{};
    std::vector<std::coroutine_handle<>> waiters;
};

/**
 * @brief Shared completion state of a void TaskCompletionSource.
 */
template<>
struct TcsState<void>
{
    std::mutex mutex;
    bool completed{ false };
    std::exception_ptr exception{};
    std::vector<std::coroutine_handle<>> waiters;
};

/**
 * @brief Awaiter that suspends until the TaskCompletionSource completes.
 *
 * Registers itself in the shared waiter list on suspension and unregisters on
 * destruction, so a completed-but-abandoned await never resumes a destroyed
 * coroutine (mirrors AsyncEvent's safety).
 */
template<typename T>
class TcsAwaiter
{
  public:
    explicit TcsAwaiter(std::shared_ptr<TcsState<T>> state) noexcept : state_(std::move(state)) {}

    TcsAwaiter(const TcsAwaiter&) = delete;
    TcsAwaiter& operator=(const TcsAwaiter&) = delete;

    ~TcsAwaiter() noexcept
    {
        if (handle_)
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            std::erase(state_->waiters, handle_);
        }
    }

    /**
     * @brief Returns whether the source is already completed.
     *
     * @return true when the result is available without suspending.
     */
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->completed;
    }

    /**
     * @brief Parks the coroutine until the source completes.
     *
     * @return false to resume inline when the source completed concurrently.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->completed)
        {
            return false; // Already completed; do not suspend.
        }
        return detail::tryRegisterWaiter(state_->waiters, h, failure_);
    }

    /**
     * @brief Yields the completed value or rethrows the recorded exception.
     *
     * @return The value of co_await (or void for a void source).
     */
    decltype(auto) await_resume()
    {
        if (failure_)
        {
            std::rethrow_exception(std::exchange(failure_, nullptr));
        }
        if (state_->exception)
        {
            std::rethrow_exception(state_->exception);
        }
        if constexpr (!std::is_void_v<T>)
        {
            return std::move(*state_->value);
        }
    }

  private:
    std::shared_ptr<TcsState<T>> state_;
    std::coroutine_handle<> handle_{};

    /// Why the waiter could not be registered; rethrown by await_resume().
    std::exception_ptr failure_{};
};

/**
 * @brief Resumes every registered waiter, popping one per lock.
 *
 * Waiters are popped from the shared list and resumed one at a time, never
 * holding a handle across a resume: a resumed waiter whose completion
 * destroys a still-suspended sibling (e.g. a whenAny winner) unregisters it
 * from the shared list, so the next iteration simply does not find it.
 */
template<typename T>
inline void resumeWaitersOneByOne(const std::shared_ptr<TcsState<T>>& state) noexcept
{
    for (;;)
    {
        std::coroutine_handle<> h;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->waiters.empty())
            {
                break;
            }
            h = state->waiters.back();
            state->waiters.pop_back();
        }
        h.resume(); // Resume outside the lock.
    }
}

} // namespace detail

/**
 * @brief Manually-completed task; the bridge to callback-driven APIs.
 *
 * A TaskCompletionSource lets non-coroutine code complete a Task: keep the
 * returned task() somewhere, then call setResult()/setException() from a
 * callback, an event handler, a worker thread, or a third-party async API.
 * The source may complete before the task is awaited, in which case awaiting
 * it yields the value immediately. Multiple tasks can be obtained from one
 * source and all complete together.
 *
 * setResult()/setException() throw std::logic_error on double completion;
 * trySetResult()/trySetException() return false instead. setException() takes
 * either a std::exception_ptr or any exception object.
 *
 * @tparam T Result type; void for a result-less source.
  *
 * Threading: the internal state is mutex-guarded, so a completion may arrive from any thread - a
 * callback, an event handler, a worker - and concurrent awaits are supported. Waiters are
 * resumed on the thread that completes the source, and the source must outlive them.
 */
template<typename T>
class TaskCompletionSource
{
  public:
    TaskCompletionSource() = default;

    TaskCompletionSource(const TaskCompletionSource&) = delete;
    TaskCompletionSource& operator=(const TaskCompletionSource&) = delete;

    /**
     * @brief Returns whether the source has been completed.
     *
     * @return true if a result or exception was set.
     */
    [[nodiscard]]
    bool isCompleted() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->completed;
    }

    /**
     * @brief Completes the source with a value.
     *
     * @param value Result delivered to every awaiting task.
     */
    void setResult(T value)
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                throw std::logic_error("async::TaskCompletionSource: already completed");
            }
            impl_->value.emplace(std::move(value));
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
    }

    /**
     * @brief Completes the source unless it is already completed.
     *
     * @param value Result delivered to every awaiting task.
     * @return true if this call completed the source.
     */
[[nodiscard]]
    bool trySetResult(T value)
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                return false;
            }
            impl_->value.emplace(std::move(value));
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
        return true;
    }

    /**
     * @brief Completes the source with a failure.
     *
     * @param exception Exception to rethrow from every awaiting task.
     */
    void setException(std::exception_ptr exception)
    {
        if (!exception)
        {
            throw std::invalid_argument("async::TaskCompletionSource: null exception");
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                throw std::logic_error("async::TaskCompletionSource: already completed");
            }
            impl_->exception = std::move(exception);
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
    }

    /**
     * @brief Completes the source with a failure.
     *
     * @tparam E Exception type.
     * @param exception Exception object to copy into the source.
     */
    template<typename E>
    void setException(const E& exception)
    {
        setException(std::make_exception_ptr(exception));
    }

    /**
     * @brief Completes the source with a failure unless already completed.
     *
     * @param exception Exception to rethrow from every awaiting task.
     * @return true if this call completed the source.
     */
[[nodiscard]]
    bool trySetException(std::exception_ptr exception)
    {
        if (!exception)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                return false;
            }
            impl_->exception = std::move(exception);
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
        return true;
    }

    /**
     * @brief Completes the source with a failure unless already completed.
     *
     * @tparam E Exception type.
     * @param exception Exception object to copy into the source.
     * @return true if this call completed the source.
     */
    template<typename E>
[[nodiscard]]
    bool trySetException(const E& exception)
    {
        return trySetException(std::make_exception_ptr(exception));
    }

    /**
     * @brief Returns a task that completes when the source completes.
     *
     * Multiple calls return independent tasks; all of them complete when the
     * source is completed.
     *
     * @return A task yielding the source's value or rethrowing its exception.
     */
    [[nodiscard]]
    Task<T> task()
    {
        if constexpr (std::is_void_v<T>)
        {
            co_await detail::TcsAwaiter<T>{ impl_ };
            co_return;
        }
        else
        {
            co_return co_await detail::TcsAwaiter<T>{ impl_ };
        }
    }

  private:
    std::shared_ptr<detail::TcsState<T>> impl_{ std::make_shared<detail::TcsState<T>>() };
};

/**
 * @brief TaskCompletionSource specialization for void results.
 */
template<>
class TaskCompletionSource<void>
{
  public:
    TaskCompletionSource() = default;

    TaskCompletionSource(const TaskCompletionSource&) = delete;
    TaskCompletionSource& operator=(const TaskCompletionSource&) = delete;

    /**
     * @brief Returns whether the source has been completed.
     *
     * @return true if a result or exception was set.
     */
    [[nodiscard]]
    bool isCompleted() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->completed;
    }

    /**
     * @brief Completes the source.
     */
    void setResult()
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                throw std::logic_error("async::TaskCompletionSource: already completed");
            }
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
    }

    /**
     * @brief Completes the source unless it is already completed.
     *
     * @return true if this call completed the source.
     */
[[nodiscard]]
    bool trySetResult()
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                return false;
            }
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
        return true;
    }

    /**
     * @brief Completes the source with a failure.
     *
     * @param exception Exception to rethrow from every awaiting task.
     */
    void setException(std::exception_ptr exception)
    {
        if (!exception)
        {
            throw std::invalid_argument("async::TaskCompletionSource: null exception");
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                throw std::logic_error("async::TaskCompletionSource: already completed");
            }
            impl_->exception = std::move(exception);
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
    }

    /**
     * @brief Completes the source with a failure.
     *
     * @tparam E Exception type.
     * @param exception Exception object to copy into the source.
     */
    template<typename E>
    void setException(const E& exception)
    {
        setException(std::make_exception_ptr(exception));
    }

    /**
     * @brief Completes the source with a failure unless already completed.
     *
     * @param exception Exception to rethrow from every awaiting task.
     * @return true if this call completed the source.
     */
[[nodiscard]]
    bool trySetException(std::exception_ptr exception)
    {
        if (!exception)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->completed)
            {
                return false;
            }
            impl_->exception = std::move(exception);
            impl_->completed = true;
        }
        detail::resumeWaitersOneByOne(impl_);
        return true;
    }

    /**
     * @brief Completes the source with a failure unless already completed.
     *
     * @tparam E Exception type.
     * @param exception Exception object to copy into the source.
     * @return true if this call completed the source.
     */
    template<typename E>
[[nodiscard]]
    bool trySetException(const E& exception)
    {
        return trySetException(std::make_exception_ptr(exception));
    }

    /**
     * @brief Returns a task that completes when the source completes.
     *
     * @return A task that completes when setResult() or setException() is called.
     */
    [[nodiscard]]
    Task<void> task()
    {
        co_await detail::TcsAwaiter<void>{ impl_ };
        co_return;
    }

  private:
    std::shared_ptr<detail::TcsState<void>> impl_{ std::make_shared<detail::TcsState<void>>() };
};

VN_ASYNC_NS_END
