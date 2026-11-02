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
 * Hand-off rule: an operation that gives a peer its chance (a pop freeing a slot, a push handing
 * over an item) first suspends at WakePeerAndContinue; only then does a timer-thread action wake
 * the peer and resume the operation. Posting the wake alone is not enough - the operation would
 * still be running on its own thread when the peer completes a whenAny and its Destroy policy
 * destroys that operation (or a frame above it). Suspended first, this coroutine is destroyed at
 * a legal point instead.
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
 * @brief Which waiter list a handing-off operation wakes.
 */
enum class PeerKind
{
    Poppers, // push() handed an item over: a waiting consumer can run.
    Pushers  // pop() freed a slot: a waiting producer can run.
};

/**
 * @brief Wakes one waiting peer, but only after this operation has suspended.
 *
 * @details
 * - The operation suspends here first. One posted action then wakes one peer on the timer thread
 *   and resumes this coroutine only if the peer did not destroy it.
 * - Why not wake from the operation itself: a resumed peer can complete a whenAny whose Destroy
 *   policy destroys this operation, or a frame above it. By the time the peer runs, this coroutine
 *   must be suspended; the peer's destruction then hits a coroutine frame that is at a suspension
 *   point, which is exactly the state Destroy requires. Posting the wake to the timer is not
 *   enough on its own - the operation would still be running on its own thread.
 * - The destructor unregisters the handle, so a destroyed operation is never resumed. The posted
 *   action still wakes the peer: the slot or item it handed over must not be lost with it.
 *
 * @tparam T Element type of the queue.
 */
template <typename T>
class WakePeerAndContinue {
  public:
    /**
     * @brief Prepares the hand-off; nothing is posted until the coroutine suspends.
     *
     * @param state Queue state whose waiter list is woken.
     * @param kind Which waiter list to wake when the action runs.
     */
    WakePeerAndContinue(std::shared_ptr<AsyncQueueState<T>> state, PeerKind kind)
      : work_(std::make_shared<Work>(std::move(state), kind))
    {}

    WakePeerAndContinue(const WakePeerAndContinue&)            = delete;
    WakePeerAndContinue& operator=(const WakePeerAndContinue&) = delete;

    ~WakePeerAndContinue() noexcept
    {
        std::lock_guard<std::mutex> lock(work_->mutex);
        work_->waiter = {};
    }

    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return false;
    }

    /**
     * @brief Registers this coroutine and posts the hand-off, then suspends.
     *
     * @param h Coroutine to resume after the peer has had its turn.
     * @return true; the peer action always runs before this coroutine resumes.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(work_->mutex);
            work_->waiter = h;
        }
        TimerService::instance().postWake(work_, &WakePeerAndContinue<T>::run);
        return true;
    }

    void await_resume() const noexcept
    {}

  private:
    /// Hand-off shared with the posted action: the queue state plus this coroutine's registration.
    struct Work {
        /**
         * @brief Binds one hand-off to its queue state and waiter list.
         *
         * @param state_ Queue state whose waiter list is woken.
         * @param kind_ Which waiter list to wake when the action runs.
         */
        Work(std::shared_ptr<AsyncQueueState<T>> state_, PeerKind kind_) noexcept
          : state(std::move(state_))
          , kind(kind_)
        {}

        Work(const Work&)            = delete;
        Work& operator=(const Work&) = delete;

        std::shared_ptr<AsyncQueueState<T>> state;
        PeerKind                            kind;
        std::mutex                          mutex;
        std::coroutine_handle<>             waiter{};
    };

    /// Runs on the timer thread: wake one peer, then resume this operation if it is still alive.
    static void run(void* p) noexcept
    {
        auto& work = *static_cast<Work*>(p);

        std::coroutine_handle<> peer{};
        {
            std::lock_guard<std::mutex> lock(work.state->mutex);
            auto&                       list = (work.kind == PeerKind::Poppers) ? work.state->poppers : work.state->pushers;
            if (!list.empty()) {
                peer = list.front();
                list.erase(list.begin());
            }
        }
        if (peer) {
            peer.resume(); // May destroy the suspended operation; it unregisters itself.
        }

        std::coroutine_handle<> self{};
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            self = std::exchange(work.waiter, {});
        }
        if (self) {
            self.resume();
        }
    }

    std::shared_ptr<Work> work_;
};

