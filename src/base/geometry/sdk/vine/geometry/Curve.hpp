#pragma once

#include "geometry_global.hpp"

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Continuity.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An evaluable curve: a point for every parameter in its domain, plus the domain and its periodicity.
 *
 * @details
 * - This is the **interface** an algorithm consumes when it does not know which curve it holds: it evaluates a point,
 *   asks for a derivative, asks for the parameter range, and asks whether the curve closes on itself. The concrete
 *   value types (`LineCurve`, `CircleCurve`, ...) implement it, so a caller that does know the type keeps their
 *   cheaper, non-virtual members.
 * - The interface carries the four properties every parameterised curve has: a point and its derivatives along the
 *   parameter, a parameter range with its periodicity, a smoothness class, and the map that turns a parameter into
 *   the one measuring from the other end.
 * - A curve answers for parameters outside its range too: the domain is a property of the curve, not a precondition
 *   of `evaluate`. What an outside parameter means is the type's own answer - a polynomial extrapolation for a Bezier
 *   curve, the clamped end point for a B-spline, a periodic wrap for a circle - and each type documents it.
 * - `period()` is only meaningful when `isPeriodic()` is true; a non-periodic curve answers 0.
 * - `Ray` is deliberately **not** a `Curve`: it is copied per intersection query inside the picker, and a vptr there
 *   costs the inlined slab/triangle tests (`geometry-ray.md` 2.7). It stays a plain value with `pointAt`.
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
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

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
     * - The vector lives in the same space as `evaluate()`; it is not a point and not displaced by any origin.
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
     *   the same fact in the form an algorithm asks it ("is there a continuous tangent? `isCN(1)`").
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
     * - Every curve refuses its degenerate inputs in one place: a zero direction, a degenerate frame, a non-positive
     *   weight, a reversed trim, a net that disagrees with its knots. The answer is a property of the state, not of a
     *   parameter, so it does not depend on @p u.
     * - A curve `isValid()` refuses answers instead of failing: the numeric queries return the degenerate value its
     *   type documents (a zero vector, the first pole, the origin). Those answers are for keeping a mistake a wrong
     *   answer rather than undefined behaviour - they are not geometry, so a caller checks here first.
     * - The question belongs to the interface because a caller holding a `const Curve<T>&` cannot otherwise ask it,
     *   and `Shape` / `Primitive` already answer the same question the same way.
     *
     * @return True when the curve's stored state can be evaluated.
     */
    [[nodiscard]] virtual bool isValid() const = 0;

    /**
     * @brief Returns the parameter that maps to the same point when the curve is walked the other way.
     *
     * @details
     * - Reversal is a property of the parameterisation, not an edit of the object: the curve is not mutated. A
     *   caller that wants the reversed curve constructs its mirror (a line with `-direction`, a circle with
     *   `-normal`, a trim with its range swapped); this function tells it how the parameters move.
     * - The map is an involution: `reversedParameter(reversedParameter(u)) == u`.
     * - It reflects within the curve's own range, so a bounded piece answers `first + last - u`, while an unbounded
     *   or periodic curve answers `-u` and `period - u` respectively.
     *
     * @param u Parameter in this curve's own units.
     * @return The parameter of the same point under the opposite direction.
     */
    [[nodiscard]] virtual T reversedParameter(T u) const = 0;
};

VN_GEOMETRY_NS_END
