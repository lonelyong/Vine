#include <vine/vsg/core/RetirementQueue.hpp>

#include <utility>

VN_VSG_NS_BEGIN

namespace core
{

RetirementQueue::RetirementQueue(std::uint32_t slots)
  : slots_(slots)
{
    // No clamping and no default: a session that could not learn the count passes 0, and 0 means
    // parking is unavailable rather than "the shortest window". Guessing here would be silent in the
    // direction that destroys an object a submitted command buffer still names.
}

bool RetirementQueue::retire(const FrameTimeline& timeline, ReleaseFn release)
{
    if (!release || !parkingAvailable())
    {
        return false;
    }
    parked_.push_back(Entry{retirePoint(timeline), std::move(release)});
    return true;
}

bool RetirementQueue::parkingAvailable() const noexcept
{
    return slots_ != 0;
}

void RetirementQueue::setSlots(std::uint32_t slots) noexcept
{
    slots_ = slots;
}

void RetirementQueue::advance(const FrameTimeline& timeline)
{
    const std::uint64_t completed = timeline.completedFrame();

    // Partition first, release second: a release callback is allowed to park something new, and doing
    // that while iterating the same vector would invalidate the walk.
    std::vector<Entry> due;
    auto               keep = parked_.begin();
    for (auto& entry : parked_)
    {
        if (entry.retire_at <= completed)
        {
            due.push_back(std::move(entry));
        }
        else
        {
            *keep++ = std::move(entry);
        }
    }
    parked_.erase(keep, parked_.end());

    released_ += due.size();
    for (auto& entry : due)
    {
        entry.release();
    }
}

std::uint32_t RetirementQueue::slots() const noexcept
{
    return slots_;
}

std::uint64_t RetirementQueue::retirePoint(const FrameTimeline& timeline) const noexcept
{
    return FrameTimeline::retirePoint(timeline.submittedFrame(), slots_);
}

std::size_t RetirementQueue::pending() const noexcept
{
    return parked_.size();
}

std::size_t RetirementQueue::released() const noexcept
{
    return released_;
}

std::size_t RetirementQueue::deviceWaits() const noexcept
{
    return device_waits_;
}

void RetirementQueue::noteDeviceWait() noexcept
{
    ++device_waits_;
}

}  // namespace core

VN_VSG_NS_END
