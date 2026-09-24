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

#include "Concepts.hpp"
#include "DetachedTask.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Cached result storage; empty for a void shared task.
 */
template<typename T>
struct SharedValue
{
    std::optional<T> value{};
};

template<>
struct SharedValue<void>
{
};

/**
 * @brief Shared state of a SharedTask: the source task plus the cached outcome.
 *
 * Lives behind a shared_ptr so copies of a SharedTask share one computation
 * and one cached result. The first awaiter takes ownership of the source task
 * and drives it to completion (runShared); later awaiters just read the cache.
 *
 * State machine:
 *   NotStarted: source set, started == false
 *     -> first co_await claims ownership -> Running
 *   Running: started == true, source moved out
 *     -> source completes -> Completed
 *   Completed: completed == true, result or exception set; awaiters read
 *   the cache directly.
 */
template<typename T>
struct SharedTaskState
{
    std::mutex mutex;
    std::optional<Task<T>> source{};
    bool started{ false };
    bool completed{ false };
    SharedValue<T> result{};
    std::exception_ptr exception{};
    std::vector<std::coroutine_handle<>> waiters;
};

/**
 * @brief Drives a SharedTask's source task once and publishes the outcome.
 *
 * Runs as a DetachedTask so it survives its own suspensions. On completion it
 * caches the result (or exception) in the shared state and resumes every
 * waiter. The caller must not hold the state mutex when spawning this.
 *
 * Waiters are popped and resumed one at a time, never holding a handle across
 * a resume: a resumed waiter whose completion destroys a still-suspended
 * sibling (e.g. a whenAny winner destroying the losers) unregisters itself, so
 * the next iteration simply does not find it. Swapping the whole list out
 * first would resume a destroyed frame instead.
 */
template<typename T>
DetachedTask runShared(std::shared_ptr<SharedTaskState<T>> state)
{
    Task<T> source;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        source = std::move(*state->source);
        state->source.reset();
    }

    try
    {
        if constexpr (std::is_void_v<T>)
        {
            co_await std::move(source);
        }
        else
        {
            std::optional<T> value;
            value.emplace(co_await std::move(source));
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result.value = std::move(value);
        }
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->exception = std::current_exception();
    }

    for (;;)
    {
        std::coroutine_handle<> h;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            // Published before the first resume: a waiter resumed below may
            // re-enter (await the SharedTask again) and must see it completed.
            state->completed = true;
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

/**
 * @brief Awaiter that awaits a SharedTask; unregisters on destruction.
 */
template<typename T>
class SharedTaskAwaiter
{
  public:
    explicit SharedTaskAwaiter(std::shared_ptr<SharedTaskState<T>> state) noexcept
      : state_(std::move(state))
    {}

    SharedTaskAwaiter(const SharedTaskAwaiter&) = delete;
    SharedTaskAwaiter& operator=(const SharedTaskAwaiter&) = delete;

    ~SharedTaskAwaiter()
    {
        if (handle_)
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            std::erase(state_->waiters, handle_);
        }
    }

    /**
     * @brief Returns whether the shared task is already complete.
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
     * @brief Starts the source on the first await, then suspends until done.
     *
     * The source is spawned before this awaiter registers itself, so a source
     * that completes synchronously never resumes the still-suspending
     * coroutine (no re-entrancy).
     *
     * @return false to resume inline when already completed.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        bool should_start = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->completed)
            {
                return false;
            }
            if (!state_->started)
            {
                state_->started = true;
                should_start = true;
            }
        }
        if (should_start)
        {
            detail::runShared(state_);
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->completed)
            {
                return false; // Completed during spawn; resume inline.
            }
            state_->waiters.push_back(h);
        }
        return true;
    }

    /**
     * @brief Yields the cached result or rethrows the recorded exception.
     *
     * @return The shared result (const reference), or void for a void task.
     */
    decltype(auto) await_resume()
    {
        if (state_->exception)
        {
            std::rethrow_exception(state_->exception);
        }
        if constexpr (!std::is_void_v<T>)
        {
            return std::as_const(*state_->result.value);
        }
    }

  private:
    std::shared_ptr<SharedTaskState<T>> state_;
    std::coroutine_handle<> handle_{};
};

} // namespace detail

/**
 * @brief Lazy, copyable task whose result is cached and shared; C# Task-like.
 *
 * A SharedTask wraps a source Task that runs exactly once, on the first
 * co_await; every awaiter receives the same cached result (or rethrows the
 * same exception). It is copyable, so multiple places can hold and await the
 * same computation, and after completion result()/isReady() give non-coroutine
 * access. The value type only needs to be movable: the cached value is exposed
 * as a const reference and is never copied by the library.
 *
 * Lifecycle: awaiting coroutines unregister themselves on destruction, so an
 * abandoned await is safe; but a coroutine suspended here must not be destroyed
 * concurrently by another thread while the source is completing (see the
 * framework contract in async_global.hpp).
 *
 * @tparam T Result type; void for a result-less shared task. Must be storable
 *           (see StorableValue).
 */
template<StorableValue T>
class SharedTask
{
  public:
    using value_type = T;

    SharedTask() noexcept = default;

    SharedTask(const SharedTask&) = default;
    SharedTask& operator=(const SharedTask&) = default;

    /**
     * @brief Returns whether the source has completed.
     *
     * @return true once result() may be called; false for an empty task.
     */
    [[nodiscard]]
    bool isReady() const noexcept
    {
        if (!state_)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->completed;
    }

    /**
     * @brief Returns whether the task is non-empty.
     *
     * @return true if the SharedTask was created from a source task.
     */
    [[nodiscard]]
    explicit operator bool() const noexcept
    {
        return static_cast<bool>(state_);
    }

    /**
     * @brief Returns the cached result; call only when ready.
     *
     * @return The shared result (const reference).
     */
    [[nodiscard]]
    decltype(auto) result() const requires (!std::is_void_v<T>)
    {
        if (!state_)
        {
            throw std::logic_error("async::SharedTask: accessing an empty task");
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->exception)
        {
            std::rethrow_exception(state_->exception);
        }
        return std::as_const(*state_->result.value);
    }

    /**
     * @brief Awaits the shared computation; yields the cached result.
     */
    [[nodiscard]]
    detail::SharedTaskAwaiter<T> operator co_await() const
    {
        if (!state_)
        {
            throw std::logic_error("async::SharedTask: awaiting an empty task");
        }
        return detail::SharedTaskAwaiter<T>{ state_ };
    }

  private:
    template<StorableValue U>
    friend SharedTask<U> sharedTask(Task<U>);

    explicit SharedTask(std::shared_ptr<detail::SharedTaskState<T>> state) noexcept
      : state_(std::move(state))
    {}

    std::shared_ptr<detail::SharedTaskState<T>> state_;
};

/**
 * @brief Wraps a lazy task into a shared, repeatedly-awaitable task.
 *
 * The source task is moved in and runs exactly once, on the first co_await of
 * any copy of the returned SharedTask. All awaiters share the cached result.
 *
 * @tparam T Result type of the task.
 * @param task Source task; must be non-empty.
 * @return A copyable SharedTask sharing the single computation.
 */
template<StorableValue T>
[[nodiscard]]
SharedTask<T> sharedTask(Task<T> task)
{
    auto state = std::make_shared<detail::SharedTaskState<T>>();
    state->source.emplace(std::move(task));
    return SharedTask<T>{ std::move(state) };
}

VN_ASYNC_NS_END
