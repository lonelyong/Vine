#include <vine/vsg/VsgMeshResourceCache.hpp>

#include <cstdint>
#include <unordered_map>
#include <utility>

#include <vine/vsg/OwnedCache.hpp>

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief One cached bind plus the sequence its insertion order is tracked by.
 *
 * The sequence is what the shared FIFO trim orders entries by (see trimToCapacity), which is why it is part
 * of the entry and not of the key.
 */
template <typename Bind>
struct SharedBind
{
    ::vsg::ref_ptr<Bind> bind;
    std::uint64_t        sequence_ = 0;

    /** @brief Gets the insertion sequence (FIFO order for capacity trims). */
    std::uint64_t sequence() const noexcept { return sequence_; }
};

/// @brief Whether the cache is the only holder of @p bind (i.e. no retained node binds it any more).
template <typename Bind>
bool bindIsUnused(const ::vsg::ref_ptr<Bind>& bind) noexcept
{
    return bind == nullptr || bind->referenceCount() <= 1u;
}

}  // namespace

struct VsgMeshResourceCache::Data
{
    using VertexMap = std::unordered_map<ChannelKey, SharedBind<::vsg::BindVertexBuffers>, ChannelKeyHash>;
    using IndexMap  = std::unordered_map<ChannelKey, SharedBind<::vsg::BindIndexBuffer>, ChannelKeyHash>;

    VertexMap     vertex_binds;
    IndexMap      index_binds;
    InsertionClock clock;
};

std::size_t VsgMeshResourceCache::ChannelKeyHash::operator()(const ChannelKey& key) const noexcept
{
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    const auto    mix = [&h](std::uint64_t value) {
        h ^= value;
        h *= 1099511628211ull;
    };
    mix(key.binding);
    mix(key.components);
    mix(reinterpret_cast<std::uintptr_t>(key.buffer));
    mix(key.revision);
    mix(key.count);
    return static_cast<std::size_t>(h);
}

VsgMeshResourceCache::VsgMeshResourceCache() : d(std::make_unique<Data>()) {}

VsgMeshResourceCache::~VsgMeshResourceCache() = default;

::vsg::ref_ptr<::vsg::BindVertexBuffers> VsgMeshResourceCache::getOrCreateVertexBind(
    const ChannelKey&                  key,
    const ::vsg::ref_ptr<::vsg::Data>& array)
{
    if (const auto it = d->vertex_binds.find(key); it != d->vertex_binds.end()) {
        return it->second.bind;
    }
    auto bind = ::vsg::BindVertexBuffers::create(key.binding, ::vsg::DataList{ array });
    d->vertex_binds.insert_or_assign(key, SharedBind<::vsg::BindVertexBuffers>{ bind, d->clock.tick() });
    // The FIFO half of the memory bound: the newest entries are the ones a live scene binds, the oldest fall
    // off (a stream whose entry went simply uploads again when a geometry asks for it).
    trimToCapacity(d->vertex_binds, kMaxEntries);
    return bind;
}

::vsg::ref_ptr<::vsg::BindIndexBuffer> VsgMeshResourceCache::getOrCreateIndexBind(
    const ChannelKey&                  key,
    const ::vsg::ref_ptr<::vsg::Data>& indices)
{
    if (const auto it = d->index_binds.find(key); it != d->index_binds.end()) {
        return it->second.bind;
    }
    auto bind = ::vsg::BindIndexBuffer::create(indices);
    d->index_binds.insert_or_assign(key, SharedBind<::vsg::BindIndexBuffer>{ bind, d->clock.tick() });
    trimToCapacity(d->index_binds, kMaxEntries);
    return bind;
}

std::size_t VsgMeshResourceCache::releaseAbandoned()
{
    std::size_t released = 0u;
    for (auto it = d->vertex_binds.begin(); it != d->vertex_binds.end();) {
        if (bindIsUnused(it->second.bind)) {
            it = d->vertex_binds.erase(it);
            ++released;
        }
        else {
            ++it;
        }
    }
    for (auto it = d->index_binds.begin(); it != d->index_binds.end();) {
        if (bindIsUnused(it->second.bind)) {
            it = d->index_binds.erase(it);
            ++released;
        }
        else {
            ++it;
        }
    }
    return released;
}

std::size_t VsgMeshResourceCache::count() const
{
    return d->vertex_binds.size() + d->index_binds.size();
}

void VsgMeshResourceCache::clear()
{
    d->vertex_binds.clear();
    d->index_binds.clear();
}

V_VSG_NS_END
