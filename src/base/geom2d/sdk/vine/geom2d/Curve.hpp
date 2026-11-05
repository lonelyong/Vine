#pragma once

#include "geom2d_global.hpp"

#include <vine/math/Continuity.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

VN_GEOM2D_NS_BEGIN

/**
 * @brief An evaluable planar curve: a point for every parameter in its domain, plus the domain and its periodicity.
 *
 * @details
 * - This is the same contract as `geom3d::Curve`, one space down: the point and the vectors are two-dimensional and
 *   everything else - the parameter range, its closure and period, the smoothness class, the reversal map - means
 *   exactly what it means there.
 * - This is the **interface** an algorithm consumes when it does not know which curve it holds. The concrete value
 *   types (`LineCurve`, `CircleCurve`, ...) implement it, so a caller that does know the type keeps their cheaper,
 *   non-virtual members.
 * - A curve answers for parameters outside its range too: the domain is a property of the curve, not a precondition
 *   of `evaluate`. What an outside parameter means is the type's own answer, and each type documents it.
 * - `period()` is only meaningful when `isPeriodic()` is true; a non-periodic curve answers 0.
 *
 * @warning
 * - A destructor here means every concrete curve carries a vptr; keep hot query paths on the value types, not on
 *   `Curve<T>&`.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class Curve {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

  public:
    virtual ~Curve() = default;

  public:
    /**
     * @brief Returns the point at parameter @p u.
     *
     * @param u Parameter in this curve's own units (a distance for a line, an angle for a circle).
     * @return The point on the curve.
     */
    [[nodiscard]] virtual Point evaluate(T u) const = 0;

    /**
     * @brief Returns the first derivative with respect to the parameter, `dP/du`.
     *
     * @details
     * - The derivative is the parameterisation's velocity, not the unit tangent: its length is the curve's speed at
     *   @p u, which is 1 only for an arc-length curve. A caller that wants a direction normalises it.
     * - The vector is a free vector in the plane; it is not a point and not displaced by any origin.
     *
     * @param u Parameter in this curve's own units.
     * @return The velocity vector at @p u; the zero vector at a singular point of the parameterisation.
     */
    [[nodiscard]] virtual Vector derivative(T u) const = 0;

    /**
     * @brief Returns the second derivative with respect to the parameter, `d2P/du2`.
     *
     * @details
     * - For a curve parameterised by arc length this is the curvature vector: its length is the curvature and its
     *   direction points to the centre of curvature.
     * - It is not the curvature of a curve with any other parameterisation: divide by the squared speed to get the
     *   geometric curvature.
     *
     * @param u Parameter in this curve's own units.
     * @return The acceleration vector at @p u.
     */
    [[nodiscard]] virtual Vector secondDerivative(T u) const = 0;

    /**
     * @brief Returns the first parameter of the natural range.
     *
     * @return The lower bound; `-infinity` for an unbounded curve.
     */
    [[nodiscard]] virtual T firstParameter() const = 0;

    /**
     * @brief Returns the last parameter of the natural range.
     *
     * @return The upper bound; `+infinity` for an unbounded curve.
     */
    [[nodiscard]] virtual T lastParameter() const = 0;

    /**
     * @brief Returns whether the curve meets itself at the ends of its range.
     *
     * @return True when `evaluate(firstParameter()) == evaluate(lastParameter())`.
     */
    [[nodiscard]] virtual bool isClosed() const = 0;

    /**
     * @brief Returns whether the curve repeats with a period.
     *
     * @return True when `evaluate(u + period()) == evaluate(u)` for every u.
     */
    [[nodiscard]] virtual bool isPeriodic() const = 0;

    /**
     * @brief Returns the period, meaningful only when `isPeriodic()` is true.
     *
     * @return The period in this curve's parameter units; 0 for a non-periodic curve.
     */
    [[nodiscard]] virtual T period() const = 0;

    /**
     * @brief Returns the smoothness class inside the domain.
     *
     * @details
     * - This is the highest order of derivative that is continuous everywhere inside the domain; the answer for an
     *   analytic curve is `Continuity::CN`.
     * - The description covers the interior only: a closed curve may still close with a corner at the seam.
     *
     * @return The curve's continuity.
     */
    [[nodiscard]] virtual Continuity continuity() const = 0;

    /**
     * @brief Returns whether the curve is at least `C`-continuous of order @p order.
     *
     * @details
     * - This is a query over `continuity()`, not a second description of the curve: it reads the ladder and answers
     *   the same fact in the form an algorithm asks it.
     * - A geometric continuity does not answer a derivative question: `G1` is a common tangent *direction* with an
     *   arbitrary magnitude, so `isCN(1)` is false for it although the curve looks smooth.
     * - `CN` answers true for every order; a negative order is refused rather than treated as zero.
     *
     * @param order Derivative order being asked about.
     * @return True when every derivative up to @p order is continuous inside the domain.
     */
    [[nodiscard]] bool isCN(int order) const
    {
        if (order < 0) {
            return false;
        }

        if (order >= 4) {
            return continuity() == Continuity::CN;
        }

        const Continuity rungs[4] = { Continuity::C0, Continuity::C1, Continuity::C2, Continuity::C3 };
        return continuity() >= rungs[order];
    }

    /**
     * @brief Returns whether the state this curve was built from describes a curve at all.
     *
     * @details
     * - Every curve refuses its degenerate inputs in one place: a zero direction, a non-positive radius, a reversed
     *   trim. The answer is a property of the state, not of a parameter, so it does not depend on @p u.
     * - A curve `isValid()` refuses answers instead of failing: the numeric queries return the degenerate value its
     *   type documents (a zero vector, the origin). Those answers keep a mistake a wrong answer rather than undefined
     *   behaviour - they are not geometry, so a caller checks here first.
     *
     * @return True when the curve's stored state can be evaluated.
     */
    [[nodiscard]] virtual bool isValid() const = 0;

    /**
     * @brief Returns the parameter that maps to the same point when the curve is walked the other way.
     *
     * @details
     * - Reversal is a property of the parameterisation, not an edit of the object: the curve is not mutated. A caller
     *   that wants the reversed curve constructs its mirror; this function tells it how the parameters move.
     * - The map is an involution: `reversedParameter(reversedParameter(u)) == u`.
     * - It reflects within the curve's own range, so a bounded piece answers `first + last - u`, while an unbounded
     *   or periodic curve answers `-u` and `period - u` respectively.
     *
     * @param u Parameter in this curve's own units.
     * @return The parameter of the same point under the opposite direction.
     */
    [[nodiscard]] virtual T reversedParameter(T u) const = 0;
};

VN_GEOM2D_NS_END
