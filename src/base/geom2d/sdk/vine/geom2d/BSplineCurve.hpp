#pragma once

#include "geom2d_global.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/spline/BSplineBasis.hpp>

#include "Curve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A planar B-spline curve in clamped form: degree, poles, distinct knots and multiplicities.
 *
 * @details
 * - The knot vector stores **distinct values plus multiplicities**, because the multiplicity is the only thing a caller
 *   asks about (a join's smoothness) and an expanded vector would have to be regrouped on every query.
 * - `continuity()` is derived from the data, which is the reason a B-spline is worth having as a type: the answer is
 *   the worst internal join, `C(degree - largest internal multiplicity)`, not a constant of the type.
 * - The numeric answers are the basis functions of the span that contains the parameter: `evaluate()` sums
 *   `N[i, p](u) * P[i]`, the two derivatives sum `N[i, p]'` and `N[i, p]''` the same way, and a rational curve applies
 *   the quotient rule to the homogeneous sums. `isClosed()` is a geometric fact - the two end points coincide - so it
 *   is evaluated rather than read from the data, unlike `BezierCurve::isClosed()` whose ends are its end poles.
 * - A parameter outside the knot domain is clamped into it: a clamped spline has no polynomial span outside, so the
 *   curve continues as its end point instead of extrapolating a piece that does not exist.
 * - This is a memory-owning curve: `std::vector` poles, knots and weights mean deep copies (R2's stated exception).
 *
 * @warning
 * - The fields are public and nothing re-checks them on write, so `isValid()` is the only guarantee that a curve built
 *   field by field can be evaluated at all.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BSplineCurve : public Curve<T> {
  public:
    using value_type        = T;
    using Point             = vn::math::Point2<T>;
    using Vector            = vn::math::Vector2<T>;
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
     * @param the_multiplicities One multiplicity per distinct knot: `degree + 1` at both ends, `1` to `degree` inside.
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
     * - The domain of this type is `[knots.front(), knots.back()]`, which is the curve's own domain only when both ends
     *   are clamped, so the two end multiplicities must equal `degree + 1`.
     * - Distinct knots must increase and weights must be strictly positive, for the same reason `BezierCurve` asks it:
     *   a zero or negative weight makes the rational denominator vanish inside the domain.
     *
     * @return True when every count agrees, both ends are clamped and every internal multiplicity is at most the
     *         degree.
     */
    [[nodiscard]] bool isValid() const override
    {
        if (!math::spline::isClampedKnotVector(std::span<const T>(knots), std::span<const int>(multiplicities), degree,
                                               poles.size())) {
            return false;
        }

        if (weights.empty()) {
            return true;
        }

        if (weights.size() != poles.size()) {
            return false;
        }

        for (const T weight : weights) {
            if (!(weight > vn::math::EPS<T>())) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Returns the smoothness at the worst join, derived from the internal multiplicities.
     *
     * @details
     * - The answer is the knot vector's own fact, so it comes from `math::spline::knotContinuity()`: the worst internal
     *   join is `C(degree - largest internal multiplicity)`, one polynomial span is `CN`, and the ladder tops out at
     *   `C3`. The end multiplicities are the clamp and are not joins.
     *
     * @return The continuity at the least smooth join.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return math::spline::knotContinuity(degree, std::span<const int>(multiplicities));
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
     * @return always false.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: the clamped form is not periodic.
     *
     * @return 0.
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
     * - It is an involution and swaps the two ends of the domain.
     *
     * @param u Parameter in the curve's own units.
     * @return `firstParameter() + lastParameter() - u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return firstParameter() + lastParameter() - u;
    }

  private:
    /**
     * @brief Returns @p u clamped into `[firstParameter(), lastParameter()]`.
     *
     * @param u Parameter as the caller gave it.
     * @return The parameter inside the knot domain.
     */
    [[nodiscard]] T clampedParameter(T u) const
    {
        const T first = firstParameter();
        const T last  = lastParameter();
        return (u < first) ? first : ((u > last) ? last : u);
    }

    /**
     * @brief Sums `N[i]^(k)(u) * P[i]` and `N[i]^(k)(u) * w[i]` over the poles that influence @p span.
     *
     * @details
     * - Only the `degree + 1` poles of the span take part, because every other basis function is zero there.
     * - The weight sum belongs to the rational form; for a polynomial curve it is the sum of the basis derivatives,
     *   which is 1 for order 0 and 0 above, and the caller ignores it.
     *
     * @param span             Knot span containing @p parameter, from `knotSpan()`.
     * @param parameter        Parameter already clamped into the knot domain.
     * @param derivative_order Derivative to take of the basis functions.
     * @param numerator        Receives the (weighted) sum of the poles.
     * @param weight           Receives the sum of the (weighted) basis functions.
     */
    void accumulate(std::size_t span, T parameter, int derivative_order, Vector& numerator, T& weight) const
    {
        numerator = Vector::zero();
        weight    = T(0);

        const std::size_t first = span - static_cast<std::size_t>(degree);
        for (std::size_t step = 0; step <= static_cast<std::size_t>(degree); ++step) {
            const std::size_t index = first + step;
            const T basis = math::spline::basisValue<T>(knots, multiplicities, index, degree, derivative_order, parameter);
            const T factor = isRational() ? basis * weights[index] : basis;
            numerator += poles[index].toVector() * factor;
            weight += factor;
        }
    }

  public:
    /**
     * @brief Returns the point at parameter @p u by summing the basis functions of its knot span.
     *
     * @details
     * - @p u is clamped into the knot domain first: outside it a clamped spline has no polynomial span, so the curve
     *   continues as its end point instead of extrapolating a piece that does not exist.
     * - A rational curve divides the weighted sum of its poles by the sum of the weights; both come from one pass.
     *
     * @param u Parameter in the knot domain; clamped to it when outside.
     * @return The point on the curve; the first pole for a curve `isValid()` refuses.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        if (!isValid()) {
            return poles.empty() ? Point() : poles.front();
        }

        const T           parameter = clampedParameter(u);
        const std::size_t span      = math::spline::knotSpan<T>(knots, multiplicities, degree, parameter);

        Vector numerator = Vector::zero();
        T      weight_sum = T(0);
        accumulate(span, parameter, 0, numerator, weight_sum);

        if (!isRational() || std::abs(weight_sum) <= vn::math::EPS<T>()) {
            return Point(numerator.x, numerator.y);
        }

        return Point(numerator.x / weight_sum, numerator.y / weight_sum);
    }

    /**
     * @brief Returns the first derivative at @p u.
     *
     * @details
     * - The polynomial form differentiates the basis functions and keeps the poles: `P' = sum N[i, p]'(u) * P[i]`.
     * - The rational form applies the quotient rule to the homogeneous sums: `P' = (A' - P * w') / w`.
     *
     * @param u Parameter in the knot domain; clamped to it when outside.
     * @return The velocity vector at @p u; the zero vector for a curve `isValid()` refuses.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        if (!isValid()) {
            return Vector::zero();
        }

        const T           parameter = clampedParameter(u);
        const std::size_t span      = math::spline::knotSpan<T>(knots, multiplicities, degree, parameter);

        Vector velocity      = Vector::zero();
        T      weight_velocity = T(0);
        accumulate(span, parameter, 1, velocity, weight_velocity);

        if (!isRational()) {
            return velocity;
        }

        Vector value = Vector::zero();
        T      weight = T(0);
        accumulate(span, parameter, 0, value, weight);
        if (std::abs(weight) <= vn::math::EPS<T>()) {
            return Vector::zero();
        }

        return (velocity - (value / weight) * weight_velocity) / weight;
    }

    /**
     * @brief Returns the second derivative at @p u.
     *
     * @details
     * - The polynomial form sums the second derivative of the basis functions against the poles.
     * - The rational form answers `P'' = (A'' - 2 * P' * w' - P * w'') / w` on the homogeneous sums.
     *
     * @param u Parameter in the knot domain; clamped to it when outside.
     * @return The acceleration vector at @p u; the zero vector for a curve `isValid()` refuses.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        if (!isValid()) {
            return Vector::zero();
        }

        const T           parameter = clampedParameter(u);
        const std::size_t span      = math::spline::knotSpan<T>(knots, multiplicities, degree, parameter);

        Vector acceleration        = Vector::zero();
        T      weight_acceleration = T(0);
        accumulate(span, parameter, 2, acceleration, weight_acceleration);

        if (!isRational()) {
            return acceleration;
        }

        Vector value  = Vector::zero();
        T      weight = T(0);
        accumulate(span, parameter, 0, value, weight);
        Vector velocity        = Vector::zero();
        T      weight_velocity = T(0);
        accumulate(span, parameter, 1, velocity, weight_velocity);
        if (std::abs(weight) <= vn::math::EPS<T>()) {
            return Vector::zero();
        }

        const Vector point      = value / weight;
        const Vector point_rate = (velocity - point * weight_velocity) / weight;
        return (acceleration - point_rate * (T(2) * weight_velocity) - point * weight_acceleration) / weight;
    }

    /**
     * @brief Returns the third derivative at @p u.
     *
     * @details
     * - The polynomial form sums the third derivative of the basis functions against the poles.
     * - The rational form answers `P''' = (A''' - 3 * P'' * w' - 3 * P' * w'' - P * w''') / w` on the homogeneous sums,
     *   which is Leibniz' rule applied to `A / w`.
     *
     * @param u Parameter in the knot domain; clamped to it when outside.
     * @return The jerk vector at @p u; the zero vector for a curve `isValid()` refuses.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        if (!isValid()) {
            return Vector::zero();
        }

        const T           parameter = clampedParameter(u);
        const std::size_t span      = math::spline::knotSpan<T>(knots, multiplicities, degree, parameter);

        Vector jerk        = Vector::zero();
        T      weight_jerk = T(0);
        accumulate(span, parameter, 3, jerk, weight_jerk);

        if (!isRational()) {
            return jerk;
        }

        Vector value             = Vector::zero();
        T      weight            = T(0);
        accumulate(span, parameter, 0, value, weight);
        Vector velocity          = Vector::zero();
        T      weight_velocity   = T(0);
        accumulate(span, parameter, 1, velocity, weight_velocity);
        Vector acceleration      = Vector::zero();
        T      weight_acceleration = T(0);
        accumulate(span, parameter, 2, acceleration, weight_acceleration);
        if (std::abs(weight) <= vn::math::EPS<T>()) {
            return Vector::zero();
        }

        const Vector point  = value / weight;
        const Vector first  = (velocity - point * weight_velocity) / weight;
        const Vector second = (acceleration - first * (T(2) * weight_velocity) - point * weight_acceleration) / weight;
        return (jerk - second * (T(3) * weight_velocity) - first * (T(3) * weight_acceleration) - point * weight_jerk)
             / weight;
    }

    /**
     * @brief Returns whether the two end points of the clamped domain coincide.
     *
     * @details
     * - Unlike `BezierCurve`, whose ends are its end poles, a spline ends where its end poles place it only after the
     *   basis functions have been applied, so closure is evaluated here rather than read from the data.
     *
     * @return True when the first and the last point are closer than the floating-point epsilon.
     */
    [[nodiscard]] bool isClosed() const override
    {
        if (!isValid()) {
            return false;
        }

        return (evaluate(firstParameter()) - evaluate(lastParameter())).length() <= vn::math::EPS<T>();
    }

    /**
     * @brief Returns the axis-aligned box around the poles.
     *
     * @details
     * - A B-spline curve lies inside the convex hull of its poles, so the pole box contains the curve; it is generally
     *   looser than the curve's own box, and tightening it needs the roots of each coordinate's derivative.
     *
     * @return Bounds of the poles; the degenerate box at the origin when the curve has none.
     */
    [[nodiscard]] vn::math::Rect2<T> bounds() const
    {
        if (poles.empty()) {
            return vn::math::Rect2<T>();
        }

        T xmin = poles.front().x;
        T xmax = poles.front().x;
        T ymin = poles.front().y;
        T ymax = poles.front().y;
        for (const Point& pole : poles) {
            xmin = std::min(xmin, pole.x);
            xmax = std::max(xmax, pole.x);
            ymin = std::min(ymin, pole.y);
            ymax = std::max(ymax, pole.y);
        }

        return vn::math::Rect2<T>(Point(xmin, ymin), Point(xmax, ymax));
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

VN_GEOM2D_NS_END
