#pragma once

#include <vine/graphics/graphics_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vine/graphics/Topology.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

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
 */
struct TriangleStream {
    /// Where corners are read from: the geometry's position segment, as drawn.
    std::span<const vn::math::Vec3f> positions;
    /// Corner indices into @ref positions, or empty when the draw reads the vertices in order.
    std::span<const std::uint32_t> indices;
    /// How the draw's corners assemble into triangles.
    Topology topology{ Topology::Triangles };

    /**
     * @brief Returns how many corners the draw reads, through the index buffer or not.
     *
     * @return Corner count.
     */
    [[nodiscard]]
    std::size_t cornerCount() const noexcept
    {
        return indices.empty() ? positions.size() : indices.size();
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
        const std::size_t corners = cornerCount();
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
        switch (topology) {
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
        const std::size_t index = indices.empty() ? slot : static_cast<std::size_t>(indices[slot]);
        if (index >= positions.size()) {
            return false;
        }
        out = static_cast<std::uint32_t>(index);
        return true;
    }
};

VN_GRAPHICS_NS_END