/**
 * @brief Posts a wake-up that resumes one waiting popper, on the timer thread.
 *
 * For the non-suspending operations (tryPush/tryPop/close), which cannot put themselves at a
 * suspension point first. A coroutine that may be a Destroy composition's loser should use the
 * suspending form instead, so the destruction it triggers hits a suspended frame.
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
     * - Taking an item frees a slot for a bounded queue; whether a pusher was waiting is reported
     *   through wokePeer(), and pop() hands the slot over by suspending at WakePeerAndContinue -
     *   not from here, where the peer's completion could destroy this still-running coroutine.
     *
     * @return The value, or nullopt when there was nothing to take and the caller must re-await.
     */
    std::optional<T> await_resume()
    {
        if (failure_) {
            std::rethrow_exception(std::exchange(failure_, nullptr));
        }

        std::optional<T> value;
        bool             closed = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->items.empty()) {
                value.emplace(std::move(state_->items.front()));
                state_->items.pop_front();
                woke_peer_ = !state_->pushers.empty();
            }
            else {
                closed = state_->closed;
            }
        }

        if (value) {
            return value;
        }
        if (closed) {
            throw std::runtime_error("async::AsyncQueue: closed");
        }
        return std::nullopt;
    }

    /**
     * @brief Reports whether taking the value freed a slot a bounded pusher was waiting for.
     *
     * @return true when the caller must wake a pusher before it finishes.
     */
    [[nodiscard]]
    bool wokePeer() const noexcept
    {
        return woke_peer_;
    }

  private:
    std::shared_ptr<AsyncQueueState<T>> state_;
    std::coroutine_handle<>             handle_{};

    /// Why the waiter could not be registered; rethrown by await_resume().
    std::exception_ptr failure_{};

    /// true when await_resume() took an item while a bounded pusher was parked.
    bool woke_peer_{ false };
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
     * - A waiting popper is only recorded here; push() hands the item over by suspending at
     *   WakePeerAndContinue, never from this still-running frame.
     *
     * @param h Coroutine to park when the queue is full.
     * @return false to resume inline, either because the value was enqueued or to report a failure.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        try {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) {
                return false; // await_resume() reports the closed queue.
            }
            if (state_->capacity == 0 || state_->items.size() < state_->capacity) {
                detail::enqueueLocked(state_, std::move(value_));
                enqueued_  = true;
                woke_peer_ = !state_->poppers.empty();
            }
            else {
                return detail::tryRegisterWaiter(state_->pushers, h, failure_);
            }
        }
        catch (...) {
            failure_ = std::current_exception();
            return false; // await_resume() rethrows the enqueue failure.
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
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->closed) {
                    throw std::logic_error("async::AsyncQueue: push on closed queue");
                }
                detail::enqueueLocked(state_, std::move(value_));
                enqueued_  = true;
                woke_peer_ = !state_->poppers.empty();
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

    /**
     * @brief Reports whether this push enqueued the value while a popper was waiting.
     *
     * @return true when the caller must wake a popper before it finishes.
     */
    [[nodiscard]]
    bool wokePeer() const noexcept
    {
        return woke_peer_;
    }

  private:
    std::shared_ptr<AsyncQueueState<T>> state_;
    T                                   value_;
    std::coroutine_handle<>             handle_{};

    /// Why the waiter could not be registered; rethrown by await_resume().
    std::exception_ptr failure_{};

    /// true once await_suspend() enqueued the value itself, so await_resume() has nothing left to do.
    bool enqueued_{ false };

    /// true when the value was enqueued while a popper was parked.
    bool woke_peer_{ false };
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
 * Wake-up timing: a push or pop that hands a peer its chance suspends first, then lets a
 * timer-thread action wake the peer and resume it. A resumed peer can complete a whenAny whose
 * Destroy policy destroys the push()/pop() that handed it the slot, and a coroutine can only be
 * destroyed safely while it is suspended. A consequence worth knowing: pop() and push() complete
 * only after the peer they freed has had its turn, and a Destroy loser may be destroyed at that
 * point instead - its value (pop) is then discarded with it. The non-suspending forms
 * (tryPush/tryPop/close) cannot put themselves at a suspension point; they only post the wake, so a
 * caller that can itself be a Destroy loser should use the suspending form.
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
        if (awaiter.wokePeer()) {
            // Hand the item to a waiting consumer and stay suspended until it has had its turn:
            // a consumer that wins a whenAny destroys its losers, and this coroutine has to be
            // suspended - a legal destruction point - when that happens.
            co_await detail::WakePeerAndContinue<T>{ state_, detail::PeerKind::Poppers };
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
                if (awaiter.wokePeer()) {
                    // Free a bounded producer's slot and stay suspended until it has had its turn:
                    // a producer that wins a whenAny destroys its losers, and this coroutine has to
                    // be suspended - a legal destruction point - when that happens. A destroyed
                    // pop has already taken its value; Destroy discards it with the task.
                    co_await detail::WakePeerAndContinue<T>{ state_, detail::PeerKind::Pushers };
                }
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
