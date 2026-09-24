#pragma once

#include "async_global.hpp"

#include <type_traits>
#include <utility>

VN_ASYNC_NS_BEGIN

/**
 * @brief RAII guard that runs a callback when it goes out of scope.
 *
 * Lives in a coroutine frame, so the callback runs across co_await boundaries
 * — on normal completion, on exception unwinding, and when the frame is
 * destroyed mid-suspension. The callback must not throw (destructors are
 * noexcept). Use dismiss() to skip it.
 *
 * @tparam F Callback type invoked on destruction.
 */
template<typename F>
class Finally
{
  public:
    explicit Finally(F f) noexcept : f_(std::move(f)) {}

    Finally(Finally&& other) noexcept
      : f_(std::move(other.f_)), active_(std::exchange(other.active_, false))
    {}

    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
    Finally& operator=(Finally&&) = delete;

    ~Finally()
    {
        if (active_)
        {
            f_();
        }
    }

    /**
     * @brief Prevents the callback from running.
     */
    void dismiss() noexcept
    {
        active_ = false;
    }

  private:
    F    f_;
    bool active_{ true };
};

/**
 * @brief Creates a Finally guard running f when it goes out of scope.
 *
 * @tparam F Callback type.
 * @param f Callback invoked on destruction; must not throw.
 * @return A move-only guard.
 */
template<typename F>
Finally<std::decay_t<F>> makeFinally(F&& f)
{
    return Finally<std::decay_t<F>>{ std::forward<F>(f) };
}

VN_ASYNC_NS_END
