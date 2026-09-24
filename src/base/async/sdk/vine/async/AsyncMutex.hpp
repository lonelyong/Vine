#pragma once

#include "async_global.hpp"

#include <cassert>
#include <coroutine>
#include <mutex>
#include <utility>

VN_ASYNC_NS_BEGIN

/**
 * @brief FIFO coroutine-aware mutex.
 *
 * co_await mutex.lock() suspends until the mutex is acquired; waiters are
 * resumed in FIFO order. Thread-safe: the locked flag and waiter queue are
 * guarded by an internal mutex, and resumed waiters run on the thread that
 * released the mutex.
 *
 * Lifetime: the mutex must outlive every coroutine waiting on it and every
 * guard obtained from it. An abandoned wait (the awaiting coroutine is
 * destroyed while queued) is safe — the awaiter unregisters itself — but the
 * coroutine owning a queued awaiter must not be destroyed concurrently with
 * unlock() (see the framework contract in async_global.hpp).
 *
 * Never resume a waiter while holding the internal mutex_: the resumed
 * coroutine may immediately lock or unlock this mutex again.
 */
class AsyncMutex
{
  public:
    AsyncMutex() noexcept = default;

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;

    ~AsyncMutex()
    {
        // Debug check: destroying a mutex that is locked or has waiters is a
        // lifetime violation.
        assert(!locked_ && head_ == nullptr && tail_ == nullptr);
    }

    /**
     * @brief Acquires the mutex without suspending.
     *
     * @return true if acquired, false if already locked.
     */
    bool try_lock() noexcept;

    /**
     * @brief Awaiter that acquires the mutex, suspending if it is held.
     */
    class LockAwaiter
    {
      public:
        explicit LockAwaiter(AsyncMutex& mutex) noexcept : mutex_(mutex) {}

        LockAwaiter(const LockAwaiter&) = delete;
        LockAwaiter& operator=(const LockAwaiter&) = delete;

        ~LockAwaiter();

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return mutex_.try_lock();
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept;

        void await_resume() const noexcept {}

      private:
        friend class AsyncMutex;

        AsyncMutex& mutex_;

        /// The queued coroutine; non-null while waiting or being resumed.
        std::coroutine_handle<> waiter_{};
        /// Non-null while this awaiter is in the FIFO queue.
        LockAwaiter* next_{ nullptr };
        LockAwaiter* prev_{ nullptr };
    };

    /**
     * @brief Returns an awaiter that acquires the mutex.
     *
     * @return An awaiter; co_await it to hold the lock until unlock().
     */
    [[nodiscard]]
    LockAwaiter lock() noexcept
    {
        return LockAwaiter{ *this };
    }

    /**
     * @brief Releases the mutex, resuming the next waiter if any.
     */
    void unlock() noexcept;

  private:
    void enqueue(LockAwaiter& a) noexcept;
    void dequeue(LockAwaiter& a) noexcept;

    /// Acquires if free; caller must hold mutex_.
    bool tryAcquireLocked() noexcept;

    std::mutex mutex_{};

    /// true while a coroutine owns the mutex; stays true during waiter hand-off.
    bool locked_{ false };
    LockAwaiter* head_{ nullptr };
    LockAwaiter* tail_{ nullptr };
};

/**
 * @brief RAII guard that unlocks an AsyncMutex on destruction.
 */
class AsyncLockGuard
{
  public:
    AsyncLockGuard() noexcept = default;

    explicit AsyncLockGuard(AsyncMutex* mutex) noexcept : mutex_(mutex) {}

    /// Transfers ownership of the held lock; the source guard becomes empty.
    AsyncLockGuard(AsyncLockGuard&& other) noexcept
      : mutex_(std::exchange(other.mutex_, nullptr))
    {}

    AsyncLockGuard& operator=(AsyncLockGuard&& other) noexcept
    {
        if (this != &other)
        {
            unlock();
            mutex_ = std::exchange(other.mutex_, nullptr);
        }
        return *this;
    }

