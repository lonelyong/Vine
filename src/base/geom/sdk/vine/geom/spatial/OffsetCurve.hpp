#pragma once

#include <vine/geom/geom_global.hpp>

#include <memory>
#include <optional>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A curve held at a signed distance from a basis curve, in the plane the reference direction defines.
 *
 * @details
 * - The offset point is the basis point pushed along `normalize(reference_direction x tangent)`: the reference
 *   direction picks the plane, the basis' first derivative picks the side. That is why an offset curve needs three
 *   things - the basis, the distance and a reference direction - and why the distance alone is not enough.
 * - Every parameter-domain answer is the basis': the offset inherits the range, the period and the closure, because it
 *   is evaluated at the same parameter and is only displaced at right angles to the basis.
 * - `evaluate()`, `derivative()` and `secondDerivative()` are built from the basis' point, tangent, second and third
 *   derivatives, with the cross-product guard that turns a degenerate input into the basis itself instead of a NaN.
 * - Derivative order follows the basis: the offset answers **one less** than its basis does, clipped to two, because
 *   `P'''` would need the basis' fourth. Over any space curve type it reports 2; over a basis that answers only two
 *   (another offset) it reports 1 and answers `std::nullopt` for the second derivative.
 *
 * @warning
 * - A null @p basis_curve is a caller error that `isValid()` reports; a query asked anyway answers the degenerate
 *   value of its family (the origin point, a zero vector, `C0`, false) rather than dereferencing the handle, so the
 *   mistake stays a wrong answer instead of undefined behaviour.
 * - The offset is only well defined where `normalize(reference_direction x tangent)` exists. Nothing here checks
 *   that, and the basis' continuity does not describe it, so a curve whose tangent becomes parallel to the reference
 *   direction has no offset there.
 * - A basis that cannot answer a third derivative leaves this offset a position-and-velocity view: `isValid()`
 *   reports it, `maxDerivativeOrder()` drops to 1, and `secondDerivative()` answers the zero vector.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class OffsetCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;
    using BasisPtr   = std::shared_ptr<const Curve<T>>;

  public:
    /**
     * @brief Constructs an offset curve from a basis curve, a signed distance and a reference direction.
     *
     * @param basis                     The curve being offset; must not be null.
     * @param offset_distance            Signed distance from the basis, positive towards
     *                                  `normalize(reference_direction x tangent)`.
     * @param the_reference_direction    Direction that, crossed with the basis' tangent, gives the offset direction.
     *                                  Only its direction matters: any non-zero vector works, and the unit vector is
     *                                  formed where it is needed.
     */
    OffsetCurve(BasisPtr basis, T offset_distance, const Vector& the_reference_direction)
      : basis_curve(std::move(basis))
      , offset(offset_distance)
      , reference_direction(the_reference_direction)
    {}

  public:
    /**
     * @brief Returns the first parameter of the basis curve.
     *
     * @return The basis' `firstParameter()`.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return basis_curve != nullptr ? basis_curve->firstParameter() : T(0);
    }

    /**
     * @brief Returns the last parameter of the basis curve.
     *
     * @return The basis' `lastParameter()`, or 0 without a basis.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return basis_curve != nullptr ? basis_curve->lastParameter() : T(0);
    }

    /**
     * @brief Returns whether the offset closes, which it does exactly when the basis closes.
     *
     * @return The basis' `isClosed()`; false without a basis.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return basis_curve != nullptr && basis_curve->isClosed();
    }

    /**
     * @brief Returns whether the offset repeats, which it does exactly when the basis repeats.
     *
     * @return The basis' `isPeriodic()`; false without a basis.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return basis_curve != nullptr && basis_curve->isPeriodic();
    }

    /**
     * @brief Returns the basis period, or 0 when the offset itself does not repeat.
     *
     * @return The basis period when `isPeriodic()`, otherwise 0.
     */
    [[nodiscard]] T period() const override
    {
        return isPeriodic() ? basis_curve->period() : T(0);
    }

    /**
     * @brief Returns the basis continuity.
     *
     * @details
     * - The offset cannot be smoother than what it is pushed along: it is a function of the basis' first derivative,
     *   so a `C0` basis gives no offset direction at all. The value is forwarded unchanged; the caveat above is the
     *   caller's to check, not something this class can answer from the basis alone.
     *
     * @return The basis' `continuity()`; `C0` without a basis, since nothing is known to be smoother.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis_curve != nullptr ? basis_curve->continuity() : Continuity::C0;
    }

    /**
     * @brief Returns the basis' reversed parameter: offsetting does not reparameterise.
     *
     * @param u Parameter in the basis curve's units.
     * @return The basis' `reversedParameter(u)`; @p u unchanged without a basis, which is the one map that is an
     *         involution and needs no curve at all.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return basis_curve != nullptr ? basis_curve->reversedParameter(u) : u;
    }

    /**
     * @brief Returns whether the basis and the reference direction are usable.
     *
     * @details
     * - Three things can make an offset unusable: no basis, a zero reference direction (which selects no side), and a
     *   basis that cannot answer the third derivative this offset's second derivative needs. The last one is a
     *   capability, so it is reported here rather than papered over with a zero.
     *
     * @return True when the basis is present and valid, the reference direction is non-zero, and the basis reports
     *         `maxDerivativeOrder() >= 3`.
     */
    [[nodiscard]] bool isValid() const override
    {
        return basis_curve != nullptr && basis_curve->isValid()
            && reference_direction.length() > vn::math::EPS<T>() && basis_curve->maxDerivativeOrder() >= 3;
    }

    /**
     * @brief Returns the basis point pushed along `normalize(reference_direction x tangent)` by the offset distance.
     *
     * @details
     * - The displacement is perpendicular to the basis' tangent by construction, because it is the tangent crossed with
     *   the reference direction - which is why the reference direction is part of the data and the distance alone is
     *   not enough.
     * - Where the tangent is parallel to the reference direction there is no perpendicular, so the answer is the basis
     *   point itself: a degenerate input yields a degenerate offset rather than a NaN.
     *
     * @param u Parameter in the basis curve's units.
     * @return The point at distance `offset` from the basis, on the side the reference direction selects; the origin
     *         without a basis.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        if (basis_curve == nullptr) {
            return Point();
        }

        const Point  basis_point = basis_curve->evaluate(u);
        const Vector direction   = offsetDirection(u);
        return direction.isZero(vn::math::EPS<T>()) ? basis_point : basis_point + direction * offset;
    }

    /**
     * @brief Returns the velocity of the offset curve at @p u.
     *
     * @details
     * - With `N = reference_direction x p'` and `R = |N|`, the offset is `P = p + d * N / R`, hence
     *   `P' = p' + d * (N' * R - N * (N . N') / R) / R^2`; `N'` is the basis' second derivative crossed with the
     *   reference direction, so no higher derivative than the interface already has is needed.
     * - At a singular point the displacement does not vary, so the basis' own velocity is the answer.
     *
     * @param u Parameter in the basis curve's units.
     * @return The velocity vector of the offset curve at @p u; the zero vector without a basis.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        if (basis_curve == nullptr) {
            return Vector(T(0), T(0), T(0));
        }

        const Vector tangent = basis_curve->derivative(u);
        const Vector normal  = reference_direction.cross(tangent);
        const T      length  = normal.length();
        if (length <= vn::math::EPS<T>()) {
            return tangent;
        }

        const Vector normal_velocity = reference_direction.cross(basis_curve->secondDerivative(u));
        const T      radial_velocity = normal.dot(normal_velocity);
        return tangent
             + (normal_velocity * length - normal * (radial_velocity / length)) * (offset / (length * length));
    }

    /**
     * @brief Returns the offset's second derivative at @p u, from the basis' third derivative.
     *
     * @details
     * - With `N = reference_direction x p'` and `R = |N|`, the offset is `P = p + d * N / R`. Differentiating twice
     *   gives `P'' = p'' + d * U''` with `U = N / R`, `N' = e x p''` and `N'' = e x p'''`:
     *   `U'' = N''/R - 2 N' (N.N')/R^3 - N (|N'|^2 + N.N'')/R^3 + 3 N (N.N')^2/R^5`.
     * - That `N''` term is why the basis has to answer three orders, and why the answer is exact rather than a finite
     *   difference of the first derivative.
     * - At a singular point the displacement does not vary, so the basis' own acceleration is the answer.
     *
     * @param u Parameter in the basis curve's units.
     * @return The acceleration vector of the offset at @p u; the zero vector without a basis or when the basis cannot
     *         answer a third derivative.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        if (basis_curve == nullptr || basis_curve->maxDerivativeOrder() < 3) {
            return Vector(T(0), T(0), T(0));
        }

        const Vector acceleration = basis_curve->secondDerivative(u);
        const Vector tangent      = basis_curve->derivative(u);
        const Vector normal       = reference_direction.cross(tangent);
        const T      length       = normal.length();
        if (length <= vn::math::EPS<T>()) {
            return acceleration;
        }

        const Vector jerk             = basis_curve->derivativeAt(u, 3).value_or(Vector(T(0), T(0), T(0)));
        const Vector normal_velocity  = reference_direction.cross(acceleration);
        const Vector normal_accel     = reference_direction.cross(jerk);
        const Vector unit             = normal / length;
        const T      length_squared   = length * length;
        const T      radial_velocity  = normal.dot(normal_velocity);
        const T      radial_accel     = normal_velocity.dot(normal_velocity) + normal.dot(normal_accel);

        // With U = N / R and k = (N . N') / R^2, U' = N' / R - U k, so U'' = N''/R - 2 N' k/R + U (k^2 - k').
        const T k             = radial_velocity / length_squared;
        const T k_rate        = radial_accel / length_squared - T(2) * k * k;
        const Vector unit_second_rate = normal_accel / length - normal_velocity * (T(2) * k / length)
                                      + unit * (k * k - k_rate);
        return acceleration + unit_second_rate * offset;
    }

    /**
     * @brief Returns the highest derivative order the basis lets this offset answer, which is one less than the basis.
     *
     * @details
     * - Each order of the offset needs one more order of the basis: the first needs the basis' second, the second needs
     *   the basis' third. So the offset answers `basis.maxDerivativeOrder() - 1`, clipped to `[0, 2]` because the
     *   offset itself has no closed form above its second.
     * - A missing basis reports the contract default of two, matching the zero vectors it answers otherwise.
     *
     * @return `min(2, basis.maxDerivativeOrder() - 1)`; 2 without a basis.
     */
    [[nodiscard]] int maxDerivativeOrder() const override
    {
        if (basis_curve == nullptr) {
            return 2;
        }

        const int basis_order = basis_curve->maxDerivativeOrder();
        return basis_order <= 1 ? 0 : (basis_order >= 3 ? 2 : 1);
    }

    /**
     * @brief Returns the derivative of the requested order, empty above what the basis lets this offset answer.
     *
     * @param u Parameter in the basis curve's units.
     * @param order Derivative order, at least 1.
     * @return The derivative when `1 <= order <= maxDerivativeOrder()`, otherwise `std::nullopt`.
     */
    [[nodiscard]] std::optional<Vector> derivativeAt(T u, int order) const override
    {
        if (order < 1 || order > maxDerivativeOrder()) {
            return std::nullopt;
        }

        return Curve<T>::derivativeAt(u, order);
    }

  private:
    /**
     * @brief Returns the unit offset direction at @p u.
     *
     * @param u Parameter in the basis curve's units.
     * @return `normalize(reference_direction x tangent(u))`, or a zero vector at a singular point.
     */
    [[nodiscard]] Vector offsetDirection(T u) const
    {
        const Vector normal = reference_direction.cross(basis_curve->derivative(u));
        const T      length = normal.length();
        return (length <= vn::math::EPS<T>()) ? Vector(T(0), T(0), T(0)) : normal / length;
    }

  public:
    /// The curve the distance is measured from.
    BasisPtr basis_curve;

    /// Signed distance from the basis curve.
    T offset{ T(0) };

    /// Direction that, crossed with the basis' tangent, gives the offset direction.
    Vector reference_direction;
};

using OffsetCurvef = OffsetCurve<float>;
using OffsetCurved = OffsetCurve<double>;

VN_GEOM_SPATIAL_NS_END
