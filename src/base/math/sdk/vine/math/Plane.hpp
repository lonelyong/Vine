#pragma once

#include "Line.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief A 3D plane represented by an origin point and a normal vector.
 * @tparam T Only accepts float and double.
 */
template <typename T>
class Plane {
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct a plane from origin and normal.
     * @param origin A point on the plane.
     * @param normal Plane normal vector.
     */
    constexpr Plane(const Point3<T>& origin, const Vector3<T>& normal)
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
    bool intersectWith(const Line<T>& line, Point3<T>& intersection_pt, T eps = EPS<T>()) const;
    /**
     * @brief Intersect this plane with another plane.
     * @param plane Other plane.
     * @param intersection_line Output intersection line.
     * @param eps Numerical tolerance.
     * @return True when intersection line exists.
     */
    [[nodiscard]]
    bool intersectWith(const Plane<T>& plane, Line<T>& intersection_line, T eps = EPS<T>()) const;

  public:
    Point3<T>  origin;
    Vector3<T> normal;
};

using Planef = Plane<float>;
using Planed = Plane<double>;
VN_MATH_NS_END
