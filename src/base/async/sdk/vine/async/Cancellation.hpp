#pragma once

#include "async_global.hpp"

#include <type_traits>
#include <utility>

#include <vine/CancellationToken.hpp>
#include <vine/Exception.hpp>
#include <vine/String.hpp>

#include "Task.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Exception thrown when a cancellable Task observes cancellation.
 *
 * Derives from vn::Exception with code Exception::CANCELLED.
 */
class TaskCancelledException : public vn::Exception {
  public:
    TaskCancelledException()
      : vn::Exception(vn::Exception::Code::CANCELLED, String(u8"task cancelled"))
    {}
};

/**
 * @brief What a composition does with children it is no longer waiting for.
 *
 * Both poles are legitimate, and they differ in what they ask of the child - which is the contract
 * that matters, not the name:
 *
 * - Destroy: the unfinished children are destroyed as soon as the composition completes. This is
 *   what Rust's tokio::select! does ("cancelling the remaining branches", implemented as drop).
 *   The child must be safe to destroy at any suspension point, and it is never told it was
 *   dropped.
 * - StopAndWait: the children are asked to stop through the environment token they were handed,
 *   and the composition stays parked until every one of them has finished. This is what Trio
 *   nurseries, Kotlin's coroutineScope and P2300/asio request_stop do, and it is what C#'s
 *   Task.WhenAll amounts to when the caller hands its token down to the tasks. The child must
 *   observe that token: one that ignores it parks the composition until it is done, which is the
 *   price this policy knowingly pays.
 */
enum class CancelPolicy
{
    Destroy,    // Destroy the unfinished children once the composition completes.
    StopAndWait // Ask the unfinished children to stop, then wait for them to finish.
};

/**
 * @brief Throws TaskCancelledException if the token has a stop requested.
 *
 * A cooperative cancellation point: call it at safe points inside a task that
 * received a CancellationToken so the task can observe cancellation.
 *
 * @param token Token to poll.
 */
inline void throwIfCancelled(const CancellationToken& token)
{
    if (token.stop_requested()) {
        throw TaskCancelledException{};
    }
}

/**
 * @brief Runs a task under a cancellation token, which becomes the task's environment.
 *
 * If the token is already cancelled when the returned task is awaited, it
 * throws TaskCancelledException immediately without running the task;
 * otherwise the task runs and its result or exception propagates unchanged.
 *
 * The token is written into the task's promise before its body starts, so the body reads it with
 * co_await currentStopToken() and can hand it to the operations it awaits (a sleep, a nested
 * withCancellation). Cancelling a running task is cooperative: nothing is interrupted, and only
 * code that reads the token - the environment, throwIfCancelled(token) at a safe point - reacts.
 *
 * @tparam T Result type of the task.
 * @param token Cancellation token handed to the task as its environment.
 * @param task Task to run; handed over, so it must not be awaited anywhere else.
 * @return A task that completes as task does, or throws TaskCancelledException.
 */
template <typename T>
[[nodiscard]]
Task<T> withCancellation(CancellationToken token, Task<T> task)
{
    throwIfCancelled(token);

    // Before the body starts: a lazy Task has not run a line yet, which is the last moment at
    // which its environment can still be set (the same injection withStopToken() and the
    // compositions use).
    detail::TaskEnvironment::setToken(task, token);

    if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
        co_return;
    }
    else {
        co_return co_await std::move(task);
    }
}

VN_ASYNC_NS_END
