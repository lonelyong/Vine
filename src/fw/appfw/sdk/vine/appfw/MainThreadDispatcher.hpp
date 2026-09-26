#pragma once

#include "appfw_global.hpp"

#include <chrono>
#include <coroutine>
#include <functional>
#include <utility>

#include <vine/async/RunToCompletion.hpp>
#include <vine/async/Task.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Main-thread marshaller: one entry point per question about the application thread.
 *
 * Concrete (non-virtual) Qt-backed implementation, owned by Application and injected into EventBus at construction. The
 * "main thread" is the thread that created the QCoreApplication, i.e. the thread that runs exec(); it is not
 * necessarily the operating system's process main thread.
 *
 * Every entry point answers one question and only one, and the names say which:
 *
 * - isMainThread() / hasEventLoop()  - what thread is this, and is there a queue to reach the other one;
 * - postToMainThread()               - hand a call over and forget it (the queued spelling);
 * - invokeOnMainThread()             - lend the application thread a piece of work and wait for it (bounded);
 * - resumeOnMainThread()             - give this coroutine up, so the rest of it continues over there;
 * - deliverPostedCalls()             - run what is already queued (the pump; application thread only);
 * - runToCompletion()                - drive a coroutine that needs that pump, while this thread blocks.
 *
 * All of them but the pump are static. A worker thread has no marshaller to hold - the object belongs to the
 * application and dies with it, so a call made on a worker through a stale one is a use-after-free - and none of these
 * needs one anyway. deliverPostedCalls() stays an instance method on purpose: it is reached through the marshaller the
 * EventBus was handed, which is also how a bus built without one keeps delivering on the publishing thread.
 */
class VN_APPFW_API MainThreadDispatcher {
  public:
    MainThreadDispatcher() = default;
    ~MainThreadDispatcher() = default;

    /**
     * @brief Reports whether the calling thread is the application thread.
     *
     * @return true if the calling thread created the QCoreApplication, false
     *         otherwise (including when no QCoreApplication exists).
     */
    static bool isMainThread() noexcept;

    /**
     * @brief Reports whether tasks can be marshalled to an event loop.
     *
     * @return true while a QCoreApplication exists; its event loop does not have
     *         to be running yet for tasks to be queued.
     */
    static bool hasEventLoop() noexcept;

    /**
     * @brief Queues task for execution on the application thread, without holding a marshaller.
     *
     * Never runs the task on the calling thread and never blocks. The task runs once the event loop gets to it, and is
     * dropped if the event loop stops before that.
     *
     * @param task Task to run on the application thread.
     * @return true if the task was queued, false if it was dropped without running.
     */
    static bool postToMainThread(std::function<void()> task);

    /**
     * @brief Runs task ON the application thread and returns once it has run: a blocking delegation.
     *
     * What a worker thread needs when part of its work belongs to the application thread - building widgets, creating
     * graphics objects, touching the render control - and what comes AFTER that part depends on it: the callable runs
     * there, this thread waits for it, and the caller keeps its own thread for the rest. It is the synchronous
     * counterpart of postToMainThread(), and the contrast with resumeOnMainThread() is the point: that one gives the
     * calling thread UP (the rest of the coroutine continues on the application thread), this one only LENDS the other
     * thread the piece that must run there.
     *
     * Called on the application thread it simply runs the task inline: queueing to itself and waiting would deadlock.
     *
     * THE WAIT IS BOUNDED, and only a worker may do it: the application thread must never block on a worker (that stops
     * the loop, and this call from the application thread could never complete). When the bound passes, the task may
     * still be queued - the call logs it and reports false, and the caller must not assume the work happened.
     *
     * @param task    Task to run on the application thread.
     * @param timeout How long to wait for it (bounded on purpose; see above).
     * @return true if the task ran (inline or on the application thread), false when it could not be handed over or the
     *         wait ran out.
     */
    static bool invokeOnMainThread(std::function<void()> task, std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * @brief Delivers the calls that are already queued for the application thread.
     *
     * Runs only posted method calls (what postToMainThread() queues), not timers, paint
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
        ResumeOnMainThread() noexcept = default;

        /**
         * @brief Reports whether the coroutine has to suspend at all.
         *
         * No suspension when there is no event loop to post to (the coroutine would never be resumed, so continuing
         * here is the only option left) or when the calling thread already is the application thread.
         *
         * @return true when the continuation can run without suspending.
         */
        [[nodiscard]] bool await_ready() const noexcept
        {
            return !hasEventLoop() || isMainThread();
        }

        void await_suspend(std::coroutine_handle<> handle) const
        {
            if (!postToMainThread([handle] { handle.resume(); })) {
                // The event loop went away between the await_ready() check and the post. Resuming
                // here runs the continuation on this thread, which is what the awaitable exists
                // to avoid - but the alternative is a coroutine that never resumes, and this only
                // happens while the application is going down.
                handle.resume();
            }
        }

        void await_resume() const noexcept {}
    };

    /**
     * @brief Suspends the coroutine until it can continue on the application thread.
     *
     * This is the coroutine-shaped counterpart of postToMainThread(): use it inside a command before
     * touching UI, because a command resumes on the thread that completed whatever it awaited:
     *
     *     co_await MainThreadDispatcher::resumeOnMainThread();
     *     io->putString(...);   // now on the application thread
     *
     * When there is no event loop (a headless run, or shutdown in progress) the coroutine does
     * not suspend: it continues on the calling thread rather than never running.
     *
     * @return The awaitable to co_await.
     */
    [[nodiscard]] static ResumeOnMainThread resumeOnMainThread() noexcept
    {
        return ResumeOnMainThread{};
    }

    /**
     * @brief Runs a coroutine to completion on the calling thread, delivering the calls it queues for the application
     *        thread while it waits.
     *
     * The application-thread flavour of vn::async::runToCompletion() - read that for the mechanics, and for why a plain
     * Task::result() deadlocks here (a coroutine waiting to be resumed by the application thread has to have its queued call
     * delivered by whoever is driving it). The pump delivers the calls queued for the application thread, so the step the
     * coroutine left there runs while the caller waits; timers, paint and input still do not.
     *
     * Why not simply read the result (vn::async::Task::result(), which blocks and leaves the trap to its caller): because
     * the load has to START on the application thread - plugin hooks build widgets and graphics objects - and once that
     * thread blocks inside itself there is nobody left to deliver the step a pool-hopping hook posts back to it. The pump
     * is not an optimisation here; it is the only candidate.
     *
     * What it is for: a blocking API that has to drive a coroutine whose next step belongs to the application thread -
     * the plugin lifecycle hooks are the case in point. What it costs: the calling thread is blocked for as long as the
     * coroutine takes, which on the application thread is exactly as long as the loop stands still - so a host that runs
     * a loop should await the coroutine instead of calling this.
     *
     * @param task Coroutine to drive to completion on the calling thread.
     * @return What the coroutine produced.
     */
    template<typename T>
    static T runToCompletion(vn::async::Task<T> task)
    {
        return vn::async::runToCompletion(std::move(task), [] { deliverQueuedApplicationCalls(); });
    }

  private:
    /**
     * @brief Delivers the calls queued for the application thread, when there is an application and this thread may
     *        deliver them.
     *
     * The pump of runToCompletion(): without an application there is no queue to deliver, and from a thread that is not
     * the application thread deliverPostedCalls() refuses to run anything.
     */
    static void deliverQueuedApplicationCalls();
};

VN_APPFW_NS_END
