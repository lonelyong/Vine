#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "BoxSolid.hpp"
#include "ConeSolid.hpp"
#include "CylinderSolid.hpp"
#include "EllipsoidSolid.hpp"
#include "SphereSolid.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief How finely one analytic solid is triangulated.
 *
 * @details
 * - `around` is the number of azimuth intervals of a surface of revolution; `along` is the number of latitude
 *   intervals of a sphere / ellipsoid.
 * - A box is a closed polyhedron and ignores both, and a cylinder's cap and a cone's base are fans whose rim is
 *   `around` segments.
 * - The values are clamped where they would stop describing a surface: `around` is at least 3 (a ring needs three
 *   corners) and `along` is at least 2 (a sphere needs at least one ring between its poles).
 * - More segments cost vertices and triangles linearly; the shape itself does not change.
 */
struct TessellationQuality
{
    /// Azimuth intervals of a surface of revolution; clamped to at least 3.
    int around{ 32 };

    /// Latitude intervals of a sphere / ellipsoid; clamped to at least 2.
    int along{ 16 };
};

/**
 * @brief The triangles one analytic solid tessellates into, as vertex data in the solid's own local space.
 *
 * @details
 * - A VALUE, not a mesh: it owns the three arrays a mesh channel would hold and states no identity, no revision and no
 *   topology beyond "three indices per triangle". Putting it into a mesh is the modelling side's step
 *   (`vn::models::meshFromTriangulation()`).
 * - The precision follows the solid it came from: `Triangulation<float>` from a `BoxSolidf`, `Triangulation<double>`
 *   from a `BoxSolidd`. Nothing narrows behind a caller's back.
 * - `positions` and `normals` hold one entry per vertex; `indices` holds three vertex indices per triangle,
 *   counter-clockwise seen from outside.
 * - Vertices are DUPLICATED where one position carries more than one normal - along a box edge, between a cylinder's
 *   side and its cap, and at a cone's apex - so `vertexCount()` is not the smallest possible vertex set.
 * - An unusable solid answers an empty value: all three arrays are empty and `empty()` is true.
 *
 * @warning
 * - The arrays are owned by the value; a caller that hands it on moves it instead of copying it.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
struct Triangulation
{
    /// One position per vertex, in the solid's local space.
    std::vector<vn::math::Vector3<T>> positions;

    /// One outward unit normal per vertex, in the same order as `positions`.
    std::vector<vn::math::Vector3<T>> normals;

    /// Three vertex indices per triangle, counter-clockwise seen from outside.
    std::vector<std::uint32_t> indices;

    /**
     * @brief Returns the number of vertices.
     *
     * @return The size of `positions`.
     */
    [[nodiscard]]
    std::size_t vertexCount() const noexcept
    {
        return positions.size();
    }

    /**
     * @brief Returns the number of triangles.
     *
     * @return The number of index triples, `indices.size() / 3`.
     */
    [[nodiscard]]
    std::size_t triangleCount() const noexcept
    {
        return indices.size() / 3u;
    }

    /**
     * @brief Reports whether the tessellation produced any triangle.
     *
     * @return true when `indices` is empty.
     */
    [[nodiscard]]
    bool empty() const noexcept
    {
        return indices.empty();
    }
};

/// The tessellation of a `float` solid; the precision a model mesh is built from.
using Triangulationf = Triangulation<float>;

/// The tessellation of a `double` solid; a model mesh converts it explicitly.
using Triangulationd = Triangulation<double>;

/**
 * @brief Triangulates a box into positions, normals and triangle indices, in the box's own local space.
 *
 * @details
 * - Six quads, one per face, each with its own outward normal: 24 vertices and 12 triangles.
 * - Winding is counter-clockwise seen from outside, so the triangles can be drawn with back-face culling as they are.
 *
 * @param box     The box to triangulate; one whose `isValid()` is false answers an empty value.
 * @param quality Accepted for a uniform entry set and ignored: a box is a closed polyhedron, not a surface of
 *                revolution.
 * @return The triangles, empty when @p box is unusable.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
[[nodiscard]] Triangulation<T> tessellateSolid(const BoxSolid<T>& box, const TessellationQuality& quality = {});

/**
 * @brief Triangulates a sphere into positions, normals and triangle indices, in the sphere's own local space.
 *
 * @details
 * - A latitude band closed by two pole fans, with the exact radial normals of the analytic sphere.
 * - The local space is the sphere's: centred on the origin, radius `sphere.radius`.
 *
 * @param sphere  The sphere to triangulate; one whose `isValid()` is false answers an empty value.
 * @param quality Fineness of the triangulation; `around` is clamped to at least 3 and `along` to at least 2.
 * @return The triangles, empty when @p sphere is unusable.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
[[nodiscard]] Triangulation<T> tessellateSolid(const SphereSolid<T>& sphere, const TessellationQuality& quality = {});

/**
 * @brief Triangulates a cylinder into positions, normals and triangle indices, in the cylinder's own local space.
 *
 * @details
 * - A side band plus a cap fan at each end; the rim is duplicated between the side (radial normal) and the caps (axial
 *   normals), because one position cannot carry two normals.
 * - The local space is the cylinder's: axis on +Z, spanning `[-half_height, +half_height]`.
 *
 * @param cylinder The cylinder to triangulate; one whose `isValid()` is false answers an empty value.
 * @param quality  Fineness of the triangulation; `around` is clamped to at least 3, `along` is ignored.
 * @return The triangles, empty when @p cylinder is unusable.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
[[nodiscard]] Triangulation<T> tessellateSolid(const CylinderSolid<T>& cylinder, const TessellationQuality& quality = {});

/**
 * @brief Triangulates a cone into positions, normals and triangle indices, in the cone's own local space.
 *
 * @details
 * - A side fan from the rim to an apex plus a base fan; the apex is duplicated per azimuth segment, each copy carrying
 *   that segment's slanted normal.
 * - The local space is the cone's: axis on +Z, spanning `[-half_height, +half_height]`, base at `-half_height`.
 *
 * @param cone    The cone to triangulate; one whose `isValid()` is false answers an empty value.
 * @param quality Fineness of the triangulation; `around` is clamped to at least 3, `along` is ignored.
 * @return The triangles, empty when @p cone is unusable.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
[[nodiscard]] Triangulation<T> tessellateSolid(const ConeSolid<T>& cone, const TessellationQuality& quality = {});

/**
 * @brief Triangulates an ellipsoid into positions, normals and triangle indices, in the ellipsoid's own local space.
 *
 * @details
 * - A latitude band closed by two pole fans, with the exact normal `(x / a^2, y / b^2, z / c^2)` normalised.
 * - The local space is the ellipsoid's: centred on the origin, with the three semi-axes.
 *
 * @param ellipsoid The ellipsoid to triangulate; one whose `isValid()` is false answers an empty value.
 * @param quality   Fineness of the triangulation; `around` is clamped to at least 3 and `along` to at least 2.
 * @return The triangles, empty when @p ellipsoid is unusable.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
[[nodiscard]] Triangulation<T> tessellateSolid(const EllipsoidSolid<T>& ellipsoid, const TessellationQuality& quality = {});

VN_GEOM_SPATIAL_NS_END
