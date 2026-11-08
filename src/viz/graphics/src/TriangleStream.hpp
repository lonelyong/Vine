#pragma once

#include <vine/graphics/graphics_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/geom/spatial/TriangleBvh.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Topology.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Returns how many triangles @p corners corners make under @p topology.
 *
 * @details
 * - THE rule: a triangle list makes one per three corners and ignores a trailing partial triple; a strip and a fan make
 *   one per corner past the first two; a topology that draws no triangles makes none.
 * - `TriangleStream::triangleCount()`, `Geometry::triangleCount()` and a tree's build all read it, so a count and a walk
 *   cannot disagree about how many triangles a draw has.
 *
 * @param corners  Corner count the draw reads (through its index buffer or not).
 * @param topology How those corners assemble.
 * @return Triangle count; zero for a topology that draws no triangles.
 */
[[nodiscard]]
constexpr std::size_t triangleCountOf(std::size_t corners, Topology topology) noexcept
{
    switch (topology) {
    case Topology::Triangles: return corners / 3u;
    case Topology::TriangleStrip:
    case Topology::TriangleFan: return corners >= 3u ? corners - 2u : 0u;
    case Topology::Points:
    case Topology::Lines:
    case Topology::LineStrip: return 0u;
    }
    return 0u;
}

/**
 * @brief A read-only view of ONE draw's triangle stream: its positions, its corner indices and its topology.
 *
 * @details
 * - This is the single definition of "which triangle is triangle number `t`" for a draw. The linear pick walk, a
 *   triangle tree's build and the ordinal a tree query reports all take their numbering from here, so a tree and a hit
 *   can never disagree about which triangle a number names.
 * - A triangle comes back as three VERTEX INDICES in WINDING order: a strip's odd triangles have their last two corners
 *   swapped - the winding flip both APIs state - and a fan's are `(0, i + 1, i + 2)`. A caller therefore reads
 *   `positions[corners[0..2]]` and never needs to know the topology.
 * - It borrows: the positions live in a geometry channel and the indices in a primitive's slice, so a view is valid
 *   only while the holder of those keeps them there - one picking call, or one build.
 *
 * @note A topology that draws no triangles (points, lines) reports zero triangles instead of failing: such a primitive
 *       has nothing to pick, which is a property of the stream and not an error at the call site.
 * @note It models `vn::geom::spatial::TriangleSource`, so a triangle tree can be built over a draw's stream by the same
 *       call that builds one over a mesh - the static_assert below is what keeps the two frames' contract identical.
 */
class TriangleStream
{
  public:
    /// An empty stream: no positions, no corners, nothing to index.
    TriangleStream() noexcept = default;

    /**
     * @brief Builds a stream over @p positions' corners.
     *
     * @param positions Vertex positions the corner indices address.
     * @param indices   Corner indices, or empty when the corners are read in order.
     * @param topology  How those corners assemble into triangles.
     */
    TriangleStream(std::span<const vn::math::Vec3f> positions, std::span<const std::uint32_t> indices, Topology topology) noexcept
      : positions_(positions)
      , indices_(indices)
      , topology_(topology)
    {}

    /**
     * @brief Returns where the corners are read from: the geometry's position segment, as drawn.
     *
     * @return Borrowed vertex positions.
     */
    [[nodiscard]]
    std::span<const vn::math::Vec3f> positions() const noexcept
    {
        return positions_;
    }

    /**
     * @brief Returns the corner indices into @ref positions, or an empty span when the corners are read in order.
     *
     * @return Borrowed corner indices.
     */
    [[nodiscard]]
    std::span<const std::uint32_t> indices() const noexcept
    {
        return indices_;
    }

    /**
     * @brief Returns how the draw's corners assemble into triangles.
     *
     * @return The topology.
     */
    [[nodiscard]]
    Topology topology() const noexcept
    {
        return topology_;
    }

    /**
     * @brief Returns how many corners the draw reads, through the index buffer or not.
     *
     * @return Corner count.
     */
    [[nodiscard]]
    std::size_t cornerCount() const noexcept
    {
        return indices_.empty() ? positions_.size() : indices_.size();
    }

    /**
     * @brief Returns how many triangles the draw's topology makes from its corners.
     *
     * @details
     * - A triangle list makes one triangle per three corners and ignores a trailing partial triple; a strip and a fan
     *   make one per corner past the first two.
     *
     * @return Triangle count; zero for a topology that draws no triangles.
     */
    [[nodiscard]]
    std::size_t triangleCount() const noexcept
    {
        return triangleCountOf(cornerCount(), topology_);
    }

