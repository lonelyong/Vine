#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include "CurveMeasure.hpp"
#include "CurveTraits.hpp"

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief Fills `segments + 1` points spaced evenly in the parameter, which is not evenly spaced in arc length.
 *
 * @details
 * - The parameters are `first + k * (last - first) / segments`; the last one is written as @p last exactly, so the result
 *   ends on the window's end instead of on a rounded copy of it.
 * - Only `evaluate()` is used, so the rule works for any curve whose parameterisation is meaningful to the caller, and it
 *   costs one evaluation per point.
 * - The two output arrays are cleared first, so a caller may hand in a buffer it reuses.
 *
 * @param curve      The curve to sample; must be usable.
 * @param first      Lower parameter of the window.
 * @param last       Upper parameter of the window; must not precede @p first.
 * @param segments   Number of chords to produce; must be at least 1.
 * @param parameters Output: one parameter per point, strictly increasing.
 * @param points     Output: the point at each parameter, in the same order.
 * @return True when the arrays were filled; false for a degenerate request, with both arrays left empty.
 *
 * @tparam T Floating-point scalar.
 * @tparam C Concrete curve type; only `evaluate()` and `Point` are used.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] bool sampleByParameter(const C& curve, T first, T last, int segments, std::vector<T>& parameters,
                                     std::vector<typename C::Point>& points)
{
    parameters.clear();
    points.clear();

    if (segments < 1 || !(last > first)) {
        return false;
    }

    const auto count = static_cast<std::size_t>(segments) + 1;
    parameters.reserve(count);
    points.reserve(count);

    const T span    = last - first;
    const T divisor = static_cast<T>(segments);
    for (int index = 0; index <= segments; ++index) {
        const T u = (index == segments) ? last : first + span * (static_cast<T>(index) / divisor);
        parameters.push_back(u);
        points.push_back(curve.evaluate(u));
    }

    return true;
}

