#pragma once

#include "geometry_global.hpp"

#include <memory>
#include <utility>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>

#include "BoundedCurve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A curve restricted to a parameter range: a basis curve plus the interval the caller cares about.
 *
 * @details
 * - The trim is a view, not a copy: `evaluate()` forwards to the basis, so trimming a spline costs one handle and two
 *   numbers. The basis is held by `std::shared_ptr<const Curve<T>>`, so it may be shared and may outlive a local.
 * - Closure and periodicity belong to the basis. A trimmed curve is closed only when its range spans the whole
 *   natural range of a closed basis; it is periodic only when the basis is periodic and the range spans a full
 *   period. Anything shorter is a piece with two ends.
 * - The range is stored as given: nothing checks that `first_parameter <= last_parameter`. A reversed range is a
 *   caller error that `evaluate()` reports honestly rather than a silently reordered interval.
 *
 * @warning
 * - @p basis_curve must not be null: `evaluate()` dereferences it. A null basis is refused at construction.
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
     * @return The point on the basis curve.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return basis_curve->evaluate(u);
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
     * @brief Returns whether the trim spans the whole closed basis.
     *
     * @return True when the basis is closed and the trim covers its natural range.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return basis_curve->isClosed() && spansWholeRange();
    }

    /**
     * @brief Returns whether the trim spans a full period of a periodic basis.
     *
     * @return True when the basis is periodic and the trim covers one period.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return basis_curve->isPeriodic() && spansWholePeriod();
    }

    /**
     * @brief Returns the basis period, meaningful only when `isPeriodic()` is true.
     *
     * @return The basis period, or 0 when the basis is not periodic.
     */
    [[nodiscard]] T period() const override
    {
        return basis_curve->isPeriodic() ? basis_curve->period() : T(0);
    }

    /**
     * @brief Returns the first derivative at parameter @p u by asking the basis curve.
     *
     * @details
     * - Trimming a range does not reparameterise: the piece keeps the basis' speed, so the derivative is the
     *   basis' derivative at the same parameter, not a rescaled one.
     *
     * @param u Parameter in the basis curve's units.
     * @return The basis curve's velocity vector at @p u.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        return basis_curve->derivative(u);
    }

    /**
     * @brief Returns the second derivative at parameter @p u by asking the basis curve.
     *
     * @param u Parameter in the basis curve's units.
     * @return The basis curve's acceleration vector at @p u.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return basis_curve->secondDerivative(u);
    }

    /**
     * @brief Returns the basis continuity: a trim removes a range, it does not smooth a corner.
     *
     * @return The basis curve's continuity.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis_curve->continuity();
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

VN_GEOMETRY_NS_END
