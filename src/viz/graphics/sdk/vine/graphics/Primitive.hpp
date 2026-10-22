#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>

// The two owning pointers are members of a VALUE type, so a caller that copies or destroys a Primitive needs
// both complete - including them here is what keeps every call site from having to remember that.
#include "Material.hpp"
#include "ShaderProgram.hpp"
#include "Topology.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief One DRAW of a geometry's data: how the vertices assemble, which indices it reads, and how it is
 *        shaded.
 *
 * @details
 * - THIS IS THE UNIT THE API REQUIRES. A draw has exactly one primitive topology, one index span and one
 *   program/material; a `Geometry` (a set of vertex channels shared by all its draws) may hold SEVERAL
 *   primitives, which is how "one asset, several parts" is expressed: a mesh, its deduplicated edge lines
 *   and a selected-point set share one vertex buffer and are three primitives.
 * - INDICES ARE PER PRIMITIVE, and that is a data lifetime decision rather than symmetry: a generated edge
 *   list or a selection set is rewritten while the mesh's own indices never change, and the backend shares
 *   index uploads per BUFFER (a bind aliases the whole buffer, the draw states the span) - so putting them
 *   in one buffer would couple their invalidation. Pointing several primitives at one buffer is still
 *   possible and then shares one upload, which is the caller's choice to make.
 * - MATERIAL AND PROGRAM are the shading side of one draw, and they are optional: an empty pointer means
 *   "the nearest StateNode's", which is how a subtree restyles what it contains. A primitive that states
 *   one owns it - reparenting the geometry cannot change it.
 * - The vertex data is NOT here: positions, normals, texcoords and custom channels belong to the
 *   `Geometry`, because they are what its primitives have in common.
 *
 * @note A `Geometry` with no primitives draws nothing; the single-primitive case (the overwhelming
 *       majority) is a geometry with one entry.
 */
struct VN_GRAPHICS_API Primitive
{
    /// The index span's type: a segment of an index buffer, described the same way an attribute channel is
    /// (see BufferSlice). Kept as a name here because it is THIS struct's field, and a caller that spells
    /// "the whole buffer" once should not have to write the template out.
    using IndexSpan = BufferSlice<std::uint32_t>;

    Topology topology{Topology::Triangles};  ///< How the vertices assemble (a pipeline's identity).

    /// The index span this draw reads, or an empty buffer for a NON-indexed draw (the vertex count then
    /// comes from the geometry's position channel). A slice of a shared arena works exactly as it does for
    /// a vertex channel: `count == 0` means "the rest of the buffer from `first`".
    IndexSpan indices{};

    intrusive_ptr<Material>      material{};  ///< Shading, or nullptr to inherit the nearest StateNode's.
    intrusive_ptr<ShaderProgram> program{};   ///< Program, or nullptr to inherit (StateNode, then the pass).

    /** @brief Gets whether this draw is indexed.
     * @return true when it reads indices (a non-empty index buffer).
     */
    [[nodiscard]] bool isIndexed() const noexcept { return !indices.empty(); }

    /** @brief Gets how many indices this draw reads (0 for a non-indexed draw).
     * @return The resolved span length, following a growing buffer when `count` is 0.
     */
    [[nodiscard]] std::size_t indexCount() const noexcept { return indices.size(); }
};

VN_GRAPHICS_NS_END
