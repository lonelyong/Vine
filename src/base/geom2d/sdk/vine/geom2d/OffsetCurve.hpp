#pragma once

#include "geom2d_global.hpp"

#include <cmath>
#include <type_traits>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A curve at a constant distance from another one, on its left side.
 *
 * @details
 * - `O(u) = C(u) + distance * N(u)`, where `N` is the **left** unit normal of the basis, `C'` rotated by +90 degrees.
 *   A positive distance therefore offsets to the left of the direction of travel: for a circle walked
 *   counter-clockwise that is towards the centre, so the offset circle has radius `r - distance`.
 * - The basis is held **by value** and the type is a template over it, not a `Curve<T>` handle: the offset needs the
 *   basis' third derivative to answer `secondDerivative()`, and a third derivative is a **member of the concrete
 *   types**, not part of `Curve<T>`. Keeping it a template is what lets the contract stay at eleven items
 *   (2.13, `../decisions/ADR-2026-11-05-planar-offset-curve.md`).
 * - `evaluate`, `derivative` and `secondDerivative` are exact: the first two differentiate the normal analytically, the
 *   third applies Leibniz' rule to `C(u) + distance * N(u)` using the basis' jerk.
 * - The parameterisation is the basis': `firstParameter()`, `lastParameter()` and `reversedParameter()` are forwarded,
 *   and `isPeriodic()` / `period()` follow the basis with this type's own `isPeriodic()` as the guard.
 * - The same geometric offset walked backwards is the offset of the reversed basis by the **negated** distance, because
 *   reversing the direction of travel turns the left side into the right one.
 *
 * @warning
 * - The offset is only as smooth as the basis' normal, which fails where the basis' speed vanishes (a cusp or a
 *   degenerate point). `continuity()` forwards the basis' answer and does not promise anything at such a parameter;
 *   `isValid()` reports the basis' own usability, not whether it has a cusp.
 * - There is no `bounds()`: a box would have to be the basis' box grown by `|distance|`, which needs the basis to have
 *   one, and no consumer has asked for the loose box yet.
 *
 * @tparam C Concrete basis curve type; must derive from `Curve<C::value_type>` and provide a `thirdDerivative(u)`.
 */
template <typename C>
    requires std::is_base_of_v<Curve<typename C::value_type>, C>
class OffsetCurve : public Curve<typename C::value_type> {
  public:
    using value_type = typename C::value_type;
    using Point      = vn::math::Point2<value_type>;
    using Vector     = vn::math::Vector2<value_type>;

  public:
    /**
     * @brief Constructs the offset of @p basis_curve at @p offset_distance.
     *
     * @param basis_curve     The curve to offset; copied by value.
     * @param offset_distance Signed distance: positive is to the left of the basis' direction of travel.
     */
    OffsetCurve(C basis_curve, value_type offset_distance)
      : basis(std::move(basis_curve))
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
    [[nodiscard]] Vector normalAt(value_type u) const
    {
        const Vector tangent = basis.derivative(u);
        const auto   speed   = tangent.length();
        if (!(speed > vn::math::EPS<value_type>())) {
            return Vector::zero();
        }

        return Vector{ -tangent.y / speed, tangent.x / speed };
    }

    /**
     * @brief Returns the offset point at @p u.
     *
     * @param u Parameter in the basis' own units.
     * @return `basis.evaluate(u) + distance * normalAt(u)`.
     */
    [[nodiscard]] Point evaluate(value_type u) const override
    {
        return basis.evaluate(u) + normalAt(u) * distance;
    }

