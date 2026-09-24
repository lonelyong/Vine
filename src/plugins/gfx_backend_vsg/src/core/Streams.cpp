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

bool sameShape(const StreamKey& a, const StreamKey& b) noexcept
{
    return a.kind == b.kind && a.location == b.location && a.components == b.components && a.count == b.count;
}

bool sameStream(const StreamKey& a, const StreamKey& b) noexcept
{
    return a == b;
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

namespace
{

/** @brief Finds a channel by location in an ascending-location snapshot. */
const StreamKey* channelAt(const std::vector<StreamKey>& channels, std::uint32_t location) noexcept
{
    const auto found = std::find_if(channels.begin(), channels.end(),
                                    [location](const StreamKey& key) { return key.location == location; });
    return found == channels.end() ? nullptr : &*found;
}

/** @brief Whether the two snapshots describe the same CHANNEL shape (see @ref sameShape). */
bool channelShapesMatch(const std::vector<StreamKey>& before, const std::vector<StreamKey>& after) noexcept
{
    if (before.size() != after.size()) {
        return false;  // a channel appeared or disappeared: the node's layout changed
    }
    for (const StreamKey& key : before) {
        const StreamKey* const current = channelAt(after, key.location);
        if (current == nullptr || !sameShape(key, *current)) {
            return false;
        }
    }
    return true;
}

}  // namespace

GeometryPlan planGeometry(const GeometrySnapshot& built, const GeometrySnapshot& now) noexcept
{
    GeometryPlan plan;

    // Nothing was built yet: everything about the node is new, so a shared upload may serve it (this is the
    // common path on the first frame a drawable appears).
    const bool nothing_built = built.streams.channels.empty() && !built.streams.index.has_value();
    if (nothing_built) {
        plan.action                 = GeometryAction::Rebuild;
        plan.shared_uploads_allowed = true;
        return plan;
    }

    // The index stream's PRESENCE is shape: an unindexed draw and an indexed one are assembled differently.
    const bool index_presence_changed =
        built.streams.index.has_value() != now.streams.index.has_value();
    if (index_presence_changed || !channelShapesMatch(built.streams.channels, now.streams.channels)) {
        plan.action                 = GeometryAction::Rebuild;
        plan.shared_uploads_allowed = true;  // every channel is read again, and its identity drives sharing
        return plan;
    }

    // A different SPAN is a different draw, not a different buffer: the assembled node states first index
    // and count, so only a rebuild can rewrite it. (Replacing the index BUFFER while drawing the same span
    // is the case an in-place re-point serves.)
    bool index_buffer_changed = false;
    if (now.streams.index.has_value() && built.streams.index.has_value()) {
        const StreamKey& before = *built.streams.index;
        const StreamKey& after  = *now.streams.index;
        const bool       span_moved = before.offset != after.offset || before.count != after.count;
        if (span_moved) {
            plan.action                 = GeometryAction::Rebuild;
            plan.shared_uploads_allowed = true;
            return plan;
        }
        index_buffer_changed = !sameStream(before, after);
    }

    // Which channels' bytes moved. A channel that is absent from one side cannot reach this point (the shape
    // check above rejected it), so every lookup here finds its counterpart.
    bool streams_moved = false;
    for (const StreamKey& key : now.streams.channels) {
        const StreamKey* const before = channelAt(built.streams.channels, key.location);
        if (before == nullptr || !sameStream(*before, key)) {
            plan.refreshed_locations.push_back(key.location);
            streams_moved = true;
        }
    }
    plan.index_refreshed = index_buffer_changed;

    if (streams_moved || index_buffer_changed) {
        plan.action                 = GeometryAction::Refresh;
        plan.shared_uploads_allowed = true;
        return plan;
    }

    // Every stream is identical. The node is current unless the model's own revision moved - and that case
    // is the one no stream can account for: the bytes may have changed under a pointer no buffer sees, so
    // the node is rebuilt from the model and NO shared upload is reused (it holds the bytes as of its own
    // insertion and cannot vouch for them).
    if (built.revision != now.revision) {
        plan.action                 = GeometryAction::Rebuild;
        plan.shared_uploads_allowed = false;
        plan.unexplained_revision   = true;
        return plan;
    }

    plan.action                 = GeometryAction::None;
    plan.shared_uploads_allowed = true;
    return plan;
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
