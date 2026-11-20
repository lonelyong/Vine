#pragma once

#include <cstdint>

#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/graphics/graphics_global.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/Array.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief Appends one triangulated part to the vertex arrays a caller is authoring, moved into place.
 *
 * @details
 * - The part's arrays are appended and its indices are re-based onto the vertex count @p positions already holds, so
 *   several parts share one stream: the FPS readout's per-segment bars, a marker's three arrows.
 * - @p rotation then @p translation place the part, which is what turns geom's +Z arrow into an X or Y one without a
 *   second set of triangles.
 * - This is the ONE append of a `Triangulation` into authoring arrays. A caller that also wants per-vertex colour
 *   appends it beside this call, over the vertices it just added - colour is not part of the geometry.
 *
 * @param positions   Vertex positions (appended).
 * @param normals     Per-vertex normals (appended).
 * @param indices     Triangle indices (appended, relative to the combined vertex list).
 * @param triangles   Triangles to append.
 * @param rotation    Orientation applied to every position and normal.
 * @param translation Offset applied after @p rotation.
 */
inline void appendTriangulation(vn::models::Vec3fArray&                  positions,
                                vn::models::Vec3fArray&                  normals,
                                vn::models::UInt32Array&                 indices,
                                const vn::geom::spatial::Triangulationf& triangles,
                                const vn::math::Quaternion<float>&       rotation,
                                const vn::math::Vec3f&                   translation)
{
    using vn::math::Vec3f;

    const std::uint32_t base = static_cast<std::uint32_t>(positions.size());
    positions.reserve(positions.size() + triangles.positions.size());
    normals.reserve(normals.size() + triangles.normals.size());
    for (const Vec3f& position : triangles.positions) {
        positions.push_back(rotation * position + translation);
    }
    for (const Vec3f& normal : triangles.normals) {
        normals.push_back(rotation * normal);
    }
    indices.reserve(indices.size() + triangles.indices.size());
    for (const std::uint32_t index : triangles.indices) {
        indices.push_back(base + index);
    }
}

/**
 * @brief Appends an axis-aligned box to the mesh a caller is authoring.
 *
 * @details
 * - The triangles are `vn::geom::spatial::tessellateSolid()`'s box - six flat faces of four corners, each with its own
 *   outward normal - moved from the solid's centre to the box's centre. Geom owns what a box is, so a winding or
 *   face-order fix lands once for every box the engine draws, the FPS readout's bars included.
 * - A box with no volume appends nothing.
 *
 * @param positions Box vertex positions (appended).
 * @param normals   Per-vertex face normals (appended).
 * @param indices   Triangle indices (appended).
 * @param mn        Minimum corner.
 * @param mx        Maximum corner.
 */
inline void appendBox(vn::models::Vec3fArray&  positions,
                      vn::models::Vec3fArray&  normals,
                      vn::models::UInt32Array& indices,
                      const vn::math::Vec3f&   mn,
                      const vn::math::Vec3f&   mx)
{
    const vn::geom::spatial::Triangulationf box =
        vn::geom::spatial::tessellateSolid(vn::geom::spatial::BoxSolidf{ (mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f });
    appendTriangulation(positions, normals, indices, box, vn::math::Quaternion<float>{}, (mn + mx) * 0.5f);
}

} // namespace detail

VN_GRAPHICS_NS_END
