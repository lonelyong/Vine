#pragma once

#include "geometry_global.hpp"

#include <cmath>
#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include "ElementarySurface.hpp"
#include "LineCurve.hpp"
#include "Ray.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A 3D plane represented by an origin point and a normal vector.
 *
 * @details
 * - As a `Surface<T>`, the plane's parameters are two in-plane coordinates measured from the axes
 *   `xAxis()` / `yAxis()`: `evaluate(u, v) = origin + u * xAxis() + v * yAxis()`, both in distance units because the
 *   axes are unit length.
 * - Those two axes are **derived from `normal` on each call**, not stored. A plane is a small value the intersection
 *   code copies, and the derivation is a projection and a normalisation - cheaper than a third vector on every copy.
 * - The parameterisation therefore follows `normal`: writing a different normal changes where `u = 0` starts, not just
 *   the plane it describes.
 * - Both parameters are unbounded and neither closes, which is what makes the plane the one surface with no seam.
 *
 * @warning
 * - `normal` is not normalised on storage: a zero normal leaves the plane undefined (`intersectWith` fails and the
 *   parameterisation falls back to the XY plane's axes).
 *
 * @tparam T Only accepts float and double.
 */
template <typename T>
class Plane : public ElementarySurface<T>
{
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct a plane from origin and normal.
     * @param origin A point on the plane.
     * @param normal Plane normal vector.
     */
    constexpr Plane(const math::Point3<T>& origin, const math::Vector3<T>& normal)
      : origin(origin)
      , normal(normal)
    {}

  public:
    /**
     * @brief Returns the point at parameters @p u and @p v.
     *
     * @param u Distance along `xAxis()` from `origin`.
     * @param v Distance along `yAxis()` from `origin`.
     * @return The point on the plane.
     */
    [[nodiscard]] math::Point3<T> evaluate(T u, T v) const override
    {
        return origin + xAxis() * u + yAxis() * v;
    }

    /**
     * @brief Returns the first derivative with respect to @p u: the in-plane X axis, which is constant.
     *
     * @param u Parameter; ignored, a plane's parameterisation is linear.
     * @param v Parameter; ignored, a plane's parameterisation is linear.
     * @return `xAxis()`, a unit in-plane vector.
     */
    [[nodiscard]] math::Vector3<T> derivativeU(T u, T v) const override
    {
        (void)u;
        (void)v;
        return xAxis();
    }

    /**
     * @brief Returns the first derivative with respect to @p v: the in-plane Y axis, which is constant.
     *
     * @param u Parameter; ignored, a plane's parameterisation is linear.
     * @param v Parameter; ignored, a plane's parameterisation is linear.
     * @return `yAxis()`, a unit in-plane vector.
     */
    [[nodiscard]] math::Vector3<T> derivativeV(T u, T v) const override
    {
        (void)u;
        (void)v;
        return yAxis();
    }

    /**
     * @brief Returns the lower end of the @p u domain: a plane is unbounded.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstUParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the @p u domain: a plane is unbounded.
     *
     * @return Positive infinity.
     */
    [[nodiscard]] T lastUParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the lower end of the @p v domain: a plane is unbounded.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the @p v domain: a plane is unbounded.
     *
     * @return Positive infinity.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns false: an infinite plane has no @p u seam.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isUClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: an infinite plane has no @p v seam.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isVClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a plane does not repeat along @p u.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isUPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a plane does not repeat along @p v.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isVPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a plane is not periodic along @p u.
     *
     * @return Always 0.
     */
    [[nodiscard]] T uPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns 0: a plane is not periodic along @p v.
     *
     * @return Always 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a plane is a linear parameterisation, so it is smooth to every order.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

  public:
    /**
     * @brief Intersect this plane with a line.
     * @param line Input line.
     * @param intersection_pt Output intersection point.
     * @param eps Numerical tolerance.
     * @return True when intersection exists.
     */
    [[nodiscard]]
    bool intersectWith(const LineCurve<T>& line, math::Point3<T>& intersection_pt, T eps = math::EPS<T>()) const;
    /**
     * @brief Intersect this plane with another plane.
     * @param plane Other plane.
     * @param intersection_line Output intersection line.
     * @param eps Numerical tolerance.
     * @return True when intersection line exists.
     */
    [[nodiscard]]
    bool intersectWith(const Plane<T>& plane, LineCurve<T>& intersection_line, T eps = math::EPS<T>()) const;

    /**
     * @brief Intersect this plane with a ray.
     *
     * @details
     * - A ray running parallel to the plane (or an aimless ray) is a miss, as is a plane behind the ray's origin.
     * - The reported normal is the plane's, turned to face back towards the ray.
     *
     * @param ray The ray to intersect.
     * @param hit Receives the hit when the call returns true; untouched otherwise.
     * @param eps Tolerance for the direction being parallel to the plane.
     * @return True when the ray crosses the plane at t >= 0.
     */
    [[nodiscard]]
    bool intersectWith(const Ray<T>& ray, RayHit<T>& hit, T eps = math::EPS<T>()) const;

  private:
    /**
     * @brief Returns the in-plane axis the @p u parameter is measured along.
     *
     * @details
     * - The world axis least aligned with the normal is projected into the plane and normalised, so the same normal
     *   always yields the same axis and no frame has to be stored or kept consistent.
     *
     * @return A unit vector in the plane; `(1, 0, 0)` when the normal is degenerate.
     */
    [[nodiscard]] math::Vector3<T> xAxis() const
    {
        const math::Vector3<T> zero(T(0), T(0), T(0));
        if (normal.length() <= math::EPS<T>()) {
            return math::Vector3<T>(T(1), T(0), T(0));
        }

        const math::Vector3<T> unit_normal = normal.normalized();
        const math::Vector3<T> candidates[3]
            = { math::Vector3<T>(T(1), T(0), T(0)), math::Vector3<T>(T(0), T(1), T(0)), math::Vector3<T>(T(0), T(0), T(1)) };

        const math::Vector3<T>* least_aligned = &candidates[0];
        for (const auto& candidate : candidates) {
            if (std::abs(unit_normal.dot(candidate)) < std::abs(unit_normal.dot(*least_aligned))) {
                least_aligned = &candidate;
            }
        }

        const math::Vector3<T> in_plane = *least_aligned - unit_normal * unit_normal.dot(*least_aligned);
        return in_plane.length() > math::EPS<T>() ? in_plane.normalized() : zero;
    }

    /**
     * @brief Returns the in-plane axis the @p v parameter is measured along: `normal x xAxis()`.
     *
     * @return A unit vector in the plane; `(0, 1, 0)` when the normal is degenerate.
     */
    [[nodiscard]] math::Vector3<T> yAxis() const
    {
        if (normal.length() <= math::EPS<T>()) {
            return math::Vector3<T>(T(0), T(1), T(0));
        }

        return normal.normalized().cross(xAxis());
    }

  public:
    math::Point3<T>  origin;
    math::Vector3<T> normal;
};

template <typename T>
bool Plane<T>::intersectWith(const LineCurve<T>& line, math::Point3<T>& intersection_pt, T eps) const
{
    // Plane–LineCurve intersection.
    //
    //   Plane:  n·(P − o) = 0
    //   LineCurve:   P = P₀ + t·d
    //
    // Substitute line into plane:
    //   n·(P₀ + t·d − o) = 0
    //   n·(P₀ − o) + n·(t·d) = 0
    //   n·(P₀ − o) + t·(n·d) = 0
    //
    // Solve for t:
    //   t·(n·d) = −n·(P₀ − o)
    //
    //         −n·(P₀ − o)
    //   t = --------------
    //            n·d
    //
    //   |n·d| ≤ eps  →  line ∥ plane, no unique intersection.

    math::Vector3<T> diff  = line.origin - origin;
    T                denom = normal.dot(line.direction);

    // Check if line is parallel to plane
    if (math::isZero(denom, eps)) {
        return false;
    }

    T numer         = -normal.dot(diff);
    T t             = numer / denom;
    intersection_pt = line.origin + line.direction * t;

    return true;
}

template <typename T>
bool Plane<T>::intersectWith(const Ray<T>& ray, RayHit<T>& hit, T eps) const
{
    const T denominator = normal.dot(ray.direction);
    if (std::abs(denominator) <= eps) {
        return false; // the ray runs parallel to the plane, or carries no direction
    }

    const math::Vector3<T> to_origin(origin.x - ray.origin.x, origin.y - ray.origin.y, origin.z - ray.origin.z);
    const T                t = normal.dot(to_origin) / denominator;
    if (t < T(0)) {
        return false; // the plane is behind the ray's origin
    }

    const math::Vector3<T> unit_normal = normal.normalized();

    hit.t      = t;
    hit.point  = ray.pointAt(t);
    hit.normal = denominator > T(0) ? math::Vector3<T>(-unit_normal.x, -unit_normal.y, -unit_normal.z) : unit_normal;
    return true;
}

using Planef = Plane<float>;
using Planed = Plane<double>;

VN_GEOMETRY_NS_END