    /**
     * @brief Returns one triangle as three vertex indices, in winding order.
     *
     * @param triangle Triangle ordinal, below triangleCount().
     * @param corners  Receives the three vertex indices; untouched when the call fails.
     * @return true when the triangle exists and every corner names a stored position.
     */
    [[nodiscard]]
    bool triangleCorners(std::size_t triangle, std::array<std::uint32_t, 3>& corners) const noexcept
    {
        if (triangle >= triangleCount()) {
            return false;
        }
        switch (topology_) {
        case Topology::Triangles: {
            const std::size_t first = triangle * 3u;
            return resolve(first, corners[0]) && resolve(first + 1u, corners[1]) && resolve(first + 2u, corners[2]);
        }
        case Topology::TriangleStrip:
            // Triangle i is (i, i+1, i+2), wound the other way on odd i: the corners are reported in the wound order.
            return (triangle % 2u) == 0u
                       ? resolve(triangle, corners[0]) && resolve(triangle + 1u, corners[1]) &&
                             resolve(triangle + 2u, corners[2])
                       : resolve(triangle, corners[0]) && resolve(triangle + 2u, corners[1]) &&
                             resolve(triangle + 1u, corners[2]);
        case Topology::TriangleFan:
            // Triangle i is (0, i+1, i+2): the first corner is the fan centre.
            return resolve(0u, corners[0]) && resolve(triangle + 1u, corners[1]) && resolve(triangle + 2u, corners[2]);
        case Topology::Points:
        case Topology::Lines:
        case Topology::LineStrip: return false;
        }
        return false;
    }

  private:
    /**
     * @brief Resolves one corner slot to the vertex index it names.
     *
     * @param slot Corner slot in the stream.
     * @param out  Receives the vertex index.
     * @return true when the slot's vertex exists.
     */
    [[nodiscard]]
    bool resolve(std::size_t slot, std::uint32_t& out) const noexcept
    {
        const std::size_t index = indices_.empty() ? slot : static_cast<std::size_t>(indices_[slot]);
        if (index >= positions_.size()) {
            return false;
        }
        out = static_cast<std::uint32_t>(index);
        return true;
    }

    std::span<const vn::math::Vec3f> positions_{};
    std::span<const std::uint32_t>   indices_{};
    Topology                         topology_{ Topology::Triangles };
};

static_assert(vn::geom::spatial::TriangleSource<TriangleStream>,
              "a draw's stream is a triangle source: the triangle tree this library builds over a mesh and the one it "
              "builds over a draw must take their numbering from the same contract");

/**
 * @brief Resolves ONE draw of a geometry into the triangle stream it reads, owning a copy only when it must.
 *
 * @details
 * - The single place that answers "what are this draw's positions, corners and topology": filling in the pick
 *   walk, building the draw's triangle tree and adopting a mesh's tree all ask this, so they cannot disagree.
 * - It copies NOTHING for the ordinary case: a three-component position channel IS a run of Vec3, so the stream
 *   reads the bytes the GPU reads. A channel with another stride (a vec4 position channel) cannot be viewed that
 *   way, so its xyz is gathered into @ref gathered_ - the same values, so a tree built over them describes the
 *   same triangles.
 * - The view lives only as long as this object, which lives only as long as the call that made it: the channel's
 *   buffer may be replaced or grown by a writer afterwards.
 *
 * @note The primitive's index span is taken as a view too, so an index buffer that grows is followed within the
 *       one call rather than snapshotted.
 */
class DrawTriangleStream {
  public:
    /**
     * @brief Resolves draw @p draw of @p geometry.
     *
     * @param geometry Geometry to read (an empty geometry yields an invalid stream).
     * @param draw     Primitive index.
     */
    DrawTriangleStream(const Geometry& geometry, std::size_t draw);

    // Not a value: when the positions had to be gathered, the stream's view points AT this object's storage - a copy
    // would leave it pointing at the source's, and a move at the vector the move emptied. It is built in place and used
    // for one call, so it refuses to be copied or moved rather than carry a dangling view.
    DrawTriangleStream(const DrawTriangleStream&)            = delete;
    DrawTriangleStream& operator=(const DrawTriangleStream&) = delete;
    DrawTriangleStream(DrawTriangleStream&&)                 = delete;
    DrawTriangleStream& operator=(DrawTriangleStream&&)      = delete;
    ~DrawTriangleStream()                                    = default;

    /** @brief Returns whether the draw has position data to read. */
    [[nodiscard]]
    bool valid() const noexcept
    {
        return !stream_.positions().empty();
    }

    /** @brief Returns the draw's stream (empty positions when the draw holds no data). */
    [[nodiscard]]
    const TriangleStream& stream() const noexcept
    {
        return stream_;
    }

  private:
    /// Positions gathered because the channel is not a packed xyz run; empty in the ordinary, zero-copy case.
    std::vector<vn::math::Vec3f> gathered_;
    TriangleStream               stream_{};
};

inline DrawTriangleStream::DrawTriangleStream(const Geometry& geometry, std::size_t draw)
{
    const Primitive*        primitive = geometry.primitive(draw);
    const AttributeChannel* positions = geometry.buffer(attributeLocation(VertexAttribute::Position));
    if (primitive == nullptr || positions == nullptr || positions->components < 3u) {
        return;
    }
    if (positions->components == 3u) {
        stream_ = TriangleStream{ positions->vec3View(), primitive->indices.span(), primitive->topology };
        return;
    }
    const std::size_t count = positions->vertexCount();
    gathered_.reserve(count);
    for (std::size_t vertex = 0; vertex < count; ++vertex) {
        const std::array<float, 3> p = positions->xyz(vertex);
        gathered_.emplace_back(p[0], p[1], p[2]);
    }
    stream_ = TriangleStream{ gathered_, primitive->indices.span(), primitive->topology };
}

VN_GRAPHICS_NS_END
