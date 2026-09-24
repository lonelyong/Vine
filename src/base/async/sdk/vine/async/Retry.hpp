#pragma once

#include "async_global.hpp"

#include <chrono>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "Sleep.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Runs a task factory, retrying on failure up to a number of attempts.
 *
 * factory() is invoked to produce a fresh Task<T> for each attempt. Failures
 * before the last attempt are swallowed (optionally after a delay); the final
 * attempt propagates its exception. A zero attempts value defaults to one.
 *
 * @tparam F Factory; factory() -> Task<T>.
 * @param factory Callable producing each attempt's task.
 * @param attempts Maximum number of attempts.
 * @param delay Optional delay between attempts.
 * @return A task yielding the first successful result.
 */
template<typename F>
[[nodiscard]]
Task<typename std::invoke_result_t<F>::value_type> retry(F factory,
                                                         std::size_t attempts,
                                                         std::chrono::milliseconds delay = {})
{
    using T = typename std::invoke_result_t<F>::value_type;

    if (attempts == 0)
    {
        attempts = 1;
    }

    for (std::size_t attempt = 1;; ++attempt)
    {
        if (attempt > 1)
        {
            co_await sleepFor(delay); // Delay before each retry (not inside a handler).
        }
        try
        {
            if constexpr (std::is_void_v<T>)
            {
                co_await factory();
                co_return;
            }
            else
            {
                co_return co_await factory();
            }
        }
        catch (...)
        {
            if (attempt >= attempts)
            {
                throw; // Exhausted: propagate.
            }
        }
    }
}

VN_ASYNC_NS_END
