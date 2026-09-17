#pragma once

#include "appfw_global.hpp"

#include <coroutine>
#include <functional>

V_APPFW_NS_BEGIN

/**
 * @brief Main-thread marshaller for EventBus Main/Auto delivery.
 *
 * Concrete (non-virtual) Qt-backed implementation, owned by Application and
 * injected into EventBus at construction. The "main thread" is the thread that
 * created the QCoreApplication, i.e. the thread that runs exec(); it is not
 * necessarily the operating system's process main thread.
 */
class V_APPFW_API MainThreadDispatcher {
  public:
    MainThreadDispatcher() = default;
    ~MainThreadDispatcher() = default;

    /**
     * @brief Reports whether the calling thread is the application thread.
     *
     * @return true if the calling thread created the QCoreApplication, false
     *         otherwise (including when no QCoreApplication exists).
     */
    bool isMainThread() const noexcept;

    /**
     * @brief Reports whether tasks can be marshalled to an event loop.
     *
     * @return true while a QCoreApplication exists; its event loop does not have
     *         to be running yet for tasks to be queued.
     */
    bool hasEventLoop() const noexcept;

    /**
     * @brief Queues task for execution on the application thread.
     *
     * Never runs the task on the calling thread and never blocks. The task runs
     * once the event loop gets to it, and is dropped if the event loop stops
     * before that.
     *
     * @param task Task to run on the application thread.
     * @return true if the task was queued, false if it was dropped without running.
     */
    bool postToMain(std::function<void()> task);

    /**
     * @brief Delivers the calls that are already queued for the application thread.
     *
     * Runs only posted method calls (what postToMain() queues), not timers, paint
     * or input events, and never blocks. Only the application thread may deliver
     * them: any other thread gets false and nothing runs, so the calls never
     * execute on the wrong thread.
     *
     * @return true if the calling thread could deliver them (it is the application
     *         thread), false otherwise.
     */
    bool deliverPostedCalls();

    /**
     * @brief Awaitable that moves a coroutine onto the application thread.
     *
     * Returned by resumeOnMainThread(): a command that was resumed on a worker thread (a timer,
     * an IO read) awaits it to reach the application thread again before touching UI.
     */
    class ResumeOnMainThread
    {
      public:
        /**
         * @brief Constructs the awaitable for a dispatcher.
         *
         * @param dispatcher Dispatcher to post the resumption to; may be null.
         */
        explicit ResumeOnMainThread(MainThreadDispatcher* dispatcher) noexcept
          : dispatcher_{ dispatcher }
        {}

        /**
         * @brief Reports whether the coroutine has to suspend at all.
         *
         * No suspension when there is no dispatcher, when there is no event loop to post to
         * (the coroutine would never be resumed, so continuing here is the only option left),
         * or when the calling thread already is the application thread.
         *
         * @return true when the continuation can run without suspending.
         */
        [[nodiscard]] bool await_ready() const noexcept
        {
            return dispatcher_ == nullptr || !dispatcher_->hasEventLoop() || dispatcher_->isMainThread();
        }

        void await_suspend(std::coroutine_handle<> handle) const
        {
            if (!dispatcher_->postToMain([handle] { handle.resume(); })) {
                // The event loop went away between the await_ready() check and the post. Resuming
                // here runs the continuation on this thread, which is what the awaitable exists
                // to avoid - but the alternative is a coroutine that never resumes, and this only
                // happens while the application is going down.
                handle.resume();
            }
        }

        void await_resume() const noexcept {}

      private:
        MainThreadDispatcher* dispatcher_;
    };

    /**
     * @brief Suspends the coroutine until it can continue on the application thread.
     *
     * This is the coroutine-shaped counterpart of postToMain(): use it inside a command before
     * touching UI, because a command resumes on the thread that completed whatever it awaited:
     *
     *     co_await app->mainThreadDispatcher()->resumeOnMainThread();
     *     io->putString(...);   // now on the application thread
     *
     * When there is no event loop (a headless run, or shutdown in progress) the coroutine does
     * not suspend: it continues on the calling thread rather than never running.
     *
     * @return The awaitable to co_await.
     */
    [[nodiscard]] ResumeOnMainThread resumeOnMainThread() noexcept
    {
        return ResumeOnMainThread{ this };
    }
};

V_APPFW_NS_END
