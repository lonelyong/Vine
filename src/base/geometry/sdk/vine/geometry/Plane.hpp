#pragma once

#include "geometry_global.hpp"

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

using Planef = Plane<float>;
using Planed = Plane<double>;

VN_GEOMETRY_NS_END
