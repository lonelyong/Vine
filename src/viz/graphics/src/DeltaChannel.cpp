#include "DeltaChannel.hpp"

#include <algorithm>
#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

DeltaChannel::DeltaChannel(std::size_t capacity)
  : capacity_(std::max<std::size_t>(1U, capacity))
{
    // Reserved once, here: a steady-state frame only moves deltas in and out of storage that already exists.
    pending_.reserve(capacity_);
}

bool DeltaChannel::post(const Delta& delta)
{
    return post(delta, std::shared_ptr<const SceneBake>{});
}

bool DeltaChannel::post(const Delta& delta, std::shared_ptr<const SceneBake> rows)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() >= capacity_) {
        ++refused_;
        return false; // full: refused and counted, never dropped quietly (see the class note)
    }
    pending_.push_back(OwnedDelta{ delta, std::move(rows) });
    ++posted_;
    return true;
}

void DeltaChannel::drain(std::vector<OwnedDelta>& into)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // THREE SWAPS, NO COPY. `into` holds the batch the caller has finished with, so its storage is exactly the spare
    // buffer the producer needs next; the pending deltas go the other way. Both sides keep one buffer each, which is
    // what makes a drain allocation-free after the first frame.
    std::vector<OwnedDelta> finished;
    finished.swap(into);
    finished.clear(); // the caller said it was done with these; only the storage is reused
    into.swap(pending_);
    pending_.swap(finished);
}

std::size_t DeltaChannel::pending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::uint64_t DeltaChannel::posted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return posted_;
}

std::uint64_t DeltaChannel::refused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return refused_;
}

std::size_t DeltaChannel::capacity() const noexcept
{
    return capacity_;
}

} // namespace detail

VN_GRAPHICS_NS_END
