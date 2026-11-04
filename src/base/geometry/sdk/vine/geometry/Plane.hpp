#pragma once

#include "geometry_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include "Line.hpp"
#include "Ray.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A 3D plane represented by an origin point and a normal vector.
 * @tparam T Only accepts float and double.
 */
template <typename T>
class Plane
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
     * @brief Intersect this plane with a line.
     * @param line Input line.
     * @param intersection_pt Output intersection point.
     * @param eps Numerical tolerance.
     * @return True when intersection exists.
     */
    [[nodiscard]]
    bool intersectWith(const Line<T>& line, math::Point3<T>& intersection_pt, T eps = math::EPS<T>()) const;
    /**
     * @brief Intersect this plane with another plane.
     * @param plane Other plane.
     * @param intersection_line Output intersection line.
     * @param eps Numerical tolerance.
     * @return True when intersection line exists.
     */
    [[nodiscard]]
    bool intersectWith(const Plane<T>& plane, Line<T>& intersection_line, T eps = math::EPS<T>()) const;

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

  public:
    math::Point3<T>  origin;
    math::Vector3<T> normal;
};

template <typename T>
bool Plane<T>::intersectWith(const Line<T>& line, math::Point3<T>& intersection_pt, T eps) const
{
    // Plane–Line intersection.
    //
    //   Plane:  n·(P − o) = 0
    //   Line:   P = P₀ + t·d
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
