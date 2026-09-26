#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <exception>

VN_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Registers a waiter without letting a failure escape a suspension point.
 *
 * The waiter lists of AsyncQueue, TaskCompletionSource and SharedTask are std::vectors, so
 * registering allocates - and it happens inside await_suspend, which has to be noexcept: by
 * then the coroutine is committing to suspend, and the only handle that could be resumed
 * afterwards is the one being registered. A bad_alloc thrown from there would terminate the
 * process, and letting it escape instead would leave the coroutine suspended with nothing
 * holding it.
 *
 * So the failure is captured here and handed back to the caller, which reports it from
 * await_resume() - on the coroutine's own stack, where unhandled_exception() turns it into the
 * task's error exactly as if the operation had failed one step earlier.
 *
 * The caller must hold the mutex that guards the container.
 *
 * @tparam Container Waiter container, e.g. std::vector<std::coroutine_handle<>>.
 * @param waiters Container to register in.
 * @param h Handle to register.
 * @param failure Receives the exception to rethrow from await_resume() when this returns false.
 * @return true when the handle is registered, false when the failure was captured instead.
 */
template<typename Container>
[[nodiscard]]
bool tryRegisterWaiter(Container& waiters, std::coroutine_handle<> h, std::exception_ptr& failure) noexcept
{
    try
    {
        waiters.push_back(h);
        return true;
    }
    catch (...)
    {
        failure = std::current_exception();
        return false;
    }
}

} // namespace detail

VN_ASYNC_NS_END