    ~AsyncLockGuard()
    {
        unlock();
    }

    AsyncLockGuard(const AsyncLockGuard&) = delete;
    AsyncLockGuard& operator=(const AsyncLockGuard&) = delete;

    /**
     * @brief Releases the lock early; the guard becomes empty.
     */
    void unlock() noexcept
    {
        if (mutex_)
        {
            mutex_->unlock();
            mutex_ = nullptr;
        }
    }

    /**
     * @brief Returns whether the guard still holds the lock.
     *
     * @return true if the lock has not been released.
     */
    [[nodiscard]]
    bool owns_lock() const noexcept
    {
        return mutex_ != nullptr;
    }

  private:
    AsyncMutex* mutex_{ nullptr };
};

/**
 * @brief Awaiter that acquires the mutex and yields an AsyncLockGuard.
 *
 * Usage: auto guard = co_await lockAsync(mutex);
 */
class AsyncLockGuardAwaiter
{
  public:
    explicit AsyncLockGuardAwaiter(AsyncMutex& mutex) noexcept : mutex_(mutex), lock_(mutex) {}

    AsyncLockGuardAwaiter(const AsyncLockGuardAwaiter&) = delete;
    AsyncLockGuardAwaiter& operator=(const AsyncLockGuardAwaiter&) = delete;

    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return lock_.await_ready();
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        return lock_.await_suspend(h);
    }

    AsyncLockGuard await_resume() noexcept
    {
        return AsyncLockGuard{ &mutex_ };
    }

  private:
    AsyncMutex&             mutex_;
    AsyncMutex::LockAwaiter lock_;
};

/**
 * @brief Awaits the mutex and yields an AsyncLockGuard on success.
 *
 * @param mutex Mutex to acquire.
 * @return An awaiter producing an AsyncLockGuard.
 */
inline AsyncLockGuardAwaiter lockAsync(AsyncMutex& mutex)
{
    return AsyncLockGuardAwaiter{ mutex };
}

inline bool AsyncMutex::tryAcquireLocked() noexcept
{
    if (locked_)
    {
        return false;
    }
    locked_ = true;
    return true;
}

inline bool AsyncMutex::try_lock() noexcept
{
    std::lock_guard lock(mutex_);
    return tryAcquireLocked();
}

inline bool AsyncMutex::LockAwaiter::await_suspend(std::coroutine_handle<> h) noexcept
{
    // await_ready() is only an optimistic fast path; the state must be
    // re-checked under mutex_ here, or a release between the two checks could
    // be lost and this coroutine would never wake.
    waiter_ = h;
    std::lock_guard lock(mutex_.mutex_);
    if (mutex_.tryAcquireLocked())
    {
        waiter_ = {};
        return false; // do not suspend
    }
    mutex_.enqueue(*this);
    return true;
}

inline AsyncMutex::LockAwaiter::~LockAwaiter()
{
    if (waiter_)
    {
        std::lock_guard lock(mutex_.mutex_);
        mutex_.dequeue(*this);
    }
}

inline void AsyncMutex::unlock() noexcept
{
    LockAwaiter* popped = nullptr;
    {
        std::lock_guard lock(mutex_);
        assert(locked_);
        if (head_)
        {
            popped = head_;
            head_  = head_->next_;
            if (!head_)
            {
                tail_ = nullptr;
            }
            else
            {
                // Detach the old head from its successor so a later dequeue of
                // the successor cannot write through the old head's freed frame.
                head_->prev_ = nullptr;
            }
            popped->next_ = nullptr;
            popped->prev_ = nullptr;
        }
        else
        {
            locked_ = false;
        }
    }
    if (popped)
    {
        auto h    = popped->waiter_;
        popped->waiter_ = {};   // ownership transfers to the waiter
        h.resume();
    }
}

inline void AsyncMutex::enqueue(LockAwaiter& a) noexcept
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

inline void AsyncMutex::dequeue(LockAwaiter& a) noexcept
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
