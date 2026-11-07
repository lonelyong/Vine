#pragma once

#include <vine/geom/geom_global.hpp>

#include <memory>
#include <optional>
#include <utility>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>

#include "BoundedCurve.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A curve restricted to a parameter range: a basis curve plus the interval the caller cares about.
 *
 * @details
 * - The trim is a view, not a copy: `evaluate()` forwards to the basis, so trimming a spline costs one handle and two
 *   numbers. The basis is held by `std::shared_ptr<const Curve<T>>`, so it may be shared and may outlive a local.
 * - Closure and periodicity belong to the basis. A trimmed curve is closed only when its range spans the whole
 *   natural range of a closed basis **and its two ends meet**; it is periodic only when the basis is periodic and the
 *   range spans a full period. Anything shorter is a piece with two ends.
 * - The range is stored as given: nothing checks that `first_parameter <= last_parameter`. A reversed range is a
 *   caller error that `isValid()` reports, and `evaluate()` answers it honestly rather than reordering the interval.
 * - `evaluate()` forwards at any parameter, including one outside the trim: asking past an end answers the basis, not a
 *   clamped end point, because a trim removes a range rather than cutting the curve short.
 *
 * @warning
 * - A null @p basis_curve is a caller error that `isValid()` reports; a query asked anyway answers the degenerate
 *   value of its family (the origin point, a zero vector, `C0`, false) rather than dereferencing the handle, so the
 *   mistake stays a wrong answer instead of undefined behaviour.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class TrimmedCurve : public BoundedCurve<T> {
  public:
    using typename Curve<T>::Point;
    using typename Curve<T>::Vector;
    using BasisPtr = std::shared_ptr<const Curve<T>>;

  public:
    /**
     * @brief Constructs a trimmed curve over a basis curve and a parameter range.
     *
     * @param basis            The curve the range is taken on; must not be null.
     * @param first_parameter  First parameter of the trim.
     * @param last_parameter   Last parameter of the trim.
     */
    TrimmedCurve(BasisPtr basis, T first_parameter, T last_parameter)
      : basis_curve(std::move(basis))
      , first_parameter(first_parameter)
      , last_parameter(last_parameter)
    {}

  public:
    /**
     * @brief Returns the point at parameter @p u by evaluating the basis curve.
     *
     * @param u Parameter in the basis curve's units.
     * @return The point on the basis curve; the origin when the trim has no basis.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return basis_curve != nullptr ? basis_curve->evaluate(u) : Point();
    }

    /**
     * @brief Returns the first parameter of the trim.
     *
     * @return The stored lower bound.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return first_parameter;
    }

    /**
     * @brief Returns the last parameter of the trim.
     *
     * @return The stored upper bound.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return last_parameter;
    }

    /**
     * @brief Returns whether the trim spans the whole closed basis and ends where it started.
     *
     * @details
     * - Covering the basis' range is necessary but not sufficient: a range that ends past it - `[0, 2*pi + 0.3]` on a
     *   circle, for example - covers the range and still ends at a different point, so the two ends are compared too.
     * - A whole turn of a periodic basis that starts away from the basis' seam meets at its ends but does not cover
     *   the natural range, so it answers false. The same structural rule is stated by `Curve::isClosed()`.
     *
     * @return True when the basis is closed, the trim covers its natural range and the two end points coincide.
     */
    [[nodiscard]] bool isClosed() const override
    {
        if (basis_curve == nullptr || !basis_curve->isClosed() || !spansWholeRange()) {
            return false;
        }

        return (evaluate(first_parameter) - evaluate(last_parameter)).length() <= vn::math::EPS<T>();
    }

    /**
     * @brief Returns whether the trim spans a full period of a periodic basis.
     *
     * @return True when the basis is periodic and the trim covers one period; false without a basis.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return basis_curve != nullptr && basis_curve->isPeriodic() && spansWholePeriod();
    }

    /**
     * @brief Returns the period of this piece, or 0 when the piece itself does not repeat.
     *
     * @details
     * - The contract belongs to the interface, not to the basis: a trim shorter than a period is a piece with two
     *   ends, so it answers 0 even though its basis repeats. A caller that wants the basis' period asks the basis,
     *   which this type keeps public as `basis_curve`.
     * - The basis' period is still what decides `isPeriodic()`, because spanning one period is exactly what makes the
     *   piece repeat.
     *
     * @return The basis period when `isPeriodic()`, otherwise 0.
     */
    [[nodiscard]] T period() const override
    {
        return isPeriodic() ? basis_curve->period() : T(0);
    }

    /**
     * @brief Returns the first derivative at parameter @p u by asking the basis curve.
     *
     * @details
     * - Trimming a range does not reparameterise: the piece keeps the basis' speed, so the derivative is the
     *   basis' derivative at the same parameter, not a rescaled one.
     *
     * @param u Parameter in the basis curve's units.
     * @return The basis curve's velocity vector at @p u; the zero vector when the trim has no basis.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        return basis_curve != nullptr ? basis_curve->derivative(u) : Vector(T(0), T(0), T(0));
    }

    /**
     * @brief Returns the second derivative at parameter @p u by asking the basis curve.
     *
     * @param u Parameter in the basis curve's units.
     * @return The basis curve's acceleration vector at @p u; the zero vector when the trim has no basis.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return basis_curve != nullptr ? basis_curve->secondDerivative(u) : Vector(T(0), T(0), T(0));
    }

    /**
     * @brief Returns the basis curve's highest derivative order, since the trim forwards every order.
     *
     * @details
     * - Trimming removes parameters, not smoothness, so the view answers exactly what its basis answers.
     *   A view without a basis reports the contract's default of two, matching the zero vectors it answers otherwise.
     *
     * @return The basis curve's `maxDerivativeOrder()`; 2 when the trim has no basis.
     */
    [[nodiscard]] int maxDerivativeOrder() const override
    {
        return basis_curve != nullptr ? basis_curve->maxDerivativeOrder() : 2;
    }

    /**
     * @brief Returns the derivative of the requested order by asking the basis curve.
     *
     * @param u Parameter in the basis curve's units.
     * @param order Derivative order, at least 1.
     * @return The basis curve's answer, or `std::nullopt` when the trim has no basis or @p order exceeds the basis'
     *         capability.
     */
    [[nodiscard]] std::optional<Vector> derivativeAt(T u, int order) const override
    {
        if (basis_curve == nullptr) {
            return std::nullopt;
        }

        return basis_curve->derivativeAt(u, order);
    }

    /**
     * @brief Returns the basis continuity: a trim removes a range, it does not smooth a corner.
     *
     * @return The basis curve's continuity; `C0` when the trim has no basis, since nothing is known to be smoother.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis_curve != nullptr ? basis_curve->continuity() : Continuity::C0;
    }

    /**
     * @brief Returns the parameter of the same point when this piece is walked the other way.
     *
     * @details
     * - The reflection is inside the trim, not inside the basis: `first + last - u`. Reflecting on the basis would
     *   answer parameters outside the piece for any trim shorter than the basis' own range.
     *
     * @param u Parameter in the basis curve's units.
     * @return `first_parameter + last_parameter - u`; infinite when the trim itself is unbounded.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return first_parameter + last_parameter - u;
    }

    /**
     * @brief Returns whether the trim has a basis and a range that is not reversed.
     *
     * @details
     * - Both things that can go wrong are caller input: a null basis leaves nothing to evaluate, and a reversed range
     *   (`first_parameter > last_parameter`) describes a different piece than the caller asked for.
     *
     * @return True when the basis is present and `first_parameter <= last_parameter`.
     */
    [[nodiscard]] bool isValid() const override
    {
        return basis_curve != nullptr && first_parameter <= last_parameter;
    }

  private:
    /// True when the trim covers the basis' whole natural range.
    [[nodiscard]] bool spansWholeRange() const
    {
        return first_parameter <= basis_curve->firstParameter() && last_parameter >= basis_curve->lastParameter();
    }

    /// True when the trim covers at least one full period of the periodic basis.
    [[nodiscard]] bool spansWholePeriod() const
    {
        return last_parameter - first_parameter >= basis_curve->period();
    }

  public:
    /// The curve the range is taken on.
    BasisPtr basis_curve;

    /// First parameter of the trim.
    T first_parameter{ T(0) };

    /// Last parameter of the trim.
    T last_parameter{ T(0) };
};

using TrimmedCurvef = TrimmedCurve<float>;
using TrimmedCurved = TrimmedCurve<double>;

VN_GEOM_SPATIAL_NS_END
