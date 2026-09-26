#pragma once

#include "async_global.hpp"

#include <stop_token>
#include <type_traits>
#include <utility>

#include "Concepts.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Asks the current coroutine for its environment stop token.
 *
 * Spell it as `const std::stop_token token = co_await currentStopToken();` inside a Task body.
 * The token is the one a caller handed to withStopToken(); when nobody handed one over it is a
 * default-constructed token, so `stop_possible()` is how a body tells "I was given an
 * environment" from "I was not".
 *
 * The request is answered by the promise (Task::promise_type::await_transform) rather than by an
 * awaiter of its own: a coroutine cannot name its own promise without a handle, and passing a
 * handle through every await is exactly what an environment is for. The answer never suspends.
 *
 * Only a Task body has that hook. Awaiting this inside a DetachedTask body or a plain coroutine
 * does not compile, which is the honest answer - they have no environment to report. (The operand
 * type is a detail of how the promise recognises the request; callers never name it.)
 *
 * @return An operand for co_await that the promise turns into its environment token.
 */
[[nodiscard]]
inline detail::StopTokenRequest currentStopToken() noexcept
{
    return {};
}

/**
 * @brief Runs a task in the given environment: its body sees token as its own.
 *
 * The token is stored in the task's promise before the task starts, which a lazy Task allows:
 * calling the coroutine has not executed a line yet, so its environment is still open. Inside,
 * `co_await currentStopToken()` returns it, and the operations that take an environment use it as
 * their default (see sleepFor()).
 *
 * Nothing is inherited automatically: a task created inside another task starts with an empty
 * environment unless its creator hands one over. That is deliberate for this step - no existing
 * call site changes behaviour - and it is the point to revisit if compositions should propagate
 * their token into children by default (see .ai/design/async-next.md).
 *
 * @tparam T Result type of the task.
 * @param token Environment the body should run in; a default token means "no environment".
 * @param task Task to run; handed over, so it must not be awaited anywhere else.
 * @return A task that completes as task does.
 */
template<StorableValue T>
[[nodiscard]]
Task<T> withStopToken(std::stop_token token, Task<T> task)
{
    detail::TaskEnvironment::setToken(task, std::move(token));

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

VN_ASYNC_NS_END
