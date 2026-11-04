#pragma once

#include "geometry_global.hpp"

#include <algorithm>
#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "ConicCurve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A circle in space: centre, unit normal and radius, parameterised by an angle in radians.
 *
 * @details
 * - `pointAt(angle)` walks the circle from the local X axis towards the local Y axis, both derived from `normal`;
 *   the same `normal` therefore always yields the same start point, and no separate frame is stored.
 * - This is a conic (`ConicCurve`): the parameter is an angle, unlike `LineCurve` (a multiple of its direction) and
 *   `Ray` (a distance), which is why each type documents its own parameter.
 * - The type is a value: centre, normal, radius, no identity, no allocation.
 *
 * @warning
 * - `normal` is a plain field: assigning a vector that is not unit length changes nothing about the circle but
 *   makes the parameterisation depend on that length, so build the circle from a normal instead of writing the
 *   field.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class CircleCurve : public ConicCurve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

    /// The frame is inherited from `ConicCurve`; these keep the member bodies readable.
    using ConicCurve<T>::center;
    using ConicCurve<T>::normal;
    using ConicCurve<T>::x_direction;
    using ConicCurve<T>::yAxis;

  public:
    /// Constructs a unit circle at the origin in the XY plane.
    constexpr CircleCurve() = default;

    /**
     * @brief Constructs a circle from its centre, plane normal and radius.
     *
     * @param center_point Centre of the circle.
     * @param plane_normal Normal of the plane the circle lies in; normalized before storage, and a zero vector
     *                     leaves the circle in an undefined plane.
     * @param circle_radius Radius, which must be positive for `isValid()`.
     */
    CircleCurve(const Point& center_point, const Vector& plane_normal, T circle_radius)
      : ConicCurve<T>(center_point, plane_normal)
      , radius(circle_radius)
    {}

  public:
    /**
     * @brief Returns the point at angle @p angle.
     *
     * @param angle Angle in radians, measured from the local X axis towards the local Y axis.
     * @return The point on the circle; with a degenerate normal the parameterisation collapses onto `x_direction`
     *         (see `ConicCurve`).
     */
    [[nodiscard]] Point pointAt(T angle) const
    {
        return center + (x_direction * std::cos(angle) + yAxis() * std::sin(angle)) * radius;
    }

    /**
     * @brief Returns the point at parameter @p u; the same value as `pointAt()`.
     *
     * @param u Angle in radians.
     * @return The point on the circle.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns the start of the natural range.
     *
     * @return 0 radians.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the natural range.
     *
     * @return One full turn, `2 * pi` radians.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return T(2) * static_cast<T>(vn::math::PI);
    }

    /**
     * @brief Returns true: the ends of the range meet.
     *
     * @return Always true.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return true;
    }

    /**
     * @brief Returns true: the circle repeats with its period.
     *
     * @return Always true.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return true;
    }

    /**
     * @brief Returns the period in radians.
     *
     * @return `2 * pi`.
     */
    [[nodiscard]] T period() const override
    {
        return lastParameter() - firstParameter();
    }

    /**
     * @brief Returns the first derivative at angle @p u: the velocity vector around the circle.
     *
     * @details
     * - `dP/du = radius * (-X sin u + Y cos u)`, so the speed is `radius` and the vector is a unit tangent only for
     *   the unit circle. The radius entering the result is why the derivative cannot be documented as "unit tangent".
     *
     * @param u Angle in radians.
     * @return The velocity vector, of length `radius`, pointing counter-clockwise about `normal`.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        return (yAxis() * std::cos(u) - x_direction * std::sin(u)) * radius;
    }

    /**
     * @brief Returns the second derivative at angle @p u: the vector from the point to the centre.
     *
     * @details
     * - `d2P/du2 = -radius * (X cos u + Y sin u)`, which is exactly `centre - P(u)`; its length is the radius, so the
     *   curvature of the circle is `1 / radius`.
     *
     * @param u Angle in radians.
     * @return The acceleration vector, of length `radius`, pointing towards the centre.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return center - pointAt(u);
    }

    /**
     * @brief Returns the angle of the same point when the circle is walked clockwise.
     *
     * @details
     * - `period - u` is `-u` shifted into `[0, period)`; the reversed circle is the same circle with `-normal`, whose
     *   derived in-plane axis flips and therefore evaluates the same point at this angle.
     *
     * @param u Angle in radians.
     * @return The mirrored angle in `[0, period)` for @p u in that range.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return period() - u;
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
    [[nodiscard]] bool isValid() const override
    {
        return radius > vn::math::EPS<T>() && normal.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the axis-aligned bounds of the circle.
     *
     * @details
     * - The half width along a world axis is `radius * sqrt(X_i^2 + Y_i^2)`, the exact support function of the circle
     *   along that axis. With an orthonormal frame it equals `radius * sqrt(1 - n_i^2)`; with a degenerate normal it
     *   still answers the segment the parameterisation actually traces instead of a full cube.
     *
     * @return Bounds spanning the circle; the segment along `x_direction` when the normal is degenerate.
     */
    [[nodiscard]] Aabb<T> bounds() const
    {
        const Vector y_axis = yAxis();
        const T      half_x = radius * std::sqrt(x_direction.x * x_direction.x + y_axis.x * y_axis.x);
        const T      half_y = radius * std::sqrt(x_direction.y * x_direction.y + y_axis.y * y_axis.y);
        const T      half_z = radius * std::sqrt(x_direction.z * x_direction.z + y_axis.z * y_axis.z);
        return Aabb<T>(Point(center.x - half_x, center.y - half_y, center.z - half_z),
                       Point(center.x + half_x, center.y + half_y, center.z + half_z));
    }

  public:
    /// Radius of the circle.
    T radius{ T(1) };
};

using CircleCurvef = CircleCurve<float>;
using CircleCurved = CircleCurve<double>;

VN_GEOMETRY_NS_END
