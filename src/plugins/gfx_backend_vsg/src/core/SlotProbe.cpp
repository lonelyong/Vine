#include <vine/vsg/core/SlotProbe.hpp>

VN_VSG_NS_BEGIN

namespace core
{

SlotProbeOutcome probeSlotCount(const std::function<bool(std::uint32_t)>& has_slot, std::uint32_t limit) noexcept
{
    SlotProbeOutcome outcome;
    if (!has_slot)
    {
        outcome.probe_failed = true;
        return outcome;
    }

    // Slot 0 answers first: a task that owns no fences cannot be counted, and "0 slots" must never be
    // confused with "no evidence".
    if (!has_slot(0))
    {
        outcome.probe_failed = true;
        return outcome;
    }

    std::uint32_t count = 0;
    while (count < limit && has_slot(count))
    {
        ++count;
    }

    outcome.slots        = count;
    outcome.probe_failed = false;
    return outcome;
}

bool SlotTracker::observe(const SlotProbeOutcome& outcome) noexcept
{
    if (outcome.probe_failed)
    {
        // Nothing answered: no evidence, no growth, and no claim about the count.
        return known_;
    }

    ++frames_;

    if (!known_ && frames_ > 1 && outcome.slots <= best_)
    {
        // The table stopped being filled: what we hold is the session's real in-flight count.
        known_ = true;
    }
    if (outcome.slots > best_)
    {
        best_ = outcome.slots;
    }
    previous_ = outcome.slots;
    return known_;
}

std::uint32_t SlotTracker::slots() const noexcept
{
    return best_;
}

bool SlotTracker::known() const noexcept
{
    return known_;
}

std::size_t SlotTracker::framesObserved() const noexcept
{
    return frames_;
}

bool SlotTracker::agreesWithAssumption() const noexcept
{
    return known_ && best_ == kAssumedInFlightSlots;
}

}  // namespace core

VN_VSG_NS_END
