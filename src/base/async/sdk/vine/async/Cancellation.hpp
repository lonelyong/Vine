#pragma once

#include "async_global.hpp"

#include <utility>

#include <vine/CancellationToken.hpp>
#include <vine/Exception.hpp>
#include <vine/String.hpp>

#include "Task.hpp"

V_ASYNC_NS_BEGIN

/**
 * @brief Exception thrown when a cancellable Task observes cancellation.
 *
 * Derives from vine::Exception with code Exception::CANCELLED.
 */
class TaskCancelledException : public vine::Exception
{
  public:
    TaskCancelledException()
      : vine::Exception(vine::Exception::Code::CANCELLED, String(u8"task cancelled"))
    {}
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
    if (token.stop_requested())
    {
        throw TaskCancelledException{};
    }
}

/**
 * @brief Runs a task under a cancellation token.
 *
 * If the token is already cancelled when the returned task is awaited, it
 * throws TaskCancelledException immediately without running the task;
 * otherwise the task runs and its result or exception propagates unchanged.
 * Cancelling a running task is cooperative: the task may call
 * throwIfCancelled(token) at safe points.
 *
 * @tparam T Result type of the task.
 * @param token Cancellation token passed down to the task.
 * @param task Task to run.
 * @return A task that completes as task does, or throws TaskCancelledException.
 */
template<typename T>
[[nodiscard]]
Task<T> withCancellation(CancellationToken token, Task<T> task)
{
    throwIfCancelled(token);
    co_return co_await std::move(task);
}

V_ASYNC_NS_END
