#pragma once

#include "math_global.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include "Continuity.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief Returns how many knots a distinct-knots + multiplicities pair expands to.
 *
 * @param multiplicities One multiplicity per distinct knot.
 * @return The length of the flattened knot vector.
 */
[[nodiscard]] inline std::size_t flatKnotCount(std::span<const int> multiplicities)
{
    std::size_t count = 0;
    for (const int multiplicity : multiplicities) {
        if (multiplicity > 0) {
            count += static_cast<std::size_t>(multiplicity);
        }
    }

    return count;
}

/**
 * @brief Returns the @p index-th knot of the flattened knot vector.
 *
 * @details
 * - The knots are stored as distinct values plus multiplicities, so a flat index is resolved by walking the
 *   multiplicities - a scan per lookup instead of a second copy of the same fact.
 * - An index past the end answers the last knot, so a malformed size cannot read out of bounds.
 *
 * @param knots          Distinct knot values, non-decreasing.
 * @param multiplicities One multiplicity per distinct knot.
 * @param index          Index into the flattened vector.
 * @return The knot value at @p index.
 */
template <vn::math::FP T>
[[nodiscard]] T knotAt(std::span<const T> knots, std::span<const int> multiplicities, std::size_t index)
{
    std::size_t offset = 0;
    for (std::size_t knot = 0; knot < knots.size() && knot < multiplicities.size(); ++knot) {
        if (multiplicities[knot] > 0) {
            offset += static_cast<std::size_t>(multiplicities[knot]);
        }
        if (index < offset) {
            return knots[knot];
        }
    }

    return knots.empty() ? T(0) : knots.back();
}

/**
 * @brief Returns the index of the knot span that contains @p u.
 *
 * @details
 * - The answer is the index `i` with `U[i] <= u < U[i + 1]` on the flattened vector, inside the range of spans that can
 *   carry a non-zero basis function (`degree <= i <= poles - 1`, where `poles` follows from the flattened length).
 * - A parameter at or past the last knot answers the last span, which is what makes the clamped end exact.
 *
 * @param knots          Distinct knot values.
 * @param multiplicities One multiplicity per distinct knot.
 * @param degree         Degree of the pieces.
 * @param u              Parameter already clamped into the knot domain.
 * @return The span index.
 */
template <vn::math::FP T>
[[nodiscard]] std::size_t knotSpan(std::span<const T> knots, std::span<const int> multiplicities, int degree, T u)
{
    const std::size_t first_span = static_cast<std::size_t>(degree < 0 ? 0 : degree);
    const std::size_t flat_count = flatKnotCount(multiplicities);
    // The flattened vector is `pole_count + degree + 1` long, so the last span that can carry a non-zero basis function
    // is `pole_count - 1`, and `flat_count - degree - 2` is that index written from the length.
    const std::size_t last_span
        = flat_count > static_cast<std::size_t>(degree) + 1 ? flat_count - static_cast<std::size_t>(degree) - 2 : first_span;

    if (u <= knotAt(knots, multiplicities, first_span)) {
        return first_span;
    }
    if (u >= knotAt(knots, multiplicities, last_span + 1)) {
        return last_span;
    }

    std::size_t low  = first_span;
    std::size_t high = last_span;
    std::size_t mid  = (low + high) / 2;
    while (u < knotAt(knots, multiplicities, mid) || u >= knotAt(knots, multiplicities, mid + 1)) {
        if (u < knotAt(knots, multiplicities, mid)) {
            high = mid;
        } else {
            low = mid;
        }
        mid = (low + high) / 2;
    }

    return mid;
}

/**
 * @brief Returns a derivative of one basis function at @p u.
 *
 * @details
 * - Degree zero is Cox-de Boor's recurrence; a higher order is the derivative recurrence
 *   `N'[i, p] = p / (U[i + p] - U[i]) * N[i, p - 1] - p / (U[i + p + 1] - U[i + 1]) * N[i + 1, p - 1]`, and a zero-length
 *   denominator drops its term, which keeps a repeated knot from dividing by zero.
 * - A parameter equal to the last knot belongs to the last non-empty span: the half-open convention
 *   `U[i] <= u < U[i + 1]` would otherwise make every basis function zero at the clamped end.
 * - The recursion allocates nothing; its cost grows with the number of derivative levels.
 *
 * @param knots            Distinct knot values.
 * @param multiplicities   One multiplicity per distinct knot.
 * @param index            Index of the basis function in the flattened vector.
 * @param degree           Degree of the basis function at this level of the recursion.
 * @param derivative_order Derivative to take; 0 is the basis function itself.
 * @param u                Parameter already clamped into the knot domain.
 * @return The value of `N[index, degree]` differentiated @p derivative_order times at @p u.
 */
