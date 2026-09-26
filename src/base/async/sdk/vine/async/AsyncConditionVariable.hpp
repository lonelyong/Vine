#pragma once

#include "async_global.hpp"

#include <cassert>
#include <coroutine>
#include <memory>
#include <mutex>
#include <utility>

#include "AsyncMutex.hpp"
#include "Task.hpp"

VN_ASYNC_NS_BEGIN

class AsyncConditionVariable;

namespace detail {

class CvWaiter;

/**
 * @brief Shared state of an AsyncConditionVariable.
 *
 * The waiter list is intrusive (nodes live in coroutine frames), so awaiting
 * never allocates and there is no bad_alloc in the noexcept await path.
 * notified is a sticky flag that preserves a notification issued while no
 * waiter was registered; see AsyncConditionVariable for the exact semantics.
 */
struct CvState
{
    std::mutex mutex;
    CvWaiter* head{ nullptr };
    CvWaiter* tail{ nullptr };
    bool notified{ false };
};

/**
 * @brief Intrusive waiter node for co_await cv.wait(mutex).
 *
 * Lives in the waiting coroutine's frame; created fresh per wait and never
 * reused. On destruction it unregisters itself from the list, so abandoning
 * the wait is safe. queued_ distinguishes "in the list" from "detached": a
 * detached node's destructor never touches the list, so notify can detach and
 * resume outside the lock without racing a concurrent destructor
 * (list-structure safety). Coroutine frame lifetime follows the framework
 * contract in async_global.hpp.
 */
class CvWaiter
{
  public:
    explicit CvWaiter(std::shared_ptr<CvState> state) noexcept : state_(std::move(state)) {}

    CvWaiter(const CvWaiter&) = delete;
    CvWaiter& operator=(const CvWaiter&) = delete;
    CvWaiter(CvWaiter&&) = delete;
    CvWaiter& operator=(CvWaiter&&) = delete;

    ~CvWaiter() noexcept;

    /**
     * @brief Always defers to await_suspend for a single locked decision.
     *
     * @return false.
     */
    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return false;
    }

    /**
     * @brief Consumes a pending notification or registers and suspends, all
     * under one lock.
     *
     * @param h The awaiting coroutine; stored to resume it on notify.
     * @return false to resume inline when a pending notification was consumed.
     */
    bool await_suspend(std::coroutine_handle<> h) noexcept;

    void await_resume() const noexcept {}

  private:
    friend struct CvState;
    friend class vn::async::AsyncConditionVariable;

    std::shared_ptr<CvState> state_;

    /// The awaiting coroutine; non-null while waiting or being resumed.
    std::coroutine_handle<> handle_{};

    /// true while this node is linked into the waiter list.
    bool queued_{ false };

    /// List links; non-null while queued.
    CvWaiter* next_{ nullptr };
    CvWaiter* prev_{ nullptr };
};

inline bool CvWaiter::await_suspend(std::coroutine_handle<> h) noexcept
{
    assert(h);
    handle_ = h;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->notified)
    {
        state_->notified = false;
        return false; // A pending notification was consumed; do not suspend.
    }
    queued_ = true;
    prev_ = state_->tail;
    next_ = nullptr;
    if (state_->tail)
    {
        state_->tail->next_ = this;
    }
    else
    {
        state_->head = this;
    }
    state_->tail = this;
    return true;
}

inline CvWaiter::~CvWaiter() noexcept
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (queued_)
    {
        if (prev_)
        {
            prev_->next_ = next_;
        }
        else
        {
            assert(state_->head == this);
            state_->head = next_;
        }
        if (next_)
        {
            next_->prev_ = prev_;
        }
        else
        {
            assert(state_->tail == this);
            state_->tail = prev_;
        }
        queued_ = false;
        prev_ = nullptr;
        next_ = nullptr;
    }
}

} // namespace detail

