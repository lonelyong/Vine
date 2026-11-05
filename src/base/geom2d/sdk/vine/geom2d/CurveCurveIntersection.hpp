#pragma once

#include "geom2d_global.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"
#include "CurveProjection.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief What the intersection of two planar curves turned out to be.
 *
 * @details
 * - `Isolated` and `Coincident` are different answers, not different amounts of the same one: two arcs on the same
 *   circle share a stretch, and listing that stretch as points would be both wrong and unbounded, so it is reported
 *   as one fact instead.
 * - `None` is only "no crossing was found". A tangency has no transversal crossing and is found only if a sample
 *   lands on it (P1).
 */
enum class CurveCurveContact : std::uint8_t
{
    None,      ///< No crossing was found, or the input was unusable.
    Isolated,  ///< Finitely many crossings; `hits` holds them in ascending first-curve parameter.
    Coincident ///< A stretch of the two curves coincides, so the intersection is not a point set.
};

/**
 * @brief One crossing of two planar curves.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
struct CurveCurveHit {
    /// Parameter on the first curve, in its own units.
    T first_parameter{ T(0) };

    /// Parameter on the second curve, in its own units.
    T second_parameter{ T(0) };

    /// The crossing point; equal to both `first.evaluate(first_parameter)` and `second.evaluate(second_parameter)`.
    vn::math::Point2<T> point;
};

/**
 * @brief The result of intersecting two planar curves.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
struct CurveCurveIntersection {
    /// Which of the three answers this is.
    CurveCurveContact contact{ CurveCurveContact::None };

    /// The crossings, ascending by first parameter; empty unless `contact` is `Isolated`.
    std::vector<CurveCurveHit<T>> hits;
};

namespace detail
{

/**
 * @brief The scalar a curve type is instantiated with.
 *
 * @details
 * - It exists so the intersection signature can name the scalar without taking it as a template parameter: both
 *   curves already carry it as `value_type`, and a parameter that only appears in default arguments cannot be
 *   deduced.
 *
 * @tparam C Concrete curve type.
 */
template <typename C>
using CurveScalar = typename C::value_type;

/**
 * @brief A parameter window for a curve type, or nothing to use the curve's own domain.
 *
 * @tparam C Concrete curve type.
 */
template <typename C>
using CurveWindow = std::optional<std::pair<CurveScalar<C>, CurveScalar<C>>>;

/**
 * @brief Refines a pair of parameters towards `C1(u) == C2(v)` with a bounded Newton iteration.
 *
 * @details
 * - The residual is two-dimensional, so the step solves `[C1'(u), -C2'(v)] * [du, dv] = -(C1(u) - C2(v))`. A singular
 *   Jacobian means the tangents are parallel - a tangency or a coincident stretch - and the current pair is accepted
 *   as is when the residual is already inside @p tolerance.
 * - Steps are clamped to a quarter of each window, so a bad Newton step cannot jump to a different branch.
 *
 * @param first      First curve.
 * @param second     Second curve.
 * @param first_range  Search window on @p first.
 * @param second_range Search window on @p second.
 * @param start_u    Starting parameter on @p first.
 * @param start_v    Starting parameter on @p second.
 * @param tolerance  Geometric tolerance of the residual.
 * @return The refined pair, or `std::nullopt` when the iteration did not converge.
 */
template <vn::math::FP T, typename C1, typename C2>
[[nodiscard]] std::optional<std::pair<T, T>> refineCurveCurveCrossing(const C1& first, const C2& second,
                                                                     const std::pair<T, T>& first_range,
                                                                     const std::pair<T, T>& second_range, T start_u,
                                                                     T start_v, T tolerance)
{
    const T max_step_u = (first_range.second - first_range.first) * T(0.25);
    const T max_step_v = (second_range.second - second_range.first) * T(0.25);

    T u = start_u;
    T v = start_v;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto    first_point  = first.evaluate(u);
        const auto    second_point = second.evaluate(v);
        const auto    offset       = first_point - second_point;
        if (offset.length() <= tolerance) {
            return std::pair<T, T>{ u, v };
        }

        const auto first_tangent  = first.derivative(u);
        const auto second_tangent = second.derivative(v);
        const T    determinant    = first_tangent.y * second_tangent.x - first_tangent.x * second_tangent.y;
        if (!(std::abs(determinant) > vn::math::EPS<T>())) {
            return std::nullopt;
        }

        T step_u = (offset.x * second_tangent.y - second_tangent.x * offset.y) / determinant;
        T step_v = (offset.x * first_tangent.y - first_tangent.x * offset.y) / determinant;
        step_u   = std::clamp(step_u, -max_step_u, max_step_u);
        step_v   = std::clamp(step_v, -max_step_v, max_step_v);

        u = std::clamp(u + step_u, first_range.first, first_range.second);
        v = std::clamp(v + step_v, second_range.first, second_range.second);
    }

    return std::nullopt;
}

} // namespace detail

