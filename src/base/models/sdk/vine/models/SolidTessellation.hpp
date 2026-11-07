#pragma once

#include <vine/models/models_global.hpp>

#include <vine/intrusive_ptr.hpp>
#include <vine/geom/spatial/SolidShape.hpp>
#include <vine/models/TriangleMesh.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief How finely one analytic solid is triangulated.
 *
 * @details
 * - `around` is the number of azimuth intervals of a surface of revolution; `along` is the number of latitude
 *   intervals of a sphere / ellipsoid. A box is a closed polyhedron and ignores both, and a cylinder's cap and a cone's
 *   base are fans whose rim is `around` segments.
 * - The values are clamped where they would stop describing a surface: `around` is at least 3 (a ring needs three
 *   corners) and `along` is at least 2 (a sphere needs at least one ring between its poles).
 * - More segments cost vertices and triangles linearly; the shape itself does not change.
 */
struct TessellationQuality {
    /// Azimuth intervals of a surface of revolution; clamped to at least 3.
    int around{ 32 };

    /// Latitude intervals of a sphere / ellipsoid; clamped to at least 2.
    int along{ 16 };
};

/**
 * @brief Triangulates an analytic solid into an indexed triangle mesh, in the solid's own local space.
 *
 * @details
 * - An analytic solid is a volume, so the mesh is **closed**: a box is six quads, a sphere / ellipsoid is a latitude
 *   band closed by two pole fans, a cylinder is a side band plus two cap fans, and a cone is a side fan plus its base
 *   fan. There is no separate solid / shell switch, because the analytic solid has no shell form.
 * - Per-vertex normals are the exact analytic ones: the face normal of a box face, the radial normal of a sphere or
 *   ellipsoid (`(x / a^2, y / b^2, z / c^2)` normalised), the radial side normal and axial cap normal of a cylinder,
 *   and the slanted side normal of a cone. Vertices are therefore **duplicated** where a position carries more than one
 *   normal - along a box edge, between a cylinder's side and cap, and at a cone's apex - which is why the mesh is
 *   indexed instead of the smallest possible vertex set.
 * - Winding is counter-clockwise seen from outside, so the mesh can be drawn with back-face culling as it is.
 * - The local space is the solid's: a box, sphere and ellipsoid are centred on the origin, and a cylinder / cone spans
 *   `[-half_height, +half_height]` on +Z with the cone's base at `-half_height`.
 * - No texture-coordinate channel is written: the analytic solids carry no parameterisation choice, and a consumer that
 *   needs one can derive it from the positions.
 *
 * @param solid   The solid to triangulate; a solid whose `isValid()` is false answers `nullptr`.
 * @param quality Fineness of the triangulation; ignored by a box.
 * @return An indexed triangle mesh with positions, normals and indices, or `nullptr` when @p solid is unusable.
 *
 * @warning
 * - The result is a new mesh every call: nothing is cached, and no revision is shared with any modeling shape.
 *
 * @note
 * - The entry takes the stored `vn::geom::spatial::SolidShapef` variant rather than the `vn::geom::spatial::Solid<float>` interface: producing a mesh needs a
 *   different answer per kind, and a `std::visit` over the closed set is exhaustive at compile time, while a virtual
 *   interface would let a new kind fall through silently. A caller that holds a `Primitive` passes `shape()`.
 * - This is the one place an analytic solid becomes drawable: `graphics::geometryFromShape()` takes a mesh, so a caller
 *   hands it the result of this function rather than teaching the renderer a second shape kind (`brep-operations.md`
 *   1.3).
 */
VN_MODELS_API vn::intrusive_ptr<TriangleMesh> tessellateSolid(const vn::geom::spatial::SolidShapef& solid, const TessellationQuality& quality = {});

VN_MODELS_NS_END
