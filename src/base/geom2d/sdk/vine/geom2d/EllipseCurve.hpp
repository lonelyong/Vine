#pragma once

#include "geom2d_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Vector2.hpp>

#include "ConicCurve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A planar ellipse: a centre, a major-axis direction and two semi-axes.
 *
 * @details
 * - `pointAt(angle)` walks from the major axis towards the minor axis; the minor axis is `x_direction` rotated by +90
 *   degrees, so the frame stores one vector and derives the other instead of keeping two in sync by hand.
 * - The parameter is the **eccentric angle**, not the geometric angle seen from the centre: `pointAt(u)` is the point
 *   the circle of radius `major_radius` reaches at `u`, pulled in by `minor_radius / major_radius` towards the minor
 *   axis. That is what CAD kernels mean by an ellipse parameter.
 * - There is no `circumference()`: an ellipse's perimeter is a complete elliptic integral, that is an algorithm, not
 *   an attribute. `CircleCurve` can answer it because its integral is closed-form.
 * - The frame (`center`, `x_direction`, `yAxis()`) and the `CN` answer are inherited from `ConicCurve`; a circle does
 *   not share that layer because its start direction is fixed by the plane.
 *
 * @warning
 * - The frame is normalised at construction: assigning `center` or `x_direction` afterwards leaves a frame that no
 *   longer agrees with the parameterisation, so build a new ellipse instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class EllipseCurve : public ConicCurve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

    /// The frame is inherited from `ConicCurve`; these keep the member bodies readable.
    using ConicCurve<T>::center;
    using ConicCurve<T>::x_direction;
    using ConicCurve<T>::yAxis;

  public:
    /**
     * @brief Constructs the ellipse centred at @p center_point and starting from @p major_direction.
     *
     * @param center_point    Centre of the ellipse.
     * @param major_direction Direction of the major axis; normalised before storage, and a zero vector leaves the
     *                        curve without an axis.
     * @param major_length    Semi-axis along the major direction; must be positive for `isValid()`.
     * @param minor_length    Semi-axis along the derived minor axis; must be positive for `isValid()`.
     */
    constexpr EllipseCurve(const Point& center_point, const Vector& major_direction, T major_length, T minor_length)
      : ConicCurve<T>(center_point, major_direction)
      , major_radius(major_length)
      , minor_radius(minor_length)
    {}

  public:
    /**
     * @brief Returns the point at eccentric angle @p angle; the same value as `evaluate()`.
     *
     * @details
     * - `P(u) = centre + major_radius * cos(u) * X + minor_radius * sin(u) * Y`, where `X` is `x_direction` and `Y`
     *   is the derived minor axis.
     *
     * @param angle Eccentric angle in radians, measured from the major axis towards the minor axis.
     * @return The point on the ellipse; the centre when the ellipse was built without a usable direction.
     */
    [[nodiscard]] Point pointAt(T angle) const
    {
        const Vector y_axis = yAxis();
        return center + x_direction * (major_radius * std::cos(angle)) + y_axis * (minor_radius * std::sin(angle));
    }

    /**
     * @brief Returns the point at eccentric angle @p u; the same value as `pointAt()`.
     *
     * @param u Eccentric angle in radians.
     * @return The point on the ellipse.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns the velocity vector at eccentric angle @p u.
     *
     * @details
     * - `dP/du = -major_radius * sin(u) * X + minor_radius * cos(u) * Y`; its length is the speed, which is
     *   `minor_radius` at the major axis and `major_radius` at the minor axis, and nothing in between is constant.
     *
     * @param u Eccentric angle in radians.
     * @return The velocity vector at @p u.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        const Vector y_axis = yAxis();
        return x_direction * (-major_radius * std::sin(u)) + y_axis * (minor_radius * std::cos(u));
    }

    /**
     * @brief Returns the acceleration vector at eccentric angle @p u.
     *
     * @details
     * - Both terms of `pointAt()` differentiate twice into their own negation, so the acceleration is exactly
     *   `centre - P(u)` - the same shape as a circle's, with different lengths along the two axes.
     *
     * @param u Eccentric angle in radians.
     * @return The acceleration vector at @p u.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return center - pointAt(u);
    }

    /**
     * @brief Returns the derivative of the acceleration: `a * sin(u) * X - b * cos(u) * Y`.
     *
     * @details
     * - It exists as a member, not on `Curve<T>`, because the offset curve asks its basis for it (2.13).
     *
     * @param u Eccentric angle in radians.
     * @return The jerk vector at @p u.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        const Vector y_axis = yAxis();
        return x_direction * (major_radius * std::sin(u)) + y_axis * (-minor_radius * std::cos(u));
    }

    /**
     * @brief Returns the start of the natural range.
     *
     * @return 0 radians, at the major axis.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the natural range.
     *
     * @return One full turn, `2*pi` radians.
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
     * @brief Returns true: the ellipse repeats with its period.
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
     * @brief Returns whether the two semi-axes and the major direction are usable.
     *
     * @details
     * - A non-positive semi-axis or a zero major direction names no ellipse. The queries then collapse onto whatever
     *   survives - a segment along the remaining axis, or the centre when no direction is stored - and never produce a
     *   NaN, so a caller checks here first.
     *
     * @return True when both semi-axes and the stored direction are longer than the floating-point epsilon.
     */
    [[nodiscard]] bool isValid() const override
    {
        return major_radius > vn::math::EPS<T>() && minor_radius > vn::math::EPS<T>()
            && x_direction.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the eccentric angle of the same point when the ellipse is walked the other way.
     *
     * @details
     * - The range is periodic, so the reflection is `period - u`; it is an involution and it swaps the two ends of the
     *   range.
     * - The point it names is the one on the reversed ellipse, which is this ellipse reflected about the major axis -
     *   it is not `evaluate(u)` itself.
     *
     * @param u Eccentric angle in radians.
     * @return `2*pi - u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return period() - u;
    }

    /**
     * @brief Returns the axis-aligned box around the ellipse.
     *
     * @details
     * - The half width along a world axis is `sqrt((a * X_i)^2 + (b * Y_i)^2)`, which is the exact support function of
     *   the ellipse along that axis, not the looser box the major radius alone gives.
     * - The semi-axes enter by magnitude, so the box is the point set `evaluate()` traces; with no stored direction
     *   every half width is zero and the box degenerates to `center`.
     *
     * @return Bounds of the ellipse.
     */
    [[nodiscard]] vn::math::Rect2<T> bounds() const
    {
        const Vector y_axis = yAxis();
        const T      major  = std::abs(major_radius);
        const T      minor  = std::abs(minor_radius);
        const T      half_x = std::sqrt(major * x_direction.x * major * x_direction.x
                                 + minor * y_axis.x * minor * y_axis.x);
        const T      half_y = std::sqrt(major * x_direction.y * major * x_direction.y
                                 + minor * y_axis.y * minor * y_axis.y);
        return vn::math::Rect2<T>(Point(center.x - half_x, center.y - half_y),
                                  Point(center.x + half_x, center.y + half_y));
    }

  public:
    /// Semi-axis along `x_direction`.
    T major_radius{ T(1) };

    /// Semi-axis along the derived minor axis.
    T minor_radius{ T(1) };
};

using EllipseCurvef = EllipseCurve<float>;
using EllipseCurved = EllipseCurve<double>;

VN_GEOM2D_NS_END
