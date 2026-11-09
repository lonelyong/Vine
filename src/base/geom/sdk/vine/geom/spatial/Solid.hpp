#pragma once

#include <vine/geom/geom_global.hpp>

#include <optional>
#include <vector>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>

#include "Aabb.hpp"
#include "Ray.hpp"
#include "RayHit.hpp"
#include "SolidProjection.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief The analytic solid family: the five bodies a model is built from, behind one interface.
 *
 * @details
 * - A solid is a **volume**: it has an inside, so the nearest point of a query inside it is the query itself and its
 *   distance is 0, and a ray reports where it enters and where it leaves. That is what separates this family from
 *   `Surface<T>`, whose sheets have no inside and no caps (`geometry-solid.md` 2.2).
 * - The interface is the queries, because there is nothing to parameterise: an analytic body has no parameter domain,
 *   so it cannot offer the `evaluate` an interface like `Curve<T>` / `Surface<T>` is built around.
 * - `BoxSolid`, `SphereSolid`, `CylinderSolid`, `ConeSolid` and `EllipsoidSolid` are the closed set this repository models. Anything without an
 *   analytic formula — a NURBS body, a mesh, a boolean result — is a `BrepShape`, never a `Solid` (`G13`).
 * - The canonical local space is each kind's own: a box, sphere and ellipsoid are centred on the origin, and a cylinder
 *   / cone spans `[-height / 2, +height / 2]` on +Z with the cone's base at the lower end. Placement belongs to the node
 *   transform that holds the shape, never here.
 *
 * @warning
 * - A dimension at or below 0 makes a kind unusable: `isValid()` reports it and every query answers nothing
 *   (`std::nullopt` / an empty result) instead of dividing by zero.
 *
 * @note
 * - A function that only needs to ask a question takes `const Solid<T>&` and calls the virtuals. One that must build
 *   something per kind (a mesh, a B-rep, an element in a description file) takes the concrete kind - one overload per
 *   kind, or a visitor handed to `vn::models::visitPrimitive()` - so a new kind is a compile error at that call site
 *   instead of a silent fall-through.
 *
 * @tparam T Only accepts float and double.
 */
template <vn::math::FP T>
class Solid {
  public:
    virtual ~Solid() = default;

  public:
    /**
     * @brief Returns whether the dimensions of this kind can be queried.
     * @return `true` when every dimension is positive.
     */
    [[nodiscard]] virtual bool isValid() const noexcept = 0;

    /**
     * @brief Returns whether this body encloses a volume larger than @p eps in every characteristic size.
     *
     * @details
     * - Each kind answers from its own dimensions: a box from its three edges, a sphere / ellipsoid from its radii, a
     *   cylinder / cone from the radius and the height.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the body encloses a volume by that measure.
     */
    [[nodiscard]] virtual bool hasVolume(T eps) const noexcept = 0;

    /**
     * @brief Returns the axis-aligned bounds of this body, in the canonical local space.
     * @return The bounds; a degenerate body still answers a well formed box.
     */
    [[nodiscard]] virtual Aabb<T> bounds() const noexcept = 0;

    /**
     * @brief Returns the nearest point of this body to @p query, and the distance to it.
     *
     * @details
     * - The body is convex, so the nearest point is on the boundary when the query is outside and is the query itself
     *   when it is inside; `distance` is 0 in the latter case.
     * - Every kind is exact except the ellipsoid, whose Lagrange multiplier is solved numerically.
     *
     * @param query     The point to measure.
     * @param tolerance Relative bracket width at which the ellipsoid's solve stops; the inside tests are exact, so
     *                  whether a query is inside does not depend on this value.
     * @return The nearest point and the distance, or `std::nullopt` when `isValid()` is false.
     */
    [[nodiscard]] virtual std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const = 0;

    /**
     * @brief Returns the crossings of @p ray with this body, ascending by distance.
     *
     * @details
     * - A body is convex, so a ray reports at most two crossings: where it enters and where it leaves. A ray starting
     *   inside reports only the exit, because the entering crossing is behind its origin.
     * - Normals are the outward normals at the crossing, **turned back towards the ray** exactly as `RayHit` defines.
     * - Duplicates (a ray through a rim or an edge) are merged within @p eps.
     *
     * @param ray The ray to cast; an aimless ray crosses nothing.
     * @param eps Threshold for a vanished quadratic coefficient, a parallel slab and duplicate distances.
     * @return The crossings, ascending by `t`; empty when the ray misses or `isValid()` is false.
     *
     * @warning
     * - A crossing exactly at a cone's apex has no normal (the side's gradient vanishes there) and reports a zero
     *   vector, the same rule the surface algorithms follow at a singular parameter point.
     */
    [[nodiscard]] virtual std::vector<RayHit<T>> intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const = 0;
};

using Solidf      = Solid<float>;
using Solidd      = Solid<double>;

VN_GEOM_SPATIAL_NS_END
