#include <vine/vsg/api/StreamUploads.hpp>

#include <unordered_map>

#include <vine/graphics/ShaderAbi.hpp>

V_VSG_NS_BEGIN

namespace
{

/// @brief Whether a location is one of the canonical attributes (a custom channel is not).
bool isCanonicalLocation(std::uint32_t location) noexcept
{
    return location == vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Position) ||
           location == vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Normal) ||
           location == vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Color) ||
           location == vine::graphics::attributeLocation(vine::graphics::VertexAttribute::TexCoord0);
}

/// @brief Gets the key a bind is shared under.
///
/// An INDEX bind aliases the whole buffer and the draw states the span, so its key is the WHOLE buffer (no
/// offset, no count): two geometries slicing one index arena share one upload, and release() finds the entry
/// without being handed the draw's span. A vertex key is already the slice its array reads, so it is used as
/// it is.
core::StreamKey bindKeyOf(const core::StreamKey& key) noexcept
{
    if (key.kind != core::StreamKind::Index) {
        return key;
    }
    core::StreamKey whole = key;
    whole.offset          = 0U;
    whole.count           = 0U;
    return whole;
}

}  // namespace

struct StreamUploads::Data
{
    explicit Data(std::size_t capacity) : registry(capacity) {}

    core::SharedStreams registry;
    std::unordered_map<core::StreamKey, ::vsg::ref_ptr<::vsg::BindVertexBuffers>, core::StreamKeyHash> vertex;
    std::unordered_map<core::StreamKey, ::vsg::ref_ptr<::vsg::BindIndexBuffer>, core::StreamKeyHash>  index;
    std::uint64_t                                                                                     refusals{0};
    /// The frame the acquires are stamped with, and the window an unnamed entry survives (see beginFrame).
    std::uint64_t frame{0};
    std::uint32_t grace{0};
    /// The keys a sweep dropped, so the two maps can follow without allocating per sweep.
    std::vector<core::StreamKey> dropped;

    /** @brief Drops one key's bind from the map that holds it (both are tried: the key is opaque here). */
    void dropBind(const core::StreamKey& key)
    {
        vertex.erase(key);
        index.erase(key);
    }
};

StreamUploads::StreamUploads(std::size_t capacity) : d(std::make_unique<Data>(capacity))
{
}

StreamUploads::~StreamUploads() = default;

std::uint32_t StreamUploads::bindingOfCanonical(std::uint32_t location) noexcept
{
    // The CASES are the shader ABI's locations (ShaderAbi.hpp), the VALUES are this backend's binding
    // numbers - vsg's own order for its canonical arrays (texcoords at 2, colour at 3), which is a spelling
    // of the ABI here, not a definition of it. A custom channel (location >= 3, never the reserved texcoord
    // slot) has no canonical binding: its arrays are bound through one command whose identity is the whole
    // custom layout.
    using vine::graphics::attributeLocation;
    using vine::graphics::VertexAttribute;
    switch (location) {
    case attributeLocation(VertexAttribute::Position): return 0U;
    case attributeLocation(VertexAttribute::Normal): return 1U;
    case attributeLocation(VertexAttribute::TexCoord0): return 2U;
    case attributeLocation(VertexAttribute::Color): return 3U;
    default: return kNoBinding;
    }
}

StreamUploads::VertexResult StreamUploads::acquireVertex(const core::StreamKey& key,
                                                         ::vsg::ref_ptr<::vsg::Data> array)
{
    // A derived channel is built per geometry, and a custom channel's bind identity is the whole custom
    // layout: sharing either draws every peer with the first drawable's data. Refusing here - loudly, and
    // counted - is what stops the rule from depending on a caller remembering it.
    if (key.kind != core::StreamKind::Vertex || key.derived() || !isCanonicalLocation(key.location) ||
        array == nullptr) {
        ++d->refusals;
        return {};
    }

    const core::SharedStreams::Decision decision = d->registry.acquire(key, d->frame);
    if (decision.evicted.has_value()) {
        // The registry dropped the stalest lookup; the bind goes with it. A reader that already bound it holds
        // its own reference, so nothing it reads disappears (see the file note).
        d->dropBind(*decision.evicted);
    }
    if (decision.action == core::SharedStreams::Action::Alias) {
        const auto found = d->vertex.find(key);
        return {Action::Aliased, found == d->vertex.end() ? nullptr : found->second};
    }

    const auto binding = bindingOfCanonical(key.location);
    auto       bind    = ::vsg::BindVertexBuffers::create(binding, ::vsg::DataList{ array });
    d->vertex.insert_or_assign(key, bind);
    return {Action::Uploaded, bind};
}

StreamUploads::IndexResult StreamUploads::acquireIndex(const core::StreamKey& key,
                                                       ::vsg::ref_ptr<::vsg::Data> indices)
{
    if (key.kind != core::StreamKind::Index || key.derived() || indices == nullptr) {
        ++d->refusals;
        return {};
    }

    // The bind aliases the WHOLE buffer and the DRAW states the span (first index / count), so the span is
    // deliberately not part of the bind identity: every geometry slicing one index arena resolves to one
    // entry and shares one index upload (see bindKeyOf).
    const core::StreamKey normalised = bindKeyOf(key);

    const core::SharedStreams::Decision decision = d->registry.acquire(normalised, d->frame);
    if (decision.evicted.has_value()) {
        d->dropBind(*decision.evicted);
    }
    if (decision.action == core::SharedStreams::Action::Alias) {
        const auto found = d->index.find(normalised);
        return {Action::Aliased, found == d->index.end() ? nullptr : found->second};
    }

    auto bind = ::vsg::BindIndexBuffer::create(indices);
    d->index.insert_or_assign(normalised, bind);
    return {Action::Uploaded, bind};
}

void StreamUploads::beginFrame(std::uint64_t frame, std::uint32_t grace) noexcept
{
    d->frame = frame;
    d->grace = grace;
}

std::uint64_t StreamUploads::releaseUnseen()
{
    const std::uint64_t released = d->registry.releaseUnseen(d->frame, d->grace, d->dropped);
    for (const core::StreamKey& key : d->dropped) {
        d->dropBind(key);
    }
    return released;
}


std::size_t StreamUploads::live() const noexcept
{
    return d->registry.live();
}

std::size_t StreamUploads::objects() const noexcept
{
    return d->vertex.size() + d->index.size();
}

std::uint64_t StreamUploads::uploads() const noexcept
{
    return d->registry.uploads();
}

std::uint64_t StreamUploads::aliases() const noexcept
{
    return d->registry.aliases();
}

std::uint64_t StreamUploads::refusals() const noexcept
{
    return d->refusals;
}

std::uint64_t StreamUploads::evictions() const noexcept
{
    return d->registry.evictions();
}

std::uint64_t StreamUploads::unused() const noexcept
{
    return d->registry.unused();
}

bool StreamUploads::agreesWithRegistry() const noexcept
{
    return live() == objects();
}

void StreamUploads::clear()
{
    d->vertex.clear();
    d->index.clear();
    d->registry.clear();
}

V_VSG_NS_END
