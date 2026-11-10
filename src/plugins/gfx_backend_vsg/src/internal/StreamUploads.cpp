#include <vine/vsg/internal/StreamUploads.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/backend/ResourceTable.hpp>
#include <vine/vsg/support/VsgSceneRules.hpp>

VN_VSG_NS_BEGIN

namespace
{

/**
 * @brief The backend binding the instance-matrix stream occupies.
 *
 * @details
 * - 0-3 are vsg's canonical arrays (position, normal, texcoord, colour). The instance matrix is the next
 *   free binding, and it is the one `ContentPipeline::create` declares when the `VN_INSTANCED` define is
 *   in the variant's list.
 */
constexpr std::uint32_t kInstanceBinding = 4U;

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

/**
 * @brief The offset of @p slice inside @p buffer, or nothing when the slice is not a range of that buffer.
 *
 * @details
 * - The offset is what lets an uploaded array read the model's own bytes: `vsg::Array` aliases storage from a byte
 *   offset, so a channel that is a SEGMENT of one arena binds exactly that segment, with no copy (`GeometryFacts`
 *   states the same thing from the other side: "a sliced channel keeps its own scalars").
 * - The guard is not decoration: the acquire entry takes a span, and a caller whose span is its own scratch memory has
 *   nothing to alias - it gets a snapshot instead of an array over unrelated bytes.
 *
 * @tparam Element Scalar element type of both the buffer and the slice.
 * @param buffer Buffer @p slice is expected to be a range of.
 * @param slice  Scalars to locate.
 * @return The first element's index inside @p buffer, or `std::nullopt` when @p slice is not a range of it.
 */
template <typename Element>
[[nodiscard]] std::optional<std::size_t> sliceOffset(const vn::intrusive_ptr<const vn::Buffer<Element>>& buffer, std::span<const Element> slice) noexcept
{
    if (buffer == nullptr || slice.empty()) {
        return std::nullopt;
    }
    const auto base  = reinterpret_cast<std::uintptr_t>(buffer->data());
    const auto first = reinterpret_cast<std::uintptr_t>(slice.data());
    const auto bytes = buffer->size() * sizeof(Element);
    if (first < base || first - base > bytes || (first - base) % sizeof(Element) != 0u) {
        return std::nullopt;
    }
    const std::size_t offset = (first - base) / sizeof(Element);
    if (offset + slice.size() > buffer->size()) {
        return std::nullopt;
    }
    return offset;
}

} // namespace

struct StreamUploads::Data {
    /// @brief One cached stream: its bind, and the model buffer its key's ADDRESS names (see the file note).
    ///
    /// The owner is not memory policy: it is what keeps the address reserved for exactly as long as the key can
    /// be looked up, so a new buffer cannot be handed this entry's uploaded bytes.
    struct VertexEntry {
        ::vsg::ref_ptr<::vsg::BindVertexBuffers>   bind;
        vn::intrusive_ptr<const vn::Buffer<float>> owner;
    };

    /// @brief One cached index stream (see VertexEntry).
    struct IndexEntry {
        ::vsg::ref_ptr<::vsg::BindIndexBuffer>             bind;
        vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> owner;
    };

    core::ResourceTable<core::StreamKey, VertexEntry, core::StreamKeyHash> vertex;
    core::ResourceTable<core::StreamKey, IndexEntry, core::StreamKeyHash>  index;

    std::uint64_t refusals{ 0 };
    std::uint64_t aliases{ 0 };
};

StreamUploads::StreamUploads()
  : d(std::make_unique<Data>())
{}

StreamUploads::~StreamUploads() = default;

std::uint32_t StreamUploads::bindingOfCanonical(std::uint32_t location) noexcept
{
    // The CASES are the shader ABI's locations (ShaderAbi.hpp), the VALUES are this backend's binding
    // numbers - vsg's own order for its canonical arrays (texcoords at 2, colour at 3), which is a spelling
    // of the ABI here, not a definition of it. The instance matrix is ONE stream starting at its first
    // column; a custom channel has no canonical binding: its arrays are bound through one command whose
    // identity is the whole custom layout.
    using vn::graphics::attributeLocation;
    using vn::graphics::VertexAttribute;
    if (location >= attributeLocation(VertexAttribute::InstanceModel) &&
        location < attributeLocation(VertexAttribute::InstanceModel) + vn::graphics::kInstanceMatrixColumns)
    {
        return kInstanceBinding;
    }
    switch (location) {
    case attributeLocation(VertexAttribute::Position): return 0U;
    case attributeLocation(VertexAttribute::Normal): return 1U;
    case attributeLocation(VertexAttribute::TexCoord0): return 2U;
    case attributeLocation(VertexAttribute::Color): return 3U;
    default: return kNoBinding;
    }
}

