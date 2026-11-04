#pragma once

#include "geometry_global.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "BoundedCurve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A B-spline curve: a degree, poles, distinct knots with their multiplicities, and optional weights.
 *
 * @details
 * - The knot storage is `knots` (the **distinct** values) plus `multiplicities` (how many times each repeats), not a
 *   flat vector: the multiplicity is the only thing a caller ever asks about, and a flat vector would have to be
 *   grouped again on every query. Their total is `poles.size() + degree + 1` for the clamped form this type
 *   describes.
 * - `continuity()` is derived from the data, which is the reason a B-spline is worth having as a type: the curve is
 *   `C(degree - internal max multiplicity)` at its joins and `CN` between them, and both facts are readable from
 *   `multiplicities` without evaluating anything.
 * - **The non-numeric answers that the clamped form makes unambiguous are defined**: the domain is the first and the
 *   last knot, the curve is not periodic, its period is 0 and its reversed parameter reflects inside the domain.
 * - **The numeric answers are not implemented**: `evaluate()`, `derivative()`, `secondDerivative()` (the de Boor
 *   passes) and `isClosed()` are still pure, so the type cannot be constructed. `isClosed()` is left out on purpose:
 *   for a spline it is a geometric fact - the two end points coincide - not a property of the data the way it is for
 *   a Bezier curve, whose end points are its end poles.
 * - This is a memory-owning curve: `std::vector` poles, knots and weights mean deep copies.
 *
 * @warning
 * - The fields are public and nothing re-checks them on write, so `isValid()` is the only guarantee that a curve
 *   built field by field can be evaluated at all.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BSplineCurve : public BoundedCurve<T> {
  public:
    using value_type        = T;
    using Point             = vn::math::Point3<T>;
    using Vector            = vn::math::Vector3<T>;
    using PoleArray         = std::vector<Point>;
    using KnotArray         = std::vector<T>;
    using MultiplicityArray = std::vector<int>;
    using WeightArray       = std::vector<T>;

  public:
    /// Constructs the curve without data, which `isValid()` reports as unusable.
    BSplineCurve() = default;

    /**
     * @brief Constructs a B-spline curve from its degree, poles, knots and multiplicities.
     *
     * @param the_degree         Degree of the polynomial pieces; at least 1 for `isValid()`.
     * @param the_poles          Poles in order.
     * @param the_knots          Distinct knot values, non-decreasing.
     * @param the_multiplicities One multiplicity per distinct knot, each in `[1, degree + 1]`.
     * @param the_weights        One weight per pole, or empty for a polynomial (non-rational) curve.
     */
    BSplineCurve(int the_degree, PoleArray the_poles, KnotArray the_knots, MultiplicityArray the_multiplicities,
                 WeightArray the_weights = {})
      : degree(the_degree)
      , poles(std::move(the_poles))
      , knots(std::move(the_knots))
      , multiplicities(std::move(the_multiplicities))
      , weights(std::move(the_weights))
    {}

  public:
    /**
     * @brief Returns whether the curve carries weights.
     *
     * @return True when `weights` is non-empty.
     */
    [[nodiscard]] bool isRational() const
    {
        return !weights.empty();
    }

    /**
     * @brief Returns whether the degree, poles, knots, multiplicities and weights form a usable curve.
     *
     * @details
     * - The flat knot vector is rebuilt from the multiplicities and compared with `poles.size() + degree + 1`, which
     *   is the length the clamped form requires; a curve whose counts disagree cannot be evaluated.
     *
     * @return True when every count agrees and every multiplicity is within `[1, degree + 1]`.
     */
    [[nodiscard]] bool isValid() const
    {
        if (degree < 1 || poles.size() < 2) {
            return false;
        }

        if (knots.empty() || knots.size() != multiplicities.size()) {
            return false;
        }

        if (!weights.empty() && weights.size() != poles.size()) {
            return false;
        }

        std::size_t flat_count = 0;
        for (const int multiplicity : multiplicities) {
            if (multiplicity < 1 || multiplicity > degree + 1) {
                return false;
            }
            flat_count += static_cast<std::size_t>(multiplicity);
        }

        return flat_count == poles.size() + static_cast<std::size_t>(degree) + 1;
    }

    /**
     * @brief Returns the smoothness at the worst join, derived from the internal multiplicities.
     *
     * @details
     * - Every join inside the curve loses one order of continuity per extra multiplicity: the join at an internal
     *   knot of multiplicity `m` is `C(degree - m)`, so the curve's global continuity is set by the largest internal
     *   multiplicity.
     * - A curve with no internal knot is one polynomial span and answers `CN`; the end multiplicities are the clamp
     *   and are not joins, so they are not looked at.
     * - The ladder tops out at `C3`: `Continuity` has no `C4`, and a curve with a high degree and simple internal
     *   knots reports `C3` rather than a rung that does not exist.
     *
     * @return The continuity at the least smooth join.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        if (multiplicities.size() < 3) {
            return Continuity::CN;
        }

        int max_internal = 0;
        for (std::size_t index = 1; index + 1 < multiplicities.size(); ++index) {
            max_internal = std::max(max_internal, multiplicities[index]);
        }

        const int order = degree - max_internal;
        if (order <= 0) {
            return Continuity::C0;
        }
        if (order == 1) {
            return Continuity::C1;
        }
        if (order == 2) {
            return Continuity::C2;
        }

        return Continuity::C3;
    }

    /**
     * @brief Returns the first knot: the lower end of the domain.
     *
     * @details
     * - The clamped form makes the domain unambiguous: the first and the last distinct knots are its ends, because the
     *   ends of the curve are clamped onto them.
     *
     * @return `knots.front()`, or 0 when no knots are set.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return knots.empty() ? T(0) : knots.front();
    }

    /**
     * @brief Returns the last knot: the upper end of the domain.
     *
     * @return `knots.back()`, or 0 when no knots are set.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return knots.empty() ? T(0) : knots.back();
    }

    /**
     * @brief Returns false: this type describes the clamped, non-periodic form.
     *
     * @details
     * - A periodic B-spline is represented by a wrapped knot vector, which `isValid()` does not accept; the answer is
     *   therefore a fact about the type rather than a field that could disagree with the data.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: the clamped form is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the parameter of the same point when the curve is walked the other way.
     *
     * @details
     * - The reflection is inside the knot domain: `first + last - u`. A spline is not reparameterised by reversal.
     *
     * @param u Parameter in the curve's own units.
     * @return `firstParameter() + lastParameter() - u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return firstParameter() + lastParameter() - u;
    }

  public:
    /// Degree of the polynomial pieces.
    int degree{ 0 };

    /// Poles in order.
    PoleArray poles;

    /// Distinct knot values, non-decreasing.
    KnotArray knots;

    /// Multiplicity of each distinct knot, in the same order as `knots`.
    MultiplicityArray multiplicities;

    /// One weight per pole for a rational curve, empty for a polynomial one.
    WeightArray weights;
};

using BSplineCurvef = BSplineCurve<float>;
using BSplineCurved = BSplineCurve<double>;

VN_GEOMETRY_NS_END
