#include <vine/vsg/internal/GeometryFacts.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <vsg/core/Array.h>

#include <vine/graphics/ShaderAbi.hpp>

VN_VSG_NS_BEGIN

namespace
{

/// @brief The canonical role a location carries, or nothing when it is a custom channel.
[[nodiscard]] bool canonicalRoleOf(std::uint32_t location, vn::graphics::VertexAttribute& role) noexcept
{
    switch (location)
    {
    case 0U:
        role = vn::graphics::VertexAttribute::Position;
        return true;
    case 1U:
        role = vn::graphics::VertexAttribute::Normal;
        return true;
    case 2U:
        role = vn::graphics::VertexAttribute::Color;
        return true;
    case 8U:
        role = vn::graphics::VertexAttribute::TexCoord0;
        return true;
    default:
        return false;
    }
}

/// @brief Orders the locations the way the entry's channels must be bound: canonical first, then customs.
[[nodiscard]] std::vector<std::uint32_t> orderedLocations(const std::vector<std::uint32_t>& locations)
{
    std::vector<std::uint32_t> canonical;
    std::vector<std::uint32_t> custom;
    for (const std::uint32_t location : locations)
    {
        vn::graphics::VertexAttribute role{};
        (canonicalRoleOf(location, role) ? canonical : custom).push_back(location);
    }
    std::sort(canonical.begin(), canonical.end());
    std::sort(custom.begin(), custom.end());
    canonical.insert(canonical.end(), custom.begin(), custom.end());
    return canonical;
}

}  // namespace

FactMiss buildGeometryFacts(const vn::graphics::Geometry& geometry, GeometryFacts& out,
                            std::vector<ChannelFacts>& storage)
{
    storage.clear();
    out          = GeometryFacts{};
    out.geometry = &geometry;
    out.revision = geometry.revision();

    // THE BOUNDARY OF THIS SLICE: one draw per geometry. The SDK and the plan already carry a draw's identity
    // (Geometry::primitiveCount / RenderCommand::primitive_index / CompiledCommand::primitive), but this table
    // still describes ONE index stream, so a geometry that states several primitives is REFUSED rather than
    // drawn as its first primitive N times - which would be a wrong picture nobody could explain.
    // The follow-up is per-primitive rows (topology + index span + count) and one draw per command; until then
    // a multi-part asset is expressed as several geometries sharing buffers (the arena rule, see the guide's
    // §2.7), which needs no new backend work.
    if (geometry.primitiveCount() > 1U) {
        return FactMiss::Malformed;
    }
    if (geometry.primitiveCount() == 0U) {
        return FactMiss::Unknown;  // no draw at all: nothing to describe
    }

    const std::vector<std::uint32_t> locations = geometry.bufferLocations();
    if (locations.empty())
    {
        return FactMiss::Unknown;  // nothing to describe
    }

    bool        positioned        = false;
    std::size_t position_vertices = 0;
    std::size_t vertex_base       = 0;
    bool        base_known        = false;

    for (const std::uint32_t location : orderedLocations(locations))
    {
        const vn::graphics::AttributeChannel* channel = geometry.buffer(location);
        if (channel == nullptr || channel->values == nullptr || channel->components == 0U)
        {
            continue;  // an empty channel feeds nothing, so it is not part of the layout either
        }

        const std::span<const float> scalars = channel->scalars();
        if (scalars.empty())
        {
            continue;
        }

        // Every channel of one geometry must start at the same VERTEX: the draw's indices name vertices, and a
        // channel that began elsewhere would read a different vertex for the same index.
        const std::size_t base = channel->offset / channel->components;
        if (!base_known)
        {
            vertex_base = base;
            base_known  = true;
        }
        else if (vertex_base != base)
        {
            return FactMiss::Malformed;
        }

        ChannelFacts facts;
        facts.key.kind       = core::StreamKind::Vertex;
        facts.key.location   = location;
        facts.key.components = channel->components;
        facts.key.buffer     = channel->values.get();
        facts.key.revision   = channel->values != nullptr ? channel->values->revision() : 0U;
        facts.key.offset     = static_cast<std::uint32_t>(channel->offset);
        facts.key.count      = static_cast<std::uint32_t>(channel->floatCount());

        // The slice IS the stream: what is uploaded is this channel's own scalars, so the bind covers it from
        // its first vertex and the draw needs no vertex offset. The bytes are NOT copied here: the entry keeps
        // the buffer handle and the slice, and the upload layer reads them when a stream is actually uploaded
        // (see ChannelFacts) - a copy taken now would be the bytes as of this description, which is exactly
        // what a host edit announced afterwards must not serve.

        facts.values = channel->values;
        storage.push_back(facts);

        vn::graphics::VertexAttribute role{};
        if (canonicalRoleOf(location, role))
        {
            out.layout.canonical_mask |= 1U << static_cast<std::uint32_t>(role);
            if (role == vn::graphics::VertexAttribute::Position)
            {
                positioned        = true;
                // The vertices a NON-indexed draw assembles from: the position stream's own count, which is
                // also what the SDK's `Geometry::vertexCount()` reports (positions are the one required
                // attribute, so they are the stream the count can be derived from).
                position_vertices = channel->vertexCount();
            }
        }
        else
        {
            out.layout.custom_locations.push_back(location);
        }
    }

    if (storage.empty())
    {
        return FactMiss::Unknown;
    }
    if (!positioned)
    {
        return FactMiss::Malformed;  // positions are the one required attribute
    }

    // The entry BORROWS the caller's storage: the channels are read in the order they were built in (canonical
    // roles first, then the custom locations), which is the vertex-binding order.
    out.channels = storage;

    if (!geometry.hasIndices())
    {
        // NON-INDEXED: there is no index stream to describe, and the draw assembles its primitives from the
        // vertex streams themselves - so the count it carries is the position stream's own vertex count. This
        // is a different draw COMMAND rather than a degenerate indexed one (see GeometryFacts), and the
        // topology that turns those vertices into points, lines or triangles is the state's business.
        out.vertex_count = static_cast<std::uint32_t>(position_vertices);
        if (out.vertex_count == 0U)
        {
            return FactMiss::Malformed;  // positions too short to form a single vertex: nothing to assemble
        }
        return FactMiss::None;
    }

    // The index stream, normalized to the whole buffer (see the file note): the buffer is the identity, the
    // segment travels with the draw.
    const vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> index_buffer = geometry.indicesBuffer();
    if (index_buffer == nullptr || geometry.indexCount() == 0U)
    {
        return FactMiss::Malformed;  // hasIndices() promised a range: an empty one draws nothing
    }

    IndexFacts index_facts;
    index_facts.key.kind       = core::StreamKind::Index;
    index_facts.key.location   = 0U;
    index_facts.key.components = 1U;
    index_facts.key.buffer     = index_buffer.get();
    index_facts.key.revision   = index_buffer->revision();
    index_facts.key.offset     = 0U;
    index_facts.key.count      = 0U;  // the whole buffer: two geometries over one arena share one upload

    index_facts.values = index_buffer;
    out.indices      = index_facts;

    out.index_count   = static_cast<std::uint32_t>(geometry.indexCount());
    out.first_index   = static_cast<std::uint32_t>(geometry.firstIndex());
    out.vertex_offset = 0U;  // the stream starts at the geometry's first vertex (see the file note)

    return FactMiss::None;
}

VN_VSG_NS_END
