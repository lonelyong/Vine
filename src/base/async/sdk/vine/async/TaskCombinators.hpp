#pragma once

#include "async_global.hpp"

#include <type_traits>
#include <utility>

#include "Task.hpp"

V_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Result type of andThen: the value_type of the task f returns.
 */
template<typename F, typename T>
struct AndThenResult
{
    using type = typename std::invoke_result_t<F, T>::value_type;
};

template<typename F>
struct AndThenResult<F, void>
{
    using type = typename std::invoke_result_t<F>::value_type;
};

} // namespace detail

/**
 * @brief Maps a task's result with a synchronous function.
 *
 * The C# Task.Select / LINQ-style equivalent: runs the task, feeds its result
 * to f, and yields f's return value.
 *
 * @tparam T Result type of the task.
 * @tparam F Mapping function; f(taskResult) or f() for a void task.
 * @param task Task to run.
 * @param f Function applied to the result.
 * @return A task yielding f's return value.
 */
template<typename T, typename F>
    requires (!std::is_void_v<T>)
[[nodiscard]]
Task<std::invoke_result_t<F, T>> transform(Task<T> task, F f)
{
    co_return f(co_await std::move(task));
}

/**
 * @brief Maps a void task's completion with a synchronous function.
 *
 * @tparam F Mapping function taking no arguments.
 * @param task Void task to run.
 * @param f Function called after the task completes.
 * @return A task yielding f's return value.
 */
template<typename F>
[[nodiscard]]
Task<std::invoke_result_t<F>> transform(Task<void> task, F f)
{
    co_await std::move(task);
    co_return f();
}

/**
 * @brief Chains a task to a task-returning continuation.
 *
 * The C# ContinueWith equivalent: awaits task, passes its result (or nothing
 * for a void task) to f, awaits the task f returns, and yields its result.
 *
 * @tparam T Result type of the first task.
 * @tparam F Continuation; f(taskResult) -> Task<U> or f() -> Task<U>.
 * @param task First task to run.
 * @param f Continuation producing the next task.
 * @return A task yielding the continuation's result.
 */
template<typename T, typename F>
[[nodiscard]]
Task<typename detail::AndThenResult<F, T>::type> andThen(Task<T> task, F f)
{
    if constexpr (std::is_void_v<T>)
    {
        co_await std::move(task);
        co_return co_await f();
    }
    else
    {
        co_return co_await f(co_await std::move(task));
    }
}

V_ASYNC_NS_END
