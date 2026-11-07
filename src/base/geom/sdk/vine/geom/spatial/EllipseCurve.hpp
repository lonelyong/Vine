#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "ConicCurve.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief An ellipse: a plane, an origin, a major-axis direction and two semi-axes.
 *
 * @details
 * - `pointAt(angle)` walks from the major axis towards the minor axis; the minor axis is `normal x x_direction`, so
 *   the frame stores two vectors and the third is derived instead of kept in sync by hand.
 * - The parameter is the **eccentric angle**, not the geometric angle seen from the centre: `pointAt(u)` is the point
 *   the circle of radius `major_radius` reaches at `u`, pulled in by `minor_radius / major_radius` towards the
 *   minor axis. That is what CAD kernels mean by an ellipse parameter.
 * - A circle is not this type: an equal-axes ellipse can be built, but `CircleCurve` stores one radius instead of two
 *   and keeps its own cheaper contract.
 * - There is no `circumference()`: an ellipse's perimeter is a complete elliptic integral, that is an algorithm, not
 *   an attribute. `CircleCurve` can answer it because its integral is closed-form.
 *
 * @warning
 * - The frame (`center`, `normal`, `x_direction`) is inherited from `ConicCurve` and is normalised once at
 *   construction: assigning those fields afterwards leaves three vectors describing frames that no longer agree, so
 *   build a new ellipse instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class EllipseCurve : public ConicCurve<T> {
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
    /// Constructs the unit circle in the XY plane, which is the degenerate ellipse used as a default.
    constexpr EllipseCurve() = default;

    /**
     * @brief Constructs an ellipse from its frame and two semi-axes.
     *
     * @param center_point    Centre of the ellipse.
     * @param plane_normal    Normal of the plane the ellipse lies in; normalized before storage, and a zero vector
     *                        leaves the curve in an undefined plane.
     * @param major_direction Direction of the major axis; projected into the plane and normalized before storage.
     * @param major_length    Semi-axis along the major direction, which must be positive for `isValid()`.
     * @param minor_length    Semi-axis along the derived minor axis, which must be positive for `isValid()`.
     */
    EllipseCurve(const Point& center_point, const Vector& plane_normal, const Vector& major_direction,
                 T major_length, T minor_length)
      : ConicCurve<T>(center_point, plane_normal, major_direction)
      , major_radius(major_length)
      , minor_radius(minor_length)
    {}

  public:
    /**
     * @brief Returns the point at eccentric angle @p angle.
     *
     * @details
     * - `P(u) = centre + major_radius * cos(u) * X + minor_radius * sin(u) * Y`, where `X` is `x_direction` and `Y`
     *   is the derived minor axis.
     *
     * @param angle Eccentric angle in radians, measured from the major axis towards the minor axis.
     * @return The point on the ellipse; with a degenerate frame the parameterisation collapses onto `x_direction`
     *         (see `ConicCurve`).
     */
    [[nodiscard]] Point pointAt(T angle) const
    {
        const Vector y_axis = yAxis();
        return center + x_direction * (major_radius * std::cos(angle)) + y_axis * (minor_radius * std::sin(angle));
    }

    /**
     * @brief Returns the point at parameter @p u; the same value as `pointAt()`.
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
     * @brief Returns the third derivative at eccentric angle @p u.
     *
     * @details
     * - Both terms of `pointAt()` differentiate into their own negation twice, so the third derivative is the negated
     *   velocity, exactly as for a circle: `d3P/du3 = -dP/du`.
     *
     * @param u Eccentric angle in radians.
     * @return The jerk vector at @p u, equal to `-derivative(u)`.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        return -derivative(u);
    }

    /**
     * @brief Returns 3: this type answers the third derivative in closed form.
     *
     * @return 3.
     */
    [[nodiscard]] int maxDerivativeOrder() const override
    {
        return 3;
    }

    /**
     * @brief Returns the derivative of the requested order, including the third.
     *
     * @param u Parameter in this curve's own units.
     * @param order Derivative order, at least 1.
     * @return The derivative, or `std::nullopt` outside `[1, 3]`.
     */
    [[nodiscard]] std::optional<Vector> derivativeAt(T u, int order) const override
    {
        if (order == 3) {
            return thirdDerivative(u);
        }

        return Curve<T>::derivativeAt(u, order);
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
     * @brief Returns true: the ellipse repeats with its period.
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
     * @brief Returns the eccentric angle of the same point when the ellipse is walked the other way.
     *
     * @param u Eccentric angle in radians.
     * @return `period - u`, which mirrors within `[0, period)`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return period() - u;
    }

    /**
     * @brief Returns whether the two semi-axes and the normal are usable.
     *
     * @return True when both semi-axes are positive and the normal is non-zero.
     */
    [[nodiscard]] bool isValid() const override
    {
        return major_radius > vn::math::EPS<T>() && minor_radius > vn::math::EPS<T>()
            && normal.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the axis-aligned bounds of the ellipse.
     *
     * @details
     * - The half width along a world axis is `sqrt((major_radius * X_i)^2 + (minor_radius * Y_i)^2)`, which is the
     *   exact support function of the ellipse along that axis, not the looser box the major radius alone gives.
     *
     * @return Bounds spanning the ellipse; the segment along `x_direction` when the frame is degenerate.
     */
    [[nodiscard]] Aabb<T> bounds() const
    {
        const Vector y_axis = yAxis();
        const T      half_x = std::sqrt(major_radius * x_direction.x * major_radius * x_direction.x
                                 + minor_radius * y_axis.x * minor_radius * y_axis.x);
        const T      half_y = std::sqrt(major_radius * x_direction.y * major_radius * x_direction.y
                                 + minor_radius * y_axis.y * minor_radius * y_axis.y);
        const T      half_z = std::sqrt(major_radius * x_direction.z * major_radius * x_direction.z
                                 + minor_radius * y_axis.z * minor_radius * y_axis.z);
        return Aabb<T>(Point(center.x - half_x, center.y - half_y, center.z - half_z),
                       Point(center.x + half_x, center.y + half_y, center.z + half_z));
    }

  public:
    /// Semi-axis along `x_direction`.
    T major_radius{ T(1) };

    /// Semi-axis along the derived minor axis.
    T minor_radius{ T(1) };
};

using EllipseCurvef = EllipseCurve<float>;
using EllipseCurved = EllipseCurve<double>;

VN_GEOM_SPATIAL_NS_END
