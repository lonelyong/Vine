#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include <vine/geom/detail/CurvePairSearch.hpp>
#include <vine/geom/detail/CurveTraits.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"
#include "CurveProjection.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief The closest pair of points of two curves: both parameters, both points and the distance between them.
 *
 * @details
 * - One result answers the whole question: `distance == |first_point - second_point|`,
 *   `first_point == first.evaluate(first_parameter)` and `second_point == second.evaluate(second_parameter)`.
 * - Keeping both points means a caller that needs a contact pair (for a gap test or a bridge) does not evaluate the
 *   curves again, and `distance` cannot disagree with the points it was computed from.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
struct CurveCurveDistance {
    /// Closest point on the first curve.
    vn::math::Point3<T> first_point{};

    /// Closest point on the second curve.
    vn::math::Point3<T> second_point{};

    /// Parameter of @p first_point, in the first curve's own units.
    T first_parameter{ T(0) };

    /// Parameter of @p second_point, in the second curve's own units.
    T second_parameter{ T(0) };

    /// Distance between the two points; never negative.
    T distance{ T(0) };
};

/**
 * @brief Returns the closest pair of points of two curves inside their windows.
 *
 * @details
 * - The first curve is walked; at every sample its nearest point on the second curve comes from
 *   `closestPointOnCurve()`, which is exact for that sample, and the pair is then refined by a few Gauss-Newton steps
 *   on the two-parameter distance. The cheapest pair seen is the answer.
 * - Only `evaluate()` and `derivative()` are used, so any two `Curve<T>` implementations work; the curves may be of
 *   different concrete types but must share the scalar.
 * - The windows are the caller's; without one a bounded curve uses its own domain. **An unbounded curve must be
 *   narrowed by the caller** - the same rule the nearest-point query follows (`geometry-curve-queries.md` C1).
 * - `distance` is zero for curves that touch or cross when the scan or the refinement lands on the contact.
 *
 * @param first          First curve; must satisfy `isValid()`.
 * @param second         Second curve; must satisfy `isValid()`.
 * @param first_window   Parameter window on @p first, or nothing to use its own domain.
 * @param second_window  Parameter window on @p second, or nothing to use its own domain.
 * @param tolerance      Geometric resolution: the projection tolerance and the refinement stop distance.
 * @param sample_count   Number of scan intervals on @p first; more finds finer features and costs more.
 * @return The closest pair found, or `std::nullopt` when a curve is invalid or a window is missing, reversed or
 *         zero-width.
 *
 * @warning
 * - This is a local search: the answer is the best minimum the samples and the refinement found, not a guaranteed
 *   global one. Curves that approach each other in several places need a `sample_count` fine enough to land in each
 *   basin.
 * - At a locally parallel pair the Gauss-Newton normal matrix is singular, so the refinement keeps the sampled pair;
 *   the sampled projection is still exact for that sample, so the distance is right at the sample.
 *
 * @tparam C1 Concrete type of @p first; must derive from `Curve<C1::value_type>`.
 * @tparam C2 Concrete type of @p second; must use the same scalar.
 */
// clang-format off
// The declaration starts on the line carrying the opening parenthesis so the documentation gate can attach this block
// to it; LLVM style would move the name onto its own line, which the gate reads as an undocumented declaration.
template <typename C1, typename C2>
[[nodiscard]] std::optional<CurveCurveDistance<geom::detail::CurveScalar<C1>>> curveToCurveDistance(
    const C1& first, const C2& second, geom::detail::CurveWindow<C1> first_window = std::nullopt,
    geom::detail::CurveWindow<C2> second_window = std::nullopt,
    geom::detail::CurveScalar<C1> tolerance = vn::math::EPS<geom::detail::CurveScalar<C1>>(),
    int sample_count = 64)
    requires std::is_same_v<geom::detail::CurveScalar<C1>, geom::detail::CurveScalar<C2>>
          && std::is_base_of_v<Curve<geom::detail::CurveScalar<C1>>, C1>
          && std::is_base_of_v<Curve<geom::detail::CurveScalar<C2>>, C2>
{
    // clang-format on
    using T = geom::detail::CurveScalar<C1>;

    if (!first.isValid() || !second.isValid()) {
        return std::nullopt;
    }

    const auto first_range  = geom::detail::resolveCurveWindow<T>(first, first_window);
    const auto second_range = geom::detail::resolveCurveWindow<T>(second, second_window);
    if (!first_range || !second_range) {
        return std::nullopt;
    }

    const T   on_curve = geom::detail::resolveTolerance(tolerance);
    const int steps    = std::clamp(sample_count, 8, 512);
    const T   step     = (first_range->second - first_range->first) / static_cast<T>(steps);

    CurveCurveDistance<T> best;
    best.distance         = std::numeric_limits<T>::infinity();
    best.first_parameter  = first_range->first;
    best.second_parameter = second_range->first;

    const auto consider = [&](T u, T v) {
        const auto first_point  = first.evaluate(u);
        const auto second_point = second.evaluate(v);
        const T    distance     = (first_point - second_point).length();
        if (distance < best.distance) {
            best = CurveCurveDistance<T>{ first_point, second_point, u, v, distance };
        }
    };

    for (int index = 0; index <= steps; ++index) {
        const T u = (index == steps) ? first_range->second : first_range->first + step * static_cast<T>(index);

        const auto projection = closestPointOnCurve(second, first.evaluate(u), second_window, on_curve, steps);
        if (!projection) {
            continue;
        }

        consider(u, projection->parameter);
        const auto refined = geom::detail::refineCurvePair(first, second, *first_range, *second_range, u,
                                                           projection->parameter, on_curve);
        consider(refined.first, refined.second);
    }

    if (!std::isfinite(best.distance)) {
        return std::nullopt;
    }

    return best;
}

VN_GEOM_SPATIAL_NS_END
