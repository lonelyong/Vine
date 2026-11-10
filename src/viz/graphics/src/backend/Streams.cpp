#include <vine/graphics/backend/Streams.hpp>

VN_GRAPHICSBACKEND_NS_BEGIN

bool StreamKey::operator==(const StreamKey& other) const noexcept
{
    return kind == other.kind && location == other.location && components == other.components && buffer == other.buffer && revision == other.revision &&
           offset == other.offset && count == other.count;
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
    const auto  mix  = [&hash](std::uint64_t value) noexcept { hash ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2); };
    mix(reinterpret_cast<std::uintptr_t>(key.buffer));
    mix(key.revision);
    mix(key.offset);
    mix(key.count);
    mix(key.location);
    mix(key.components);
    return hash;
}

VN_GRAPHICSBACKEND_NS_END
