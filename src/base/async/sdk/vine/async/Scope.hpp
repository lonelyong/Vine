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
 * Environment: the scope owns a std::stop_source and hands its token to every child it adds, so
 * a child that reads co_await currentStopToken() - or sleeps with an inherited token - can be
 * asked to stop. Destroying the scope makes that request (see ~Scope); it cannot cancel a child
 * that does not read the token, and it does not wait for one, so pendingChildren() is what a host
 * uses to notice a scope that went away with work still running. detach() is how a caller says
 * "leave this batch alone".
 *
 * add() and join() must not be called concurrently from different threads:
 * the pending counter and the first-failure slot are batch-level state whose
 * add/join interleaving is intentionally left to the caller to serialize.
 */
class Scope
{
  public:
    Scope() = default;

    /**
     * @brief Asks the children of the current batch to stop, and stops tracking them for stop requests.
     *
     * Children that read their environment token (co_await currentStopToken(), or a sleep that
     * inherits one) observe the request and can finish early; one that never looks is unaffected.
     * The scope still waits for them in join() - detach() is the call that lets them be ignored.
     * The next add() starts a fresh batch that is tracked again.
     */
    void detach() noexcept
    {
        detached_ = true;
    }

    /**
     * @brief Asks every child of the current batch to stop.
     *
     * A request, not a cancellation: it is recorded in the scope's stop_source, and only a child
     * that reads the token reacts to it. Calling this is harmless when there are no children, or
     * when they have all finished.
     */
    void requestStop() noexcept
    {
        source_.request_stop();
    }

    /**
     * @brief Reports how many children of the current batch are still running.
     *
     * What a host needs to decide whether a scope that is going away left work behind - the scope
     * itself cannot do more than ask (a destructor is not a coroutine, so it cannot wait, and
     * blocking the thread or pumping an event loop would be the host's decision).
     *
     * @return Number of added children that have not completed yet.
     */
    [[nodiscard]]
    std::size_t pendingChildren() const noexcept
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->pending;
    }

    /**
     * @brief Asks the children of the current batch to stop, without waiting for them.
     *
     * The scope cannot cancel what ignores the token, and it must not wait: waiting would mean
     * blocking this thread or pumping someone else's event loop, and that is the host's call (the
     * same reason the module has no global pump). So going away does the only thing that is
     * universally safe - request stop - and pendingChildren() is how a host turns that into a
     * diagnostic of its own.
     */
    ~Scope()
    {
        if (!detached_)
        {
            requestStop();
        }
    }

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
                // A fresh batch starts: clear the previous completion state, and track this batch
                // again - detach() applied to the batch before it.
                state_->done.reset();
                state_->first_exception = nullptr;
                detached_ = false;
            }
            ++state_->pending;
        }
        // The child's environment is written before it starts: a lazy Task has not run a line
        // yet, so this is the last moment at which it can still be told what environment it lives
        // in (see currentStopToken()).
        detail::TaskEnvironment::setToken(task, source_.get_token());
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

    /// Environment handed to every child of the scope; see add() and requestStop().
    std::stop_source source_{};

    /// true while the current batch is exempt from the destructor's stop request; see detach().
    bool detached_{ false };
};

VN_ASYNC_NS_END
