#include <vine/vsg/core/Observe.hpp>

VN_VSG_NS_BEGIN

namespace core
{

FrameCounters& Observe::counters() noexcept
{
    return counters_;
}

RetentionStats& Observe::retention() noexcept
{
    return retention_;
}

const FrameCounters& Observe::counters() const noexcept
{
    return counters_;
}

const RetentionStats& Observe::retention() const noexcept
{
    return retention_;
}

bool Observe::agreesWith(const RetirementQueue& queue) const noexcept
{
    // The queue owns the truth about what it released and how often it had to stop the device; the
    // retention aggregate is what the rest of the code reads. A disagreement means a number was
    // incremented in one place and forgotten in the other - the failure mode that makes a counter
    // useless (it says "no leaks" because nobody updates it).
    return retention_.released_nodes == queue.released() && retention_.device_waits == queue.deviceWaits();
}

}  // namespace core

VN_VSG_NS_END
