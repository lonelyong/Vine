#include "Scheduler.hpp"

#include <QMetaObject>

namespace vn {
namespace appfw {
namespace async {

Scheduler::Scheduler(QObject* parent)
    : QObject(parent)
{
}

Scheduler::~Scheduler() = default;

void Scheduler::postResume(std::coroutine_handle<> h)
{
    // Queued invocation runs on this object's thread once its event loop turns.
    QMetaObject::invokeMethod(this, [h] { h.resume(); }, Qt::QueuedConnection);
}

} // namespace async
} // namespace appfw
} // namespace vine
