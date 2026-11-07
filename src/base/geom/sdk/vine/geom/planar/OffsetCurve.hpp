#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief A curve at a constant distance from another one, on its left side.
 *
 * @details
 * - `O(u) = C(u) + distance * N(u)`, where `N` is the **left** unit normal of the basis, `C'` rotated by +90 degrees.
 *   A positive distance therefore offsets to the left of the direction of travel: for a circle walked
 *   counter-clockwise that is towards the centre, so the offset circle has radius `r - distance`.
 * - The basis is held as `std::shared_ptr<const Curve<T>>`, not as a template argument: any curve - a value type, a
 *   `TrimmedCurve`, another offset - can be offset, and the caller decides whether the handle is shared. The space
 *   `geom::spatial::OffsetCurve` already had this shape; it stays abstract only because no space curve answers a third
 *   derivative yet, whereas here every concrete planar type does.
 * - `evaluate`, `derivative` and `secondDerivative` are exact, never a finite difference: the first two differentiate
 *   the unit normal analytically, the third applies Leibniz' rule to `C(u) + distance * N(u)` using the basis' jerk.
 * - Derivative order follows the basis: the offset answers **one less** than its basis does, clipped to two, because
 *   `O'''` would need `C''''`. Over a basis that answers three (every planar concrete type) it reports 2; over a basis
 *   that answers two (another offset) it reports 1 and answers `std::nullopt` for the second derivative.
 * - The parameterisation is the basis': the range and `reversedParameter()` are forwarded, and `isPeriodic()` /
 *   `period()` follow the basis with this type's own `isPeriodic()` as the guard.
 * - The same geometric offset walked backwards is the offset of the reversed basis by the **negated** distance, because
 *   reversing the direction of travel turns the left side into the right one.
 *
 * @warning
 * - The offset is only as smooth as the basis' normal, which fails where the basis' speed vanishes (a cusp or a
 *   degenerate point). `continuity()` forwards the basis' answer and does not promise anything at such a parameter.
 * - A basis that cannot answer a third derivative makes this offset a position-and-velocity view: `isValid()` reports
 *   it, `maxDerivativeOrder()` drops to 1, and `secondDerivative()` answers the zero vector instead of a wrong number.
 * - There is no `bounds()`: a box would have to be the basis' box grown by `|distance|`, which needs the basis to have
 *   one, and no consumer has asked for the loose box yet.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class OffsetCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;
    using BasisPtr   = std::shared_ptr<const Curve<T>>;

  public:
    /**
     * @brief Constructs the offset of @p basis_curve at @p offset_distance.
     *
     * @param basis_curve     The curve to offset; shared, never copied.
     * @param offset_distance Signed distance: positive is to the left of the basis' direction of travel.
     */
    OffsetCurve(BasisPtr basis_curve, T offset_distance)
      : basis_curve(std::move(basis_curve))
      , distance(offset_distance)
    {}

  public:
    /**
     * @brief Returns the left unit normal of the basis at @p u.
     *
     * @details
     * - It is `derivative(u)` rotated by +90 degrees and normalised; the zero vector at a parameter where the basis'
     *   speed vanishes, which is the one place the offset degenerates to the basis point itself.
     *
     * @param u Parameter in the basis' own units.
     * @return The left unit normal, or the zero vector at a singular parameter.
     */
    [[nodiscard]] Vector normalAt(T u) const
    {
        if (basis_curve == nullptr) {
            return Vector::zero();
        }

        return leftNormal(basis_curve->derivative(u));
    }

    /**
     * @brief Returns the offset point at @p u.
     *
     * @param u Parameter in the basis' own units.
     * @return `basis.evaluate(u) + distance * normalAt(u)`; the origin without a basis.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        if (basis_curve == nullptr) {
            return Point();
        }

        return basis_curve->evaluate(u) + leftNormal(basis_curve->derivative(u)) * distance;
    }

    /**
     * @brief Returns the first derivative at @p u.
     *
     * @details
     * - `O' = C' + distance * N'`, with `N'` the rotated derivative of the unit tangent: the part of the acceleration
     *   orthogonal to the tangent, divided by the speed. Nothing is approximated.
     *
     * @param u Parameter in the basis' own units.
     * @return The velocity vector at @p u; the basis' velocity where the basis' speed vanishes, and the zero vector
     *         without a basis.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        if (basis_curve == nullptr) {
            return Vector::zero();
        }

        const Vector tangent = basis_curve->derivative(u);
        const auto   speed   = tangent.length();
        if (!(speed > vn::math::EPS<T>())) {
            return tangent;
        }

        const Vector unit         = tangent / speed;
        const Vector acceleration = basis_curve->secondDerivative(u);
        const Vector normal_rate  = rotated(acceleration - unit * acceleration.dot(unit)) / speed;
        return tangent + normal_rate * distance;
    }

    /**
     * @brief Returns the second derivative at @p u.
     *
     * @details
     * - `O'' = C'' + distance * N''`, where `N''` differentiates the unit tangent twice: the first step uses the
     *   acceleration, the second uses the basis' **third** derivative, which is why the basis has to answer three.
     *
     * @param u Parameter in the basis' own units.
     * @return The acceleration vector at @p u; the basis' acceleration where the basis' speed vanishes, and the zero
     *         vector when the basis cannot answer a third derivative or there is no basis.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        if (basis_curve == nullptr || !answersSecondDerivative()) {
            return Vector::zero();
        }

        const Vector tangent      = basis_curve->derivative(u);
        const auto   speed        = tangent.length();
        const Vector acceleration = basis_curve->secondDerivative(u);

        if (!(speed > vn::math::EPS<T>())) {
            return acceleration;
        }

        const Vector unit        = tangent / speed;
        const Vector normal_part = acceleration - unit * acceleration.dot(unit);
        const Vector unit_rate   = normal_part / speed;
        const auto   speed_rate  = unit.dot(acceleration);

        const Vector jerk             = basis_curve->derivativeAt(u, 3).value_or(Vector::zero());
        const Vector normal_part_rate = jerk - unit * unit_rate.dot(acceleration) - unit * unit.dot(jerk)
                                      - unit_rate * unit.dot(acceleration);
        const Vector unit_second_rate = (normal_part_rate * speed - normal_part * speed_rate) / (speed * speed);

        return acceleration + rotated(unit_second_rate) * distance;
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
     * @param u Parameter in the basis' own units.
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

    /**
     * @brief Returns the first parameter of the basis.
     *
     * @return The basis' first parameter; 0 without a basis.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return basis_curve != nullptr ? basis_curve->firstParameter() : T(0);
    }

    /**
     * @brief Returns the last parameter of the basis.
     *
     * @return The basis' last parameter; 0 without a basis.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return basis_curve != nullptr ? basis_curve->lastParameter() : T(0);
    }

    /**
     * @brief Returns whether the offset ends where it started.
     *
     * @details
     * - A closed basis is not enough on its own: the normal at the seam has to agree as well, or the offset jumps.
     *   The answer is the two offset end points compared, so a basis that closes with a corner answers false.
     *
     * @return True when the offset's two end points are closer than the floating-point epsilon.
     */
    [[nodiscard]] bool isClosed() const override
    {
        if (basis_curve == nullptr || !basis_curve->isClosed()) {
            return false;
        }

        return (evaluate(firstParameter()) - evaluate(lastParameter())).length() <= vn::math::EPS<T>();
    }

    /**
     * @brief Returns whether the offset repeats with the basis' period.
     *
     * @return True when the basis is periodic; false without a basis.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return basis_curve != nullptr && basis_curve->isPeriodic();
    }

    /**
     * @brief Returns the basis' period, or 0 when the offset itself does not repeat.
     *
     * @return The basis' period when `isPeriodic()`, otherwise 0.
     */
    [[nodiscard]] T period() const override
    {
        return isPeriodic() ? basis_curve->period() : T(0);
    }

    /**
     * @brief Returns the basis' continuity.
     *
     * @details
     * - An offset adds smoothness nowhere and removes it only where the normal fails, so the basis' answer is the one
     *   to forward; see the warning on the class for the singular case.
     *
     * @return The basis' continuity; `C0` without a basis.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis_curve != nullptr ? basis_curve->continuity() : Continuity::C0;
    }

    /**
     * @brief Returns whether the basis is usable and can answer the third derivative this offset needs.
     *
     * @details
     * - The distance does not affect usability, and neither does the capability of the basis for the position and the
     *   velocity: what a basis that answers only two derivatives costs is the offset's **second** derivative. That is
     *   a different piece than the caller asked for, so it is reported here rather than papered over with a zero.
     *
     * @return True when the basis is present, usable, and reports `maxDerivativeOrder() >= 3`.
     */
    [[nodiscard]] bool isValid() const override
    {
        return basis_curve != nullptr && basis_curve->isValid() && answersSecondDerivative();
    }

    /**
     * @brief Returns the basis' reversal map: the offset is not reparameterised by reversal.
     *
     * @details
     * - It is an involution and it swaps the ends of the domain, both inherited from the basis. The offset that a
     *   caller builds on the reversed basis traces the same geometric curve backwards only with the **negated**
     *   distance, because reversing the direction of travel swaps left and right.
     *
     * @param u Parameter in the basis' own units.
     * @return `basis.reversedParameter(u)`; `u` without a basis.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return basis_curve != nullptr ? basis_curve->reversedParameter(u) : u;
    }

  private:
    /**
     * @brief Returns whether the basis reports enough derivative orders for this offset's second derivative.
     *
     * @return True when the basis can answer a third derivative.
     */
    [[nodiscard]] bool answersSecondDerivative() const
    {
        return basis_curve != nullptr && basis_curve->maxDerivativeOrder() >= 3;
    }

    /**
     * @brief Returns @p tangent rotated by +90 degrees and normalised: the left unit normal.
     *
     * @param tangent The basis' velocity.
     * @return The left unit normal, or the zero vector where the speed vanishes.
     */
    [[nodiscard]] static Vector leftNormal(const Vector& tangent)
    {
        const auto speed = tangent.length();
        if (!(speed > vn::math::EPS<T>())) {
            return Vector::zero();
        }

        return Vector{ -tangent.y / speed, tangent.x / speed };
    }

    /**
     * @brief Returns @p value rotated by +90 degrees: the left normal of a direction.
     *
     * @param value The direction to rotate.
     * @return `(-value.y, value.x)`.
     */
    [[nodiscard]] static Vector rotated(const Vector& value)
    {
        return Vector{ -value.y, value.x };
    }

  public:
    /// The curve being offset.
    BasisPtr basis_curve;

    /// Signed offset distance: positive is to the left of the basis' direction of travel.
    T distance{ T(0) };
};

using OffsetCurvef = OffsetCurve<float>;
using OffsetCurved = OffsetCurve<double>;

/**
 * @brief Wraps a concrete curve and a signed distance into an `OffsetCurve` of the matching scalar type.
 *
 * @details
 * - It exists so a caller holding a value type does not have to spell `std::make_shared` and the scalar type.
 * - A basis that answers only two derivative orders is accepted and produces an offset whose `isValid()` is false;
 *   the capability check is the caller's, at the point where the basis is chosen.
 *
 * @param basis_curve     The curve to offset; copied into the handle.
 * @param offset_distance Signed distance: positive is to the left of the basis' direction of travel.
 * @return An offset over a shared copy of @p basis_curve.
 */
template <typename C>
    requires std::is_base_of_v<Curve<typename C::value_type>, C>
[[nodiscard]] OffsetCurve<typename C::value_type> offsetCurve(C basis_curve, typename C::value_type offset_distance)
{
    using Scalar = typename C::value_type;
    return OffsetCurve<Scalar>(std::make_shared<C>(std::move(basis_curve)), offset_distance);
}

VN_GEOM_PLANAR_NS_END
