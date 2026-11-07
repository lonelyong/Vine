#pragma once

#include <vine/geom/geom_global.hpp>

#include <optional>
#include <type_traits>

#include <vine/geom/detail/CurveMeasure.hpp>
#include <vine/geom/detail/CurveTraits.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include "Curve.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief Returns the length of the curve's trace between two parameters, by adaptive quadrature of its speed.
 *
 * @details
 * - The length is the integral of `|C'(u)|` over the window, so it is the distance travelled along the curve, not the
 *   distance between the two end points: a curve that doubles back counts the return trip.
 * - The quadrature is adaptive Simpson: each interval is halved while the two half estimates disagree by more than
 *   `15 * tolerance`, which is Richardson's error estimate for Simpson. `max_depth` caps the halving so a cusp (where
 *   `|C'|` has a kink) cannot recurse without bound. The recursion itself is `vn::geom::detail::adaptiveSimpson()`, the
 *   same kernel the planar module uses; this function only supplies the speed.
 * - Only `derivative()` and `isValid()` are used, so any `Curve<T>` works; the curve's parameterisation is irrelevant
 *   because the speed already carries it.
 * - The window follows the family rule (`geometry-curve-queries.md` C1): an unbounded curve (a line, a hyperbola, a
 *   parabola) must be narrowed by the caller, and a missing, reversed or zero-width window answers `std::nullopt`
 *   instead of an invented range.
 *
 * @param curve      The curve to measure; must satisfy `isValid()`.
 * @param window     Parameter window to measure over, or nothing to use the curve's own domain.
 * @param tolerance  Absolute length error to aim for; a non-positive value falls back to `EPS<T>()`.
 * @param max_depth  Halving cap per interval; negative values are treated as 0 (one Simpson estimate).
 * @return The length over the window, or `std::nullopt` when the curve is invalid or there is no finite forward
 *         window, or when the integral is not finite.
 *
 * @warning
 * - The tolerance is on the length, not on the parameter: shrinking it refines the quadrature, it does not move the
 *   window. A curve whose speed is unbounded inside the window (a hyperbola at an infinite parameter) has no finite
 *   answer and is refused rather than returning an infinity.
 *
 * @note
 * - A circle of radius `r` over an angle `span` answers `|r| * span`; [`parameterAtLength()`] answers the inverse
 *   question. The planar module carries the same pair of functions.
 *
 * @tparam C Concrete curve type; must derive from `Curve<C::value_type>`.
 */
template <typename C>
[[nodiscard]] std::optional<geom::detail::CurveScalar<C>> arcLength(
    const C& curve, geom::detail::CurveWindow<C> window = std::nullopt,
    geom::detail::CurveScalar<C> tolerance = vn::math::EPS<geom::detail::CurveScalar<C>>(), int max_depth = 20)
    requires std::is_base_of_v<Curve<geom::detail::CurveScalar<C>>, C>
{
    using T = geom::detail::CurveScalar<C>;

    if (!curve.isValid()) {
        return std::nullopt;
    }

    const auto range = geom::detail::resolveCurveWindow<T>(curve, window);
    if (!range) {
        return std::nullopt;
    }

    return geom::detail::arcLengthBetween<T>(curve, range->first, range->second,
                                             geom::detail::resolveTolerance(tolerance), max_depth);
}

/**
 * @brief Returns the parameter at which the trace has covered a given length from the window's start.
 *
 * @details
 * - This is the inverse of [`arcLength()`]: for a valid window `[a, b]` the answer `u` satisfies
 *   `arcLength(curve, {a, u}) == arc_length` within @p tolerance.
 * - The search is a safeguarded Newton iteration: each step jumps by `(arc_length - reached) / |C'(u)|` and falls back
 *   to bisection when that jump leaves the bracket, so a flat or fast section cannot stall it. The iteration itself is
 *   `vn::geom::detail::parameterReaching()`, shared with the planar module; this function only supplies the travelled
 *   length and the speed.
 * - Length is measured from the window's first parameter, not from the curve's own first parameter, so the same
 *   arc length on two different windows names two different points.
 * - The window follows the family rule, exactly as [`arcLength()`] states.
 *
 * @param curve       The curve to walk; must satisfy `isValid()`.
 * @param arc_length  Length to cover from the window's start; must be non-negative and no longer than the window.
 * @param window      Parameter window to walk, or nothing to use the curve's own domain.
 * @param tolerance   Absolute length error to aim for; a non-positive value falls back to `EPS<T>()`.
 * @param max_depth   Halving cap passed to every `arcLength()` evaluation.
 * @return The parameter covering @p arc_length, or `std::nullopt` when the curve is invalid, there is no finite
 *         forward window, @p arc_length is negative, or it exceeds the window's length by more than @p tolerance.
 *
 * @warning
 * - Each iteration integrates the speed again, so the cost is the number of iterations times the cost of one
 *   `arcLength()`. The parameter tolerance is therefore not a speed guarantee; it is a correctness trade.
 *
 * @tparam C Concrete curve type; must derive from `Curve<C::value_type>`.
 */
template <typename C>
[[nodiscard]] std::optional<geom::detail::CurveScalar<C>> parameterAtLength(
    const C& curve, geom::detail::CurveScalar<C> arc_length, geom::detail::CurveWindow<C> window = std::nullopt,
    geom::detail::CurveScalar<C> tolerance = vn::math::EPS<geom::detail::CurveScalar<C>>(), int max_depth = 20)
    requires std::is_base_of_v<Curve<geom::detail::CurveScalar<C>>, C>
{
    using T = geom::detail::CurveScalar<C>;

    if (!curve.isValid()) {
        return std::nullopt;
    }

    const auto range = geom::detail::resolveCurveWindow<T>(curve, window);
    if (!range) {
        return std::nullopt;
    }

    return geom::detail::parameterCovering<T>(curve, arc_length, range->first, range->second,
                                              geom::detail::resolveTolerance(tolerance), max_depth);
}

VN_GEOM_SPATIAL_NS_END