/**
 * @brief Async condition variable used with AsyncMutex.
 *
 * wait(mutex) releases the mutex, suspends until notified, then re-acquires
 * the mutex before returning; the caller must hold the mutex and re-check its
 * predicate in a loop, as with std::condition_variable.
 *
 * Notification semantics: unlike std::condition_variable, a notify issued
 * while no waiter is registered is NOT a no-op — it is preserved by a sticky
 * flag and consumed by the next waiter. This deviation is structurally
 * necessary: with a separate AsyncMutex, "release the mutex and register the
 * wait" cannot be atomic, and dropping the notification would deadlock a
 * waiter whose notify arrived between its unlock and its registration. Under
 * the standard predicate-loop usage the extra wakeup is benign (std::condition_variable
 * already permits spurious wakeups).
 *
 * Lifecycle (framework contract, see async_global.hpp): the internal mutex
 * protects the notified flag and the intrusive waiter list. The list stores
 * raw waiter pointers living in coroutine frames, so:
 *   - the CV must outlive every coroutine awaiting it;
 *   - a coroutine must not be destroyed concurrently with the notify that
 *     resumes it (no concurrent destroy during the resume window);
 *   - resuming a waiter can synchronously destroy sibling waiters (a whenAny
 *     winner destroys the still-suspended losers on completion), so notify
 *     never holds a waiter pointer across a resume;
 *   - abandoning a wait is safe: the waiter unregisters itself (queued_).
 * queued_ protects the LIST against concurrent destruction, not the frame.
  *
 * Threading: notify_one()/notify_all() may be called from any thread, and the waiters they wake
 * are resumed on the notifying thread. The condition variable must outlive every waiting
 * coroutine; the predicate and the mutex stay the caller's (see the loop rule above).
 */
class AsyncConditionVariable
{
  public:
    AsyncConditionVariable() noexcept = default;

    AsyncConditionVariable(const AsyncConditionVariable&) = delete;
    AsyncConditionVariable& operator=(const AsyncConditionVariable&) = delete;

    /**
     * @brief Releases the mutex and suspends until notified, then re-acquires.
     *
     * @param mutex Mutex the caller holds; re-acquired before returning.
     * @return A task completing when notified and the mutex is re-held.
     */
    [[nodiscard]]
    Task<void> wait(AsyncMutex& mutex)
    {
        mutex.unlock();
        co_await detail::CvWaiter{ state_ };
        co_await mutex.lock();
    }

    /**
     * @brief Resumes one waiting coroutine, or records a pending notification.
     */
    void notify_one() noexcept
    {
        detail::CvWaiter* w = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->head)
            {
                w = state_->head;
                state_->head = w->next_;
                if (!state_->head)
                {
                    state_->tail = nullptr;
                }
                else
                {
                    state_->head->prev_ = nullptr;
                }
                w->next_ = nullptr;
                w->prev_ = nullptr;
                w->queued_ = false;
            }
            else
            {
                state_->notified = true; // Preserve the notification.
            }
        }
        if (w)
        {
            assert(w->handle_);
            w->handle_.resume(); // Resume outside the lock.
        }
    }

    /**
     * @brief Resumes every waiting coroutine, or records a pending notification.
     *
     * Waiters are popped and resumed one at a time; this never holds a waiter
     * pointer across a resume. That is essential because resuming a waiter
     * can synchronously destroy another waiter: if the resumed waiter is the
     * winner of a whenAny, its completion destroys the still-suspended
     * siblings (structured concurrency). A batch loop that detached the whole
     * list first would then resume a dangling handle for each destroyed
     * sibling; popping one at a time lets a destroyed sibling's destructor
     * unregister it under the lock, so the next iteration simply finds it
     * gone.
     *
     * The sticky flag is armed in the SAME critical section as the emptiness
     * check. Arming it afterwards lets a waiter slip into the gap: it
     * registers (sees no sticky), then the sticky is set, and it is never
     * resumed — a lost wakeup. With the atomic check-and-arm, a waiter that
     * registers during this notify either finds a non-empty list (and is
     * popped) or consumes the sticky flag; it can never be lost.
     */
    void notify_all() noexcept
    {
        bool woke_any = false;
        for (;;)
        {
            detail::CvWaiter* w = nullptr;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->head)
                {
                    woke_any = true;
                    w = state_->head;
                    state_->head = w->next_;
                    if (!state_->head)
                    {
                        state_->tail = nullptr;
                    }
                    else
                    {
                        state_->head->prev_ = nullptr;
                    }
                    w->next_ = nullptr;
                    w->prev_ = nullptr;
                    w->queued_ = false;
                }
                else
                {
                    if (!woke_any)
                    {
                        state_->notified = true; // Preserve the notification.
                    }
                    break;
                }
            }
            assert(w && w->handle_);
            w->handle_.resume(); // Resume outside the lock.
        }
    }

  private:
    std::shared_ptr<detail::CvState> state_{ std::make_shared<detail::CvState>() };
};

VN_ASYNC_NS_END
