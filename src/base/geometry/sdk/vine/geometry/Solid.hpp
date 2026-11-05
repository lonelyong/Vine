#pragma once

#include "geometry_global.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Ray.hpp"
#include "RayHit.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief The nearest point of a solid to a query point, and the distance to it.
 *
 * @details
 * - `point` is the nearest point of the **solid**, not only of its boundary: a query inside the solid answers the query
 *   itself, and `distance` is therefore 0 there.
 * - `distance` is `|point - query|`, so one result answers both "where is the nearest point" and "how far is it".
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct SolidProjection {
    /// The nearest point of the solid; the query itself when the query is inside.
    vn::math::Point3<T> point{};

    /// Distance from the query to @p point; 0 inside the solid.
    T distance{ T(0) };
};

namespace detail
{

/**
 * @brief Solves `sum_i (a_i * q_i / (a_i^2 + lambda))^2 = 1` for the ellipsoid's nearest point.
 *
 * @details
 * - The Lagrange condition of "nearest point on `sum (p_i / a_i)^2 = 1`" gives `p_i = a_i^2 q_i / (a_i^2 + lambda)`;
 *   substituting it into the constraint leaves this one-variable, strictly decreasing equation, whose root is unique
 *   for a query outside the ellipsoid.
 * - The bracket is grown by doubling and then halved, which is slower than Newton but cannot leave the root and needs
 *   no derivative.
 *
 * @param radius_x First semi-axis.
 * @param radius_y Second semi-axis.
 * @param radius_z Third semi-axis.
 * @param query    Query point, known to be outside the ellipsoid.
 * @param tolerance Relative bracket width at which the search stops.
 * @return The Lagrange multiplier `lambda >= 0`.
 */
template <vn::math::FP T>
[[nodiscard]] T solidEllipsoidMultiplier(T radius_x, T radius_y, T radius_z, const vn::math::Point3<T>& query, T tolerance)
{
    const T numerator_x = radius_x * query.x;
    const T numerator_y = radius_y * query.y;
    const T numerator_z = radius_z * query.z;
    const T square_x    = radius_x * radius_x;
    const T square_y    = radius_y * radius_y;
    const T square_z    = radius_z * radius_z;

    const auto residual = [&](T lambda) {
        const T nx = numerator_x / (square_x + lambda);
        const T ny = numerator_y / (square_y + lambda);
        const T nz = numerator_z / (square_z + lambda);
        return nx * nx + ny * ny + nz * nz - T(1);
    };

    T high = T(1);
    while (residual(high) > T(0)) {
        high *= T(2);
    }

    T low = T(0);
    for (int iteration = 0; iteration < 256; ++iteration) {
        if (high - low <= tolerance * std::max(T(1), high)) {
            break;
        }
        const T middle = low + (high - low) * T(0.5);
        if (residual(middle) > T(0)) {
            low = middle;
        }
        else {
            high = middle;
        }
    }

    return low + (high - low) * T(0.5);
}

/**
 * @brief Appends one crossing if it is on the forward ray, with the normal turned back towards the ray.
 *
 * @param hits     Collected crossings.
 * @param ray      The ray being cast.
 * @param distance Distance along the ray; a negative one is behind the origin and is dropped.
 * @param outward  Outward normal at the crossing, before the orientation fix; may be zero at a singular point.
 * @param eps      Numerical vanishing threshold.
 */
template <vn::math::FP T>
void solidAppendHit(std::vector<RayHit<T>>& hits, const Ray<T>& ray, T distance, const vn::math::Vector3<T>& outward, T eps)
{
    if (!std::isfinite(distance) || distance < -eps) {
        return;
    }

    RayHit<T> hit{};
    hit.t     = (distance < T(0)) ? T(0) : distance;
    hit.point = ray.pointAt(hit.t);

    const T length = outward.length();
    hit.normal     = length > eps ? outward / length : vn::math::Vector3<T>(T(0), T(0), T(0));
    if (hit.normal.dot(ray.direction) > T(0)) {
        hit.normal = -hit.normal;
    }
    hits.push_back(hit);
}

/**
 * @brief Sorts the crossings by distance and merges the ones closer together than @p eps.
 *
 * @param hits Collected crossings.
 * @param eps  Distance within which two crossings are the same crossing.
 */
template <vn::math::FP T>
void solidSortHits(std::vector<RayHit<T>>& hits, T eps)
{
    if (hits.size() < 2) {
        return;
    }

    std::sort(hits.begin(), hits.end(), [](const RayHit<T>& left, const RayHit<T>& right) { return left.t < right.t; });
    hits.erase(std::unique(hits.begin(), hits.end(), [eps](const RayHit<T>& left, const RayHit<T>& right) { return std::abs(left.t - right.t) <= eps; }),
               hits.end());
}

/**
 * @brief Solves `a t^2 + b t + c = 0`, answering the root count (0, 1 or 2).
 *
 * @param a   Quadratic coefficient; a vanishing one degrades to the linear equation.
 * @param b   Linear coefficient.
 * @param c   Constant term.
 * @param low  Receives the smaller root.
 * @param high Receives the larger root.
 * @param eps Numerical vanishing threshold for @p a and @p b.
 * @return The number of roots written.
 */
template <vn::math::FP T>
int solidSolveQuadratic(T a, T b, T c, T& low, T& high, T eps)
{
    if (!(std::abs(a) > eps)) {
        if (!(std::abs(b) > eps)) {
            return 0;
        }
        low = high = -c / b;
        return 1;
    }

    const T discriminant = b * b - 4 * a * c;
    if (discriminant < T(0)) {
        return 0;
    }

    const T root = std::sqrt(discriminant);
    low          = (-b - root) / (2 * a);
    high         = (-b + root) / (2 * a);
    if (low > high) {
        std::swap(low, high);
    }
    return 2;
}

/**
 * @brief Appends the crossings of a ray with an axis-aligned box at the origin, by the slab method.
 *
 * @param hits   Collected crossings.
 * @param ray    The ray being cast.
 * @param half_x Half extent along X.
 * @param half_y Half extent along Y.
 * @param half_z Half extent along Z.
 * @param eps    Numerical vanishing threshold for a parallel slab.
 */
template <vn::math::FP T>
void solidAppendBoxHits(std::vector<RayHit<T>>& hits, const Ray<T>& ray, T half_x, T half_y, T half_z, T eps)
{
    T                    near_distance = -std::numeric_limits<T>::infinity();
    T                    far_distance  = std::numeric_limits<T>::infinity();
    vn::math::Vector3<T> near_normal(T(0), T(0), T(0));
    vn::math::Vector3<T> far_normal(T(0), T(0), T(0));

    const T origin[3]    = { ray.origin.x, ray.origin.y, ray.origin.z };
    const T direction[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
    const T half[3]      = { half_x, half_y, half_z };

    // One face normal per axis; `sign` picks which of the pair.
    const auto axis_normal = [](int axis, T sign) { return vn::math::Vector3<T>(axis == 0 ? sign : T(0), axis == 1 ? sign : T(0), axis == 2 ? sign : T(0)); };

    for (int axis = 0; axis < 3; ++axis) {
        if (!(std::abs(direction[axis]) > eps)) {
            // Parallel to this pair of faces: inside the slab it never leaves it, outside it never enters.
            if (std::abs(origin[axis]) > half[axis] + eps) {
                return;
            }
            continue;
        }

        T    low         = (-half[axis] - origin[axis]) / direction[axis];
        T    high        = (half[axis] - origin[axis]) / direction[axis];
        auto low_normal  = axis_normal(axis, T(-1));
        auto high_normal = axis_normal(axis, T(1));
        if (low > high) {
            std::swap(low, high);
            std::swap(low_normal, high_normal);
        }

        if (low > near_distance) {
            near_distance = low;
            near_normal   = low_normal;
        }
        if (high < far_distance) {
            far_distance = high;
            far_normal   = high_normal;
        }
        if (near_distance > far_distance) {
            return;
        }
    }

    solidAppendHit(hits, ray, near_distance, near_normal, eps);
    solidAppendHit(hits, ray, far_distance, far_normal, eps);
}

} // namespace detail

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
 * - Holding one of these by value: `SolidShape<T>` is the variant of the five kinds, which is what a model stores
 *   (`Primitive`). A function that only needs to ask a question takes `const Solid<T>&` and calls the virtuals; one
 *   that must build something per kind (a mesh, a B-rep, an element in a description file) visits `SolidShape<T>`
 *   instead, which is exhaustive at compile time.
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

/**
 * @brief Axis-aligned box centred on the origin.
 *
 * @details
 * - The three extents are stored **halved**, because every formula in this space is written that way and the field
 *   names say which half each one is.
 */
template <vn::math::FP T>
class BoxSolid final : public Solid<T> {
  public:
    /// Half extent along X.
    T half_x{ T(0) };

