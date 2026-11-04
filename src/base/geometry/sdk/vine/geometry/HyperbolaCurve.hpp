#pragma once

#include "geometry_global.hpp"

#include <cmath>
#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "ConicCurve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief One branch of a hyperbola: a plane, an origin, a transverse-axis direction and two semi-axes.
 *
 * @details
 * - `pointAt(u)` walks the `major_radius > 0` branch: `P(u) = centre + major_radius * cosh(u) * X +
 *   minor_radius * sinh(u) * Y`. The other branch is the same curve with `-x_direction`, not a negative `u`.
 * - The parameter is a hyperbolic angle, so it is not an arc measure and the speed is never constant; `u = 0` is the
 *   vertex closest to the centre.
 * - The curve is unbounded and not periodic: `firstParameter()` / `lastParameter()` are `-infinity` / `+infinity`.
 * - The minor axis is `normal x x_direction`, derived rather than stored.
 *
 * @warning
 * - The frame (`center`, `normal`, `x_direction`) is inherited from `ConicCurve` and is normalised once at
 *   construction: assigning those fields afterwards leaves three vectors describing frames that no longer agree, so
 *   build a new hyperbola instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class HyperbolaCurve : public ConicCurve<T> {
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
    /// Constructs the unit hyperbola in the XY plane.
    constexpr HyperbolaCurve() = default;

    /**
     * @brief Constructs a hyperbola branch from its frame and two semi-axes.
     *
     * @param center_point      Centre of the hyperbola, the point the two branches approach.
     * @param plane_normal      Normal of the plane the hyperbola lies in; normalized before storage, and a zero
     *                          vector leaves the curve in an undefined plane.
     * @param transverse_direction Direction of the transverse (real) axis; projected into the plane and normalized
     *                          before storage, and the branch that is built is the one along `+X`.
     * @param major_length      Transverse semi-axis, which must be positive for `isValid()`.
     * @param minor_length      Conjugate semi-axis, which must be positive for `isValid()`.
     */
    HyperbolaCurve(const Point& center_point, const Vector& plane_normal, const Vector& transverse_direction,
                   T major_length, T minor_length)
      : ConicCurve<T>(center_point, plane_normal, transverse_direction)
      , major_radius(major_length)
      , minor_radius(minor_length)
    {}

  public:
    /**
     * @brief Returns the point at hyperbolic parameter @p u.
     *
     * @details
     * - `P(u) = centre + major_radius * cosh(u) * X + minor_radius * sinh(u) * Y`.
     *
     * @param u Hyperbolic angle; `0` is the vertex closest to the centre.
     * @return The point on the branch; with a degenerate frame the branch collapses onto the two rays along
     *         `x_direction` (see `ConicCurve`).
     */
    [[nodiscard]] Point pointAt(T u) const
    {
        const Vector y_axis = yAxis();
        return center + x_direction * (major_radius * std::cosh(u)) + y_axis * (minor_radius * std::sinh(u));
    }

    /**
     * @brief Returns the point at parameter @p u; the same value as `pointAt()`.
     *
     * @param u Hyperbolic angle.
     * @return The point on the branch.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns the velocity vector at parameter @p u.
     *
     * @details
     * - `dP/du = major_radius * sinh(u) * X + minor_radius * cosh(u) * Y`: the speed is smallest at the vertex and
     *   grows without bound towards both ends, which is why a hyperbola is never arc-length parameterised this way.
     *
     * @param u Hyperbolic angle.
     * @return The velocity vector at @p u.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        const Vector y_axis = yAxis();
        return x_direction * (major_radius * std::sinh(u)) + y_axis * (minor_radius * std::cosh(u));
    }

    /**
     * @brief Returns the acceleration vector at parameter @p u.
     *
     * @details
     * - A hyperbolic angle differentiates into itself (unlike the circular case, without a sign change), so the
     *   acceleration is exactly `P(u) - centre`: it points away from the centre on both branches.
     *
     * @param u Hyperbolic angle.
     * @return The acceleration vector at @p u.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return pointAt(u) - center;
    }

    /**
     * @brief Returns the lower end of the range: a hyperbola is unbounded.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the range: a hyperbola is unbounded.
     *
     * @return Positive infinity.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns false: the two ends never meet.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a hyperbola does not repeat.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a hyperbola is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `-u`: the same point reached by walking the branch from the other end.
     *
     * @param u Hyperbolic angle.
     * @return `-u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return -u;
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

  public:
    /// Transverse semi-axis.
    T major_radius{ T(1) };

    /// Conjugate semi-axis.
    T minor_radius{ T(1) };
};

using HyperbolaCurvef = HyperbolaCurve<float>;
using HyperbolaCurved = HyperbolaCurve<double>;

VN_GEOMETRY_NS_END
