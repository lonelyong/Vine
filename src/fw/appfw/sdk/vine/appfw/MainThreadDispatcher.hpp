#pragma once

#include "appfw_global.hpp"

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
};

V_APPFW_NS_END
