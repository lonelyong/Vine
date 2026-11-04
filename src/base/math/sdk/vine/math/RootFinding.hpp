#pragma once

#include "math_global.hpp"

#include <cmath>
#include <optional>

#include "Math.hpp"
#include "Types.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief Narrows a bracket whose two ends straddle a root, or reports that it does not straddle one.
 *
 * @details
 * - The search is **safeguarded false position**: each step takes the false-position estimate and falls back to
 *   bisection whenever that estimate lands within a tenth of the bracket width from an end. Plain false position can
 *   stall with one end fixed for many iterations (its estimate creeps towards the fixed end), and plain Newton or
 *   secant can leave the bracket or stall at a flat spot; this combination keeps both the guaranteed halving and the
 *   fast first steps.
 * - The bracket belongs to the caller: `function(lower)` and `function(upper)` must have opposite signs, or one of
 *   them may be exactly zero, which is returned as the root. A bracket that does not straddle a root answers
 *   `std::nullopt` rather than an endpoint - from inside one bracket, "the root is elsewhere" and "there is no root"
 *   cannot be told apart.
 * - The tolerance is on the **parameter**, not on the function value: the search stops when the bracket is narrower
 *   than @p tolerance. A caller working in geometry converts a distance tolerance into a parameter one before calling
 *   it (`tolerance / |dP/du|`), because those two only coincide for an arc-length parameterisation.
 * - A non-finite value from @p function (a NaN at a degenerate parameter) is reported as `std::nullopt` instead of
 *   being compared: every comparison with NaN is false, so a bracketing test would silently take the "no root" branch.
 * - No allocation, no retained state: @p function is only ever called.
 *
 * @param function       Continuous function of one parameter.
 * @param lower          Lower end of the bracket.
 * @param upper          Upper end of the bracket.
 * @param tolerance      Bracket width at which the search stops; must be positive.
 * @param max_iterations Hard cap, so a flat or hostile function cannot spin.
 * @return A parameter inside the final bracket, or `std::nullopt` when the bracket does not straddle a root.
 *
 * @tparam T        Floating-point scalar.
 * @tparam Function Callable answering `T(T)`.
 */
template <vn::math::FP T, typename Function>
[[nodiscard]] std::optional<T> refineRoot(Function&& function, T lower, T upper, T tolerance,
                                          int max_iterations = 64)
{
    if (!(tolerance > T(0)) || max_iterations < 1 || !(upper > lower)) {
        return std::nullopt;
    }

    T f_lower = function(lower);
    T f_upper = function(upper);
    if (std::isnan(f_lower) || std::isnan(f_upper)) {
        return std::nullopt;
    }

    if (f_lower == T(0)) {
        return lower;
    }
    if (f_upper == T(0)) {
        return upper;
    }
    if ((f_lower < T(0)) == (f_upper < T(0))) {
        return std::nullopt;
    }

    T width = upper - lower;
    for (int iteration = 0; iteration < max_iterations && width > tolerance; ++iteration) {
        T estimate = upper - f_upper * width / (f_upper - f_lower);
        const T guard = width * T(0.1);
        if (!(estimate > lower + guard) || !(estimate < upper - guard)) {
            estimate = lower + width * T(0.5);
        }

        const T f_estimate = function(estimate);
        if (std::isnan(f_estimate)) {
            return std::nullopt;
        }
        if (f_estimate == T(0)) {
            return estimate;
        }

        if ((f_estimate < T(0)) == (f_lower < T(0))) {
            lower   = estimate;
            f_lower = f_estimate;
        } else {
            upper   = estimate;
            f_upper = f_estimate;
        }
        width = upper - lower;
    }

    return lower + width * T(0.5);
}

VN_MATH_NS_END
