#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

#include <vine/CancellationToken.hpp>

#include "AsyncEvent.hpp"
#include "Cancellation.hpp"
#include "DetachedTask.hpp"
#include "Sleep.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Shared state of a Scope: pending children, first failure, done event.
 *
 * Lives behind a shared_ptr so children (self-owned coroutines) keep it alive
 * even after the Scope object itself is destroyed.
 */
struct ScopeState
{
    std::mutex mutex;
    std::size_t pending{ 0 };
    std::exception_ptr first_exception{};
    AsyncEvent done;
};

/**
 * @brief Drives one scoped child task and reports its outcome.
 *
 * Runs as a DetachedTask: the child starts eagerly and owns itself, so it
 * never dangles even if the Scope goes out of scope first. On completion it
 * records the first exception and ticks the shared pending counter, setting
 * done when the last child finishes.
 *
 * @tparam T Result type of the child task (ignored).
 * @param state Shared scope state.
 * @param task Task to run to completion.
 */
template<typename T>
DetachedTask runScopeChild(std::shared_ptr<ScopeState> state, Task<T> task)
{
    try
    {
        co_await std::move(task);
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->first_exception)
        {
            state->first_exception = std::current_exception();
        }
    }

    bool done_now = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        done_now = (--state->pending == 0);
    }
    if (done_now)
    {
        state->done.set();
    }
}

} // namespace detail

/**
 * @brief Structured concurrency scope for dynamically-created children.
 *
 * The C++/folly co_scope equivalent. add() starts a child task eagerly so it
 * runs concurrently with the enclosing coroutine; co_await join() suspends
 * until every added child completes, rethrowing the first child failure.
 *
 * Children are self-owned (they run to completion independently), so the
 * Scope must not be destroyed while a join() task is still waiting; await
 * join() before the Scope goes out of scope to guarantee all children have
 * finished. A child may be added after a previous join(); this starts a fresh
 * batch whose failures are collected by the next join().
 *
 * add() and join() must not be called concurrently from different threads:
 * the pending counter and the first-failure slot are batch-level state whose
 * add/join interleaving is intentionally left to the caller to serialize.
 */
class Scope
{
  public:
    Scope() = default;

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    /**
     * @brief Starts a child task immediately and tracks its completion.
     *
     * The child runs concurrently with the caller. Exceptions thrown by the
     * child are collected and rethrown by join().
     *
     * @tparam T Result type of the task (discarded).
     * @param task Task to run; must be non-empty.
     */
    template<typename T>
    void add(Task<T> task)
    {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->pending == 0)
            {
                // A fresh batch starts: clear the previous completion state.
                state_->done.reset();
                state_->first_exception = nullptr;
            }
            ++state_->pending;
        }
        detail::runScopeChild(state_, std::move(task));
    }

    /**
     * @brief Waits until every added child completes.
     *
     * Rethrows the first exception recorded by any child. If the token is
     * cancelled while waiting, TaskCancelledException is thrown and the
     * children keep running to completion independently; a later join() waits
     * for them normally (cancellation never leaves the internal event armed).
     *
     * @param token Optional cancellation token.
     * @return A task that completes when all children have completed.
     */
    [[nodiscard]]
    Task<void> join(CancellationToken token = {})
    {
        throwIfCancelled(token);

        // Cancellation only wakes the wait; join() re-checks the token and the
        // pending counter after every wakeup, so a cancelled join never leaves
        // done armed for a later join(). The wake-up is handed to the timer thread: this
        // callback runs inside request_stop(), and resuming join() here would destroy this
        // stop_callback from inside its own callback (see detail::deferWake).
        std::stop_callback cancellation{ token, [state = state_]() noexcept { detail::deferWake(state); } };

        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->pending == 0)
                {
                    break;
                }
                // Re-arm before waiting: done is manual-reset and may have
                // been set by a previous cancellation.
                state_->done.reset();
            }

            if (token.stop_requested())
            {
                throw TaskCancelledException{};
            }

            co_await state_->done;

            if (token.stop_requested())
            {
                throw TaskCancelledException{};
            }
        }

        std::exception_ptr ex;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ex = state_->first_exception;
        }
        if (ex)
        {
            std::rethrow_exception(ex);
        }
    }

  private:
    std::shared_ptr<detail::ScopeState> state_{ std::make_shared<detail::ScopeState>() };
};

VN_ASYNC_NS_END
