#include <vine/vsg/core/Streams.hpp>

#include <algorithm>

VN_VSG_NS_BEGIN

namespace core
{

bool StreamKey::operator==(const StreamKey& other) const noexcept
{
    return kind == other.kind && location == other.location && components == other.components &&
           buffer == other.buffer && revision == other.revision && offset == other.offset && count == other.count;
}

bool StreamKey::derived() const noexcept
{
    return buffer == nullptr;
}

std::size_t StreamKeyHash::operator()(const StreamKey& key) const noexcept
{
    // A plain combine over the fields that make up the identity. The pointer is mixed as an integer, which
    // is all a hash may assume about it (the identity comparison is what decides equality).
    std::size_t hash = static_cast<std::size_t>(key.kind);
    const auto mix   = [&hash](std::uint64_t value) noexcept {
        hash ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };
    mix(reinterpret_cast<std::uintptr_t>(key.buffer));
    mix(key.revision);
    mix(key.offset);
    mix(key.count);
    mix(key.location);
    mix(key.components);
    return hash;
}

SharedStreams::SharedStreams(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1U))
{
}

SharedStreams::Decision SharedStreams::acquire(const StreamKey& key, std::uint64_t frame)
{
    const auto found = index_.find(key);
    if (found != index_.end()) {
        // Named again: the grace window restarts. This is the whole of "how many readers it has" - an entry
        // is alive because a frame named it, not because a tally said so (see the class note).
        found->second->last_frame = frame;
        ++aliases_;
        return {Action::Alias, std::nullopt};
    }

    order_.push_back(Entry{key, frame});
    index_[key] = std::prev(order_.end());
    ++uploads_;

    Decision decision{Action::Upload, std::nullopt};

    // Past the capacity the STALEST entry leaves the MAP - not the oldest INSERTED, which is a different row
    // as soon as a scene re-names old streams (see the class note). Readers keep reading what they bound (the
    // entry leaving the registry is not a release), so only the lookup disappears - a later acquire of the
    // same identity uploads again, which is the price of the bound.
    if (order_.size() > capacity_) {
        auto stalest = order_.begin();
        for (auto it = std::next(order_.begin()); it != order_.end(); ++it) {
            if (it->last_frame < stalest->last_frame) {
                stalest = it;
            }
        }
        decision.evicted = stalest->key;
        index_.erase(stalest->key);
        order_.erase(stalest);
        ++evictions_;
    }

    return decision;
}

std::uint64_t SharedStreams::releaseUnseen(std::uint64_t frame, std::uint64_t grace,
                                           std::vector<StreamKey>& dropped)
{
    dropped.clear();
    std::uint64_t released = 0U;

    for (auto it = order_.begin(); it != order_.end();) {
        // Frames only go up, so the difference is the age. `age <= grace` keeps an entry the CURRENT frame
        // named, which is what makes "a stream a frame still names is never dropped" true by algebra and not
        // by ordering (a sweep that ran before the frame's own acquires would otherwise drop them).
        const std::uint64_t age = frame >= it->last_frame ? frame - it->last_frame : 0U;
        if (age <= grace) {
            ++it;
            continue;
        }
        dropped.push_back(it->key);
        index_.erase(it->key);
        it = order_.erase(it);
        ++released;
    }

    unused_ += released;
    return released;
}

std::size_t SharedStreams::live() const noexcept
{
    return index_.size();
}

std::uint64_t SharedStreams::uploads() const noexcept
{
    return uploads_;
}

std::uint64_t SharedStreams::aliases() const noexcept
{
    return aliases_;
}

std::uint64_t SharedStreams::evictions() const noexcept
{
    return evictions_;
}

std::uint64_t SharedStreams::unused() const noexcept
{
    return unused_;
}

void SharedStreams::clear() noexcept
{
    index_.clear();
    order_.clear();
}

}  // namespace core

VN_VSG_NS_END
