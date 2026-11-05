#pragma once

#include "geom2d_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

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
 * - The class does not derive from a planar conic layer: a circle's parameterisation is fixed by the plane and has no
 *   axis of its own, so the two conics share nothing but the `CN` answer.
 *
 * @warning
 * - `x_direction` is normalised at construction: assigning it afterwards leaves a frame that no longer agrees with the
 *   parameterisation, so build a new ellipse instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class EllipseCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

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
      : center(center_point)
      , x_direction(major_direction.length() > vn::math::EPS<T>() ? major_direction.normalized() : Vector::zero())
      , major_radius(major_length)
      , minor_radius(minor_length)
    {}

  public:
    /**
     * @brief Returns the minor axis: `x_direction` rotated by +90 degrees.
     *
     * @details
     * - Derived rather than stored: a second vector would be a second thing to keep orthogonal to the first.
     * - It is the zero vector when the ellipse was built without a usable direction, which is what makes `pointAt()`
     *   collapse onto the centre instead of producing a NaN.
     *
     * @return The unit minor-axis direction when `x_direction` is unit, otherwise the zero vector.
     */
    [[nodiscard]] constexpr Vector yAxis() const
    {
        return Vector{ -x_direction.y, x_direction.x };
    }

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
     * @brief Returns `CN`: every derivative exists inside the domain.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns whether the two semi-axes and the major direction are usable.
     *
     * @details
     * - A non-positive semi-axis or a zero major direction names no ellipse; the numeric queries then answer the
     *   centre rather than producing a NaN, so a caller checks here first.
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

  public:
    /// Centre of the ellipse.
    Point center;

    /// Unit direction the parameterisation starts from; zero when the ellipse was built without a usable direction.
    Vector x_direction{ T(1), T(0) };

    /// Semi-axis along `x_direction`.
    T major_radius{ T(1) };

    /// Semi-axis along the derived minor axis.
    T minor_radius{ T(1) };
};

using EllipseCurvef = EllipseCurve<float>;
using EllipseCurved = EllipseCurve<double>;

VN_GEOM2D_NS_END
