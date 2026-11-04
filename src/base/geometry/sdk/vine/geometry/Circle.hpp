#pragma once

#include "geometry_global.hpp"

#include <algorithm>
#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Curve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A circle in space: centre, unit normal and radius, parameterised by an angle in radians.
 *
 * @details
 * - `pointAt(angle)` walks the circle from the local X axis towards the local Y axis, both derived from `normal`;
 *   the same `normal` therefore always yields the same start point, and no separate frame is stored.
 * - This is a curve (`Curve::kDimension == 1`); the parameter is an angle, unlike `Ray` (a distance) and
 *   `LineSegment` (a fraction), which is why each type documents its own.
 * - The type is a value: centre, normal, radius, no identity, no allocation; `Curve` adds nothing to its size.
 *
 * @warning
 * - `normal` is a plain field: assigning a vector that is not unit length changes nothing about the circle but
 *   makes the parameterisation depend on that length, so build the circle from a normal instead of writing the
 *   field.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class Circle : public Curve {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

  public:
    /// Constructs a unit circle at the origin in the XY plane.
    constexpr Circle() = default;

    /**
     * @brief Constructs a circle from its centre, plane normal and radius.
     *
     * @param center_point Centre of the circle.
     * @param plane_normal Normal of the plane the circle lies in; normalized before storage, and a zero vector
     *                     leaves the circle in an undefined plane.
     * @param circle_radius Radius, which must be positive for `isValid()`.
     */
    Circle(const Point& center_point, const Vector& plane_normal, T circle_radius)
      : center(center_point)
      , normal(plane_normal.length() > vn::math::EPS<T>() ? plane_normal.normalized() : Vector(T(0), T(0), T(0)))
      , radius(circle_radius)
    {}

  public:
    /**
     * @brief Returns the point at angle @p angle.
     *
     * @param angle Angle in radians, measured from the local X axis towards the local Y axis.
     * @return The point on the circle; the centre when the normal is degenerate.
     */
    [[nodiscard]] Point pointAt(T angle) const
    {
        const Vector x_axis = xAxis();
        const Vector y_axis = normal.cross(x_axis);
        return center + (x_axis * std::cos(angle) + y_axis * std::sin(angle)) * radius;
    }

    /**
     * @brief Returns the normal of the plane the circle lies in.
     *
     * @return The stored unit normal, or a zero vector when the circle was built without one.
     */
    [[nodiscard]] constexpr const Vector& planeNormal() const
    {
        return normal;
    }

    /**
     * @brief Returns the circumference.
     *
     * @return `2 * pi * radius`.
     */
    [[nodiscard]] T circumference() const
    {
        return T(2) * static_cast<T>(vn::math::PI) * radius;
    }

    /**
     * @brief Returns whether the radius and the normal are usable.
     *
     * @return True when the radius is positive and the normal is non-zero.
     */
    [[nodiscard]] bool isValid() const
    {
        return radius > vn::math::EPS<T>() && normal.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the axis-aligned bounds of the circle.
     *
     * @details
     * - The bound along a world axis is the radius times the sine of the angle between the normal and that axis,
     *   which is exact for a circle rather than the looser cube the radius alone would give.
     *
     * @return Bounds spanning the circle; the centre when the normal is degenerate.
     */
    [[nodiscard]] Aabb<T> bounds() const
    {
        const T half_x = radius * std::sqrt(std::max(T(0), T(1) - normal.x * normal.x));
        const T half_y = radius * std::sqrt(std::max(T(0), T(1) - normal.y * normal.y));
        const T half_z = radius * std::sqrt(std::max(T(0), T(1) - normal.z * normal.z));
        return Aabb<T>(Point(center.x - half_x, center.y - half_y, center.z - half_z),
                       Point(center.x + half_x, center.y + half_y, center.z + half_z));
    }

  private:
    /// The in-plane axis the angle starts from: the world axis least aligned with the normal, projected.
    [[nodiscard]] Vector xAxis() const
    {
        if (normal.length() <= vn::math::EPS<T>()) {
            return Vector(T(1), T(0), T(0));
        }

        const Vector candidates[3] = { Vector(T(1), T(0), T(0)), Vector(T(0), T(1), T(0)), Vector(T(0), T(0), T(1)) };
        const Vector* least_aligned = &candidates[0];
        for (const auto& candidate : candidates) {
            if (std::abs(normal.dot(candidate)) < std::abs(normal.dot(*least_aligned))) {
                least_aligned = &candidate;
            }
        }

        const Vector in_plane = *least_aligned - normal * normal.dot(*least_aligned);
        return in_plane.length() > vn::math::EPS<T>() ? in_plane.normalized() : Vector(T(1), T(0), T(0));
    }

  public:
    /// Centre of the circle.
    Point center;

    /// Unit normal of the plane the circle lies in; zero when the circle was built without a usable normal.
    Vector normal{ T(0), T(0), T(1) };

    /// Radius of the circle.
    T radius{ T(1) };
};

using Circlef = Circle<float>;
using Circled = Circle<double>;

VN_GEOMETRY_NS_END
