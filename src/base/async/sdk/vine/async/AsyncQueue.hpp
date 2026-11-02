#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Sleep.hpp"
#include "Task.hpp"
#include "WaiterList.hpp"

VN_ASYNC_NS_BEGIN

namespace detail
{

/**
 * @brief Shared state of an AsyncQueue.
 *
 * capacity == 0 means unbounded (push never suspends). poppers/pushers hold the
 * coroutines waiting for a value or for room.
 *
 * Wakes are deferred: an operation that changes the queue posts a wake-up to the process-wide
 * timer service (wakeOnePopper/wakeOnePusher/wakeAll) instead of resuming a waiter on its own
 * stack. The resumed coroutine may synchronously destroy the one that woke it - a whenAny winner
 * destroys the losers, and a loser can be the push() or pop() that is still returning - so the
 * wake-up has to run after that stack has unwound. The timer thread resumes one waiter at a time,
 * so a waiter destroyed by a resume unregisters itself before the next iteration reads the list.
 */
template <typename T>
struct AsyncQueueState {
    std::mutex                           mutex;
    std::size_t                          capacity{ 0 };
    std::deque<T>                        items;
    std::vector<std::coroutine_handle<>> poppers;
    std::vector<std::coroutine_handle<>> pushers;
    bool                                 closed{ false };
};

/**
 * @brief Posts a wake-up that resumes one waiting popper, on the timer thread.
 *
 * @details
 * - The handle is taken under the lock at wake-up time, not at post time: a popper destroyed in
 *   between has unregistered itself, so the wake-up simply finds nobody.
 * - Runs on the timer thread, never on the caller's stack (see AsyncQueueState).
 *
 * @param state Queue state; kept alive until the wake-up has run.
 */
template <typename T>
void wakeOnePopper(const std::shared_ptr<AsyncQueueState<T>>& state) noexcept
{
    TimerService::instance().postWake(state, [](void* p) noexcept {
        auto&                   s = *static_cast<AsyncQueueState<T>*>(p);
        std::coroutine_handle<> h{};
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.poppers.empty()) {
                h = s.poppers.front();
                s.poppers.erase(s.poppers.begin());
            }
        }
        if (h) {
            h.resume();
        }
    });
}

/**
 * @brief Posts a wake-up that resumes one waiting pusher, on the timer thread.
 *
 * @param state Queue state; kept alive until the wake-up has run.
 */
template <typename T>
void wakeOnePusher(const std::shared_ptr<AsyncQueueState<T>>& state) noexcept
{
    TimerService::instance().postWake(state, [](void* p) noexcept {
        auto&                   s = *static_cast<AsyncQueueState<T>*>(p);
        std::coroutine_handle<> h{};
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.pushers.empty()) {
                h = s.pushers.front();
                s.pushers.erase(s.pushers.begin());
            }
        }
        if (h) {
            h.resume();
        }
    });
}

/**
 * @brief Posts a wake-up that releases every waiter, one at a time, on the timer thread.
 *
 * One action drains both lists instead of the caller looping over them: a resumed waiter whose
 * completion destroys a sibling unregisters that sibling under the lock before this loop looks at
 * the lists again. That is also why the loop runs where it does - on the timer thread, no other
 * thread is resuming waiters of this queue concurrently, so the list cannot change under a handle
 * that was already taken out.
 *
 * @param state Queue state; kept alive until the wake-up has run.
 */
template <typename T>
void wakeAll(const std::shared_ptr<AsyncQueueState<T>>& state) noexcept
{
    TimerService::instance().postWake(state, [](void* p) noexcept {
        auto& s = *static_cast<AsyncQueueState<T>*>(p);
        for (;;) {
            std::coroutine_handle<> h{};
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                if (!s.poppers.empty()) {
                    h = s.poppers.back();
                    s.poppers.pop_back();
                }
                else if (!s.pushers.empty()) {
                    h = s.pushers.back();
                    s.pushers.pop_back();
                }
                else {
                    break;
                }
            }
            h.resume();
        }
    });
}

/**
 * @brief Removes one item for the non-suspending tryPop(); never wakes anybody.
 *
 * The value is move-constructed out of the front element, so only move-construction is required
 * of T - not default construction or move-assignment.
 *
 * @param state Queue state.
 * @return The item, or nullopt when the queue holds none; throws when it is closed and empty.
 */
