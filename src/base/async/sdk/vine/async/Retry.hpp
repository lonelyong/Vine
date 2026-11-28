#pragma once

#include "async_global.hpp"

#include <chrono>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "Cancellation.hpp"
#include "Sleep.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Runs a task factory, retrying on failure up to a number of attempts.
 *
 * @details
 * - factory() is invoked to produce a fresh Task<T> for each attempt; a zero attempts value defaults to one.
 * - Failures before the last attempt are swallowed (optionally after a delay); the final attempt propagates its
 *   exception.
 * - A cancellation is never retried: `TaskCancelledException` ends the loop at once, and the token taken here is
 *   checked before each attempt and forwarded to the delay, so a cancelled caller does not sit out the remaining
 *   attempts and their delays.
 *
 * @tparam F Factory; factory() -> Task<T>.
 * @param factory Callable producing each attempt's task.
 * @param attempts Maximum number of attempts.
 * @param delay Optional delay between attempts.
 * @param token Optional token checked before each attempt and used by the delays.
 * @return A task yielding the first successful result.
 */
template <typename F>
[[nodiscard]]
Task<typename std::invoke_result_t<F>::value_type> retry(F factory, std::size_t attempts, std::chrono::milliseconds delay = {}, CancellationToken token = {})
{
    using T = typename std::invoke_result_t<F>::value_type;

    if (attempts == 0) {
        attempts = 1;
    }

    for (std::size_t attempt = 1;; ++attempt) {
        throwIfCancelled(token);
        if (attempt > 1) {
            co_await sleepFor(delay, token); // Delay before each retry; the token cuts it short.
        }
        try {
            if constexpr (std::is_void_v<T>) {
                co_await factory();
                co_return;
            }
            else {
                co_return co_await factory();
            }
        }
        catch (const TaskCancelledException&) {
            throw; // A cancellation is a request to stop, not a transient failure.
        }
        catch (...) {
            if (attempt >= attempts) {
                throw; // Exhausted: propagate.
            }
        }
    }
}

VN_ASYNC_NS_END
