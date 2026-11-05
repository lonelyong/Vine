#pragma once

#include "geom2d_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A planar circle: `P(u) = center + radius * (cos u, sin u)`.
 *
 * @details
 * - The parameter is the angle in radians measured from the positive X axis, so the domain is `[0, 2*pi]`, the curve
 *   is closed and periodic with period `2*pi`, and the parameter is not a distance.
 * - The direction of travel is counter-clockwise, which is what fixes the sign of `derivative()` and of the
 *   `reversedParameter()` map.
 * - Every derivative exists inside the domain, so the curve answers `Continuity::CN`.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class CircleCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

  public:
    /**
     * @brief Constructs the circle centred at @p center_point with radius @p circle_radius.
     *
     * @param center_point The centre in the plane.
     * @param circle_radius Radius; must be positive for `isValid()`.
     */
    constexpr CircleCurve(const Point& center_point, T circle_radius)
      : center(center_point)
      , radius(circle_radius)
    {
    }

  public:
    /**
     * @brief Returns the point at angle @p angle; the same value as `evaluate()`.
     *
     * @details
     * - It names the parameter's meaning at the call site (an angle, not a distance) and is the concrete type's own
     *   entry point, as `LineCurve::pointAt()` is for a line.
     * - It is not `constexpr`: `std::cos` and `std::sin` are not constant expressions in C++20, so unlike the line
     *   there is no constant-evaluation form of this query.
     *
     * @param angle Angle in radians, measured from the positive X axis; values outside `[0, 2*pi]` wrap by periodicity.
     * @return The point on the circle.
     */
    [[nodiscard]] Point pointAt(T angle) const
    {
        return center + Vector{ std::cos(angle), std::sin(angle) } * radius;
    }

    /**
     * @brief Returns the point at angle @p u; the same value as `pointAt()`.
     *
     * @param u Angle in radians; values outside `[0, 2*pi]` wrap by periodicity.
     * @return The point on the circle.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns the velocity vector at angle @p u.
     *
     * @details
     * - Its length is `radius`, not 1: the parameterisation is by angle, so the speed is the radius.
     *
     * @param u Angle in radians.
     * @return `radius * (-sin u, cos u)`.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        return Vector{ -std::sin(u), std::cos(u) } * radius;
    }

    /**
     * @brief Returns the acceleration vector at angle @p u.
     *
     * @details
     * - It points from the point on the circle towards the centre and its length is `radius`.
     *
     * @param u Angle in radians.
     * @return `radius * (-cos u, -sin u)`.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return Vector{ -std::cos(u), -std::sin(u) } * radius;
    }

    /**
     * @brief Returns zero: the natural range starts at the positive X axis.
     *
     * @return 0.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `2*pi`: the natural range is one full turn.
     *
     * @return `2*pi`.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return T(2) * static_cast<T>(vn::math::PI);
    }

    /**
     * @brief Returns true: the ends of the range meet.
     *
     * @return true always.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return true;
    }

    /**
     * @brief Returns true: the circle repeats with its period.
     *
     * @return true always.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return true;
    }

    /**
     * @brief Returns the period in radians.
     *
     * @return `2*pi`.
     */
    [[nodiscard]] T period() const override
    {
        return lastParameter() - firstParameter();
    }

    /**
     * @brief Returns `CN`: every derivative exists inside the domain.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns whether the radius describes a circle.
     *
     * @details
     * - A non-positive radius names no circle. The queries still answer finite values - the sign only mirrors the same
     *   circle, and a radius of zero collapses every parameter onto the centre - never a NaN, so a caller checks here
     *   first.
     *
     * @return True when `radius` is greater than the floating-point epsilon.
     */
    [[nodiscard]] bool isValid() const override
    {
        return radius > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the angle of the same point when the circle is walked the other way.
     *
     * @details
     * - The range is periodic, so the reflection is `period - u`.
     *
     * @param u Angle in radians.
     * @return `2*pi - u`.
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
     * @brief Returns the axis-aligned box around the circle.
     *
     * @details
     * - The box is the point set `evaluate()` traces: `center` grown by `|radius|`, so a negative radius answers the
     *   same box as its positive value.
     *
     * @return Bounds of the circle; the degenerate box at `center` when the radius is zero.
     */
    [[nodiscard]] vn::math::Rect2<T> bounds() const
    {
        const T extent = std::abs(radius);
        return vn::math::Rect2<T>(Point(center.x - extent, center.y - extent),
                                  Point(center.x + extent, center.y + extent));
    }

  public:
    Point center;
    T     radius{ T(1) };
};

using CircleCurvef = CircleCurve<float>;
using CircleCurved = CircleCurve<double>;

VN_GEOM2D_NS_END
