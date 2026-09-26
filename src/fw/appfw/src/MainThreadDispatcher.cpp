#include <vine/appfw/MainThreadDispatcher.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QThread>

#include <vine/async/Finally.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/logging/Log.hpp>

VN_APPFW_NS_BEGIN

bool MainThreadDispatcher::isMainThread() const noexcept
{
    const auto* app = QCoreApplication::instance();
    return app != nullptr && QThread::currentThread() == app->thread();
}

bool MainThreadDispatcher::hasEventLoop() const noexcept
{
    return QCoreApplication::instance() != nullptr;
}

bool MainThreadDispatcher::invokeOnMainThread(std::function<void()> task, std::chrono::milliseconds timeout)
{
    if (task == nullptr) {
        return true;
    }
    if (isMainThread()) {
        // Already there: queueing to ourselves and waiting for the loop would block this thread forever.
        task();
        return true;
    }

    struct Handoff {
        std::mutex              mutex;
        std::condition_variable done;
        bool                    ran = false;
    };
    auto handoff = std::make_shared<Handoff>();

    const bool queued = postToMain([handoff, task = std::move(task)] {
        // The flag is raised by a guard: a task that throws must not leave the caller waiting out the whole bound
        // (the exception itself is the queued call's business, exactly as it is for postToMain()).
        const auto raise = vn::async::makeFinally([handoff] {
            {
                std::lock_guard lock(handoff->mutex);
                handoff->ran = true;
            }
            handoff->done.notify_all();
        });
        task();
    });
    if (!queued) {
        VN_LOGW("MainThreadDispatcher: no event loop to delegate a call to; it did not run");
        return false;
    }

    std::unique_lock lock(handoff->mutex);
    if (!handoff->done.wait_for(lock, timeout, [&] { return handoff->ran; })) {
        VN_LOGW("MainThreadDispatcher: the application thread did not run a delegated call within {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        return false;
    }
    return true;
}

bool MainThreadDispatcher::postToMain(std::function<void()> task)
{
    return postToMainThread(std::move(task));
}

bool MainThreadDispatcher::postToMainThread(std::function<void()> task)
{
    auto* app = QCoreApplication::instance();
    if (app == nullptr) {
        return false;  // Nothing to queue onto: the caller decides the fallback.
    }
    return QMetaObject::invokeMethod(app, std::move(task), Qt::QueuedConnection);
}

bool MainThreadDispatcher::deliverPostedCalls()
{
    if (!isMainThread()) {
        return false;
    }
    // MetaCall is what invokeMethod(QueuedConnection) posts, so this drains the
    // queued calls without running timers, paint or input events.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    return true;
}

void MainThreadDispatcher::deliverQueuedApplicationCalls()
{
    Application* app = Application::current();
    if (app == nullptr) {
        return;  // No application: no dispatcher, no queue, nothing to deliver.
    }
    // The dispatcher is created with the application and destroyed with it, so a caller that drives a coroutine while
    // the application is going down may find none - and then there is no queue left to deliver either.
    if (MainThreadDispatcher* dispatcher = app->mainThreadDispatcher(); dispatcher != nullptr) {
        static_cast<void>(dispatcher->deliverPostedCalls());
    }
}

VN_APPFW_NS_END