/**
 * @brief Fills points whose consecutive arc lengths are @p step, with the last chord shortened to the window's end.
 *
 * @details
 * - The window's length is measured once; each following point is the answer of "walk @p step of length from the previous
 *   parameter", solved by `parameterReaching()` over an interval measured on the fly, so no chord is derived by scaling a
 *   parameter step.
 * - Length is measured from the previous point, not from the window's start: that keeps every evaluation short instead of
 *   re-integrating a growing interval per point (the O(n^2) shape).
 * - The point budget is checked before any of that work: the length alone says how many points to expect.
 * - When the remaining length is at most `step + tolerance`, the last point is the window's end, so the final chord is
 *   the remainder rather than a point placed past the end.
 *
 * @param curve      The curve to sample; must be usable.
 * @param first      Lower parameter of the window.
 * @param last       Upper parameter of the window; must not precede @p first.
 * @param step       Arc length between two consecutive points; must be positive and finite.
 * @param tolerance  Absolute length error to aim for; a non-positive value falls back to `EPS<T>()`.
 * @param max_points Maximum number of points to produce; must be at least 2.
 * @param parameters Output: one parameter per point, strictly increasing.
 * @param points     Output: the point at each parameter, in the same order.
 * @return True when the arrays were filled; false for a degenerate request or an exhausted budget, with both arrays left
 *         empty.
 *
 * @warning
 * - The cost is the point count times one local inverse solve, and each solve integrates the speed several times. A very
 *   small @p step on a long curve is therefore expensive by construction, not by accident.
 *
 * @tparam T Floating-point scalar.
 * @tparam C Concrete curve type; `evaluate()`, `derivative()` and `Point` are used.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] bool sampleByLength(const C& curve, T first, T last, T step, T tolerance, int max_points,
                                  std::vector<T>& parameters, std::vector<typename C::Point>& points)
{
    parameters.clear();
    points.clear();

    if (!(step > T(0)) || !std::isfinite(static_cast<double>(step)) || max_points < 2 || !(last > first)) {
        return false;
    }

    const T    tol   = resolveTolerance(tolerance);
    const auto total = arcLengthBetween<T>(curve, first, last, tol, 20);
    if (!total || !std::isfinite(static_cast<double>(*total))) {
        return false;
    }

    // Points to expect: one per whole step, plus the remainder's endpoint, plus the window's start.
    const auto whole = static_cast<long long>(std::floor(static_cast<double>(*total / step)));
    if (whole + 2 > static_cast<long long>(max_points)) {
        return false;
    }

    parameters.reserve(static_cast<std::size_t>(whole) + 2);
    points.reserve(static_cast<std::size_t>(whole) + 2);

    parameters.push_back(first);
    points.push_back(curve.evaluate(first));

    const auto speed = [&](T u) { return curve.derivative(u).length(); };

    T    cursor  = first;
    T    covered = T(0);
    while (true) {
        if (*total - covered <= step + tol) {
            break; // the rest of the window is the final, shorter chord
        }

        const T    from    = cursor;
        const auto reached = [&](T candidate) { return arcLengthBetween<T>(curve, from, candidate, tol, 20); };

        const auto next = parameterReaching<T>(reached, speed, from, last, step, tol);
        if (!next || !(*next > from)) {
            parameters.clear();
            points.clear();
            return false;
        }

        if (parameters.size() + 1 >= static_cast<std::size_t>(max_points)) {
            parameters.clear();
            points.clear();
            return false;
        }

        cursor = *next;
        covered += step;
        parameters.push_back(cursor);
        points.push_back(curve.evaluate(cursor));
    }

    parameters.push_back(last);
    points.push_back(curve.evaluate(last));
    return true;
}

/**
 * @brief Fills points whose chords stay within @p deviation of the curve, measured at each chord's parameter midpoint.
 *
 * @details
 * - A chord from `a` to `b` is accepted when `|C((a+b)/2) - (P(a) + P(b))/2|`, the sagitta at the parameter midpoint, is
 *   at most @p deviation; otherwise the chord is halved and the two halves are judged the same way.
 * - The window is first cut into four chords whether or not it needs them: one midpoint test is blind to an S-shaped
 *   curve, whose midpoint can lie exactly on the chord joining its ends, and four seeds turn that coincidence into one
 *   that has to repeat on every quarter.
 * - Only `evaluate()` is used and every trial costs one evaluation (the endpoints come from the parent chord), so the cost
 *   is linear in the number of points. No derivative, no distance function, no knowledge of the curve's type.
 * - The budget is a refusal, not a truncation: a run that would need more than @p max_points answers false instead of
 *   returning a coarser polyline that no longer honours @p deviation.
 *
 * @param curve      The curve to sample; must be usable.
 * @param first      Lower parameter of the window.
 * @param last       Upper parameter of the window; must not precede @p first.
 * @param deviation  Largest sagitta a chord may have, measured at the chord's parameter midpoint; must be positive.
 * @param max_points Maximum number of points to produce; must be at least 2.
 * @param parameters Output: one parameter per point, strictly increasing.
 * @param points     Output: the point at each parameter, in the same order.
 * @return True when the arrays were filled; false for a degenerate request, an exhausted budget or a parameter interval
 *         that can no longer be halved, with both arrays left empty.
 *
 * @warning
 * - The guarantee is on the sampled midpoint, not on the segment's true maximum distance: a curve that bulges away from
 *   the chord between midpoints can exceed @p deviation, and one whose shape repeats faster than the accepted chords is
 *   aliased (`geometry-curve-discretization.md` P1).
 *
 * @tparam T Floating-point scalar.
 * @tparam C Concrete curve type; only `evaluate()` and `Point` are used.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] bool subdivideByDeflection(const C& curve, T first, T last, T deviation, int max_points,
                                         std::vector<T>& parameters, std::vector<typename C::Point>& points)
{
    parameters.clear();
    points.clear();

    if (!(deviation > T(0)) || !std::isfinite(static_cast<double>(deviation)) || max_points < 2 || !(last > first)) {
        return false;
    }

    using Point = typename C::Point;

    const auto fail = [&]() {
        parameters.clear();
        points.clear();
        return false;
    };

    parameters.push_back(first);
    points.push_back(curve.evaluate(first));

    // Four seeded chords: `boundaries[0]` is the window's start, `boundaries[kSeed]` its end.
    constexpr int kSeed = 4;
    T             boundaries[kSeed + 1];
    Point         boundary_points[kSeed + 1];
    boundaries[0]      = first;
    boundary_points[0] = points.front();
    const T span       = last - first;
    for (int index = 1; index <= kSeed; ++index) {
        boundaries[index]      = (index == kSeed) ? last : first + span * (static_cast<T>(index) / static_cast<T>(kSeed));
        boundary_points[index] = curve.evaluate(boundaries[index]);
    }

    // Depth cap: halving a parameter interval runs out of room long before this, but a cap makes the loop terminate on a
    // curve whose midpoint evaluation keeps answering a deviation above the request.
    constexpr int kMaxDepth = 60;

    const auto refine = [&](auto&& self, T a, const Point& point_a, T b, const Point& point_b, int depth) -> bool {
        const T    middle       = a + (b - a) * T(0.5);
        const auto middle_point = curve.evaluate(middle);
        const auto chord_middle = point_a + (point_b - point_a) * T(0.5);

        if ((middle_point - chord_middle).length() <= deviation) {
            if (parameters.size() >= static_cast<std::size_t>(max_points)) {
                return false;
            }
            parameters.push_back(b);
            points.push_back(point_b);
            return true;
        }

        if (depth <= 0 || !(middle > a) || !(middle < b)) {
            return false; // the interval cannot be halved any further: the request cannot be met
        }

        return self(self, a, point_a, middle, middle_point, depth - 1)
            && self(self, middle, middle_point, b, point_b, depth - 1);
    };

    for (int index = 0; index < kSeed; ++index) {
        if (!refine(refine, boundaries[index], boundary_points[index], boundaries[index + 1], boundary_points[index + 1],
                    kMaxDepth)) {
            return fail();
        }
    }

    return true;
}

VN_GEOM_DETAIL_NS_END
