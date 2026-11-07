#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <limits>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include "ConicCurve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief One branch of a planar hyperbola: an origin, a transverse-axis direction and two semi-axes.
 *
 * @details
 * - `pointAt(u)` walks the `major_radius > 0` branch: `P(u) = centre + major_radius * cosh(u) * X +
 *   minor_radius * sinh(u) * Y`. The other branch is the same curve with `-x_direction`, not a negative `u`.
 * - The parameter is a hyperbolic angle, so it is not an arc measure and the speed is never constant; `u = 0` is the
 *   vertex closest to the centre.
 * - The curve is unbounded and not periodic: `firstParameter()` / `lastParameter()` are `-infinity` / `+infinity`.
 * - The frame (`center`, `x_direction`, `yAxis()`) and the `CN` answer are inherited from `ConicCurve`.
 *
 * @warning
 * - The frame is normalised at construction: assigning `center` or `x_direction` afterwards leaves a frame that no
 *   longer agrees with the parameterisation, so build a new hyperbola instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class HyperbolaCurve : public ConicCurve<T> {
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
     * @brief Constructs a hyperbola branch from its centre, its transverse axis and two semi-axes.
     *
     * @param center_point         Centre of the hyperbola, the point the two branches approach.
     * @param transverse_direction Direction of the transverse (real) axis; normalised before storage, and the branch
     *                             that is built is the one along `+x_direction`.
     * @param major_length         Transverse semi-axis, which must be positive for `isValid()`.
     * @param minor_length         Conjugate semi-axis, which must be positive for `isValid()`.
     */
    constexpr HyperbolaCurve(const Point& center_point, const Vector& transverse_direction, T major_length,
                             T minor_length)
      : ConicCurve<T>(center_point, transverse_direction)
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
     * @return The point on the branch; `center` when the frame has no usable direction.
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
     *   acceleration is exactly `P(u) - centre`: it points away from the centre along the branch.
     *
     * @param u Hyperbolic angle.
     * @return The acceleration vector at @p u.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return pointAt(u) - center;
    }

    /**
     * @brief Returns the jerk vector, which is `derivative(u)` again: a hyperbolic angle differentiates into itself.
     *
     * @details
     * - It exists as a member, not on `Curve<T>`, because the offset curve asks its basis for it (2.13).
     *
     * @param u Hyperbolic angle.
     * @return `major_radius * sinh(u) * X + minor_radius * cosh(u) * Y`.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        return derivative(u);
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
     * @return false always.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a hyperbola does not repeat.
     *
     * @return false always.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a non-periodic curve has no period.
     *
     * @return 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns whether the two semi-axes and the transverse direction are usable.
     *
     * @return True when both semi-axes and the stored direction are longer than the floating-point epsilon.
     */
    [[nodiscard]] bool isValid() const override
    {
        return major_radius > vn::math::EPS<T>() && minor_radius > vn::math::EPS<T>()
            && x_direction.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns `-u`: the other end of the same branch.
     *
     * @details
     * - The branch is unbounded, so the reflection is the negation; it is an involution and swaps the two infinite
     *   ends. The other branch is a different curve, reached by negating `x_direction`.
     *
     * @param u Hyperbolic angle.
     * @return `-u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return -u;
    }

  public:
    /// Transverse semi-axis.
    T major_radius{ T(1) };

    /// Conjugate semi-axis.
    T minor_radius{ T(1) };
};

using HyperbolaCurvef = HyperbolaCurve<float>;
using HyperbolaCurved = HyperbolaCurve<double>;

VN_GEOM_PLANAR_NS_END