    /// Half extent along Y.
    T half_y{ T(0) };

    /// Half extent along Z.
    T half_z{ T(0) };

  public:
    /// Constructs the degenerate box (all extents 0), which `isValid()` refuses.
    BoxSolid() = default;

    /**
     * @brief Constructs a box from its three half extents.
     *
     * @param hx Half extent along X.
     * @param hy Half extent along Y.
     * @param hz Half extent along Z.
     */
    BoxSolid(T hx, T hy, T hz)
      : half_x(hx)
      , half_y(hy)
      , half_z(hz)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

/**
 * @brief SphereSolid centred on the origin.
 */
template <vn::math::FP T>
class SphereSolid final : public Solid<T> {
  public:
    /// Radius.
    T radius{ T(0) };

  public:
    /// Constructs the degenerate sphere (radius 0), which `isValid()` refuses.
    SphereSolid() = default;

    /**
     * @brief Constructs a sphere of the given radius.
     * @param r Radius.
     */
    explicit SphereSolid(T r)
      : radius(r)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

/**
 * @brief Right circular cylinder around +Z, spanning `[-half_height, +half_height]`.
 */
template <vn::math::FP T>
class CylinderSolid final : public Solid<T> {
  public:
    /// Radius.
    T radius{ T(0) };

    /// Half of the height; the end caps sit at `-half_height` and `+half_height`.
    T half_height{ T(0) };

