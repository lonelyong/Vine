#include <vine/graphics/DeviceClock.hpp>

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

FrameDriverId DeviceClock::addDriver() noexcept
{
    // IDENTITY ONLY (see the header): the number is never reused, so an id that outlived its driver answers "unknown"
    // instead of naming another driver's tables - which is why the vector only ever grows.
    const auto id = next_driver_++;
    drivers_.push_back(true);
    return id;
}

void DeviceClock::removeDriver(const FrameDriverId driver) noexcept
{
    if (driver < drivers_.size()) {
        drivers_[driver] = false;
    }
}

std::size_t DeviceClock::drivers() const noexcept
{
    return static_cast<std::size_t>(std::count(drivers_.begin(), drivers_.end(), true));
}

FrameToken DeviceClock::begin() noexcept
{
    return timeline_.begin();
}

void DeviceClock::submitted(FrameToken token) noexcept
{
    timeline_.submitted(token);
}

void DeviceClock::abandoned(FrameToken token) noexcept
{
    timeline_.abandoned(token);
}

FrameTimeline& DeviceClock::timeline() noexcept
{
    return timeline_;
}

const FrameTimeline& DeviceClock::timeline() const noexcept
{
    return timeline_;
}

std::uint64_t DeviceClock::noteSubmission() noexcept
{
    // ASCENDING BY CONSTRUCTION: the caller holds the queue's external-synchronization lock, so these numbers are the
    // order the queue receives the submissions (see the header).
    ++submissions_;
    unproven_.push_back(submissions_);
    return submissions_;
}

void DeviceClock::noteCompletion(const std::uint64_t submission) noexcept
{
    const auto found = std::find(unproven_.begin(), unproven_.end(), submission);
    if (found == unproven_.end()) {
        // A submission that was never numbered, or one already proven: completion is a fact about the past that a
        // second report cannot change.
        return;
    }
    unproven_.erase(found);
    updateWatermark();
}

std::uint64_t DeviceClock::submissions() const noexcept
{
    return submissions_;
}

std::uint64_t DeviceClock::completedSubmission() const noexcept
{
    return completed_;
}

void DeviceClock::updateWatermark() noexcept
{
    // THE OLDEST UNPROVEN SUBMISSION DECIDES: every number below it is complete (the queue proved each one or it would
    // still be in the list), and the ones above it prove nothing about it. With nothing unproven, every number handed
    // out is complete - which is the answer a device reaches as soon as its last submission's fence signalled.
    const std::uint64_t floor = unproven_.empty() ? submissions_ : unproven_.front() - 1U;
    completed_                = std::max(completed_, floor);
}

VN_GRAPHICS_NS_END
