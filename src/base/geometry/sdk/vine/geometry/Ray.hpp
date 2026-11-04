#pragma once

#include "geometry_global.hpp"

#include <cmath>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"
#include "RayHit.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A half-line in space: an origin and a unit direction.
 *
 * @details
 * - The direction is normalized on construction, so `t` in `pointAt()` and in every query is a distance in the ray's
 *   own space rather than a multiple of the direction's length. This is what separates a ray from `Line`, whose
 *   direction may be any length and whose parameter is tied to it.
 * - A ray built without a direction is zero-length and aimless: every query answers false instead of dividing by
 *   zero, so a ray can be stored before it is aimed.
 * - The type is a value: no `Shape` identity, no revision, no allocation (see `geometry-primitives.md` 2.4).
 * - It is defined entirely in this header and exports no symbol, so a query compiles into the caller: a ray cast in
 *   another module is inlined instead of crossing a library boundary per triangle (see `geometry-ray.md` 2.7).
 *
 * @warning
 * - Normalizing converts a zero direction to zero instead of throwing; check `direction` when the input may be
 *   degenerate and the difference matters.
 * - `direction` is a plain field: assigning a vector that is not unit length to it breaks what `t` means. Construct
 *   the ray from an origin and a direction instead of writing the field.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class Ray {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

  public:
    /**
     * @brief Constructs an aimless ray: origin and direction both at zero.
     */
    Ray();

    /**
     * @brief Constructs a ray from an origin and a direction.
     *
     * @param origin_point     The point the ray starts at.
     * @param direction_vector Direction of travel; normalized before storage, and a zero vector leaves the ray aimless.
     */
    Ray(const Point& origin_point, const Vector& direction_vector);

  public:
    /**
     * @brief Returns the point at distance @p t along the ray.
     *
     * @param t Distance from the origin, in the ray's space; negative values point backwards.
     * @return `origin + direction * t`.
     */
    [[nodiscard]]
    Point pointAt(T t) const;

    /**
     * @brief Returns the distance from @p point to the half-line (not to the infinite line).
     *
     * @details
     * - A point behind the origin measures to the origin, because the ray does not extend backwards.
     *
     * @param point The point to measure.
     * @return The distance; never negative.
     */
    [[nodiscard]]
    T distanceToPoint(const Point& point) const;

  public:
    /// Where the ray starts.
    Point origin;

    /// Unit direction of travel; zero when the ray was constructed without one.
    Vector direction;
};

using Rayf = Ray<float>;
using Rayd = Ray<double>;

template <vn::math::FP T>
Ray<T>::Ray()
  : origin(T(0), T(0), T(0))
  , direction(T(0), T(0), T(0))
{}

template <vn::math::FP T>
Ray<T>::Ray(const Point& origin_point, const Vector& direction_vector)
  : origin(origin_point)
  , direction(direction_vector.length() > math::EPS<T>() ? direction_vector.normalized() : Vector(T(0), T(0), T(0)))
{}

template <vn::math::FP T>
typename Ray<T>::Point Ray<T>::pointAt(T t) const
{
    return origin + direction * t;
}

template <vn::math::FP T>
T Ray<T>::distanceToPoint(const Point& point) const
{
    // The ray stops at its origin, so a point behind it measures to the origin rather than to the infinite line.
    const Vector to_point = point - origin;
    const T      t        = to_point.dot(direction);
    if (t <= T(0)) {
        return to_point.length();
    }
    return (point - pointAt(t)).length();
}

/**
 * @brief Intersects a ray with one triangle, face and back face alike.
 *
 * @details
 * - The normal is the triangle's geometric normal, turned to face back towards the ray, so a back-face hit reports
 *   the same surface orientation as a front-face hit.
 * - A degenerate (collinear) triangle is a miss, not a NaN.
 * - The body lives in this header, so a traversal loop keeps the per-triangle test inlined (see `geometry-ray.md` 2.7).
 *
 * @param ray The ray to test.
 * @param a   First triangle corner.
 * @param b   Second triangle corner.
 * @param c   Third triangle corner.
 * @param hit Receives the hit when the call returns true; untouched otherwise.
 * @param eps Relative tolerance for the ray running parallel to the triangle's plane (a sine of that angle, not an
 *            area, so a small triangle is still hit).
 * @return True when the ray crosses the triangle at t >= 0.
 */
template <vn::math::FP T>
[[nodiscard]] bool intersectTriangle(const Ray<T>& ray, const vn::math::Point3<T>& a, const vn::math::Point3<T>& b,
                                     const vn::math::Point3<T>& c, RayHit<T>& hit, T eps = math::EPS<T>())
{
    // Möller-Trumbore. The determinant is zero for a ray parallel to the triangle's plane and for a degenerate
    // triangle, which is the single place both cases are rejected.
    const vn::math::Vector3<T> edge_a = b - a;
    const vn::math::Vector3<T> edge_b = c - a;
    const vn::math::Vector3<T> pvec   = ray.direction.cross(edge_b);
    const T                    det    = edge_a.dot(pvec);

    // Compared against the two edge lengths, so the tolerance is an ANGLE rather than an area: a small triangle is
    // still hit, while a triangle the ray runs into edge-on is not.
    if (std::abs(det) <= eps * edge_a.length() * edge_b.length()) {
        return false;
    }

    const T                    inv_det = T(1) / det;
    const vn::math::Vector3<T> tvec    = ray.origin - a;
    const T                    u       = tvec.dot(pvec) * inv_det;
    if (u < T(0) || u > T(1)) {
        return false;
    }

    const vn::math::Vector3<T> qvec = tvec.cross(edge_a);
    const T                    v    = ray.direction.dot(qvec) * inv_det;
    if (v < T(0) || u + v > T(1)) {
        return false;
    }

    const T t = edge_b.dot(qvec) * inv_det;
    if (t < T(0)) {
        return false;
    }

    vn::math::Vector3<T> normal = edge_a.cross(edge_b).normalized();
    if (normal.dot(ray.direction) > T(0)) {
        normal = normal * T(-1);
    }

    hit.t      = t;
    hit.point  = ray.pointAt(t);
    hit.normal = normal;
    return true;
}

VN_GEOMETRY_NS_END