template <vn::math::FP T>
[[nodiscard]] T basisValue(std::span<const T> knots, std::span<const int> multiplicities, std::size_t index, int degree,
                           int derivative_order, T u)
{
    if (degree < 0 || derivative_order < 0) {
        return T(0);
    }

    if (degree == 0) {
        if (derivative_order > 0) {
            return T(0);
        }

        const T lower = knotAt(knots, multiplicities, index);
        const T upper = knotAt(knots, multiplicities, index + 1);
        if (lower <= u && u < upper) {
            return T(1);
        }

        const T last      = knotAt(knots, multiplicities, flatKnotCount(multiplicities) - 1);
        const bool at_end = u == upper && lower < upper && upper == last;
        return at_end ? T(1) : T(0);
    }

    const std::size_t upper_index = index + static_cast<std::size_t>(degree) + 1;
    const T           lower_knot  = knotAt(knots, multiplicities, index);
    const T           upper_knot  = knotAt(knots, multiplicities, upper_index);
    const T           lower_span  = knotAt(knots, multiplicities, index + static_cast<std::size_t>(degree)) - lower_knot;
    const T           upper_span  = upper_knot - knotAt(knots, multiplicities, index + 1);

    T value = T(0);
    if (derivative_order == 0) {
        if (lower_span > vn::math::EPS<T>()) {
            value += (u - lower_knot) / lower_span * basisValue(knots, multiplicities, index, degree - 1, 0, u);
        }
        if (upper_span > vn::math::EPS<T>()) {
            value += (upper_knot - u) / upper_span * basisValue(knots, multiplicities, index + 1, degree - 1, 0, u);
        }
        return value;
    }

    if (lower_span > vn::math::EPS<T>()) {
        value += T(degree) / lower_span * basisValue(knots, multiplicities, index, degree - 1, derivative_order - 1, u);
    }
    if (upper_span > vn::math::EPS<T>()) {
        value -= T(degree) / upper_span
               * basisValue(knots, multiplicities, index + 1, degree - 1, derivative_order - 1, u);
    }

    return value;
}

/**
 * @brief Returns whether a knots/multiplicities pair describes the clamped form for a degree and a pole count.
 *
 * @details
 * - The clamped form is what `BSplineCurve` / `BSplineSurface` describe: both ends repeat the knot `degree + 1` times,
 *   so the curve or patch reaches its end control points and its domain is `[first knot, last knot]`.
 * - The flattened length must be `pole_count + degree + 1`; a pair whose counts disagree cannot be evaluated.
 * - An internal multiplicity is at most the degree, never `degree + 1`: a join with `degree + 1` copies of a knot
 *   leaves the two sides independent, so the parameterisation jumps instead of meeting. That is two pieces, not one
 *   clampable curve or patch, and accepting it would make `continuity()` claim `C0` about a gap.
 *
 * @param knots          Distinct knot values, which must strictly increase.
 * @param multiplicities One multiplicity per distinct knot: `degree + 1` at both ends, `1` to `degree` inside.
 * @param degree         Degree of the pieces; at least 1.
 * @param pole_count     Number of control points in this direction.
 * @return True when every count agrees and both ends are clamped.
 */
template <vn::math::FP T>
[[nodiscard]] bool isClampedKnotVector(std::span<const T> knots, std::span<const int> multiplicities, int degree,
                                       std::size_t pole_count)
{
    if (degree < 1 || pole_count < 2 || knots.empty() || knots.size() != multiplicities.size()) {
        return false;
    }

    if (multiplicities.front() != degree + 1 || multiplicities.back() != degree + 1) {
        return false;
    }

    for (std::size_t index = 1; index < knots.size(); ++index) {
        if (!(knots[index] > knots[index - 1])) {
            return false;
        }
    }

    std::size_t flat_count = 0;
    for (std::size_t index = 0; index < multiplicities.size(); ++index) {
        const int  multiplicity = multiplicities[index];
        const bool is_end       = index == 0 || index + 1 == multiplicities.size();
        const int  limit        = is_end ? degree + 1 : degree;
        if (multiplicity < 1 || multiplicity > limit) {
            return false;
        }
        flat_count += static_cast<std::size_t>(multiplicity);
    }

    return flat_count == pole_count + static_cast<std::size_t>(degree) + 1;
}

/**
 * @brief Returns the smoothness a degree and its distinct-knot multiplicities allow.
 *
 * @details
 * - Every join inside the piecewise polynomial loses one order of continuity per extra multiplicity: a join at an
 *   internal knot of multiplicity `m` is `C(degree - m)`, so the whole piece is as smooth as its worst join and the
 *   answer is set by the **largest internal multiplicity**.
 * - The first and the last multiplicity are the clamp, not joins, so they are not looked at: fewer than three distinct
 *   knots means one span and the answer is `CN`.
 * - `isClampedKnotVector()` keeps an internal multiplicity at or below the degree, so `degree - m` is never negative
 *   and `C0` really does mean "the pieces meet at a point" (the alternative, a gap, is not a curve - see
 *   `ADR-2026-11-04-bspline-without-knot-breaks.md`).
 * - The ladder tops out at `C3`: `Continuity` has no `C4`, so a high degree with simple internal knots reports `C3`
 *   rather than a rung that does not exist.
 * - This is a knot-vector fact, not a curve or surface one, which is why both `BSplineCurve` and `BSplineSurface`
 *   call it instead of each carrying its own copy of the ladder.
 *
 * @param degree         Degree of the pieces.
 * @param multiplicities One multiplicity per distinct knot, in increasing knot order.
 * @return `CN` without an internal knot, otherwise `C(degree - largest internal multiplicity)` capped at `C3`.
 */
[[nodiscard]] inline Continuity knotContinuity(int degree, std::span<const int> multiplicities)
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

VN_MATH_NS_END