StreamUploads::VertexResult
StreamUploads::acquireVertex(const core::StreamKey& key, std::span<const float> scalars, vn::intrusive_ptr<const vn::Buffer<float>> owner)
{
    // A derived channel is built per geometry, and a custom channel's bind identity is the whole custom
    // layout: sharing either draws every peer with the first drawable's data. Refusing here - loudly, and
    // counted - is what stops the rule from depending on a caller remembering it. A stream with no buffer
    // handle is refused for the same reason: nothing would reserve the address its key names (see the file note).
    if (key.kind != core::StreamKind::Vertex || key.derived() || !vn::graphics::isCanonicalAttributeLocation(key.location) || scalars.empty() ||
        owner == nullptr || key.buffer != owner.get()) {
        ++d->refusals;
        return {};
    }

    const auto acquired = d->vertex.acquire(key, [&]() -> std::optional<Data::VertexEntry> {
        // ZERO COPY: what the bind reads IS the model's own bytes. The array is a REAL vsg array over a view of the buffer
        // (see VsgBufferView, and the note in VsgSceneRules.hpp about why the bound object cannot be the view itself): the
        // renderer therefore keeps exactly the memory it reads alive, and the vertex data exists in one place instead of in
        // the model and in the scene graph at once.
        //
        // The entry keeps the buffer and the array keeps the view that holds it, so the bytes outlive the model for exactly
        // as long as something can still draw them - the same rule the file note states for the ADDRESS.
        const std::optional<std::size_t> offset = sliceOffset(owner, scalars);
        ::vsg::ref_ptr<::vsg::Data>      array;
        if (offset.has_value()) {
            array = detail::aliasArray<::vsg::floatArray, float>(owner, scalars.size(), *offset);
        }
        else {
            // A borrowed identity (no buffer handle) or a span of somebody else's memory has nothing to alias.
            auto snapshot = ::vsg::floatArray::create(static_cast<std::uint32_t>(scalars.size()));
            std::copy(scalars.begin(), scalars.end(), snapshot->begin());
            array = snapshot;
        }

        const auto binding = bindingOfCanonical(key.location);
        auto       bind    = ::vsg::BindVertexBuffers::create(binding, ::vsg::DataList{ array });
        return Data::VertexEntry{ bind, std::move(owner) };
    });
    if (acquired.value == nullptr) {
        return {}; // the guards above ARE the rules, so the builder cannot refuse; an empty answer stays honest
    }
    if (acquired.built) {
        return { Action::Uploaded, acquired.value->bind };
    }
    ++d->aliases;
    return { Action::Aliased, acquired.value->bind };
}

StreamUploads::IndexResult
StreamUploads::acquireIndex(const core::StreamKey& key, std::span<const std::uint32_t> indices, vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> owner)
{
    if (key.kind != core::StreamKind::Index || key.derived() || indices.empty() || owner == nullptr || key.buffer != owner.get()) {
        ++d->refusals;
        return {};
    }

    // The bind aliases the WHOLE buffer and the DRAW states the span (first index / count), so the span is
    // deliberately not part of the bind identity: every geometry slicing one index arena resolves to one
    // entry and shares one index upload (see bindKeyOf).
    const core::StreamKey normalised = bindKeyOf(key);

    const auto acquired = d->index.acquire(normalised, [&]() -> std::optional<Data::IndexEntry> {
        // ZERO COPY, and the WHOLE buffer: the bind aliases all of it and the DRAW states the span (see bindKeyOf), so the
        // array has to cover the buffer the key names rather than whatever slice this caller happened to pass.
        const std::optional<std::size_t> offset = sliceOffset(owner, indices);
        ::vsg::ref_ptr<::vsg::Data>      array;
        if (offset == std::optional<std::size_t>{ 0u } && indices.size() == owner->size()) {
            array = detail::aliasArray<::vsg::uintArray, std::uint32_t>(owner, indices.size());
        }
        else {
            auto snapshot = ::vsg::uintArray::create(static_cast<std::uint32_t>(indices.size()));
            std::copy(indices.begin(), indices.end(), snapshot->begin());
            array = snapshot;
        }
        auto bind = ::vsg::BindIndexBuffer::create(array);
        return Data::IndexEntry{ bind, std::move(owner) };
    });
    if (acquired.value == nullptr) {
        return {};
    }
    if (acquired.built) {
        return { Action::Uploaded, acquired.value->bind };
    }
    ++d->aliases;
    return { Action::Aliased, acquired.value->bind };
}

std::uint64_t StreamUploads::beginFrame(const ContentFacts& facts, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    // TWO O(1) FACTS PER ENTRY, in one pass. The ADDRESS is content's answer: some live row still declares it
    // (`ContentFacts::declarations`, maintained by the content store as its rows move). The REVISION is the
    // entry's own answer: it holds the buffer it uploaded from, so a buffer edit retires the old upload without
    // any row changing. Nothing here walks the content tables - see the file note.
    const auto is_live = [&facts](const core::StreamKey& key, const auto& entry) {
        return entry.owner != nullptr && facts.declarations.declaresBuffer(key.buffer) && key.revision == entry.owner->revision();
    };

    std::uint64_t retired = d->vertex.retireUnpinned(is_live, timeline, retirement);
    retired += d->index.retireUnpinned(is_live, timeline, retirement);
    return retired;
}

std::size_t StreamUploads::live() const noexcept
{
    return d->vertex.live() + d->index.live();
}

std::uint64_t StreamUploads::uploads() const noexcept
{
    return d->vertex.builds() + d->index.builds();
}

std::uint64_t StreamUploads::aliases() const noexcept
{
    return d->aliases;
}

std::uint64_t StreamUploads::refusals() const noexcept
{
    return d->refusals;
}

std::uint64_t StreamUploads::retired() const noexcept
{
    return d->vertex.retired() + d->index.retired();
}

VN_VSG_NS_END
