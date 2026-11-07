#pragma once

#include <vine/geom/geom_global.hpp>

#include <optional>
#include <utility>

#include <vine/math/Math.hpp>

#include "CurveTraits.hpp"
#include "Quadrature.hpp"
#include "RootFinding.hpp"

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief Integrates a curve's speed over `[first, last]`, which is the length of the trace between them.
 *
 * @details
 * - The length is the integral of `|C'(u)|`, so it is the distance travelled along the curve, not the distance between
 *   the two end points: a curve that doubles back counts the return trip.
 * - The quadrature is `vn::geom::detail::adaptiveSimpson()`; this function supplies the speed and nothing else. Only
 *   `derivative()` is used, so any curve works and the parameterisation is irrelevant - the speed already carries it.
 * - It takes resolved ends, not a window: whether a window is acceptable is the caller's rule (an unbounded curve must
 *   be narrowed), and keeping the two apart means this function has no policy to get wrong.
 *
 * @param curve      The curve to measure.
 * @param first      Lower parameter of the interval.
 * @param last       Upper parameter of the interval; must not precede @p first.
 * @param tolerance  Absolute length error to aim for, already resolved by the caller.
 * @param max_depth  Halving cap per interval of the quadrature.
 * @return The length, or `std::nullopt` when it is not finite or the interval is reversed.
 *
 * @tparam T Floating-point scalar.
 * @tparam C Concrete curve type.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] std::optional<T> arcLengthBetween(const C& curve, T first, T last, T tolerance, int max_depth)
{
    const auto speed = [&](T u) { return curve.derivative(u).length(); };
    return adaptiveSimpson<T>(speed, first, last, tolerance, max_depth);
}

/**
 * @brief Returns the parameter at which the trace has covered a given length from @p first.
 *
 * @details
 * - This is the inverse of [`arcLengthBetween()`]: the answer `u` satisfies
 *   `arcLengthBetween(curve, first, u, ...) == arc_length` within @p tolerance.
 * - The search is a safeguarded Newton iteration (`vn::geom::detail::parameterReaching()`): each step jumps by
 *   `(arc_length - reached) / |C'(u)|` and falls back to bisection when that jump leaves the bracket, so a flat or fast
 *   section cannot stall it.
 * - Length is measured from @p first, not from the curve's own first parameter, so the same arc length on two windows
 *   names two different points. A length past the window's end is refused instead of extrapolated.
 *
 * @param curve       The curve to walk.
 * @param arc_length  Length to cover from @p first; must be non-negative and no longer than the interval.
 * @param first       Parameter the length is measured from.
 * @param last        Upper parameter of the interval; must not precede @p first.
 * @param tolerance   Absolute length error to aim for, already resolved by the caller.
 * @param max_depth   Halving cap passed to every length evaluation.
 * @return The parameter covering @p arc_length, or `std::nullopt` when @p arc_length is negative, or the interval has
 *         no finite length, or @p arc_length exceeds that length by more than @p tolerance.
 *
 * @warning
 * - Each iteration integrates the speed again, so the cost is the iteration count times the cost of one length
 *   evaluation. The parameter tolerance is therefore not a speed guarantee; it is a correctness trade.
 *
 * @tparam T Floating-point scalar.
 * @tparam C Concrete curve type.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] std::optional<T> parameterCovering(const C& curve, T arc_length, T first, T last, T tolerance,
                                                 int max_depth)
{
    if (arc_length < T(0)) {
        return std::nullopt;
    }

    if (!(arc_length > T(0))) {
        return first;
    }

    const auto total = arcLengthBetween<T>(curve, first, last, tolerance, max_depth);
    if (!total || arc_length > *total + tolerance) {
        return std::nullopt;
    }

    const auto reached = [&](T u) { return arcLengthBetween<T>(curve, first, u, tolerance, max_depth); };
    const auto speed   = [&](T u) { return curve.derivative(u).length(); };
    return parameterReaching<T>(reached, speed, first, last, arc_length, tolerance);
}

VN_GEOM_DETAIL_NS_END
