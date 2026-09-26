#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <cstddef>
#include <mutex>

VN_ASYNC_NS_BEGIN

/**
 * @brief Counting semaphore that can be awaited from a coroutine.
 *
 * co_await semaphore.acquire() suspends until a permit is available. When a
 * permit is released, the next waiter is resumed on the thread that called
 * release(). Thread-safe.
 */
class AsyncSemaphore
{
  public:
    explicit AsyncSemaphore(std::ptrdiff_t count = 0) noexcept : count_(count) {}

    AsyncSemaphore(const AsyncSemaphore&) = delete;
    AsyncSemaphore& operator=(const AsyncSemaphore&) = delete;

    ~AsyncSemaphore() = default;

    /**
     * @brief Consumes a permit without suspending.
     *
     * @return true if a permit was consumed, false if none is available.
     */
[[nodiscard]]
    bool try_acquire() noexcept;

    /**
     * @brief Returns a permit, resuming the next waiter if any.
     *
     * @param n Number of permits to release.
     */
    void release(std::ptrdiff_t n = 1) noexcept;

    /**
     * @brief Awaiter that consumes a permit, suspending if none is available.
     */
    class AcquireAwaiter
    {
      public:
        explicit AcquireAwaiter(AsyncSemaphore& semaphore) noexcept : semaphore_(semaphore) {}

        AcquireAwaiter(const AcquireAwaiter&) = delete;
        AcquireAwaiter& operator=(const AcquireAwaiter&) = delete;

        ~AcquireAwaiter() noexcept;

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return semaphore_.try_acquire();
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept;

        void await_resume() const noexcept {}

      private:
        friend class AsyncSemaphore;

        AsyncSemaphore&         semaphore_;
        std::coroutine_handle<> waiter_{};
        AcquireAwaiter*         next_{ nullptr };
        AcquireAwaiter*         prev_{ nullptr };
    };

    /**
     * @brief Returns an awaiter that consumes a permit.
     *
     * @return An awaiter; co_await it to hold a permit until release().
     */
    [[nodiscard]]
    AcquireAwaiter acquire() noexcept
    {
        return AcquireAwaiter{ *this };
    }

  private:
    void enqueue(AcquireAwaiter& a) noexcept;
    void dequeue(AcquireAwaiter& a) noexcept;

    std::mutex     mutex_{};
    std::ptrdiff_t count_;
    AcquireAwaiter* head_{ nullptr };
    AcquireAwaiter* tail_{ nullptr };
};

inline bool AsyncSemaphore::try_acquire() noexcept
{
    std::lock_guard lock(mutex_);
    if (count_ > 0)
    {
        --count_;
        return true;
    }
    return false;
}

inline bool AsyncSemaphore::AcquireAwaiter::await_suspend(std::coroutine_handle<> h) noexcept
{
    waiter_ = h;
    std::lock_guard lock(semaphore_.mutex_);
    if (semaphore_.count_ > 0)
    {
        --semaphore_.count_;
        waiter_ = {};
        return false;   // do not suspend
    }
    semaphore_.enqueue(*this);
    return true;
}

inline AsyncSemaphore::AcquireAwaiter::~AcquireAwaiter() noexcept
{
    if (waiter_)
    {
        std::lock_guard lock(semaphore_.mutex_);
        semaphore_.dequeue(*this);
    }
}

inline void AsyncSemaphore::release(std::ptrdiff_t n) noexcept
{
    {
        std::lock_guard lock(mutex_);
        count_ += n;
    }
    // Grant and resume waiters one at a time, popping from the shared list
    // under the lock, so a resumed waiter whose completion destroys a still-
    // suspended sibling (e.g. a whenAny winner) never leaves a stale handle:
    // the destroyed sibling unregisters itself and is simply not popped. A
    // concurrent fast-path acquirer may take a permit between iterations; the
    // permit count stays exact, so no permit is lost or double-granted.
    for (;;)
    {
        std::coroutine_handle<> h;
        {
            std::lock_guard lock(mutex_);
            if (head_ && count_ > 0)
            {
                auto* a = head_;
                head_ = head_->next_;
                if (!head_)
                {
                    tail_ = nullptr;
                }
                else
                {
                    // Detach the old head from its successor: otherwise the
                    // successor's prev_ would still point at a's frame, and a
                    // later dequeue of that successor would write through the
                    // dangling prev_ (use-after-free) after a's frame is gone.
                    head_->prev_ = nullptr;
                }
                a->prev_ = nullptr;
                a->next_ = nullptr;
                --count_;
                h = a->waiter_;
                a->waiter_ = {};
            }
        }
        if (!h)
        {
            break;
        }
        h.resume(); // Resume outside the lock.
    }
}

inline void AsyncSemaphore::enqueue(AcquireAwaiter& a) noexcept
{
    a.prev_ = tail_;
    a.next_ = nullptr;
    if (tail_)
    {
        tail_->next_ = &a;
    }
    else
    {
        head_ = &a;
    }
    tail_ = &a;
}

inline void AsyncSemaphore::dequeue(AcquireAwaiter& a) noexcept
{
    if (a.prev_)
    {
        a.prev_->next_ = a.next_;
    }
    else if (head_ == &a)
    {
        head_ = a.next_;
    }
    else
    {
        return;   // not in the queue
    }

    if (a.next_)
    {
        a.next_->prev_ = a.prev_;
    }
    else if (tail_ == &a)
    {
        tail_ = a.prev_;
    }

    a.prev_ = nullptr;
    a.next_ = nullptr;
}

VN_ASYNC_NS_END
