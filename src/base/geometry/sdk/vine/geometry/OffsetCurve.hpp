#pragma once

#include "geometry_global.hpp"

#include <memory>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A curve held at a signed distance from a basis curve, in the plane the reference direction defines.
 *
 * @details
 * - The offset point is the basis point pushed along `normalize(reference_direction x tangent)`: the reference
 *   direction picks the plane, the basis' first derivative picks the side. That is why an offset curve needs three
 *   things - the basis, the distance and a reference direction - and why the distance alone is not enough.
 * - Every parameter-domain answer is the basis': the offset inherits the range, the period and the closure, because it
 *   is evaluated at the same parameter and is only displaced at right angles to the basis.
 * - `evaluate()` and `derivative()` are built from the basis' point, tangent and second derivative, with the
 *   cross-product guard that turns a degenerate input into the basis itself instead of a NaN.
 * - `secondDerivative()` is **still pure**: it needs the basis' third derivative, which `Curve<T>` deliberately does
 *   not expose, so the type stays abstract and cannot be constructed (`../backlog/geometry.md`).
 *
 * @warning
 * - A null @p basis_curve is a caller error that `isValid()` reports; a query asked anyway answers the degenerate
 *   value of its family (the origin point, a zero vector, `C0`, false) rather than dereferencing the handle, so the
 *   mistake stays a wrong answer instead of undefined behaviour.
 * - The offset is only well defined where `normalize(reference_direction x tangent)` exists. Nothing here checks
 *   that, and the basis' continuity does not describe it, so a curve whose tangent becomes parallel to the reference
 *   direction has no offset there.
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
     * @return True when the basis is not null and the reference direction is non-zero.
     */
    [[nodiscard]] bool isValid() const override
    {
        return basis_curve != nullptr && reference_direction.length() > vn::math::EPS<T>();
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
     * @brief Not implemented: it needs the basis' third derivative, which `Curve<T>` does not expose.
     *
     * @details
     * - `P'' = p'' + d * (N / R)''` with `N = reference_direction x p'`, so the displacement's second derivative
     *   contains `p'''`. The curve interface stops at the second derivative on purpose (`geometry-kinds.md` D6), and a
     *   finite difference in its place would be a silent accuracy drop, so the answer stays pure until the interface
     *   grows a third derivative and this type can answer it exactly.
     *
     * @param u Parameter in the basis curve's units.
     * @return Never returns: the function is pure and keeps the type abstract.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override = 0;

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

VN_GEOMETRY_NS_END
