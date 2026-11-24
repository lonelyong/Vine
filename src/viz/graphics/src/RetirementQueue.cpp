#include <vine/graphics/RetirementQueue.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

RetirementQueue::RetirementQueue(std::uint32_t slots)
  : slots_(slots)
{
    // No clamping and no default: a session that could not learn the count passes 0, and 0 means
    // parking is unavailable rather than "the shortest window". Guessing here would be silent in the
    // direction that destroys an object a submitted command buffer still names.
}

bool RetirementQueue::retire(const DeviceClock& clock, ReleaseFn release)
{
    if (!release || !parkingAvailable()) {
        return false;
    }
    parked_.push_back(Entry{ retirePoint(clock), std::move(release) });
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

void RetirementQueue::advance(const DeviceClock& clock)
{
    const std::uint64_t completed = clock.completedSubmission();

    // Partition first, release second: a release callback is allowed to park something new, and doing
    // that while iterating the same vector would invalidate the walk.
    std::vector<Entry> due;
    auto               keep = parked_.begin();
    for (auto& entry : parked_) {
        if (entry.retire_at <= completed) {
            due.push_back(std::move(entry));
        }
        else {
            *keep++ = std::move(entry);
        }
    }
    parked_.erase(keep, parked_.end());

    released_ += due.size();
    for (auto& entry : due) {
        entry.release(&clock);
    }
}

std::uint32_t RetirementQueue::slots() const noexcept{
    return slots_;
}

void RetirementQueue::releaseAll()
{
    // TAKEN OUT FIRST, LIKE advance() TAKES ITS DUE SET: a release callback may park something new, and running the
    // callbacks while iterating the vector they push into would invalidate the walk.
    std::vector<Entry> due = std::move(parked_);
    parked_.clear();

    released_ += due.size();
    for (auto& entry : due) {
        entry.release(nullptr);
    }
}

std::uint64_t RetirementQueue::retirePoint(const DeviceClock& clock) const noexcept
{
    // ONE MORE THAN THE SLOT COUNT: by then the slot that could still name the object has been re-recorded, and a
    // re-record waits that slot's fence - so the submission that named it is provably complete.
    return clock.submissions() + static_cast<std::uint64_t>(slots_) + 1U;
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

VN_GRAPHICS_NS_END