template <typename T>
std::optional<T> popOne(const std::shared_ptr<AsyncQueueState<T>>& state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->items.empty()) {
        if (state->closed) {
            throw std::runtime_error("async::AsyncQueue: closed");
        }
        return std::nullopt;
    }
    std::optional<T> value;
    value.emplace(std::move(state->items.front()));
    state->items.pop_front();
    return value;
}

/**
 * @brief Enqueues one value.
 *
 * @param state Queue state; its mutex must be held by the caller.
 * @param value Value to enqueue; moved into the queue.
 */
template <typename T>
void enqueueLocked(const std::shared_ptr<AsyncQueueState<T>>& state, T value)
{
    state->items.push_back(std::move(value));
}

/**
 * @brief Awaiter for AsyncQueue::pop(); unregisters on destruction.
 */
template <typename T>
class QueuePopAwaiter {
  public:
    explicit QueuePopAwaiter(std::shared_ptr<AsyncQueueState<T>> state) noexcept
      : state_(std::move(state))
    {}

    QueuePopAwaiter(const QueuePopAwaiter&)            = delete;
    QueuePopAwaiter& operator=(const QueuePopAwaiter&) = delete;

    ~QueuePopAwaiter() noexcept
    {
        if (handle_) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            std::erase(state_->poppers, handle_);
        }
    }

    /**
     * @brief Returns whether a value is already available.
     *
     * @return true when pop can complete without suspending.
     */
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return !state_->items.empty() || state_->closed;
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->items.empty() || state_->closed) {
            return false;
        }
        return detail::tryRegisterWaiter(state_->poppers, h, failure_);
    }

    /**
     * @brief Takes the next value, or reports that a wake-up found the queue empty again.
     *
     * @details
     * - Wakes are spurious by construction: an item can be claimed by another consumer (pop() or
     *   tryPop()) between this waiter's wake-up and this call, which is what nullopt reports.
     * - A successful take frees a slot for a bounded queue, so a parked pusher is woken. It is
     *   woken on the timer thread, never from here: the pusher may complete a composition that
     *   destroys this very coroutine while it is still returning.
     *
     * @return The value, or nullopt when there was nothing to take and the caller must re-await.
     */
    std::optional<T> await_resume()
    {
        if (failure_) {
            std::rethrow_exception(std::exchange(failure_, nullptr));
        }

        std::optional<T> value;
        bool             closed     = false;
        bool             has_pusher = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->items.empty()) {
                value.emplace(std::move(state_->items.front()));
                state_->items.pop_front();
                has_pusher = !state_->pushers.empty();
            }
            else {
                closed = state_->closed;
            }
        }

        if (value) {
            if (has_pusher) {
                detail::wakeOnePusher(state_);
            }
            return value;
        }
        if (closed) {
            throw std::runtime_error("async::AsyncQueue: closed");
        }
        return std::nullopt;
    }

  private:
    std::shared_ptr<AsyncQueueState<T>> state_;
    std::coroutine_handle<>             handle_{};

    /// Why the waiter could not be registered; rethrown by await_resume().
    std::exception_ptr failure_{};
};

/**
 * @brief Awaiter for AsyncQueue::push(); suspends only when bounded and full.
 */
template <typename T>
class QueuePushAwaiter {
  public:
    /**
     * @brief Stores the value to enqueue.
     *
     * @note Not noexcept: T's move constructor may throw, and the failure belongs to the waiting
     * coroutine (its task reports it) rather than to the process.
     */
    explicit QueuePushAwaiter(std::shared_ptr<AsyncQueueState<T>> state, T value)
      : state_(std::move(state))
      , value_(std::move(value))
    {}

    QueuePushAwaiter(const QueuePushAwaiter&)            = delete;
    QueuePushAwaiter& operator=(const QueuePushAwaiter&) = delete;