/**
 * @brief Returns the crossings of two planar curves inside their windows.
 *
 * @details
 * - The first curve is walked. At every sample the nearest point of the second curve is taken with
 *   `closestPointOnCurve()`, which answers both questions at once: a sample within @p tolerance is a candidate, and a
 *   **run** of at least three consecutive such samples is a coincident stretch rather than a crossing. Short runs are
 *   refined with Newton on `C1(u) == C2(v)`.
 * - Only `evaluate()` and `derivative()` are used, so any two `Curve<T>` implementations work.
 * - The windows are the caller's; without one a bounded curve uses its own domain. **An unbounded curve must be
 *   narrowed by the caller** - the same rule as the nearest-point query (C5).
 * - Hits are ascending by the first curve's parameter, and duplicates found from neighbouring samples are merged by
 *   point distance.
 *
 * @param first          The curve to walk; must satisfy `isValid()`.
 * @param second         The curve to project onto; must satisfy `isValid()`.
 * @param first_window   Parameter window on @p first, or nothing to use its own domain.
 * @param second_window  Parameter window on @p second, or nothing to use its own domain.
 * @param tolerance      Geometric tolerance: how close the two curves must be to count as crossing.
 * @param sample_count   Number of scan intervals on @p first; more finds finer crossings and costs more.
 * @return The crossings, the coincidence classification, or `None`.
 *
 * @warning
 * - This is a sampling method: a tangency - where the curves touch without crossing - has no transversal root and is
 *   found only when a sample lands on it (P1). Two crossings closer than three sampling steps are read as a
 *   coincident stretch.
 *
 * @tparam T  Floating-point scalar, deduced from the arguments.
 * @tparam C1 Concrete type of @p first; must derive from `Curve<T>`.
 * @tparam C2 Concrete type of @p second; must derive from `Curve<T>`; both must use the same scalar.
 */
// clang-format off
// The declaration starts on the line carrying the opening parenthesis so the documentation gate can attach this block
// to it; LLVM style would move the name onto its own line, which the gate reads as an undocumented declaration.
template <typename C1, typename C2>
[[nodiscard]] CurveCurveIntersection<detail::CurveScalar<C1>> intersectCurveWithCurve(
    const C1& first, const C2& second, detail::CurveWindow<C1> first_window = std::nullopt,
    detail::CurveWindow<C2> second_window = std::nullopt,
    detail::CurveScalar<C1> tolerance = vn::math::EPS<detail::CurveScalar<C1>>(), int sample_count = 64)
    requires std::is_same_v<detail::CurveScalar<C1>, detail::CurveScalar<C2>>
          && std::is_base_of_v<Curve<detail::CurveScalar<C1>>, C1>
          && std::is_base_of_v<Curve<detail::CurveScalar<C2>>, C2>
{
    // clang-format on
    using T = detail::CurveScalar<C1>;

    CurveCurveIntersection<T> result;
    if (!first.isValid() || !second.isValid()) {
        return result;
    }

    const auto resolve = [](const auto& curve, const std::optional<std::pair<T, T>>& window) {
        T low  = curve.firstParameter();
        T high = curve.lastParameter();
        if (window) {
            low  = window->first;
            high = window->second;
        }
        if (!std::isfinite(low) || !std::isfinite(high) || !(high > low)) {
            return std::optional<std::pair<T, T>>{};
        }
        return std::optional<std::pair<T, T>>{ std::pair<T, T>{ low, high } };
    };

    const auto first_range  = resolve(first, first_window);
    const auto second_range = resolve(second, second_window);
    if (!first_range || !second_range) {
        return result;
    }

    const T   on_curve = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    const int steps    = std::clamp(sample_count, 8, 512);
    const T   step     = (first_range->second - first_range->first) / static_cast<T>(steps);

    int near_run = 0;
    for (int index = 0; index <= steps; ++index) {
        const T u = (index == steps) ? first_range->second : first_range->first + step * static_cast<T>(index);

        const auto projection = closestPointOnCurve(second, first.evaluate(u), second_window, on_curve, steps);
        if (!projection) {
            near_run = 0;
            continue;
        }

        if (projection->distance <= on_curve) {
            if (++near_run >= 3) {
                result.contact = CurveCurveContact::Coincident;
                result.hits.clear();
                return result;
            }
        } else {
            near_run = 0;
        }

        // A crossing lies within about one sampling step of some sample, because one step moves the point by
        // `step * |C1'|`; anything farther cannot be pulled in by a local iteration.
        const T speed = first.derivative(u).length();
        const T reach = std::max(on_curve * T(4), step * speed);
        if (!(projection->distance <= reach)) {
            continue;
        }

        const auto refined =
            detail::refineCurveCurveCrossing(first, second, *first_range, *second_range, u, projection->parameter, on_curve);
        if (!refined) {
            continue;
        }

        const auto point = first.evaluate(refined->first);
        const bool known = std::any_of(result.hits.begin(), result.hits.end(),
                                       [&](const CurveCurveHit<T>& hit) { return (hit.point - point).length() <= on_curve; });
        if (!known) {
            result.hits.push_back(CurveCurveHit<T>{ refined->first, refined->second, point });
        }

        result.contact = CurveCurveContact::Isolated;
    }

    std::sort(result.hits.begin(), result.hits.end(),
              [](const CurveCurveHit<T>& left, const CurveCurveHit<T>& right) { return left.first_parameter < right.first_parameter; });
    return result;
}

VN_GEOM2D_NS_END
