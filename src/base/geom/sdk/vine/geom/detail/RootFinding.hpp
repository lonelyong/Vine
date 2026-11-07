#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <concepts>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief Returns the root of a function inside a bracket the caller already knows straddles it.
 *
 * @details
 * - The name is the contract: there is a **bracket**, and the **root** is inside it. This function does not search for
 *   roots - it does not sample, split the domain or find sign changes. The caller supplies two parameters where the
 *   function has opposite signs, which is what a scan over a curve's window has already established.
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
 * - What it is **not**: this is not a general equation solver. It handles one root that **changes sign** inside one
 *   bracket; it does not find tangential roots (even multiplicity, no sign change to bracket), it picks whichever root
 *   the bracket happens to contain when there are several, it has no derivative or secant step (so a skewed bracket
 *   converges at the bisection rate), it solves no systems and no complex domain, and it makes no claim about `|f|` at
 *   the answer - only about the width of the bracket that contains the root.
 *
 * @param function       Continuous, and finite at every parameter the search samples.
 * @param lower          Lower end of the bracket.
 * @param upper          Upper end of the bracket.
 * @param tolerance      Bracket width at which the search stops; must be positive.
 * @param max_iterations Hard cap, so a flat or hostile function cannot spin.
 * @return A parameter inside the final bracket, or `std::nullopt` when the bracket does not straddle a root.
 *
 * @note
 * - [`parameterReaching()`] is the sibling for a monotone quantity with a target value instead of a sign change.
 *
 * @tparam T        Floating-point scalar.
 * @tparam Function Callable answering `T(T)`.
 */
template <vn::math::FP T, typename Function>
[[nodiscard]] std::optional<T> rootInBracket(Function&& function, T lower, T upper, T tolerance,
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

/**
 * @brief Returns the parameter at which a monotone quantity reaches a target, inside a bracket the caller supplies.
 *
 * @details
 * - The name says what comes back: a **parameter** - not the value, and not a root of anything in particular. Use it to
 *   invert a monotone quantity: the travelled length along a curve at a given speed, the volume under a rising profile,
 *   any `value(u)` that only increases or only decreases.
 * - Each step jumps by `(value(u) - target) / slope(u)`, which is Newton's step for such a quantity, whose derivative
 *   is the speed. When that jump leaves the bracket, or when the slope is too small to divide by, the step becomes a
 *   bisection instead, so a flat or fast section cannot stall the search.
 * - The bracket belongs to the caller and must contain the answer: `value(lower) <= target <= value(upper)` for an
 *   increasing @p value. The function is not required to clamp, because a caller that knows the monotone quantity is
 *   already able to bracket it.
 * - The stop rule is on the **value**, not on the parameter: the search ends when `|value(u) - target| <= tolerance`.
 *   That is what a caller measuring a length wants; a parameter tolerance would mean something different on every
 *   parameterisation.
 * - A `std::nullopt` from @p value (an integral that failed, a degenerate sample) ends the search with `std::nullopt`
 *   rather than being read as a value; the iteration also stops early when a step cannot make progress.
 * - What it is **not**: it inverts only a **monotone** quantity, so it cannot find the several answers of a quantity
 *   that goes up and down inside the bracket, and it takes @p slope on trust - a wrong slope only costs iterations,
 *   a wrong @p value silently stops at the wrong parameter. It is not a minimiser and not an edge finder: those start
 *   from a different question.
 *
 * @param value          Monotone quantity as a function of the parameter, answering `std::optional<T>`.
 * @param slope          Its derivative at the parameter, used for the Newton step.
 * @param lower          Lower end of the bracket.
 * @param upper          Upper end of the bracket; must not precede @p lower.
 * @param target         Value to reach.
 * @param tolerance      Absolute value tolerance at which the search stops; must be positive.
 * @param max_iterations Hard cap on the iteration count.
 * @return The parameter reaching @p target, or `std::nullopt` for a degenerate request, a failed evaluation, or a
 *         bracket the caller passed the wrong way round.
 *
 * @warning
 * - The answer is only as precise as @p value: the loop stops as soon as the value is inside @p tolerance, so a coarse
 *   @p value needs a loose tolerance or the search stops at a parameter that does not satisfy a tighter claim.
 *
 * @note
 * - [`rootInBracket()`] is the sibling for a sign change instead of a target value.
 *
 * @tparam T        Floating-point scalar.
 * @tparam Function Callable answering `std::optional<T>(T)`.
 * @tparam Slope    Callable answering `T(T)`.
 */
template <vn::math::FP T, typename Function, typename Slope>
    requires std::invocable<Function&, T> && std::invocable<Slope&, T>
[[nodiscard]] std::optional<T> parameterReaching(Function&& value, Slope&& slope, T lower, T upper, T target,
                                                 T tolerance, int max_iterations = 48)
{
    if (!(tolerance > T(0)) || !(upper > lower) || max_iterations < 1) {
        return std::nullopt;
    }

    T u = lower + (upper - lower) * T(0.5);
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        const auto reached = value(u);
        if (!reached) {
            return std::nullopt;
        }

        const T error = *reached - target;
        if (std::abs(error) <= tolerance) {
            return u;
        }

        if (error < T(0)) {
            lower = u;
        } else {
            upper = u;
        }

        const T rate = slope(u);
        T       next = (rate > vn::math::EPS<T>()) ? u - error / rate : (lower + upper) / T(2);
        if (!(next > lower && next < upper)) {
            next = (lower + upper) / T(2);
        }
        if (next == u) {
            break;
        }

        u = next;
    }

    return u;
}

VN_GEOM_DETAIL_NS_END