    ~QueuePushAwaiter() noexcept
    {
        if (handle_) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            std::erase(state_->pushers, handle_);
        }
    }

    /**
     * @brief Returns whether this push never has to wait, which only an unbounded queue does.
     *
     * A bounded queue always goes through await_suspend(): the free slot has to be claimed
     * under the same lock that enqueues, because a separate "is there room" check would let two
     * pushers both see the last slot and overfill a queue the documentation calls bounded. An
     * unbounded one has no bound to keep, so its push completes inline.
     *
     * @return true when the queue is unbounded, false otherwise.
     */
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->capacity == 0;
    }

    /**
     * @brief Enqueues under the lock, or parks this push until room appears.
     *
     * @details
     * - noexcept is required at a suspension point, so a failing enqueue (T's move constructor
     *   or the deque's allocation) is captured in failure_ and rethrown from await_resume(), on
     *   the coroutine's own stack. Letting it escape would terminate the process.
     * - A waiting popper is woken on the timer thread, never from here: it may complete a
     *   composition that destroys this very coroutine while it is still returning.
     *
     * @param h Coroutine to park when the queue is full.
     * @return false to resume inline, either because the value was enqueued or to report a failure.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        bool has_popper = false;
        try {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) {
                return false; // await_resume() reports the closed queue.
            }
            if (state_->capacity == 0 || state_->items.size() < state_->capacity) {
                detail::enqueueLocked(state_, std::move(value_));
                enqueued_  = true;
                has_popper = !state_->poppers.empty();
            }
            else {
                return detail::tryRegisterWaiter(state_->pushers, h, failure_);
            }
        }
        catch (...) {
            failure_ = std::current_exception();
            return false; // await_resume() rethrows the enqueue failure.
        }
        if (has_popper) {
            detail::wakeOnePopper(state_);
        }
        return false; // Enqueued under the lock; nothing left to wait for.
    }

    /**
     * @brief Reports the outcome of this attempt, or asks the caller to wait again.
     *
     * @details
     * - true means the value was enqueued (or the queue is closed, reported by throwing).
     * - false means a wake-up arrived on a bounded queue but the free slot had been claimed again
     *   in the meantime; the push re-awaits this same awaiter, which claims a slot under the lock
     *   in await_suspend().
     *
     * @return true when this push is finished, false when it has to be awaited again.
     */
    bool await_resume()
    {
        if (failure_) {
            std::rethrow_exception(std::exchange(failure_, nullptr));
        }
        if (enqueued_) {
            return true; // Already enqueued under the lock in await_suspend().
        }

        if (state_->capacity == 0) {
            // An unbounded queue never suspends, so the value is enqueued here.
            bool has_popper = false;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->closed) {
                    throw std::logic_error("async::AsyncQueue: push on closed queue");
                }
                detail::enqueueLocked(state_, std::move(value_));
                enqueued_  = true;
                has_popper = !state_->poppers.empty();
            }
            if (has_popper) {
                detail::wakeOnePopper(state_);
            }
            return true;
        }

        // A bounded queue that woke this push: a slot may have been claimed again, so report a
        // retry unless the queue has closed meanwhile (a retry would then rethrow the closure).
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) {
                throw std::logic_error("async::AsyncQueue: push on closed queue");
            }
        }
        return false;
    }

  private:
    std::shared_ptr<AsyncQueueState<T>> state_;
    T                                   value_;
    std::coroutine_handle<>             handle_{};

    /// Why the waiter could not be registered; rethrown by await_resume().
    std::exception_ptr failure_{};

    /// true once await_suspend() enqueued the value itself, so await_resume() has nothing left to do.
    bool enqueued_{ false };
};

} // namespace detail

/**
 * @brief Thread-safe async producer-consumer queue.
 *
 * push() enqueues a value (the returned task must be awaited or discarded);
 * pop() suspends until a value is available. With a non-zero capacity the
 * queue is bounded and push() suspends when full until a popper frees room.
 * close() stops further pushes; poppers of a closed, empty queue throw
 * std::runtime_error, while remaining items can still be drained. Multiple
 * producers and consumers are supported.
 *
 * A non-zero capacity is an exact bound: the room is claimed under the same lock
 * that enqueues, so concurrent pushers cannot overfill the queue.
 *
 * Wake-up timing: a push or pop that hands a peer its chance does not resume that peer on its own
 * stack - it posts the wake-up to the process-wide timer service. A resumed peer can complete a
 * whenAny whose Destroy policy then destroys the push()/pop() that was still returning, so the
 * wake-up has to happen after that stack has unwound. A consequence worth knowing: when pop()
 * returns, the producer it freed is woken shortly afterwards on the timer thread, not before.
 *
 * Failure contract: construction allocates the shared state, so it can throw
 * std::bad_alloc; push() and pop() report a throwing element move as the task's error instead of
 * terminating the process. pop() only requires T to be move-constructible; tryPop() additionally
 * requires move-assignment, because it stores the value into the caller's sink.
 *
 * @tparam T Element type.
 */