  public:
    /// Constructs the degenerate cylinder, which `isValid()` refuses.
    CylinderSolid() = default;

    /**
     * @brief Constructs a cylinder from its radius and half height.
     *
     * @param r           Radius.
     * @param hh          Half of the height (the full height is `2 * hh`).
     */
    CylinderSolid(T r, T hh)
      : radius(r)
      , half_height(hh)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

/**
 * @brief Right circular cone with its apex at `+half_height` and its base disk at `-half_height`.
 */
template <vn::math::FP T>
class ConeSolid final : public Solid<T> {
  public:
    /// Radius of the base disk.
    T base_radius{ T(0) };

    /// Half of the height; the apex sits at `+half_height` and the base at `-half_height`.
    T half_height{ T(0) };

  public:
    /// Constructs the degenerate cone, which `isValid()` refuses.
    ConeSolid() = default;

    /**
     * @brief Constructs a cone from its base radius and half height.
     *
     * @param r  Radius of the base disk.
     * @param hh Half of the height (the full height is `2 * hh`).
     */
    ConeSolid(T r, T hh)
      : base_radius(r)
      , half_height(hh)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

/**
 * @brief Triaxial ellipsoid centred on the origin, axes along X / Y / Z.
 */
template <vn::math::FP T>
class EllipsoidSolid final : public Solid<T> {
  public:
    /// Semi-axis along X.
    T radius_x{ T(0) };

    /// Semi-axis along Y.
    T radius_y{ T(0) };

    /// Semi-axis along Z.
    T radius_z{ T(0) };

  public:
    /// Constructs the degenerate ellipsoid, which `isValid()` refuses.
    EllipsoidSolid() = default;

