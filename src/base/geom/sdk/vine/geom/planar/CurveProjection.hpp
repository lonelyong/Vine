#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>

#include <vine/geom/detail/CrossingSearch.hpp>
#include <vine/geom/detail/CurveTraits.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief The nearest point of a planar curve to a query point, with the parameter that produces it.
 *
 * @details
 * - `point` is `curve.evaluate(parameter)`; it is kept here so a caller does not evaluate the curve a second time.
 * - `distance` is `|point - query|`, so this one result answers both "where is the nearest point" and "how far is the
 *   point from the curve" - there is no second entry that recomputes the same minimum.
 * - `parameter` is the curve's own parameter: a multiple of the direction on a line, an angle on a conic or an arc.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct CurveProjection {
    /// The nearest point on the curve.
    vn::math::Point2<T> point{};

    /// Parameter of @p point, in the curve's own units.
    T parameter{ T(0) };

    /// Distance from the query point to @p point; never negative.
    T distance{ T(0) };
};

/**
 * @brief Returns the point of a planar curve nearest to @p query, and its distance.
 *
 * @details
 * - A nearest point is a stationary point of the squared distance, i.e. a zero of `f(u) = (C(u) - query) . C'(u)`.
 *   The window is sampled, every sign change of `f` is narrowed with `vn::geom::detail::rootInBracket()`, and the
 *   cheapest of
 *   those roots, of the samples and of the two window ends is the answer. Endpoints are candidates because a foot
 *   outside the window lands on the boundary, where `f` has no root.
 * - Only `evaluate()` and `derivative()` are used, so any `Curve<T>` works - an analytic conic or an arc alike.
 * - The window is the caller's; without one a bounded curve uses its own domain. **An unbounded curve (a line, a
 *   hyperbola, a parabola) must be narrowed by the caller** - the function samples a finite interval and never
 *   invents one, the same rule the space curves follow (`geometry-curve-queries.md` C1).
 * - A curve with several local minima answers the best one the samples found; raise `sample_count` when the curve
 *   has features finer than the default grid.
 *
 * @param curve        The curve to project onto; must satisfy `isValid()`.
 * @param query        The point to measure from.
 * @param window       Parameter window to search, or nothing to use the curve's own domain.
 * @param tolerance    Geometric resolution, converted into a parameter tolerance per root because a parameter moves
 *                     the curve point by roughly `|dP/du|`.
 * @param sample_count Number of scan intervals; more finds finer features and costs more evaluations.
 * @return The nearest point found and its parameter, or `std::nullopt` when there is no finite window to search.
 *
 * @warning
 * - This is a numerical minimisation: the answer is the best **local** minimum the samples found, not a guaranteed
 *   global one.
 * - A minimum where `f` touches zero without changing sign (a flat, tangential minimum) has no bracket for the root
 *   find; it is only found if a sample lands on it.
 *
 * @note
 * - A point equidistant from a whole stretch of the curve (the centre of a circle) has no single nearest point: one
 *   of them is reported, and which one depends on where the scan and the root find land.
 *
 * @tparam T Floating-point scalar, deduced from @p query.
 * @tparam C Concrete curve type; must derive from `Curve<T>`.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] std::optional<CurveProjection<T>> closestPointOnCurve(
    const C& curve, const vn::math::Point2<T>& query, std::optional<std::pair<T, T>> window = std::nullopt,
    T tolerance = vn::math::EPS<T>(), int sample_count = 32)
    requires std::is_base_of_v<Curve<T>, C>
{
    if (!curve.isValid()) {
        return std::nullopt;
    }

    const auto range = geom::detail::resolveCurveWindow<T>(curve, window);
    if (!range) {
        return std::nullopt;
    }

    const T   first           = range->first;
    const T   last            = range->second;
    const T   value_tolerance = geom::detail::resolveTolerance(tolerance);
    const int samples         = (sample_count < 4) ? 4 : ((sample_count > 4096) ? 4096 : sample_count);
    const T   step            = (last - first) / static_cast<T>(samples);

    // The derivative of the squared distance is all the root find needs; the samples keep their point so a candidate
    // that is not chosen costs no extra evaluation.
    const auto value_at = [&](T parameter) {
        return (curve.evaluate(parameter) - query).dot(curve.derivative(parameter));
    };

    const auto         first_point = curve.evaluate(first);
    CurveProjection<T> best{ first_point, first, (first_point - query).length() };
    const auto         consider = [&](T parameter, const vn::math::Point2<T>& point) {
        const T distance = (point - query).length();
        if (distance < best.distance) {
            best = CurveProjection<T>{ point, parameter, distance };
        }
    };

    T previous_parameter = first;
    T previous_value     = value_at(first);
    for (int index = 1; index <= samples; ++index) {
        const T    parameter = (index == samples) ? last : first + step * static_cast<T>(index);
        const auto point     = curve.evaluate(parameter);
        const T    value     = (point - query).dot(curve.derivative(parameter));

        const bool crossed = (previous_value < T(0) && value > T(0)) || (previous_value > T(0) && value < T(0));
        if (crossed) {
            const auto root = geom::detail::narrowCrossing(curve, value_at, previous_parameter, parameter,
                                                           value_tolerance, step * T(1e-6));
            if (root) {
                consider(*root, curve.evaluate(*root));
            }
        }

        consider(parameter, point);
        previous_parameter = parameter;
        previous_value     = value;
    }

    return best;
}

VN_GEOM_PLANAR_NS_END
