#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <optional>
#include <utility>

#include <vine/math/Math.hpp>

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief The scalar a curve type is instantiated with.
 *
 * @details
 * - It exists so a family-level algorithm can name the scalar without taking it as a template parameter: the curve
 *   already carries it as `value_type`, and a parameter that only appears in default arguments cannot be deduced.
 * - It lives in the module's shared `detail` rather than in one tree's header because every family algorithm in both
 *   `geom2d` and `geom3d` names it, and a helper reachable only through a projection header is a hidden dependency.
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
 * @brief Turns a caller's tolerance into the one to run with: a non-positive value means `EPS<T>()`.
 *
 * @details
 * - Every family algorithm publishes the same rule ("a non-positive value falls back to `EPS<T>()`"), and this is that
 *   rule in one place: without it each entry point would spell out the same conditional, and one of them would
 *   eventually forget one of the two branches.
 *
 * @param tolerance Caller's absolute tolerance, in the units of the quantity being measured.
 * @return @p tolerance when it is positive, `vn::math::EPS<T>()` otherwise.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
[[nodiscard]] constexpr T resolveTolerance(T tolerance)
{
    return (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
}

/**
 * @brief Resolves a caller-supplied window against a curve's own domain, refusing anything not finite and forward.
 *
 * @details
 * - This is the family window rule in one place: an unbounded curve must be narrowed by the caller, and a missing,
 *   reversed or zero-width window has no answer rather than a silently invented one.
 * - It is the *only* place that reads `firstParameter()` / `lastParameter()` for a query, so a curve type whose domain
 *   is not finite cannot be searched by accident.
 *
 * @param curve  The curve the window belongs to.
 * @param window The caller's window, or nothing to use the curve's own domain.
 * @return The finite forward window, or `std::nullopt` when there is none.
 *
 * @tparam T Floating-point scalar of the curve.
 * @tparam C Concrete curve type.
 */
template <vn::math::FP T, typename C>
[[nodiscard]] std::optional<std::pair<T, T>> resolveCurveWindow(const C& curve,
                                                               const std::optional<std::pair<T, T>>& window)
{
    T first = curve.firstParameter();
    T last  = curve.lastParameter();
    if (window) {
        first = window->first;
        last  = window->second;
    }

    if (!std::isfinite(first) || !std::isfinite(last) || !(last > first)) {
        return std::nullopt;
    }

    return std::pair<T, T>{ first, last };
}

VN_GEOM_DETAIL_NS_END