    /**
     * @brief Returns the first derivative at @p u.
     *
     * @details
     * - `O' = C' + distance * N'`, with `N'` the rotated derivative of the unit tangent: the part of the acceleration
     *   orthogonal to the tangent, divided by the speed. Nothing is approximated.
     *
     * @param u Parameter in the basis' own units.
     * @return The velocity vector at @p u; the basis' velocity where the basis' speed vanishes.
     */
    [[nodiscard]] Vector derivative(value_type u) const override
    {
        const Vector tangent = basis.derivative(u);
        const auto   speed   = tangent.length();
        if (!(speed > vn::math::EPS<value_type>())) {
            return tangent;
        }

        const Vector unit          = tangent / speed;
        const Vector acceleration  = basis.secondDerivative(u);
        const Vector normal_rate   = rotated(acceleration - unit * acceleration.dot(unit)) / speed;
        return tangent + normal_rate * distance;
    }

    /**
     * @brief Returns the second derivative at @p u.
     *
     * @details
     * - `O'' = C'' + distance * N''`, where `N''` differentiates the unit tangent twice: the first step uses the
     *   acceleration, the second uses the basis' **third** derivative. That is what makes the offset exact and why the
     *   basis has to answer `thirdDerivative()`.
     *
     * @param u Parameter in the basis' own units.
     * @return The acceleration vector at @p u; the basis' acceleration where the basis' speed vanishes.
     */
    [[nodiscard]] Vector secondDerivative(value_type u) const override
    {
        const Vector tangent = basis.derivative(u);
        const auto   speed   = tangent.length();
        const Vector acceleration = basis.secondDerivative(u);

        if (!(speed > vn::math::EPS<value_type>())) {
            return acceleration;
        }

        const Vector unit        = tangent / speed;
        const Vector normal_part = acceleration - unit * acceleration.dot(unit);
        const Vector unit_rate   = normal_part / speed;
        const auto   speed_rate  = unit.dot(acceleration);

        const Vector jerk             = basis.thirdDerivative(u);
        const Vector normal_part_rate = jerk - unit * unit_rate.dot(acceleration) - unit * unit.dot(jerk)
                                      - unit_rate * unit.dot(acceleration);
        const Vector unit_second_rate = (normal_part_rate * speed - normal_part * speed_rate) / (speed * speed);

        return acceleration + rotated(unit_second_rate) * distance;
    }

    /**
     * @brief Returns the first parameter of the basis.
     *
     * @return The basis' first parameter.
     */
    [[nodiscard]] value_type firstParameter() const override
    {
        return basis.firstParameter();
    }

    /**
     * @brief Returns the last parameter of the basis.
     *
     * @return The basis' last parameter.
     */
    [[nodiscard]] value_type lastParameter() const override
    {
        return basis.lastParameter();
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
        if (!basis.isClosed()) {
            return false;
        }

        const auto first_point = evaluate(firstParameter());
        const auto last_point  = evaluate(lastParameter());
        return (first_point - last_point).length() <= vn::math::EPS<value_type>();
    }

    /**
     * @brief Returns whether the offset repeats with the basis' period.
     *
     * @return True when the basis is periodic.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return basis.isPeriodic();
    }

    /**
     * @brief Returns the basis' period, or 0 when the offset itself does not repeat.
     *
     * @return The basis' period when `isPeriodic()`, otherwise 0.
     */
    [[nodiscard]] value_type period() const override
    {
        return isPeriodic() ? basis.period() : value_type(0);
    }

    /**
     * @brief Returns the basis' continuity.
     *
     * @details
     * - An offset adds smoothness nowhere and removes it only where the normal fails, so the basis' answer is the one
     *   to forward; see the warning on the class for the singular case.
     *
     * @return The basis' continuity.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis.continuity();
    }

    /**
     * @brief Returns whether the basis can be evaluated.
     *
     * @return True when the basis is valid; the distance does not affect usability.
     */
    [[nodiscard]] bool isValid() const override
    {
        return basis.isValid();
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
     * @return `basis.reversedParameter(u)`.
     */
    [[nodiscard]] value_type reversedParameter(value_type u) const override
    {
        return basis.reversedParameter(u);
    }

  private:
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
    C basis;

    /// Signed offset distance: positive is to the left of the basis' direction of travel.
    value_type distance{ value_type(0) };
};

VN_GEOM2D_NS_END
