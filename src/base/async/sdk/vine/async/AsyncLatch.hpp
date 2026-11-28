#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>

#include "AsyncEvent.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

namespace detail
{

/**
 * @brief Shared state of an AsyncLatch.
 */
struct LatchState {
    std::mutex  mutex;
    std::size_t count{ 0 };
    AsyncEvent  done;
};

} // namespace detail

/**
 * @brief Count-down latch for one-time synchronization.
 *
 * wait() suspends until the initial count reaches zero via countDown(); once
 * released it returns immediately forever (manual-reset semantics). A latch
 * constructed with a count of zero is already released: wait() completes
 * without suspending and isReady() reports true from the start. Thread-safe.
 *
 * Threading: countDown() may be called from any thread, and the waiters it releases are
 * resumed on the thread that called it. A destroyed waiter unregisters itself (see AsyncEvent),
 * and the latch object itself may be destroyed by a waiter released inside that resume, because
 * the call holds the shared state for its duration.
 */
class AsyncLatch {
  public:
    /**
     * @brief Constructs a latch.
     *
     * @param count Number of countDown() calls required to release waiters.
     */
    explicit AsyncLatch(std::size_t count)
      : state_(std::make_shared<detail::LatchState>())
    {
        state_->count = count;
        if (count == 0) {
            // A latch that starts at zero is already released: wait() must not
            // suspend, otherwise it would hang forever (isReady() already
            // reports true for it, and no countDown() is pending).
            state_->done.set();
        }
    }

    AsyncLatch(const AsyncLatch&)            = delete;
    AsyncLatch& operator=(const AsyncLatch&) = delete;

    /**
     * @brief Decrements the count; releases waiters when it reaches zero.
     *
     * @param n Amount to decrement (clamped at the current count).
     */
    void countDown(std::size_t n = 1)
    {
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (n > state_->count) {
                n = state_->count;
            }
            state_->count -= n;
            ready = (state_->count == 0);
        }
        if (ready) {
            // The latch itself may be destroyed by the last waiter released inside set(), so the state is held here for
            // the duration of the call: the waiters keep their own reference while they wait, and this one covers the
            // resume loop that ends their waits.
            auto state = state_;
            state->done.set();
        }
    }

    /**
     * @brief Returns whether the count has reached zero.
     *
     * @return true if wait() would not suspend.
     */
    [[nodiscard]]
    bool isReady() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->count == 0;
    }

    /**
     * @brief Awaits until the count reaches zero.
     *
     * @return A task completing when the latch releases.
     */
    [[nodiscard]]
    Task<void> wait()
    {
        auto state = state_; // Keep the latch alive while awaiting.
        co_await state->done;
    }

  private:
    std::shared_ptr<detail::LatchState> state_;
};

VN_ASYNC_NS_END