    /**
     * @brief Constructs an ellipsoid from its three semi-axes.
     *
     * @param rx Semi-axis along X.
     * @param ry Semi-axis along Y.
     * @param rz Semi-axis along Z.
     */
    EllipsoidSolid(T rx, T ry, T rz)
      : radius_x(rx)
      , radius_y(ry)
      , radius_z(rz)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

/// The closed set of analytic solids, stored by value: what a model holds.
template <vn::math::FP T>
using SolidShape = std::variant<BoxSolid<T>, SphereSolid<T>, CylinderSolid<T>, ConeSolid<T>, EllipsoidSolid<T>>;

using Solidf      = Solid<float>;
using Solidd      = Solid<double>;
using BoxSolidf        = BoxSolid<float>;
using BoxSolidd        = BoxSolid<double>;
using SphereSolidf     = SphereSolid<float>;
using SphereSolidd     = SphereSolid<double>;
using CylinderSolidf   = CylinderSolid<float>;
using CylinderSolidd   = CylinderSolid<double>;
using ConeSolidf       = ConeSolid<float>;
using ConeSolidd       = ConeSolid<double>;
using EllipsoidSolidf  = EllipsoidSolid<float>;
using EllipsoidSolidd  = EllipsoidSolid<double>;
using SolidShapef = SolidShape<float>;
using SolidShaped = SolidShape<double>;

/**
 * @brief Returns the family view of a stored solid.
 *
 * @details
 * - The reference is how a caller asks a question without knowing the kind: `bounds()`, `closestPoint()` and
 *   `intersectWith()` are virtual, so no `switch` is involved.
 * - The reference stays valid as long as the variant does, and follows a replacement of the variant's contents.
 *
 * @param shape The stored solid.
 * @return The family interface of the object @p shape holds.
 */
template <vn::math::FP T>
[[nodiscard]] const Solid<T>& solidOf(const SolidShape<T>& shape)
{
    return std::visit([](const auto& kind) -> const Solid<T>& { return kind; }, shape);
}

/**
 * @brief Returns the mutable family view of a stored solid.
 *
 * @param shape The stored solid.
 * @return The family interface of the object @p shape holds.
 */
template <vn::math::FP T>
[[nodiscard]] Solid<T>& solidOf(SolidShape<T>& shape)
{
    return std::visit([](auto& kind) -> Solid<T>& { return kind; }, shape);
}

template <vn::math::FP T>
bool BoxSolid<T>::isValid() const noexcept
{
    return half_x > T(0) && half_y > T(0) && half_z > T(0);
}

template <vn::math::FP T>
bool BoxSolid<T>::hasVolume(T eps) const noexcept
{
    return half_x * T(2) > eps && half_y * T(2) > eps && half_z * T(2) > eps;
}

template <vn::math::FP T>
Aabb<T> BoxSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-half_x, -half_y, -half_z), vn::math::Point3<T>(half_x, half_y, half_z));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> BoxSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const auto point = vn::math::Point3<T>(std::clamp(query.x, -half_x, half_x), std::clamp(query.y, -half_y, half_y), std::clamp(query.z, -half_z, half_z));
    return SolidProjection<T>{ point, (point - query).length() };
}

template <vn::math::FP T>
std::vector<RayHit<T>> BoxSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    detail::solidAppendBoxHits(hits, ray, half_x, half_y, half_z, eps);
    detail::solidSortHits(hits, eps);
    return hits;
}

template <vn::math::FP T>
bool SphereSolid<T>::isValid() const noexcept
{
    return radius > T(0);
}

template <vn::math::FP T>
bool SphereSolid<T>::hasVolume(T eps) const noexcept
{
    return radius > eps;
}

template <vn::math::FP T>
Aabb<T> SphereSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-radius, -radius, -radius), vn::math::Point3<T>(radius, radius, radius));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> SphereSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const auto offset   = vn::math::Vector3<T>(query.x, query.y, query.z);
    const T    distance = offset.length();
    if (distance <= radius) {
        return SolidProjection<T>{ query, T(0) };
    }

    const T scale = radius / distance;
    return SolidProjection<T>{ vn::math::Point3<T>(offset.x * scale, offset.y * scale, offset.z * scale), distance - radius };
}

template <vn::math::FP T>
std::vector<RayHit<T>> SphereSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const auto offset = vn::math::Vector3<T>(ray.origin.x, ray.origin.y, ray.origin.z);
    const T    a      = ray.direction.dot(ray.direction);
    const T    b      = T(2) * offset.dot(ray.direction);
    const T    c      = offset.dot(offset) - radius * radius;

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point  = ray.pointAt(distance);
            const auto normal = vn::math::Vector3<T>(point.x, point.y, point.z);
            detail::solidAppendHit(hits, ray, distance, normal, eps);
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