template <typename T>
class AsyncQueue {
  public:
    /**
     * @brief Constructs a queue.
     *
     * @note The shared state is heap-allocated, so construction can throw std::bad_alloc; it is
     * deliberately not noexcept, because terminating on a recoverable allocation failure would be
     * the wrong failure mode for a constructor.
     *
     * @param capacity Maximum queued items; 0 means unbounded.
     */
    explicit AsyncQueue(std::size_t capacity = 0)
      : state_(std::make_shared<detail::AsyncQueueState<T>>())
    {
        state_->capacity = capacity;
    }

    AsyncQueue(const AsyncQueue&)            = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;

    /**
     * @brief Enqueues a value, suspending when a bounded queue is full.
     *
     * @param value Value to enqueue; moved into the queue.
     * @return A task completing once the value is enqueued.
     */
    [[nodiscard]]
    Task<void> push(T value)
    {
        // Re-awaited on a spurious wake-up (a slot that was free again was taken by another
        // pusher); the awaiter claims the slot under the lock each time.
        detail::QueuePushAwaiter<T> awaiter{ state_, std::move(value) };
        while (!co_await awaiter) {
        }
    }

    /**
     * @brief Enqueues without suspending when there is room.
     *
     * @param value Value to enqueue; moved into the queue.
     * @return true if enqueued, false if the queue is full or closed.
     */
    [[nodiscard]]
    bool tryPush(T value)
    {
        bool has_popper = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) {
                return false;
            }
            if (state_->capacity != 0 && state_->items.size() >= state_->capacity) {
                return false;
            }
            state_->items.push_back(std::move(value));
            has_popper = !state_->poppers.empty();
        }
        if (has_popper) {
            detail::wakeOnePopper(state_);
        }
        return true;
    }

    /**
     * @brief Removes and returns the next value, suspending if empty.
     *
     * @return A task yielding the next value; throws if closed and empty.
     */
    [[nodiscard]]
    Task<T> pop()
    {
        // Re-awaited on a spurious wake-up (another consumer took the item first).
        detail::QueuePopAwaiter<T> awaiter{ state_ };
        for (;;) {
            std::optional<T> value = co_await awaiter;
            if (value) {
                co_return std::move(*value);
            }
        }
    }

    /**
     * @brief Removes the next value without suspending.
     *
     * @param out Receives the value on success.
     * @return true if a value was removed.
     */
    [[nodiscard]]
    bool tryPop(T& out)
    {
        std::optional<T> value;
        bool             has_pusher = false;
        try {
            value = detail::popOne(state_);
        }
        catch (const std::exception&) {
            return false;
        }
        if (!value) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            has_pusher = !state_->pushers.empty();
        }
        if (has_pusher) {
            detail::wakeOnePusher(state_);
        }
        try {
            out = std::move(*value);
        }
        catch (...) {
            return false;
        }
        return true;
    }

    /**
     * @brief Closes the queue; further pushes throw, poppers drain then stop.
     *
     * Queued waiters are released by one wake-up action on the timer thread, which pops and
     * resumes them one at a time. A resumed waiter whose completion destroys a still-suspended
     * sibling unregisters it under the lock, so the action's next iteration simply does not find
     * it.
     */
    void close() noexcept
    {
        bool has_waiters = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed = true; // No new waiters register once closed.
            has_waiters    = !state_->poppers.empty() || !state_->pushers.empty();
        }
        if (has_waiters) {
            detail::wakeAll(state_);
        }
    }

    /**
     * @brief Returns whether the queue is closed.
     *
     * @return true if close() was called.
     */
    [[nodiscard]]
    bool isClosed() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->closed;
    }

  private:
    std::shared_ptr<detail::AsyncQueueState<T>> state_;
};

VN_ASYNC_NS_END
