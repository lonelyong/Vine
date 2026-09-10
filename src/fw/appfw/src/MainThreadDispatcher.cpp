#include <vine/appfw/MainThreadDispatcher.hpp>

#include <utility>

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QThread>

V_APPFW_NS_BEGIN

bool MainThreadDispatcher::isMainThread() const noexcept
{
    const auto* app = QCoreApplication::instance();
    return app != nullptr && QThread::currentThread() == app->thread();
}

bool MainThreadDispatcher::hasEventLoop() const noexcept
{
    return QCoreApplication::instance() != nullptr;
}

bool MainThreadDispatcher::postToMain(std::function<void()> task)
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

V_APPFW_NS_END