template <vn::math::FP T>
bool CylinderSolid<T>::isValid() const noexcept
{
    return radius > T(0) && half_height > T(0);
}

template <vn::math::FP T>
bool CylinderSolid<T>::hasVolume(T eps) const noexcept
{
    return radius > eps && half_height * T(2) > eps;
}

template <vn::math::FP T>
Aabb<T> CylinderSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-radius, -radius, -half_height), vn::math::Point3<T>(radius, radius, half_height));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> CylinderSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const T radial = std::hypot(query.x, query.y);
    if (std::abs(query.z) <= half_height && radial <= radius) {
        return SolidProjection<T>{ query, T(0) };
    }

    const T    scale = (radial > radius) ? radius / radial : T(1);
    const auto point = vn::math::Point3<T>(query.x * scale, query.y * scale, std::clamp(query.z, -half_height, half_height));
    return SolidProjection<T>{ point, (point - query).length() };
}

template <vn::math::FP T>
std::vector<RayHit<T>> CylinderSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const T a = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y;
    const T b = T(2) * (ray.origin.x * ray.direction.x + ray.origin.y * ray.direction.y);
    const T c = ray.origin.x * ray.origin.x + ray.origin.y * ray.origin.y - radius * radius;

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point = ray.pointAt(distance);
            if (std::abs(point.z) <= half_height + eps) {
                detail::solidAppendHit(hits, ray, distance, vn::math::Vector3<T>(point.x, point.y, T(0)), eps);
            }
        }
    }

    if (std::abs(ray.direction.z) > eps) {
        for (const T end : { -half_height, half_height }) {
            const T    distance = (end - ray.origin.z) / ray.direction.z;
            const auto point    = ray.pointAt(distance);
            if (point.x * point.x + point.y * point.y <= radius * radius + eps) {
                detail::solidAppendHit(hits, ray, distance, vn::math::Vector3<T>(T(0), T(0), end > T(0) ? T(1) : T(-1)), eps);
            }
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

template <vn::math::FP T>
bool ConeSolid<T>::isValid() const noexcept
{
    return base_radius > T(0) && half_height > T(0);
}

template <vn::math::FP T>
bool ConeSolid<T>::hasVolume(T eps) const noexcept
{
    return base_radius > eps && half_height * T(2) > eps;
}

template <vn::math::FP T>
Aabb<T> ConeSolid<T>::bounds() const noexcept
{
    // The base disk is the widest circle the cone has, so its circumscribing box bounds the whole body.
    return Aabb<T>(vn::math::Point3<T>(-base_radius, -base_radius, -half_height), vn::math::Point3<T>(base_radius, base_radius, half_height));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> ConeSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const T height = half_height * T(2);
    const T slope  = base_radius / height; // radius per unit distance below the apex
    const T radial = std::hypot(query.x, query.y);

    const T radius_here = slope * (half_height - query.z);
    if (query.z >= -half_height && query.z <= half_height && radial <= radius_here) {
        return SolidProjection<T>{ query, T(0) };
    }

    // Candidate 1: the base disk, which is not on the meridian profile.
    const T    base_scale = (radial > base_radius) ? base_radius / radial : T(1);
    const auto base_point = vn::math::Point3<T>(query.x * base_scale, query.y * base_scale, -half_height);

    // Candidate 2: the lateral surface, whose nearest point lies in the query's own meridian plane. Projecting onto the
    // profile segment from the base rim (base_radius, -half_height) to the apex (0, +half_height) is therefore exact.
    const T delta_radial   = -base_radius;
    const T delta_z        = height;
    const T length_squared = delta_radial * delta_radial + delta_z * delta_z;
    T       parameter      = ((radial - base_radius) * delta_radial + (query.z + half_height) * delta_z) / length_squared;
    parameter              = std::clamp(parameter, T(0), T(1));

    const T projected_radial = base_radius + delta_radial * parameter;
    const T projected_z      = -half_height + delta_z * parameter;

    // A query on the axis has no direction of its own; any meridian answers, and +X keeps `|point - query|` equal to
    // the profile distance.
    const T    direction_x   = (radial > vn::math::EPS<T>()) ? query.x / radial : T(1);
    const T    direction_y   = (radial > vn::math::EPS<T>()) ? query.y / radial : T(0);
    const auto lateral_point = vn::math::Point3<T>(direction_x * projected_radial, direction_y * projected_radial, projected_z);

    const T base_distance    = (base_point - query).length();
    const T lateral_distance = (lateral_point - query).length();
    return (base_distance <= lateral_distance) ? SolidProjection<T>{ base_point, base_distance } : SolidProjection<T>{ lateral_point, lateral_distance };
}

template <vn::math::FP T>
std::vector<RayHit<T>> ConeSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const T height = half_height * T(2);
    const T slope  = base_radius / height; // radius at a unit distance below the apex

    const T a = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y - slope * slope * ray.direction.z * ray.direction.z;
    const T b = T(2) * (ray.origin.x * ray.direction.x + ray.origin.y * ray.direction.y + slope * slope * (half_height - ray.origin.z) * ray.direction.z);
    const T c = ray.origin.x * ray.origin.x + ray.origin.y * ray.origin.y - slope * slope * (half_height - ray.origin.z) * (half_height - ray.origin.z);

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point = ray.pointAt(distance);
            if (point.z >= -half_height - eps && point.z <= half_height + eps) {
                const auto normal = vn::math::Vector3<T>(point.x, point.y, slope * slope * (half_height - point.z));
                detail::solidAppendHit(hits, ray, distance, normal, eps);
            }
        }
    }

    if (std::abs(ray.direction.z) > eps) {
        const T    distance = (-half_height - ray.origin.z) / ray.direction.z;
        const auto point    = ray.pointAt(distance);
        if (point.x * point.x + point.y * point.y <= base_radius * base_radius + eps) {
            detail::solidAppendHit(hits, ray, distance, vn::math::Vector3<T>(T(0), T(0), T(-1)), eps);
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

template <vn::math::FP T>
bool EllipsoidSolid<T>::isValid() const noexcept
{
    return radius_x > T(0) && radius_y > T(0) && radius_z > T(0);
}

template <vn::math::FP T>
bool EllipsoidSolid<T>::hasVolume(T eps) const noexcept
{
    return radius_x > eps && radius_y > eps && radius_z > eps;
}

template <vn::math::FP T>
Aabb<T> EllipsoidSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-radius_x, -radius_y, -radius_z), vn::math::Point3<T>(radius_x, radius_y, radius_z));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> EllipsoidSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    if (!isValid()) {
        return std::nullopt;
    }

    const T normalized =
        (query.x / radius_x) * (query.x / radius_x) + (query.y / radius_y) * (query.y / radius_y) + (query.z / radius_z) * (query.z / radius_z);
    if (normalized <= T(1)) {
        return SolidProjection<T>{ query, T(0) };
    }

    const T    lambda = detail::solidEllipsoidMultiplier(radius_x, radius_y, radius_z, query, tolerance);
    const auto point  = vn::math::Point3<T>(radius_x * radius_x * query.x / (radius_x * radius_x + lambda),
                                            radius_y * radius_y * query.y / (radius_y * radius_y + lambda),
                                            radius_z * radius_z * query.z / (radius_z * radius_z + lambda));
    return SolidProjection<T>{ point, (point - query).length() };
}

template <vn::math::FP T>
std::vector<RayHit<T>> EllipsoidSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const vn::math::Vector3<T> offset(ray.origin.x / radius_x, ray.origin.y / radius_y, ray.origin.z / radius_z);
    const vn::math::Vector3<T> direction(ray.direction.x / radius_x, ray.direction.y / radius_y, ray.direction.z / radius_z);
    const T                    a = direction.dot(direction);
    const T                    b = T(2) * offset.dot(direction);
    const T                    c = offset.dot(offset) - T(1);

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point  = ray.pointAt(distance);
            const auto normal = vn::math::Vector3<T>(point.x / (radius_x * radius_x), point.y / (radius_y * radius_y), point.z / (radius_z * radius_z));
            detail::solidAppendHit(hits, ray, distance, normal, eps);
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

VN_GEOMETRY_NS_END
