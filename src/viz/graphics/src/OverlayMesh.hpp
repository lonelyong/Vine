#pragma once

#include <cstdint>

#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/graphics/graphics_global.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/Array.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief Appends an axis-aligned box to the mesh a HUD overlay is authoring.
 *
 * @details
 * - The triangles are `vn::geom::spatial::tessellateSolid()`'s box - six flat faces of four corners, each with its
 *   own outward normal - moved from the solid's centre to the box's centre. Geom owns what a box is, so a winding or
 *   face-order fix lands once for every box the engine draws, the axis gizmo's sticks and the FPS readout's bars
 *   included.
 * - The arrays are APPENDED to, so one mesh can carry several boxes (see FpsOverlay's per-segment bars); the indices
 *   are relative to the combined vertex list that @p positions holds.
 *
 * @param positions Box vertex positions (appended).
 * @param normals   Per-vertex face normals (appended).
 * @param indices   Triangle indices (appended).
 * @param mn        Minimum corner; a box with no volume appends nothing.
 * @param mx        Maximum corner.
 */
inline void appendBox(vn::models::Vec3fArray&  positions,
                      vn::models::Vec3fArray&  normals,
                      vn::models::UInt32Array& indices,
                      const vn::math::Vec3f&   mn,
                      const vn::math::Vec3f&   mx)
{
    using vn::math::Vec3f;

    const vn::geom::spatial::Triangulationf triangles = vn::geom::spatial::tessellateSolid(
        vn::geom::spatial::BoxSolidf{ (mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f });
    const Vec3f         centre = (mn + mx) * 0.5f;
    const std::uint32_t base   = static_cast<std::uint32_t>(positions.size());

    positions.reserve(positions.size() + triangles.positions.size());
    for (const Vec3f& corner : triangles.positions) {
        positions.push_back(centre + corner);
    }
    normals.insert(normals.end(), triangles.normals.begin(), triangles.normals.end());
    indices.reserve(indices.size() + triangles.indices.size());
    for (const std::uint32_t index : triangles.indices) {
        indices.push_back(base + index);
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
