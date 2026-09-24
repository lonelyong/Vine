#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <thread>

VN_ASYNC_NS_BEGIN

/**
 * @brief Awaiter that yields the current thread's CPU slice.
 *
 * await_suspend calls std::this_thread::yield() so the OS can schedule other
 * threads, then transfers control back to the same coroutine, which therefore
 * continues on the same thread without growing the machine stack.
 */
class YieldAwaiter
{
  public:
    /**
     * @brief Always suspends so the yield actually happens.
     *
     * @return false.
     */
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return false;
    }

    /**
     * @brief Yields the CPU to the OS, then resumes by symmetric transfer.
     *
     * The handle is returned instead of resumed here: a nested h.resume() would
     * leave this await_suspend frame and the coroutine's resume frame on the
     * machine stack for every single yield, so a loop over co_await yield()
     * would consume O(iterations) stack and overflow it. Returning the handle
     * lets the compiler resume it without growing the stack.
     *
     * @param h Coroutine to resume after yielding.
     * @return The same handle, transferred to instead of resumed inline.
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) const noexcept
    {
        std::this_thread::yield();
        return h;
    }

    void await_resume() const noexcept {}
};

/**
 * @brief Returns an awaiter that yields the current thread.
 *
 * co_await yield() gives other threads a chance to run, then continues on the
 * same thread without changing the execution context.
 *
 * @return An awaiter; co_await it to yield.
 */
[[nodiscard]]
inline YieldAwaiter yield() noexcept
{
    return {};
}

VN_ASYNC_NS_END
